# typed: true

def self.f; Foo::Bar = 1; end # parser-error: dynamic constant assignment
