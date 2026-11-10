circuit Model :
  module Model :
    input clock : Clock
    input hostClock : Clock
    input threadIdx : UInt<2>
    input raddr : UInt<2>
    input ren : UInt<1>
    input waddr : UInt<2>
    input wen : UInt<1>
    input wmask_lo : UInt<1>
    input wdata_lo : UInt<8>
    input rwaddr : UInt<2>
    input rwen : UInt<1>
    input wmode : UInt<1>
    input rwmask_lo : UInt<1>
    input rwdata_lo : UInt<8>
    output result_lo : UInt<8>
    output rwresult_lo : UInt<8>
    mem ram :
      data-type => UInt<8>
      depth => 3
      reader => r
      writer => w
      readwriter => rw
      read-latency => 1
      write-latency => 1
      read-under-write => old
    wire readClock : Clock
    wire writeClock : Clock
    wire readwriteClock : Clock
    node clockAlias = clock
    readClock <= clockAlias
    writeClock <= clock
    readwriteClock <= clock
    ram.r.clk <= readClock
    ram.r.en <= ren
    ram.r.addr <= raddr
    result_lo <= ram.r.data
    ram.w.clk <= writeClock
    ram.w.en <= wen
    ram.w.addr <= waddr
    ram.w.data <= wdata_lo
    ram.w.mask <= wmask_lo
    ram.rw.clk <= readwriteClock
    ram.rw.en <= rwen
    ram.rw.addr <= rwaddr
    ram.rw.wmode <= wmode
    ram.rw.wdata <= rwdata_lo
    ram.rw.wmask <= rwmask_lo
    rwresult_lo <= ram.rw.rdata
