// See LICENSE for license details.
package midas.passes
import firrtl._
import firrtl.annotations.JsonProtocol
import firrtl.options.TargetDirAnnotation
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{BridgeIOAnnotation,RationalClock}
import freechips.rocketchip.diplomacy.LazyModule
import midas.widgets.{AutoCounterBridgeModule,AutoCounterParameters,TargetClockInfo}
import org.chipsalliance.cde.config.Parameters

/** Same source annotations -> unchanged CSV target transform -> actual SFC
  * widget elaboration. The local AXI ABI does not depend on clock ratio.
  * This focused boundary explicitly uses base 1/1; rational scheduling and
  * global allocation remain outside its scope.
  */
object AutoCounterSynthesisHostOracle extends App {
  require(args.length==3,"input.fir input.json output-directory")
  def read(p:String):String={val s=scala.io.Source.fromFile(p);try s.mkString finally s.close()}
  val output=new java.io.File(args(2));require(output.mkdirs()||output.isDirectory)
  val state=CircuitState(Parser.parse(read(args(0))),LowForm,
    JsonProtocol.deserialize(read(args(1))) :+ TargetDirAnnotation(output.getPath))
  val transformed=new AutoCounterTransform().doTransform(state,usePrintfImplementation=false)
  val resolved=new ResolveAndCheck().runTransform(transformed)
  val keys=resolved.annotations.collect {
    case bridge:BridgeIOAnnotation if bridge.widgetClass==classOf[AutoCounterBridgeModule].getName =>
      bridge.widgetConstructorKey.get.asInstanceOf[AutoCounterParameters]
  }
  require(keys.nonEmpty,"source fixture did not synthesize CSV constructors")
  def write(dir:java.io.File,name:String,text:String):Unit={
    val out=new java.io.PrintWriter(new java.io.File(dir,name));try out.write(text) finally out.close()
  }
  keys.zipWithIndex.foreach {case(key,index)=>
    val dir=new java.io.File(output,s"host-$index");require(dir.mkdirs()||dir.isDirectory)
    implicit val p:Parameters=new midas.F2Config().toInstance.alterPartial {
      case TargetClockInfo=>Some(RationalClock("base",1,1))
    }
    val widget=LazyModule(new AutoCounterBridgeModule(key))
    val fir=(new ChiselStage).emitChirrtl(widget.module,Array("--target-dir",dir.getPath))
    val header=new StringBuilder;widget.module.genHeader(BigInt(0),Map.empty,header)
    write(dir,"host.fir",fir);write(dir,"host.h",header.toString)
    write(dir,"abi.txt",widget.module.crRegistry.getAllRegs.map{case(n,a)=>s"$n=$a"}.mkString("\n")+"\n")
    println(s"PASS unchanged SFC CSV source -> host-$index: ${key.eventMetadata.size} ordered events")
  }
}
