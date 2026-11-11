FIRRTL version 3.2.0
circuit Top :
  module Leaf :
    input clock : Clock
    input predicate : UInt<1>
    input enable : UInt<1>
    input fallbackEnable : UInt<1>
    assert(clock, predicate, enable, "leaf assertion") : native
    printf(clock, enable, "legacy assertion\n") : legacyMessage
    stop(clock, enable, 1) : legacy
    stop(clock, fallbackEnable, 7) : fallback
    stop(clock, UInt<1>(1), 0) : success
  module Top :
    input clockA : Clock
    input clockB : Clock
    input predicate : UInt<1>
    input enable : UInt<1>
    input fallbackEnable : UInt<1>
    assert(clockA, predicate, enable, "top assertion") : native
    inst left of Leaf
    left.clock <= clockA
    left.predicate <= predicate
    left.enable <= enable
    left.fallbackEnable <= fallbackEnable
    inst right of Leaf
    right.clock <= clockB
    right.predicate <= predicate
    right.enable <= enable
    right.fallbackEnable <= fallbackEnable
