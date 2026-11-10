FIRRTL version 1.2.0
circuit Top :
  module Hub :
    input c0 : Clock
    input c1 : Clock
    input data : UInt<8>
    input second : UInt<8>
    input back : UInt<1>
    output link_fwdvalid : UInt<1>
    output link_fwdbits : UInt<8>
    output observed : UInt<8>
    output alias0 : Clock
    output alias1 : Clock
    reg state0 : UInt<8>, c0
    reg state1 : UInt<8>, c1
    state0 <= mux(bits(data, 7, 7), UInt<8>(0), bits(add(state0, and(data, UInt<8>(127))), 7, 0))
    state1 <= mux(bits(second, 7, 7), UInt<8>(0), bits(add(state1, and(second, UInt<8>(127))), 7, 0))
    link_fwdvalid <= bits(data, 0, 0)
    link_fwdbits <= bits(add(state0, data), 7, 0)
    observed <= bits(add(state1, back), 7, 0)
    alias0 <= c0
    alias1 <= c1
  module Worker :
    input clock : Clock
    input link_fwdvalid : UInt<1>
    input link_fwdbits : UInt<8>
    input enable : UInt<8>
    output back : UInt<1>
    output observed : UInt<8>
    reg state : UInt<8>, clock
    state <= mux(bits(enable, 7, 7), UInt<8>(0), mux(link_fwdvalid, bits(add(state, link_fwdbits), 7, 0), state))
    back <= bits(enable, 0, 0)
    observed <= state
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input c0 : Clock
    input c1 : Clock
    input h_data : UInt<8>
    input h_second : UInt<8>
    input h_back : UInt<1>
    output link_fwdvalid : UInt<1>
    output link_fwdbits : UInt<8>
    output h_observed : UInt<8>
    output h_alias0 : Clock
    output h_alias1 : Clock
    input w_clock : Clock
    input link_fwvalid : UInt<1>
    input link_fwbits : UInt<8>
    input w_enable : UInt<8>
    output w_back : UInt<1>
    output w_observed : UInt<8>
    inst h_ of Hub
    inst w_ of Worker
    h_.c0 <= c0
    h_.c1 <= c1
    h_.data <= h_data
    h_.second <= h_second
    h_.back <= h_back
    link_fwdvalid <= h_.link_fwdvalid
    link_fwdbits <= h_.link_fwdbits
    h_observed <= h_.observed
    h_alias0 <= h_.alias0
    h_alias1 <= h_.alias1
    w_.clock <= w_clock
    w_.link_fwdvalid <= link_fwvalid
    w_.link_fwdbits <= link_fwbits
    w_.enable <= w_enable
    w_back <= w_.back
    w_observed <= w_.observed
