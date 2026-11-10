circuit Model :
  module Model :
    input clock : Clock
    input thread : UInt<2>
    input reset : UInt<1>
    input enable : UInt<1>
    input data : UInt<8>
    output result : UInt<8>
    reg state : UInt<8>, clock with :
      reset => (reset, UInt<8>(7))
    state <= mux(enable, data, state)
    result <= state
