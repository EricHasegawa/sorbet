# typed: true

class A
  def f(a, b, c)
    if a + # parser-error: missing arg to "+" operator
    #     ^ completion: a, b, c, ...
    #      ^ completion: a, b, c, ...
    end

    if a == # parser-error: missing arg to "==" operator
    #      ^ completion: a, b, c, ...
    #       ^ completion: a, b, c, ...
    end
  end
end
