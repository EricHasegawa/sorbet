# typed: true
module A
  extend T::Sig

  class C1; end

  sig {params(x: C1).returns(NilClass)}
  def f(x)
    C2 = C1 # parser-error: dynamic constant assignment
    nil
  end
end
