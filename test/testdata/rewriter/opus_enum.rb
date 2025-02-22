# typed: strict

module Opus
  class Enum
    extend T::Sig
    sig {params(x: T.nilable(String)).void}
    def initialize(x = nil)
    end

    sig {params(blk: T.proc.void).void}
    def self.enums(&blk); end
  end
end

class MyEnum < Opus::Enum
  enums do
  X = new
  Y = new('y')
  Z = T.let(new, self)
  end
end

T.reveal_type(MyEnum::X) # error: Revealed type: `MyEnum::X`
T.reveal_type(MyEnum::Y) # error: Revealed type: `MyEnum::Y`
T.reveal_type(MyEnum::Z) # error: Revealed type: `MyEnum::Z`

class NotAnEnum
  enums do # error: does not exist
  X = new # error: Constants must have type annotations
  Y = T.let(new, self)
  end
end

T.reveal_type(NotAnEnum::X) # error: Revealed type: `T.untyped`
T.reveal_type(NotAnEnum::Y) # error: Revealed type: `NotAnEnum`
