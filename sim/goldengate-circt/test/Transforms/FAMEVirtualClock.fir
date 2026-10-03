FIRRTL version 3.2.0
circuit Model :
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
