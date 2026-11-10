FIRRTL version 1.2.0
circuit Top :
  module Worker :
    input hostClock : Clock
    input hostReset : UInt<1>
    input input_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    output output_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}

    wire targetCycleFinishing : UInt<1>
    wire alias : Clock
    reg clock_enabled : UInt<1>, hostClock with :
      reset => (hostReset, UInt<1>("h0"))
    inst clock_buffer of AbstractClockGate
    reg input_fired_0 : UInt<1>, hostClock with :
      reset => (hostReset, UInt<1>("h0"))
    reg output_fired_0 : UInt<1>, hostClock with :
      reset => (hostReset, UInt<1>("h0"))
    mem store :
      data-type => UInt<8>
      depth => 4
      read-latency => 0
      write-latency => 1
      reader => r
      writer => w
      read-under-write => old
    output_source.bits <= store.r.data
    alias <= clock_buffer.O
    store.r.addr <= bits(input_sink.bits, 1, 0)
    store.r.en <= UInt<1>("h1")
    store.r.clk <= clock_buffer.O
    store.w.addr <= bits(input_sink.bits, 1, 0)
    store.w.en <= bits(input_sink.bits, 7, 7)
    store.w.clk <= clock_buffer.O
    store.w.data <= pad(bits(input_sink.bits, 5, 0), 8)
    store.w.mask <= bits(input_sink.bits, 6, 6)
    clock_enabled <= mux(targetCycleFinishing, asUInt(UInt<1>("h1")), clock_enabled)
    clock_buffer.I <= hostClock
    clock_buffer.CE <= and(and(clock_enabled, targetCycleFinishing), not(hostReset))
    input_fired_0 <= mux(targetCycleFinishing, not(UInt<1>("h1")), or(input_fired_0, and(input_sink.ready, input_sink.valid)))
    output_fired_0 <= mux(targetCycleFinishing, not(UInt<1>("h1")), or(output_fired_0, and(output_source.ready, output_source.valid)))
    input_sink.ready <= and(targetCycleFinishing, not(input_fired_0))
    output_source.valid <= and(input_sink.valid, not(output_fired_0))
    targetCycleFinishing <= and(and(or(output_fired_0, and(output_source.ready, output_source.valid)), input_sink.valid), UInt<1>("h1"))

  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in0 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    output out0 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    input in1 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    output out1 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    input in2 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    output out2 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    input in3 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}
    output out3 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8>}

    inst worker1 of Worker
    worker1.hostClock <= hostClock
    worker1.hostReset <= hostReset
    worker1.input_sink <= in1
    out1 <= worker1.output_source
    inst worker0 of Worker
    worker0.hostClock <= hostClock
    worker0.hostReset <= hostReset
    worker0.input_sink <= in0
    out0 <= worker0.output_source
    inst worker3 of Worker
    worker3.hostClock <= hostClock
    worker3.hostReset <= hostReset
    worker3.input_sink <= in3
    out3 <= worker3.output_source
    inst worker2 of Worker
    worker2.hostClock <= hostClock
    worker2.hostReset <= hostReset
    worker2.input_sink <= in2
    out2 <= worker2.output_source

  extmodule AbstractClockGate :
    input I : Clock
    input CE : UInt<1>
    output O : Clock
    defname = AbstractClockGate
