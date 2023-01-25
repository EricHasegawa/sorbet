#ifndef SORBET_LEVENSTEIN_H
#define SORBET_LEVENSTEIN_H
#include <string_view>

namespace sorbet {

class Levenstein {
public:
    struct CasedDistanceResults {
        int sensitive;
        int insensitive;

        std::pair<int, int> toPair() {
            return std::make_pair(insensitive, sensitive);
        }
    };

    template <bool sensitive>
    static CasedDistanceResults distance(std::string_view s1, std::string_view s2, int bound) noexcept;
};

} // namespace sorbet
#endif // SORBET_LEVENSTEIN_H
