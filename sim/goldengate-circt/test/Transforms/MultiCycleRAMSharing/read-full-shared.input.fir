FIRRTL version 1.2.0
circuit Probe :
  module Probe :
    input hostClock : Clock
    input hostReset : UInt<1>
    input shared : { flip ready : UInt<1>, valid : UInt<1>, bits : { addr : UInt<3>, en : UInt<1>}}
    output response0 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<17>}
    output response1 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<17>}
    input write : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<17>, mask : UInt<1>, addr : UInt<3>, en : UInt<1>}}

    skip
