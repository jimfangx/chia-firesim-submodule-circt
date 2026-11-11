FIRRTL version 1.2.0
circuit Probe :
  module Probe :
    input hostClock : Clock
    input hostReset : UInt<1>
    input shared : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<3>}
    input en0 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}
    output response0 : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<17>}
    input write : { flip ready : UInt<1>, valid : UInt<1>, bits : { data : UInt<17>, mask : UInt<1>, en : UInt<1>}}

    skip
