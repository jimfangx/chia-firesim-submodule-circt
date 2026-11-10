FIRRTL version 1.2.0
circuit Top :
  module Hub :
    input c0 : Clock
    input c1 : Clock
    input data : UInt<8>
    input right : UInt<8>
    output out : UInt<8>
    output otherOut : UInt<8>
    output alias0 : Clock
    output alias1 : Clock

    reg state0 : UInt<8>, c0 with :
      reset => (UInt<1>("h0"), state0)
    reg state1 : UInt<8>, c1 with :
      reset => (UInt<1>("h0"), state1)
    out <= bits(add(state0, bits(data, 6, 0)), 7, 0)
    otherOut <= bits(add(state1, bits(right, 6, 0)), 7, 0)
    alias0 <= c0
    alias1 <= c1
    state0 <= mux(bits(data, 7, 7), UInt<8>("h0"), bits(add(state0, bits(data, 6, 0)), 7, 0))
    state1 <= mux(bits(right, 7, 7), UInt<8>("h0"), bits(add(state1, bits(right, 6, 0)), 7, 0))

  module Worker :
    input clock : Clock
    input w1_input_a : UInt<8>
    input w1_input_b : UInt<8>
    output w1_output_a : UInt<8>
    output w1_output_b : UInt<8>

    reg state : UInt<8>, clock with :
      reset => (UInt<1>("h0"), state)
    w1_output_a <= bits(add(state, bits(w1_input_a, 6, 0)), 7, 0)
    w1_output_b <= bits(add(state, w1_input_b), 7, 0)
    state <= mux(bits(w1_input_a, 7, 7), UInt<8>("h0"), bits(add(state, bits(w1_input_a, 6, 0)), 7, 0))

  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input c0 : Clock
    input c1 : Clock
    input h_data : UInt<8>
    input h_right : UInt<8>
    output h_out : UInt<8>
    output h_other0 : UInt<8>
    output h_other1 : UInt<8>
    output h_alias0 : Clock
    output h_alias1 : Clock
    input w0_clock : Clock
    input w0_input_a : UInt<8>
    input w0_input_b : UInt<8>
    output w0_output_a : UInt<8>
    output w0_output_b : UInt<8>
    input w1_clock : Clock
    input w1_input_a : UInt<8>
    input w1_input_b : UInt<8>
    output w1_output_a : UInt<8>
    output w1_output_b : UInt<8>

    inst h_ of Hub
    inst w0_ of Worker
    inst w1_ of Worker
    h_out <= h_.out
    h_other0 <= h_.otherOut
    h_other1 <= h_.otherOut
    h_alias0 <= h_.alias0
    h_alias1 <= h_.alias1
    w0_output_a <= w0_.w1_output_a
    w0_output_b <= w0_.w1_output_b
    w1_output_a <= w1_.w1_output_a
    w1_output_b <= w1_.w1_output_b
    h_.c0 <= c0
    h_.c1 <= c1
    h_.data <= h_data
    h_.right <= h_right
    w0_.clock <= w0_clock
    w0_.w1_input_a <= w0_input_a
    w0_.w1_input_b <= w0_input_b
    w1_.clock <= w1_clock
    w1_.w1_input_a <= w1_input_a
    w1_.w1_input_b <= w1_input_b
