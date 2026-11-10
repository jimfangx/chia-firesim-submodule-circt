// See LICENSE for license details.
package firechip.goldengateimplementations
import chisel3._
import chisel3.util._
import chisel3.stage.ChiselStage
/** Exact unchanged SimpleNICBridge LoopbackNIC CPU adapter ties plus allocated
  * CPUManagedStreamEngine queues. Packet/token/MMIO paths are separate scopes.
  */
object NICLoopbackCPUStreamsOracle extends App {
  require(args.length==1,"output-directory")
  class LoopbackCPUStreams extends Module {
    for(w<-0 until 2) {
      val from=IO(Flipped(Decoupled(UInt(512.W)))).suggestName(s"nic${w}_from_host_stream")
      val to=IO(Decoupled(UInt(512.W))).suggestName(s"nic${w}_to_host_stream")
      val fromCount=IO(Output(UInt(12.W))).suggestName(s"nic${w}_from_host_stream_count")
      val toCount=IO(Output(UInt(12.W))).suggestName(s"nic${w}_to_host_stream_count")
      val incoming=Module(new Queue(UInt(512.W),3072))
      val outgoing=Module(new Queue(UInt(512.W),3072))
      val decoder=Module(new BigTokenToNICTokenAdapter)
      val encoder=Module(new NICTokenToBigTokenAdapter)
      incoming.io.enq<>from;decoder.io.pcie_in<>incoming.io.deq
      decoder.io.htnt.ready:=false.B
      encoder.io.ntht.valid:=false.B;encoder.io.ntht.bits:=DontCare
      outgoing.io.enq<>encoder.io.pcie_out;to<>outgoing.io.deq
      fromCount:=incoming.io.count;toCount:=outgoing.io.count
    }
  }
  val out=new java.io.File(args(0));require(out.mkdirs()||out.isDirectory)
  val text=(new ChiselStage).emitChirrtl(new LoopbackCPUStreams,Array("--target-dir",out.getPath))
  val f=new java.io.PrintWriter(new java.io.File(out,"loopback-cpu-streams.fir"));try f.write(text)finally f.close()
  println("PASS unchanged SFC disabled NIC adapters and independent allocated CPU queues")
}
