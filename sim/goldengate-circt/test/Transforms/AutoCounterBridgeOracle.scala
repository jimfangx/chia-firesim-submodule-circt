// See LICENSE for license details.
package midas.widgets
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{BridgeIOAnnotation,RationalClock}
import firrtl.annotations.ModuleTarget
import firrtl.annotations.{JsonProtocol,NoTargetAnnotation}
import firrtl.annotations.HasSerializationHints
import freechips.rocketchip.diplomacy.LazyModule
import midas.targetutils.PerfCounterOps
import org.chipsalliance.cde.config.Parameters
/** Unchanged CSV widget oracle, deliberately separate from the required enabled
  * Linux representative. Produces executable RTL and the serialized constructor
  * consumed by the native diagnostic compiler path from identical input.
  */
object AutoCounterBridgeOracle extends App {
  require(args.length==1,"output-directory")
  val output=new java.io.File(args(0));require(output.mkdirs()||output.isDirectory)
  implicit val p:Parameters=new midas.F2Config().toInstance.alterPartial {
    case TargetClockInfo=>Some(RationalClock("base",1,1))
  }
  val key=AutoCounterParameters(Seq(
    EventMetadata("event0","count","fixture event",64,PerfCounterOps.Accumulate),
    EventMetadata("event1","value","fixture event",64,PerfCounterOps.Identity)),"trigger","globalReset")
  val widget=LazyModule(new AutoCounterBridgeModule(key))
  val firrtl=(new ChiselStage).emitChirrtl(widget.module,Array("--target-dir",output.getPath))
  val header=new StringBuilder;widget.module.genHeader(BigInt(0),Map.empty,header)
  def write(name:String,text:String):Unit={val out=new java.io.PrintWriter(new java.io.File(output,name));try out.write(text) finally out.close()}
  write("autocounter.fir",firrtl);write("autocounter.h",header.toString)
  val anno=BridgeIOAnnotation(ModuleTarget("Top","Top").ref("autocounter0"),Seq("trigger","globalReset","event0","event1"),classOf[AutoCounterBridgeModule].getName,key)
  // The SFC constructor has no nested PerfCounterOpType hints. Preserve the
  // actual singleton type explicitly instead of turning Accumulate/Identity
  // into the same ambiguous {} when crossing this supporting JSON boundary.
  case class ConstructorHints() extends NoTargetAnnotation with HasSerializationHints {
    def typeHints=Seq(classOf[EventMetadata],PerfCounterOps.Accumulate.getClass,PerfCounterOps.Identity.getClass)
  }
  write("input.anno.json",JsonProtocol.serialize(Seq(anno,ConstructorHints())))
  write("input.fir","FIRRTL version 1.1.0\ncircuit Top :\n  module Top :\n    input clock : Clock\n")
  write("autocounter-abi.txt",widget.module.crRegistry.getAllRegs.map{case(n,a)=>s"$n=$a"}.mkString("\n")+"\n")
  println("PASS unchanged SFC AutoCounterBridgeModule CSV oracle and constructor")
}
