FIRRTL version 1.2.0
circuit FAMETop :
  module FAMETop :
    input hostClock : Clock
    input hostReset : UInt<1>
    input clock : Clock

    inst Top of Top
    Top.clock <= clock

  module Parent :
    input clock : Clock
    input addr : UInt<2>
    input en : UInt<1>
    input data : UInt<8>
    output out : UInt<8>

    mem ram :
      data-type => UInt<8>
      depth => 4
      read-latency => 1
      write-latency => 1
      reader => r
      writer => w
      readwriter => rw
      read-under-write => undefined
    out <= ram.r.data
    ram.r.addr <= addr
    ram.r.en <= en
    ram.r.clk <= clock
    ram.w.addr <= addr
    ram.w.en <= en
    ram.w.clk <= clock
    ram.w.data <= data
    ram.w.mask <= UInt<1>("h1")
    ram.rw.addr <= addr
    ram.rw.en <= en
    ram.rw.clk <= clock
    ram.rw.wmode <= UInt<1>("h0")
    ram.rw.wdata <= data
    ram.rw.wmask <= UInt<1>("h1")

  module Middle :
    input clock : Clock

    inst p0 of Parent
    inst p1 of Parent
    p0.clock <= clock
    p0.addr <= UInt<2>("h0")
    p0.en <= UInt<1>("h1")
    p0.data <= UInt<8>("h0")
    p1.clock <= clock
    p1.addr <= UInt<2>("h1")
    p1.en <= UInt<1>("h1")
    p1.data <= UInt<8>("h1")

  module Top :
    input clock : Clock

    inst m0 of Middle
    inst m1 of Middle
    m0.clock <= clock
    m1.clock <= clock
