FIRRTL version 3.2.0
circuit Top :
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input tick : Clock
    input valid : UInt<1>
    input data : UInt<8>
    inst hub of Hub
    hub.hostClock <= hostClock
    hub.hostReset <= hostReset
    hub.tick_sink.valid <= valid
    hub.tick_sink.bits <= tick
    inst other of Other
    other.hostClock <= hostClock
    other.hostReset <= hostReset
    other.target <= hostClock
  module Hub :
    input hostClock : Clock
    input hostReset : UInt<1>
    input tick_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : Clock }
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
  module Other :
    input hostClock : Clock
    input hostReset : UInt<1>
    input target : Clock
    wire targetCycleFinishing : UInt<1>
    targetCycleFinishing <= UInt<1>(0)
    reg state : UInt<8>, target
    state <= add(state, UInt<8>(1))
  extmodule AbstractClockGate :
    input I : Clock
    input CE : UInt<1>
    output O : Clock
    defname = AbstractClockGate
