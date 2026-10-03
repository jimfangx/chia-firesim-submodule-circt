FIRRTL version 3.2.0
circuit Top :
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input tick : Clock
    input valid : UInt<1>
    input data : UInt<8>
    output hubResult : UInt<8>
    output virtualResult : UInt<8>
    inst hub of Hub
    hub.hostClock <= hostClock
    hub.hostReset <= hostReset
    hub.tick_sink.valid <= valid
    hub.tick_sink.bits <= tick
    hub.input_sink.valid <= valid
    hub.input_sink.bits <= data
    hub.output_source.ready <= UInt<1>(1)
    hubResult <= hub.output_source.bits
    inst other of Other
    other.hostClock <= hostClock
    other.hostReset <= hostReset
    other.target <= hostClock
    other.tick_sink.valid <= valid
    other.tick_sink.bits <= data
    other.output_source.ready <= UInt<1>(1)
    virtualResult <= other.output_source.bits
  module Hub :
    input hostClock : Clock
    input hostReset : UInt<1>
    input tick_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : Clock }
    input input_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    output output_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    wire targetCycleFinishing : UInt<1>
    targetCycleFinishing <= UInt<1>(0)
    regreset tick_enabled : UInt<1>, hostClock, hostReset, UInt<1>(0)
    tick_enabled <= mux(targetCycleFinishing, asUInt(tick_sink.bits), tick_enabled)
    inst tick_buffer of AbstractClockGate
    tick_buffer.I <= hostClock
    tick_buffer.CE <= and(and(tick_enabled, targetCycleFinishing), not(hostReset))
    reg state : UInt<8>, tick_buffer.O
    state <= add(state, UInt<8>(1))
    tick_sink.ready <= UInt<1>(0)
    input_sink.ready <= UInt<1>(0)
    output_source.valid <= UInt<1>(0)
    output_source.bits <= input_sink.bits
  module Other :
    input hostClock : Clock
    input hostReset : UInt<1>
    input target : Clock
    input tick_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    output output_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    wire targetCycleFinishing : UInt<1>
    targetCycleFinishing <= UInt<1>(0)
    reg state : UInt<8>, target
    state <= add(state, UInt<8>(1))
    tick_sink.ready <= UInt<1>(0)
    output_source.valid <= UInt<1>(0)
    output_source.bits <= tick_sink.bits
  extmodule AbstractClockGate :
    input I : Clock
    input CE : UInt<1>
    output O : Clock
    defname = AbstractClockGate
