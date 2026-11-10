// See LICENSE for license details.
package firechip.goldengateimplementations
import chisel3._
import chisel3.util._
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{HostPort,BridgeIOAnnotation}
import firesim.lib.bridgeutils.SimUtils.TargetRVPortType
import midas.core.{TargetChannelIO,HostPortIOConnectChannels2Port}
import firechip.bridgeinterfaces.NICBridgeTargetIO
import firrtl.annotations.ModuleTarget
import org.chipsalliance.cde.config.Parameters
/** The unchanged SFC HostToNICTokenGenerator, Chisel Queues and production
  * HostPort channel binder. Scope is loopback token transport, not CPU/MMIO.
  */
object NICLoopbackOracle extends App {
  require(args.length==1,"output-directory")
  implicit val p:Parameters=new midas.F2Config().toInstance
  class LoopbackFixture extends Module {
    val names=Seq("nic_out_bits_data","nic_out_bits_keep","nic_out_bits_last","nic_out_valid",
      "nic_in_bits_data","nic_in_bits_keep","nic_in_bits_last","nic_in_valid","nic_macAddr",
      "nic_rlimit_inc","nic_rlimit_period","nic_rlimit_size","nic_pauser_threshold","nic_pauser_quanta","nic_pauser_refresh")
    val widths=Seq(64,8,1,1,64,8,1,1,48,8,8,8,16,16,16)
    for(w<-0 until 2) {
      val tokens=names.indices.map {j=>
        val t=if(j<4)IO(Flipped(Decoupled(UInt(widths(j).W))))else IO(Decoupled(UInt(widths(j).W)))
        t.suggestName(s"w${w}_p${j}");t
      }
      val hp=Wire(HostPort(new NICBridgeTargetIO))
      val htnt=Module(new Queue(new HostToNICToken,10))
      val ntht=Module(new Queue(new NICToHostToken,10))
      val gen=Module(new HostToNICTokenGenerator(10))
      htnt.io.enq<>gen.io.out;gen.io.in<>ntht.io.deq
      hp.toHost.hReady:=ntht.io.enq.ready;ntht.io.enq.valid:=hp.toHost.hValid
      ntht.io.enq.bits.data_out:=hp.hBits.nic.out.bits
      ntht.io.enq.bits.data_out_valid:=hp.hBits.nic.out.valid
      ntht.io.enq.bits.data_in_ready:=true.B
      hp.fromHost.hValid:=htnt.io.deq.valid;htnt.io.deq.ready:=hp.fromHost.hReady
      hp.hBits.nic.in.bits:=htnt.io.deq.bits.data_in
      hp.hBits.nic.in.valid:=htnt.io.deq.bits.data_in_valid
      hp.hBits.nic.macAddr:=0.U;hp.hBits.nic.rlimit.inc:=1.U
      hp.hBits.nic.rlimit.period:=0.U;hp.hBits.nic.rlimit.size:=8.U
      hp.hBits.nic.pauser:=DontCare // actual LoopbackNIC does not drive these fields
      val done=IO(Output(Bool())).suggestName(s"nic${w}_done")
      done := !(hp.toHost.hValid&&hp.fromHost.hReady)
      val channels=new TargetChannelIO {
        def wireOutputPortMap:Map[String,ReadyValidIO[Data]]=names.take(4).zip(tokens.take(4)).map{case(n,t)=>n->t.asInstanceOf[ReadyValidIO[Data]]}.toMap
        def wireInputPortMap:Map[String,ReadyValidIO[Data]]=names.drop(4).zip(tokens.drop(4)).map{case(n,t)=>n->t.asInstanceOf[ReadyValidIO[Data]]}.toMap
        def rvOutputPortMap:Map[String,TargetRVPortType]=Map.empty
        def rvInputPortMap:Map[String,TargetRVPortType]=Map.empty
        def clockElement:(String,DecoupledIO[Data])=throw new UnsupportedOperationException("NIC does not consume clock-generator channels")
      }
      val anno=BridgeIOAnnotation(ModuleTarget("LoopbackFixture","LoopbackFixture").ref(s"nic$w"),names.map(n=>n->n).toMap,None,classOf[SimpleNICBridgeModule].getName,None)
      HostPortIOConnectChannels2Port(hp,anno,channels)
    }
  }
  val output=new java.io.File(args(0));require(output.mkdirs()||output.isDirectory)
  val text=(new ChiselStage).emitChirrtl(new LoopbackFixture,Array("--target-dir",output.getPath))
  val writer=new java.io.PrintWriter(new java.io.File(output,"loopback.fir"));try writer.write(text)finally writer.close()
  println("PASS unchanged SFC NIC loopback generator/queues/channel binder oracle")
}
