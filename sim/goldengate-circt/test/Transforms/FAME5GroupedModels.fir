FIRRTL version 1.2.0
circuit Top :
  module Worker :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1> } }
    output out : { flip ready : UInt<1>, valid : UInt<1>, bits : { sum : UInt<8>, echo : UInt<8>, flag : UInt<1> } }
    reg state : UInt<8>, hostClock with :
      reset => (or(hostReset, in.bits.clear), UInt<8>(0))
    state <= mux(and(and(in.valid, in.bits.enable), out.ready), bits(add(state, in.bits.add), 7, 0), state)
    in.ready <= out.ready
    out.valid <= in.valid
    out.bits.sum <= state
    out.bits.echo <= in.bits.add
    out.bits.flag <= in.bits.enable
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input in0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1> } }
    output out0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { sum : UInt<8>, echo : UInt<8>, flag : UInt<1> } }
    input in1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1> } }
    output out1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { sum : UInt<8>, echo : UInt<8>, flag : UInt<1> } }
    input in2 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1> } }
    output out2 : { flip ready : UInt<1>, valid : UInt<1>, bits : { sum : UInt<8>, echo : UInt<8>, flag : UInt<1> } }
    input in3 : { flip ready : UInt<1>, valid : UInt<1>, bits : { clear : UInt<1>, add : UInt<8>, enable : UInt<1> } }
    output out3 : { flip ready : UInt<1>, valid : UInt<1>, bits : { sum : UInt<8>, echo : UInt<8>, flag : UInt<1> } }
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
