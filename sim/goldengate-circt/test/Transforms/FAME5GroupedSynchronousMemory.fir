FIRRTL version 1.2.0
circuit Top :
  module Worker :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, data : UInt<8>, write : UInt<1>, mask : UInt<1>, read : UInt<1> } }
    output out : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<8>, count : UInt<8> } }
    node fired = and(in.valid, out.ready)
    inst gate of AbstractClockGate
    gate.I <= hostClock
    gate.CE <= UInt<1>(1)
    node targetClock = gate.O
    reg count : UInt<8>, hostClock with :
      reset => (hostReset, UInt<8>(0))
    count <= mux(fired, bits(add(count, UInt<8>(1)), 7, 0), count)
    mem store :
      data-type => UInt<8>
      depth => 3
      reader => r
      writer => w
      read-latency => 1
      write-latency => 1
      read-under-write => old
    store.r.clk <= targetClock
    store.r.addr <= in.bits.addr
    store.r.en <= and(in.bits.read, fired)
    store.w.clk <= targetClock
    store.w.addr <= in.bits.addr
    store.w.en <= and(in.bits.write, fired)
    store.w.data <= in.bits.data
    store.w.mask <= in.bits.mask
    in.ready <= out.ready
    out.valid <= in.valid
    out.bits.data <= store.r.data
    out.bits.count <= count
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, data : UInt<8>, write : UInt<1>, mask : UInt<1>, read : UInt<1> } }
    output out0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<8>, count : UInt<8> } }
    input in1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, data : UInt<8>, write : UInt<1>, mask : UInt<1>, read : UInt<1> } }
    output out1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<8>, count : UInt<8> } }
    input in2 : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, data : UInt<8>, write : UInt<1>, mask : UInt<1>, read : UInt<1> } }
    output out2 : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<8>, count : UInt<8> } }
    input in3 : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, data : UInt<8>, write : UInt<1>, mask : UInt<1>, read : UInt<1> } }
    output out3 : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<8>, count : UInt<8> } }
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
  extmodule AbstractClockGate :
    input I : Clock
    input CE : UInt<1>
    output O : Clock
    defname = AbstractClockGate
