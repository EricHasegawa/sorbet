#include "ast/desugar/Desugar.h"
#include "ast/substitute/substitute.h"
#include "ast/treemap/treemap.h"
#include "cfg/CFG.h"
#include "cfg/builder/builder.h"
#include "common/ConcurrentQueue.h"
#include "common/ProgressIndicator.h"
#include "core/Unfreeze.h"
#include "core/proto/proto.h"
#include "core/serialize/serialize.h"
#include "dsl/dsl.h"
#include "infer/infer.h"
#include "namer/configatron/configatron.h"
#include "namer/namer.h"
#include "parser/parser.h"
#include "realmain.h"
#include "resolver/resolver.h"
#include "spdlog/fmt/ostr.h"

using namespace std;

namespace ruby_typer {
namespace realmain {

bool wantTypedSource(const Options &opts, core::Context ctx, core::FileRef file) {
    if (opts.print.TypedSource) {
        return true;
    }
    if (opts.typedSource.empty()) {
        return false;
    }
    return file.data(ctx).path().find(opts.typedSource) != string::npos;
}

class CFG_Collector_and_Typer {
    const Options &opts;

public:
    CFG_Collector_and_Typer(const Options &opts) : opts(opts){};

    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> m) {
        if (m->loc.file.data(ctx).strict == core::StrictLevel::Stripe) {
            return m;
        }
        auto &print = opts.print;
        auto cfg = cfg::CFGBuilder::buildFor(ctx.withOwner(m->symbol), *m);

        bool printSrc = wantTypedSource(opts, ctx, m->loc.file);

        if (print.CFGRaw || printSrc) {
            cfg = cfg::CFGBuilder::addDebugEnvironment(ctx.withOwner(m->symbol), move(cfg));
        }
        if (opts.stopAfterPhase == Phase::CFG) {
            return m;
        }
        cfg = infer::Inference::run(ctx.withOwner(m->symbol), move(cfg));
        if (print.CFG || print.CFGRaw) {
            cout << cfg->toString(ctx) << '\n' << '\n';
        }
        if (printSrc) {
            cfg->recordAnnotations(ctx);
        }
        return m;
    }
};

struct thread_result {
    unique_ptr<core::GlobalState> gs;
    core::CounterState counters;
    vector<unique_ptr<ast::Expression>> trees;
};

unique_ptr<ast::Expression> indexOne(const Options &opts, core::GlobalState &lgs, core::FileRef file,
                                     std::unique_ptr<KeyValueStore> &kvstore,
                                     const shared_ptr<core::GlobalState> &pgs) {
    auto &print = opts.print;
    std::unique_ptr<ast::Expression> dslsInlined;

    try {
        if (kvstore && file.id() < pgs->filesUsed()) {
            auto maybeCached = kvstore->read(file.data(*pgs).hashKey());
            if (maybeCached) {
                auto t = core::serialize::Serializer::loadExpression(*pgs, maybeCached);
                t->loc.file.data(*pgs).cachedParseTree = true;
                dslsInlined = move(t);
            }
        }
        if (!dslsInlined) {
            // tree isn't cached. Need to start from parser

            std::unique_ptr<parser::Node> nodes;
            {
                logger->trace("Parsing: {}", file.data(lgs).path());
                core::ErrorRegion errs(lgs, file);
                core::UnfreezeNameTable nameTableAccess(lgs); // enters strings from source code as names
                nodes = parser::Parser::run(lgs, file);
            }
            if (print.ParseTree) {
                cout << nodes->toString(lgs, 0) << '\n';
            }
            if (print.ParseTreeJSON) {
                cout << nodes->toJSON(lgs, 0) << '\n';
            }
            if (opts.stopAfterPhase == Phase::PARSER) {
                return make_unique<ast::EmptyTree>(core::Loc::none(file));
            }

            std::unique_ptr<ast::Expression> ast;
            core::MutableContext ctx(lgs, core::Symbols::root());
            {
                logger->trace("Desugaring: {}", file.data(lgs).path());
                core::ErrorRegion errs(lgs, file);
                core::UnfreezeNameTable nameTableAccess(lgs); // creates temporaries during desugaring
                ast = ast::desugar::node2Tree(ctx, move(nodes));
            }
            if (print.Desugared) {
                cout << ast->toString(lgs, 0) << '\n';
            }
            if (print.DesugaredRaw) {
                cout << ast->showRaw(lgs) << '\n';
            }
            if (opts.stopAfterPhase == Phase::DESUGARER) {
                return make_unique<ast::EmptyTree>(core::Loc::none(file));
            }

            {
                logger->trace("Inlining DSLs: {}", file.data(lgs).path());
                core::ErrorRegion errs(lgs, file);
                dslsInlined = dsl::DSL::run(ctx, move(ast));
            }
        }
        if (print.DSLTree) {
            cout << dslsInlined->toString(lgs, 0) << '\n';
        }
        if (print.DSLTreeRaw) {
            cout << dslsInlined->showRaw(lgs) << '\n';
        }
        if (opts.stopAfterPhase == Phase::DSL) {
            return make_unique<ast::EmptyTree>(core::Loc::none(file));
        }

        return dslsInlined;
    } catch (SRubyException &) {
        if (auto e = lgs.beginError(ruby_typer::core::Loc::none(file), core::errors::Internal::InternalError)) {
            e.setHeader("Exception parsing file: `{}` (backtrace is above)", file.data(lgs).path());
        }
        return make_unique<ast::EmptyTree>(core::Loc::none(file));
    }
}

vector<unique_ptr<ast::Expression>> index(shared_ptr<core::GlobalState> &gs, std::vector<std::string> frs,
                                          std::vector<core::FileRef> mainThreadFiles, const Options &opts,
                                          WorkerPool &workers, std::unique_ptr<KeyValueStore> &kvstore) {
    vector<unique_ptr<ast::Expression>> ret;
    vector<unique_ptr<ast::Expression>> empty;

    if (opts.stopAfterPhase == Phase::INIT) {
        return empty;
    }

    shared_ptr<ConcurrentBoundedQueue<std::pair<int, std::string>>> fileq;

    shared_ptr<BlockingBoundedQueue<thread_result>> resultq;

    {
        Timer timeit(logger, "creating index queues");

        fileq = make_shared<ConcurrentBoundedQueue<std::pair<int, std::string>>>(frs.size());
        resultq = make_shared<BlockingBoundedQueue<thread_result>>(frs.size());
    }

    int i = gs->filesUsed();
    for (auto f : frs) {
        logger->trace("enqueue: {}", f);
        std::pair<int, std::string> job(i++, f);
        fileq->push(move(job), 1);
    }

    gs->sanityCheck();

    const std::shared_ptr<core::GlobalState> cgs = gs;
    gs = nullptr;
    logger->trace("Done deep copying global state");
    {
        ProgressIndicator indexingProgress(opts.showProgress, "Indexing", frs.size());

        workers.multiplexJob([cgs, &opts, fileq, resultq, &kvstore]() {
            logger->trace("worker deep copying global state");
            auto lgs = cgs->deepCopy();
            logger->trace("worker done deep copying global state");
            thread_result threadResult;
            int processedByThread = 0;
            std::pair<int, std::string> job;

            {
                for (auto result = fileq->try_pop(job); !result.done(); result = fileq->try_pop(job)) {
                    if (result.gotItem()) {
                        core::ErrorRegion errs(*lgs, core::FileRef(job.first));
                        processedByThread++;
                        std::string fileName = job.second;
                        logger->trace("Reading: {}", fileName);
                        int fileId = job.first;
                        string src;
                        try {
                            src = FileOps::read(fileName.c_str());
                        } catch (FileNotFoundException e) {
                            if (auto e = lgs->beginError(ruby_typer::core::Loc::none(),
                                                         core::errors::Internal::InternalError)) {
                                e.setHeader("File Not Found: `{}`", fileName);
                            }
                            // continue with an empty source, because the
                            // assertion below requires every input file to map
                            // to one output tree
                        }
                        core::prodCounterAdd("types.input.bytes", src.size());
                        core::prodCounterInc("types.input.files");

                        core::FileRef file;
                        {
                            core::UnfreezeFileTable unfreezeFiles(*lgs);
                            file = lgs->enterFileAt(fileName, src, fileId);
                        }
                        if (core::enable_counters) {
                            core::counterAdd("types.input.lines", file.data(*lgs).lineCount());
                        }

                        switch (file.data(*lgs).strict) {
                            case core::StrictLevel::Stripe:
                                core::prodCounterInc("types.input.files.sigil.none");
                                break;
                            case core::StrictLevel::Typed:
                                core::prodCounterInc("types.input.files.sigil.typed");
                                break;
                            case core::StrictLevel::Strict:
                                core::prodCounterInc("types.input.files.sigil.strict");
                                break;
                            case core::StrictLevel::Strong:
                                core::prodCounterInc("types.input.files.sigil.strong");
                                break;
                        }

                        core::StrictLevel minStrict = opts.forceMinStrict;
                        core::StrictLevel maxStrict = opts.forceMaxStrict;
                        if (!opts.typedSource.empty() &&
                            file.data(*lgs).path().find(opts.typedSource) != std::string::npos) {
                            minStrict = core::StrictLevel::Typed;
                        }
                        file.data(*lgs).strict = std::max(std::min(file.data(*lgs).sigil, maxStrict), minStrict);

                        if (!opts.storeState.empty()) {
                            file.data(*lgs).source_type = core::File::PayloadGeneration;
                        }

                        threadResult.trees.emplace_back(indexOne(opts, *lgs, file, kvstore, cgs));
                    }
                }
            }

            if (processedByThread > 0) {
                threadResult.counters = core::getAndClearThreadCounters();
                threadResult.gs = move(lgs);
                resultq->push(move(threadResult), processedByThread);
            }
        });

        logger->trace("Deep copying global state");
        auto mainTheadGs = cgs->deepCopy();
        logger->trace("Done deep copying global state");

        for (auto f : mainThreadFiles) {
            ret.emplace_back(indexOne(opts, *mainTheadGs, f, kvstore, cgs));
        }

        gs = move(mainTheadGs);

        thread_result threadResult;
        {
            logger->trace("Collecting results from indexing threads");
            for (auto result = resultq->wait_pop_timed(threadResult, PROGRESS_REFRESH_TIME_MILLIS); !result.done();
                 result = resultq->wait_pop_timed(threadResult, PROGRESS_REFRESH_TIME_MILLIS)) {
                if (result.gotItem()) {
                    logger->trace("Building global substitution");
                    core::GlobalSubstitution substitution(*threadResult.gs, *gs, cgs.get());
                    logger->trace("Consuming counters");
                    counterConsume(move(threadResult.counters));
                    core::MutableContext ctx(*gs, core::Symbols::root());
                    logger->trace("Running tree substitution");
                    for (auto &tree : threadResult.trees) {
                        auto file = tree->loc.file;
                        if (!file.data(*gs).cachedParseTree) {
                            auto subst = ast::Substitute::run(ctx, substitution, move(tree));
                            if (kvstore) {
                                kvstore->write(file.data(*gs).hashKey(),
                                               core::serialize::Serializer::store(*gs, subst));
                            }
                            ret.emplace_back(move(subst));
                        } else {
                            ret.emplace_back(move(tree));
                        }
                    }
                    logger->trace("Tree substitution done");
                }
                gs->flushErrors();
                indexingProgress.reportProgress(fileq->doneEstimate());
            }
            logger->trace("Done collecting results from indexing threads");
        }
    }
    ENFORCE(mainThreadFiles.size() + frs.size() == ret.size());

    auto by_file = [](unique_ptr<ast::Expression> const &a, unique_ptr<ast::Expression> const &b) {
        return a->loc.file < b->loc.file;
    };
    std::sort(ret.begin(), ret.end(), by_file);

    return ret;
}

unique_ptr<ast::Expression> typecheckOne(core::Context ctx, unique_ptr<ast::Expression> resolved, const Options &opts) {
    unique_ptr<ast::Expression> result;
    core::FileRef f = resolved->loc.file;
    if (opts.stopAfterPhase == Phase::NAMER) {
        return make_unique<ast::EmptyTree>(core::Loc::none(f));
    }

    try {
        if (opts.print.CFG || opts.print.CFGRaw) {
            cout << "digraph \"" << FileOps::getFileName(f.data(ctx).path()) << "\" {" << '\n';
        }
        CFG_Collector_and_Typer collector(opts);
        {
            logger->trace("CFG+Infer: {}", f.data(ctx).path());
            core::ErrorRegion errs(ctx, f);
            result = ast::TreeMap::apply(ctx, collector, move(resolved));
        }
        if (wantTypedSource(opts, ctx, f)) {
            cout << ctx.state.showAnnotatedSource(f);
        }
        if (opts.print.CFG || opts.print.CFGRaw) {
            cout << "}" << '\n' << '\n';
        }
        if (opts.suggestTyped && !f.data(ctx).hadErrors() && f.data(ctx).sigil == core::StrictLevel::Stripe) {
            core::counterInc("types.input.files.suggest_typed");
            logger->error("You could add `# typed: true` to: `{}`", f.data(ctx).path());
        }

    } catch (SRubyException &) {
        if (auto e = ctx.state.beginError(ruby_typer::core::Loc::none(f), core::errors::Internal::InternalError)) {
            e.setHeader("Exception in cfg+infer: {} (backtrace is above)", f.data(ctx).path());
        }
    }
    return result;
}

struct typecheck_thread_result {
    vector<unique_ptr<ast::Expression>> trees;
    core::CounterState counters;
};

vector<unique_ptr<ast::Expression>> resolve(core::GlobalState &gs, vector<unique_ptr<ast::Expression>> what,
                                            const Options &opts) {
    try {
        {
            core::UnfreezeNameTable nameTableAccess(gs);     // creates names from config
            core::UnfreezeSymbolTable symbolTableAccess(gs); // creates methods for them
            namer::configatron::fillInFromFileSystem(gs, opts.configatronDirs, opts.configatronFiles);
        }

        {
            ProgressIndicator namingProgress(opts.showProgress, "Naming", what.size());

            Timer timeit(logger, "naming");
            int i = 0;
            for (auto &tree : what) {
                auto file = tree->loc.file;
                try {
                    unique_ptr<ast::Expression> ast;
                    {
                        core::MutableContext ctx(gs, core::Symbols::root());
                        logger->trace("Naming: {}", file.data(gs).path());
                        core::ErrorRegion errs(gs, file);
                        core::UnfreezeNameTable nameTableAccess(gs);     // creates singletons and class names
                        core::UnfreezeSymbolTable symbolTableAccess(gs); // enters symbols
                        tree = namer::Namer::run(ctx, move(tree));
                    }
                    gs.flushErrors();
                    namingProgress.reportProgress(i);
                    i++;
                } catch (SRubyException &) {
                    if (auto e =
                            gs.beginError(ruby_typer::core::Loc::none(file), core::errors::Internal::InternalError)) {
                        e.setHeader("Exception naming file: `{}` (backtrace is above)", file.data(gs).path());
                    }
                }
            }
        }

        core::MutableContext ctx(gs, core::Symbols::root());
        ProgressIndicator namingProgress(opts.showProgress, "Resolving", 1);
        {
            Timer timeit(logger, "Resolving");
            logger->trace("Resolving (global pass)...");
            core::ErrorRegion errs(gs, ruby_typer::core::FileRef());
            core::UnfreezeNameTable nameTableAccess(gs);     // Resolver::defineAttr
            core::UnfreezeSymbolTable symbolTableAccess(gs); // enters stubs
            what = resolver::Resolver::run(ctx, move(what));
        }
    } catch (SRubyException &) {
        if (auto e = gs.beginError(ruby_typer::core::Loc::none(), core::errors::Internal::InternalError)) {
            e.setHeader("Exception resolving (backtrace is above)");
        }
    }
    gs.flushErrors();

    for (auto &resolved : what) {
        if (opts.print.NameTree) {
            cout << resolved->toString(gs, 0) << '\n';
        }
        if (opts.print.NameTreeRaw) {
            cout << resolved->showRaw(gs) << '\n';
        }
    }
    return what;
}

// If ever given a result type, it should be something along the lines of
// vector<pair<vector<unique_ptr<ast::Expression>>, unique_ptr<core::GlobalState>>>
void typecheck(shared_ptr<core::GlobalState> &gs, vector<unique_ptr<ast::Expression>> what, const Options &opts,
               WorkerPool &workers) {
    vector<vector<unique_ptr<ast::Expression>>> typecheck_result;

    {
        Timer timeit(logger, "Infer+CFG");

        shared_ptr<ConcurrentBoundedQueue<unique_ptr<ast::Expression>>> fileq;
        shared_ptr<BlockingBoundedQueue<typecheck_thread_result>> resultq;

        {
            Timer timeit(logger, "creating typecheck queues");
            fileq = make_shared<ConcurrentBoundedQueue<unique_ptr<ast::Expression>>>(what.size());
            resultq = make_shared<BlockingBoundedQueue<typecheck_thread_result>>(what.size());
        }

        core::Context ctx(*gs, core::Symbols::root());
        for (auto &resolved : what) {
            logger->trace("enqueue-typer {}", resolved->loc.file.data(*gs).path());
            fileq->push(move(resolved), 1);
        }

        {
            ProgressIndicator cfgInferProgress(opts.showProgress, "CFG+Inference", what.size());
            workers.multiplexJob([ctx, &opts, fileq, resultq]() {
                typecheck_thread_result threadResult;
                unique_ptr<ast::Expression> job;
                int processedByThread = 0;

                {
                    for (auto result = fileq->try_pop(job); !result.done(); result = fileq->try_pop(job)) {
                        if (result.gotItem()) {
                            processedByThread++;
                            core::FileRef file = job->loc.file;
                            core::ErrorRegion errs(ctx, file);
                            try {
                                threadResult.trees.emplace_back(typecheckOne(ctx, move(job), opts));
                            } catch (SRubyException &) {
                                logger->error("Exception typing file: {} (backtrace is above)", file.data(ctx).path());
                            }
                        }
                    }
                }
                if (processedByThread > 0) {
                    threadResult.counters = core::getAndClearThreadCounters();
                    resultq->push(move(threadResult), processedByThread);
                }
            });

            typecheck_thread_result threadResult;
            {
                for (auto result = resultq->wait_pop_timed(threadResult, PROGRESS_REFRESH_TIME_MILLIS); !result.done();
                     result = resultq->wait_pop_timed(threadResult, PROGRESS_REFRESH_TIME_MILLIS)) {
                    if (result.gotItem()) {
                        counterConsume(move(threadResult.counters));
                        typecheck_result.emplace_back(move(threadResult.trees));
                    }
                    cfgInferProgress.reportProgress(fileq->doneEstimate());
                    gs->flushErrors();
                }
            }
        }

        if (opts.print.NameTable) {
            cout << gs->toString() << '\n';
        }
        if (opts.print.NameTableJson) {
            auto root = core::Proto::toProto(*gs, core::Symbols::root());
            cout << core::Proto::toJSON(root) << '\n';
        }
        if (opts.print.NameTableFull) {
            cout << gs->toString(true) << '\n';
        }
        return;
    }
}
} // namespace realmain
} // namespace ruby_typer
