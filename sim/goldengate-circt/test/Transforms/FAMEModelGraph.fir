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
    input data : UInt<8>
    output out : UInt<8>

    reg state : UInt<8>, clock with :
      reset => (UInt<1>("h0"), state)
    out <= bits(add(state, bits(data, 6, 0)), 7, 0)
    state <= mux(bits(data, 7, 7), UInt<8>("h0"), bits(add(state, bits(data, 6, 0)), 7, 0))

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
    input w0_data : UInt<8>
    output w0_out : UInt<8>
    input w1_clock : Clock
    input w1_data : UInt<8>
    output w1_out : UInt<8>

    inst h_ of Hub
    inst w0_ of Worker
    inst w1_ of Worker
    h_out <= h_.out
    h_other0 <= h_.otherOut
    h_other1 <= h_.otherOut
    h_alias0 <= h_.alias0
    h_alias1 <= h_.alias1
    w0_out <= w0_.out
    w1_out <= w1_.out
    h_.c0 <= c0
    h_.c1 <= c1
    h_.data <= h_data
    h_.right <= h_right
    w0_.clock <= w0_clock
    w0_.data <= w0_data
    w1_.clock <= w1_clock
    w1_.data <= w1_data
