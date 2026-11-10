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
    reg state0 : UInt<8>, c0
    reg state1 : UInt<8>, c1
    state0 <= mux(bits(data, 7, 7), UInt<8>(0), bits(add(state0, data), 7, 0))
    state1 <= mux(bits(right, 7, 7), UInt<8>(0), bits(add(state1, right), 7, 0))
    out <= bits(add(state0, data), 7, 0)
    otherOut <= bits(add(state1, right), 7, 0)
    alias0 <= c0
    alias1 <= c1
  module Memory :
    input clock : Clock
    input request : UInt<16>
    input read : UInt<4>
    output asynchronous : UInt<8>
    output synchronous : UInt<8>
    output aliasClock : Clock
    mem asyncRam :
      data-type => UInt<8>
      depth => 16
      read-latency => 0
      write-latency => 1
      reader => r
      writer => w
      read-under-write => undefined
    mem syncRam :
      data-type => UInt<8>
      depth => 16
      read-latency => 1
      write-latency => 1
      reader => r
      writer => w
      read-under-write => old
    asyncRam.r.clk <= clock
    asyncRam.r.en <= UInt<1>(1)
    asyncRam.r.addr <= read
    asyncRam.w.clk <= clock
    asyncRam.w.en <= bits(request, 15, 15)
    asyncRam.w.mask <= bits(request, 14, 14)
    asyncRam.w.addr <= bits(request, 11, 8)
    asyncRam.w.data <= bits(request, 7, 0)
    syncRam.r.clk <= clock
    syncRam.r.en <= UInt<1>(1)
    syncRam.r.addr <= read
    syncRam.w.clk <= clock
    syncRam.w.en <= bits(request, 15, 15)
    syncRam.w.mask <= bits(request, 14, 14)
    syncRam.w.addr <= bits(request, 11, 8)
    syncRam.w.data <= bits(request, 7, 0)
    asynchronous <= asyncRam.r.data
    synchronous <= syncRam.r.data
    aliasClock <= clock
  module Combinational :
    input clock : Clock
    input data : UInt<8>
    input salt : UInt<8>
    output out : UInt<8>
    output aliasClock : Clock
    out <= xor(data, salt)
    aliasClock <= clock
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input c0 : Clock
    input c1 : Clock
    input h_data : UInt<8>
    input h_right : UInt<8>
    output h_out : UInt<8>
    output h_otherOut : UInt<8>
    output h_alias0 : Clock
    output h_alias1 : Clock
    input m_clock : Clock
    input m_request : UInt<16>
    input m_read : UInt<4>
    output m_async : UInt<8>
    output m_sync : UInt<8>
    output m_alias : Clock
    input x_clock : Clock
    input x_data : UInt<8>
    input x_salt : UInt<8>
    output x_out : UInt<8>
    output x_alias : Clock
    input m1_clock : Clock
    input m1_request : UInt<16>
    input m1_read : UInt<4>
    output m1_async : UInt<8>
    output m1_sync : UInt<8>
    output m1_alias : Clock
    input x1_clock : Clock
    input x1_data : UInt<8>
    input x1_salt : UInt<8>
    output x1_out : UInt<8>
    output x1_alias : Clock
    inst h_ of Hub
    inst m_ of Memory
    inst x_ of Combinational
    inst m1_ of Memory
    inst x1_ of Combinational
    h_.c0 <= c0
    h_.c1 <= c1
    h_.data <= h_data
    h_.right <= h_right
    h_out <= h_.out
    h_otherOut <= h_.otherOut
    h_alias0 <= h_.alias0
    h_alias1 <= h_.alias1
    m_.clock <= m_clock
    m_.request <= m_request
    m_.read <= m_read
    m_async <= m_.asynchronous
    m_sync <= m_.synchronous
    m_alias <= m_.aliasClock
    x_.clock <= x_clock
    x_.data <= x_data
    x_.salt <= x_salt
    x_out <= x_.out
    x_alias <= x_.aliasClock
    m1_.clock <= m1_clock
    m1_.request <= m1_request
    m1_.read <= m1_read
    m1_async <= m1_.asynchronous
    m1_sync <= m1_.synchronous
    m1_alias <= m1_.aliasClock
    x1_.clock <= x1_clock
    x1_.data <= x1_data
    x1_.salt <= x1_salt
    x1_out <= x1_.out
    x1_alias <= x1_.aliasClock
