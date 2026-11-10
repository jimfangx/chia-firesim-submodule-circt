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
    input wmask_hi : UInt<1>
    input wdata_lo : UInt<8>
    input wdata_hi : UInt<8>
    input rwaddr : UInt<2>
    input rwen : UInt<1>
    input wmode : UInt<1>
    input rwmask_lo : UInt<1>
    input rwmask_hi : UInt<1>
    input rwdata_lo : UInt<8>
    input rwdata_hi : UInt<8>
    output result_lo : UInt<8>
    output result_hi : UInt<8>
    output rwresult_lo : UInt<8>
    output rwresult_hi : UInt<8>
    mem ram :
      data-type => {lo : UInt<8>, hi : UInt<8>}
      depth => 3
      reader => r
      writer => w
      readwriter => rw
      read-latency => 0
      write-latency => 1
      read-under-write => old
    ram.r.clk <= clock
    ram.r.en <= ren
    ram.r.addr <= raddr
    result_lo <= ram.r.data.lo
    result_hi <= ram.r.data.hi
    ram.w.clk <= clock
    ram.w.en <= wen
    ram.w.addr <= waddr
    ram.w.data.lo <= wdata_lo
    ram.w.data.hi <= wdata_hi
    ram.w.mask.lo <= wmask_lo
    ram.w.mask.hi <= wmask_hi
    ram.rw.clk <= clock
    ram.rw.en <= rwen
    ram.rw.addr <= rwaddr
    ram.rw.wmode <= wmode
    ram.rw.wdata.lo <= rwdata_lo
    ram.rw.wdata.hi <= rwdata_hi
    ram.rw.wmask.lo <= rwmask_lo
    ram.rw.wmask.hi <= rwmask_hi
    rwresult_lo <= ram.rw.rdata.lo
    rwresult_hi <= ram.rw.rdata.hi
