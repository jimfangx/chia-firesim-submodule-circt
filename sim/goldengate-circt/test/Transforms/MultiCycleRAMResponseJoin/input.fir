FIRRTL version 1.2.0
circuit Grouped :
  module Grouped :
    input hostClock : Clock
    input hostReset : UInt<1>
    input rq0 : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, en : UInt<1>}}
    input rq1 : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, en : UInt<1>}}
    output responses : { flip ready : UInt<1>, valid : UInt<1>, bits : { d0 : UInt<8>, d1 : UInt<8>}}
    input write : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<2>, en : UInt<1>, data : UInt<8>, mask : UInt<1>}}

    skip
