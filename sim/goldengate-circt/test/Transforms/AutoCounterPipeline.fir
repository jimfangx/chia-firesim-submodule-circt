FIRRTL version 1.1.0
circuit Top :
  module Leaf :
    input clock : Clock
    input reset : UInt<1>
    input event : UInt<8>
    input value : UInt<64>
    output observed : UInt<64>
    node clockAlias = clock
    wire localClock : Clock
    localClock <= clockAlias
    wire leafCount : UInt<8>
    leafCount <= event
    reg state : UInt<64>, clock with :
      reset => (reset, UInt<64>(0))
    state <= bits(add(state, value), 63, 0)
    observed <= state
  module Mid :
    input clock : Clock
    input reset : UInt<1>
    input event : UInt<8>
    input value : UInt<64>
    output observed : UInt<64>
    inst leaf of Leaf
    leaf.clock <= clock
    leaf.reset <= reset
    leaf.event <= event
    leaf.value <= value
    observed <= leaf.observed
  module Ignored :
    input clock : Clock
  module Top :
    input clock0 : Clock
    input clock1 : Clock
    input reset0 : UInt<1>
    input reset1 : UInt<1>
    input event0 : UInt<8>
    input event1 : UInt<8>
    input value0 : UInt<64>
    input value1 : UInt<64>
    input topEvent : UInt<4>
    output targetClock0 : Clock
    output targetClock1 : Clock
    output observed0 : UInt<64>
    output observed1 : UInt<64>
    targetClock0 <= clock0
    targetClock1 <= clock1
    node globalReset = or(reset0, reset1)
    wire topCount : UInt<4>
    topCount <= topEvent
    inst left of Mid
    inst right of Mid
    left.clock <= clock0
    right.clock <= clock1
    left.reset <= reset0
    right.reset <= reset1
    left.event <= event0
    right.event <= event1
    left.value <= value0
    right.value <= value1
    observed0 <= left.observed
    observed1 <= right.observed
