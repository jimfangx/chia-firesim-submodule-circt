// See LICENSE for license details.
package midas.widgets
import chisel3._
import chisel3.stage.ChiselStage
import freechips.rocketchip.diplomacy.LazyModule
import firechip.goldengateimplementations.{SimpleNICBridgeModule,LoopbackNIC}
import org.chipsalliance.cde.config.Parameters
/** Elaborate the actual unchanged LoopbackNIC widget, including genROReg,
  * singleton MCRFile and allocated (disabled) CPU streams. */
object NICLoopbackControlOracle extends App {
  require(args.length==1,"output-directory")
  val out=new java.io.File(args(0));require(out.mkdirs()||out.isDirectory)
  implicit val p:Parameters=new midas.F2Config().toInstance.alterPartial {
    case LoopbackNIC=>true
  }
  val w=LazyModule(new SimpleNICBridgeModule)
  val fir=(new ChiselStage).emitChirrtl(w.module,Array("--target-dir",out.getPath))
  val header=new StringBuilder;w.module.genHeader(BigInt("80000000",16),Map.empty,header)
  def write(n:String,s:String):Unit={val f=new java.io.PrintWriter(new java.io.File(out,n));try f.write(s)finally f.close()}
  write("loopback-control.fir",fir);write("loopback-control.h",header.toString)
  write("abi.txt",s"registers=${w.module.numRegs}\nregionBytes=${w.memRegionSize}\ndone=${w.getCRAddr("done")}\n")
  require(w.module.numRegs==1&&w.getCRAddr("done")==0)
  // A separate status-boundary fixture invokes the same unchanged Widget
  // helpers as SimpleNICBridgeModule. The complete original NIC also emits
  // known unassigned pauser fields, and is retained separately above.
  class DoneWidget extends Widget {
    lazy val module = new WidgetImp(this) {
      val io=IO(new WidgetIO)
      val doneInput=IO(Input(Bool()))
      genROReg(doneInput,"done")
      genCRFile()
      def genHeader(base:BigInt,memoryRegions:Map[String,BigInt],sb:StringBuilder):Unit=()
    }
  }
  val done=LazyModule(new DoneWidget)
  write("done-control.fir",(new ChiselStage).emitChirrtl(done.module,Array("--target-dir",out.getPath)))
  println("PASS actual unchanged SFC LoopbackNIC widget singleton MMIO and allocated CPU streams")
}
