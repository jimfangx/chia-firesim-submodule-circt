FIRRTL version 3.2.0
circuit Top :
  module Top :
    input clockA : Clock
    input resetA : UInt<1>
    input clockB : Clock
    input resetB : UInt<1>
    input data : UInt<8>
    output resultA : UInt<8>
    output resultB : UInt<8>
    inst a of Model
    a.hostClock <= clockA
    a.hostReset <= resetA
    a.target <= clockA
    a.input_sink.valid <= UInt<1>(1)
    a.input_sink.bits <= data
    a.virtualInput_sink.valid <= UInt<1>(1)
    a.virtualInput_sink.bits <= data
    a.output_source.ready <= UInt<1>(1)
    a.virtualOutput_source.ready <= UInt<1>(1)
    resultA <= a.output_source.bits
    inst b of Other
    b.hostClock <= clockB
    b.hostReset <= resetB
    b.target <= clockB
    b.input_sink.valid <= UInt<1>(1)
    b.input_sink.bits <= data
    b.virtualInput_sink.valid <= UInt<1>(1)
    b.virtualInput_sink.bits <= data
    b.output_source.ready <= UInt<1>(1)
    b.virtualOutput_source.ready <= UInt<1>(1)
    resultB <= b.output_source.bits
  module Model :
    input hostClock : Clock
    input hostReset : UInt<1>
    input target : Clock
    input input_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    input virtualInput_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    output output_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    output virtualOutput_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    wire targetCycleFinishing : UInt<1>
    targetCycleFinishing <= UInt<1>(0)
    reg state : UInt<8>, target
    state <= add(state, UInt<8>(1))
    input_sink.ready <= UInt<1>(0)
    virtualInput_sink.ready <= UInt<1>(0)
    output_source.valid <= UInt<1>(0)
    virtualOutput_source.valid <= UInt<1>(0)
    output_source.bits <= input_sink.bits
    virtualOutput_source.bits <= virtualInput_sink.bits
  module Other :
    input hostClock : Clock
    input hostReset : UInt<1>
    input target : Clock
    input input_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    input virtualInput_sink : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    output output_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    output virtualOutput_source : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<8> }
    wire targetCycleFinishing : UInt<1>
    targetCycleFinishing <= UInt<1>(0)
    reg state : UInt<8>, target
    state <= add(state, UInt<8>(1))
    input_sink.ready <= UInt<1>(0)
    virtualInput_sink.ready <= UInt<1>(0)
    output_source.valid <= UInt<1>(0)
    virtualOutput_source.valid <= UInt<1>(0)
    output_source.bits <= input_sink.bits
    virtualOutput_source.bits <= virtualInput_sink.bits
