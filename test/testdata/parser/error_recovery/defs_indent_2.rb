# typed: false
class B
  def self.method1(abc, xyz)
# ^^^ parser-error: Hint: this "def" token might not be properly closed
    puts 'hello'
    puts 'hello'

  def self.method2
  end
end # parser-error: unexpected token "end of file"
