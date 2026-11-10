FIRRTL version 1.2.0
circuit Top :
  module Leaf :
    input clock : Clock
    input reset : UInt<1>
    input clear : UInt<1>
    input fire : UInt<1>
    input enable : UInt<1>
    input data : UInt<8>
    input addr : UInt<2>
    input write : UInt<1>
    input mask : UInt<1>
    output sum : UInt<8>
    output response : UInt<8>
    reg state : UInt<8>, clock with :
      reset => (or(reset, clear), UInt<8>(0))
    state <= mux(and(fire, enable), bits(add(state, UInt<8>(1)), 7, 0), state)
    mem store :
      data-type => UInt<8>
      depth => 3
      reader => r
      writer => w
      read-latency => 0
      write-latency => 1
      read-under-write => old
    store.r.clk <= clock
    store.r.en <= UInt<1>(1)
    store.r.addr <= addr
    store.w.clk <= clock
    store.w.en <= and(fire, write)
    store.w.mask <= mask
    store.w.addr <= addr
    store.w.data <= data
    sum <= state
    response <= store.r.data
  module Branch :
    input clock : Clock
    input reset : UInt<1>
    input clear : UInt<1>
    input fire : UInt<1>
    input enable : UInt<1>
    input data : UInt<8>
    input addr : UInt<2>
    input write : UInt<1>
    input mask : UInt<1>
    output sum : UInt<8>
    output response : UInt<8>
    reg state : UInt<8>, clock with :
      reset => (or(reset, clear), UInt<8>(0))
    state <= mux(and(fire, enable), bits(add(state, data), 7, 0), state)
    inst leaf of Leaf
    leaf.clock <= clock
    leaf.reset <= reset
    leaf.clear <= clear
    leaf.fire <= fire
    leaf.enable <= enable
    leaf.data <= data
    leaf.addr <= addr
    leaf.write <= write
    leaf.mask <= mask
    sum <= xor(state, leaf.sum)
    response <= leaf.response
  module Worker :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1>, addr : UInt<2>, write : UInt<1>, mask : UInt<1> } }
    output out : { flip ready : UInt<1>, valid : UInt<1>, bits : { left : UInt<8>, right : UInt<8>, memLeft : UInt<8>, memRight : UInt<8>, parent : UInt<8> } }
    reg state : UInt<8>, hostClock with :
      reset => (or(hostReset, in.bits.clear), UInt<8>(0))
    node fire = and(in.valid, out.ready)
    state <= mux(and(fire, in.bits.enable), bits(add(state, UInt<8>(3)), 7, 0), state)
    inst left of Branch
    inst right of Branch
    left.clock <= hostClock
    left.reset <= hostReset
    left.clear <= in.bits.clear
    left.fire <= fire
    left.enable <= in.bits.enable
    left.data <= in.bits.add
    left.addr <= in.bits.addr
    left.write <= in.bits.write
    left.mask <= in.bits.mask
    right.clock <= hostClock
    right.reset <= hostReset
    right.clear <= in.bits.clear
    right.fire <= fire
    right.enable <= in.bits.enable
    right.data <= xor(in.bits.add, UInt<8>(90))
    right.addr <= in.bits.addr
    right.write <= in.bits.write
    right.mask <= in.bits.mask
    in.ready <= out.ready
    out.valid <= in.valid
    out.bits.left <= left.sum
    out.bits.right <= right.sum
    out.bits.memLeft <= left.response
    out.bits.memRight <= right.response
    out.bits.parent <= state
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1>, addr : UInt<2>, write : UInt<1>, mask : UInt<1> } }
    output out0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { left : UInt<8>, right : UInt<8>, memLeft : UInt<8>, memRight : UInt<8>, parent : UInt<8> } }
    input in1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1>, addr : UInt<2>, write : UInt<1>, mask : UInt<1> } }
    output out1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { left : UInt<8>, right : UInt<8>, memLeft : UInt<8>, memRight : UInt<8>, parent : UInt<8> } }
    input in2 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1>, addr : UInt<2>, write : UInt<1>, mask : UInt<1> } }
    output out2 : { flip ready : UInt<1>, valid : UInt<1>, bits : { left : UInt<8>, right : UInt<8>, memLeft : UInt<8>, memRight : UInt<8>, parent : UInt<8> } }
    input in3 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1>, addr : UInt<2>, write : UInt<1>, mask : UInt<1> } }
    output out3 : { flip ready : UInt<1>, valid : UInt<1>, bits : { left : UInt<8>, right : UInt<8>, memLeft : UInt<8>, memRight : UInt<8>, parent : UInt<8> } }
    inst worker1 of Worker
    worker1.hostClock <= hostClock
    worker1.hostReset <= hostReset
    worker1.in <= in1
    out1 <= worker1.out
    inst worker0 of Worker
    worker0.hostClock <= hostClock
    worker0.hostReset <= hostReset
    worker0.in <= in0
    out0 <= worker0.out
    inst worker3 of Worker
    worker3.hostClock <= hostClock
    worker3.hostReset <= hostReset
    worker3.in <= in3
    out3 <= worker3.out
    inst worker2 of Worker
    worker2.hostClock <= hostClock
    worker2.hostReset <= hostReset
    worker2.in <= in2
    out2 <= worker2.out
