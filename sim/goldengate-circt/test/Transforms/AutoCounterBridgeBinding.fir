FIRRTL version 1.1.0
circuit Top :
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input domain0 : Clock
    input w0_trigger_valid : UInt<1>
    input w0_trigger_bits : UInt<1>
    output w0_trigger_ready : UInt<1>
    output token0_trigger : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}
    input w0_globalReset_valid : UInt<1>
    input w0_globalReset_bits : UInt<1>
    output w0_globalReset_ready : UInt<1>
    output token0_globalReset : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}
    input w0_event0_valid : UInt<1>
    input w0_event0_bits : UInt<64>
    output w0_event0_ready : UInt<1>
    output token0_event0 : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<64>}
    input w0_event1_valid : UInt<1>
    input w0_event1_bits : UInt<64>
    output w0_event1_ready : UInt<1>
    output token0_event1 : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<64>}
    input domain1 : Clock
    input w1_trigger_valid : UInt<1>
    input w1_trigger_bits : UInt<1>
    output w1_trigger_ready : UInt<1>
    output token1_trigger : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}
    input w1_globalReset_valid : UInt<1>
    input w1_globalReset_bits : UInt<1>
    output w1_globalReset_ready : UInt<1>
    output token1_globalReset : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}
    input w1_event0_valid : UInt<1>
    input w1_event0_bits : UInt<64>
    output w1_event0_ready : UInt<1>
    output token1_event0 : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<64>}
    input w1_event1_valid : UInt<1>
    input w1_event1_bits : UInt<64>
    output w1_event1_ready : UInt<1>
    output token1_event1 : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<64>}
    token0_trigger.valid <= w0_trigger_valid
    token0_trigger.bits <= w0_trigger_bits
    w0_trigger_ready <= token0_trigger.ready
    token0_globalReset.valid <= w0_globalReset_valid
    token0_globalReset.bits <= w0_globalReset_bits
    w0_globalReset_ready <= token0_globalReset.ready
    token0_event0.valid <= w0_event0_valid
    token0_event0.bits <= w0_event0_bits
    w0_event0_ready <= token0_event0.ready
    token0_event1.valid <= w0_event1_valid
    token0_event1.bits <= w0_event1_bits
    w0_event1_ready <= token0_event1.ready
    token1_trigger.valid <= w1_trigger_valid
    token1_trigger.bits <= w1_trigger_bits
    w1_trigger_ready <= token1_trigger.ready
    token1_globalReset.valid <= w1_globalReset_valid
    token1_globalReset.bits <= w1_globalReset_bits
    w1_globalReset_ready <= token1_globalReset.ready
    token1_event0.valid <= w1_event0_valid
    token1_event0.bits <= w1_event0_bits
    w1_event0_ready <= token1_event0.ready
    token1_event1.valid <= w1_event1_valid
    token1_event1.bits <= w1_event1_bits
    w1_event1_ready <= token1_event1.ready
