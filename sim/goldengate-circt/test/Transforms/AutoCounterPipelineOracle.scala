// See LICENSE for license details.
package midas.passes
import firrtl._
import firrtl.Mappers._
import firrtl.annotations.{JsonProtocol,ModuleTarget,NoTargetAnnotation,HasSerializationHints}
import firrtl.options.TargetDirAnnotation
import chisel3._
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{BridgeIOAnnotation,RationalClock}
import freechips.rocketchip.diplomacy.{LazyModule,LazyModuleImp}
import midas.core.{SimWrapperConfig,TargetBox,TargetBoxIO,HostPortIOConnectChannels2Port}
import midas.widgets.{AutoCounterBridgeModule,AutoCounterParameters,EventMetadata,TargetClockInfo,WidgetMMIO,HasWidgets}
import midas.targetutils.PerfCounterOps
import org.chipsalliance.cde.config.Parameters

/** Execute unchanged SFC target passes and actual host/channel constructor
  * binding on the same input. Only the verification top is new: target channels
  * not consumed by CSV remain externally driven/observed. No target outputs
  * bypass FAME to reach a host and no expected counter values are substituted.
  */
object AutoCounterPipelineOracle extends App {
  require(args.length==3||(args.length==4&&args(3)=="--global-control"),"input.fir input.json output-directory [--global-control]")
  val globalControl=args.length==4
  def read(p:String):String={val s=scala.io.Source.fromFile(p);try s.mkString finally s.close()}
  val output=new java.io.File(args(2));require(output.mkdirs()||output.isDirectory)
  def write(name:String,text:String):Unit={val p=new java.io.PrintWriter(new java.io.File(output,name));try p.write(text) finally p.close()}
  var state=CircuitState(Parser.parse(read(args(0))),LowForm,
    JsonProtocol.deserialize(read(args(1))) :+ TargetDirAnnotation(output.getPath))
  state=new AutoCounterTransform().doTransform(state,usePrintfImplementation=false)
  val transforms:Seq[Transform]=Seq(new ResolveAndCheck,TriggerWiring,
    GlobalResetConditionWiring,ChannelClockInfoAnalysis,UpdateBridgeClockInfo,
    fame.WrapTop,new ResolveAndCheck,new fame.ExtractModel,new ResolveAndCheck,
    new HighFirrtlToMiddleFirrtl,new MiddleFirrtlToLowFirrtl,
    fame.PromotePassthroughConnections,new ResolveAndCheck,new fame.FAMEDefaults,
    fame.FindDefaultClocks,new fame.ChannelExcision,new fame.InferModelPorts,
    new fame.FAMETransform,DefineAbstractClockGate,new ResolveAndCheck)
  transforms.zipWithIndex.foreach {case(t,i)=>
    write(s"stage-$i-before.fir",state.circuit.serialize)
    write(s"stage-$i-before.json",JsonProtocol.serialize(state.annotations))
    state=t.runTransform(state)
  }
  case class ConstructorHints() extends NoTargetAnnotation with HasSerializationHints {
    def typeHints=Seq(classOf[EventMetadata],PerfCounterOps.Accumulate.getClass,PerfCounterOps.Identity.getClass)
  }
  write("sfc-fame.fir",state.circuit.serialize)
  write("sfc-fame.json",JsonProtocol.serialize(state.annotations :+ ConstructorHints()))
  write("AbstractClockGate.v",DefineAbstractClockGate.impl)
  val bridges=state.annotations.collect {case b:BridgeIOAnnotation if b.widgetClass==classOf[AutoCounterBridgeModule].getName=>b}
  require(bridges.nonEmpty,"no CSV bridge generated")
  val target=state.circuit.modules.find(_.name==state.circuit.main).get
  val leafTypes=target.ports.map(p=>ModuleTarget(state.circuit.main,target.name).ref(p.name)->p).toMap
  val config=SimWrapperConfig(state.annotations,leafTypes)
  val boundNames=bridges.flatMap(_.channelMapping.values).toSet
  val external=SimWrapperConfig(state.annotations.filter {
    case c:fame.FAMEChannelConnectionAnnotation=> !boundNames(c.globalName)
    case _=>true
  },leafTypes)
  implicit val p:Parameters=new midas.F2Config().toInstance
  class Bound extends LazyModule with HasWidgets {
    val widgets=bridges.map {b=>addWidget(new AutoCounterBridgeModule(b.widgetConstructorKey.get.asInstanceOf[AutoCounterParameters])(
      p.alterPartial{case TargetClockInfo=>b.clockInfo}))}
    lazy val module=new LazyModuleImp(this) {
      val target=Module(new TargetBox(config))
      val channels=IO(new TargetBoxIO(external)).suggestName("channels")
      target.io.hostClock:=clock
      target.io.hostReset:=reset.asBool
      target.io.clockElement._2 <> channels.clockElement._2
      target.io.wireInputPortMap.foreach {case(n,t)=> t <> channels.wireInputPortMap(n)}
      channels.wireOutputPortMap.foreach {case(n,t)=> t <> target.io.wireOutputPortMap(n)}
      if(globalControl){val ctrl=IO(Flipped(WidgetMMIO())).suggestName("ctrl");genCtrlIO(ctrl)}
      widgets.zip(bridges).zipWithIndex.foreach {case((w,b),i)=>
        if(!globalControl){val ctrl=IO(Flipped(WidgetMMIO())).suggestName(s"ctrl$i");w.module.io.ctrl <> ctrl}
        HostPortIOConnectChannels2Port(w.module.hPort,b,target.io)
      }
    }
  }
  val owner=LazyModule(new Bound)
  val emitted=(new ChiselStage).emitChirrtl(owner.module,Array("--target-dir",output.getPath))
  val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(emitted),ChirrtlForm),Nil)
  val loweredTarget=new LowFirrtlCompiler().compile(state.copy(form=HighForm),Nil)
  write("sfc-fame-low.fir",loweredTarget.circuit.serialize)
  // TargetBoxIO.ClockRecord uses fields _0/_1; FAMETransform uses 0/1.
  // Link by the explicit ordered clock-lane relation after standard lowering.
  // This changes only references to verification-box scalar clock pin names.
  val clockPort=state.annotations.collectFirst {
    case c:fame.FAMEChannelConnectionAnnotation if c.channelInfo.isInstanceOf[fame.TargetClockChannel]=>c.sinks.get.head.ref
  }.get.stripSuffix("_bits")
  val clockPins=Map((0 until 2).map(i=>s"${clockPort}_bits__$i"->s"${clockPort}_bits_$i"):_*)
  def linkPins(e:firrtl.ir.Expression):firrtl.ir.Expression=e.map(linkPins) match {
    case s:firrtl.ir.SubField if s.expr.serialize=="TargetBox" && clockPins.contains(s.name)=>s.copy(name=clockPins(s.name))
    case other=>other
  }
  def linkStmt(s:firrtl.ir.Statement):firrtl.ir.Statement=s.map(linkStmt).map(linkPins)
  // Link the actual transformed target definition into the verification top.
  // The blackbox interface is checked by FIRRTL; no target or host logic edits.
  val rewritten=low.circuit.modules.filterNot(_.name=="TargetBox").map {
    case m:firrtl.ir.Module=>m.copy(body=linkStmt(m.body))
    case other=>other
  } ++ loweredTarget.circuit.modules.map {
    case m if m.name==state.circuit.main=> m match {
      case module:firrtl.ir.Module=>module.copy(name="TargetBox")
      case other=>sys.error(s"unexpected transformed target $other")
    }
    case other=>other
  }
  val linked=new ResolveAndCheck().runTransform(low.copy(circuit=low.circuit.copy(modules=rewritten)))
  // Standard SFC emission preparation; validif-false payloads are unspecified.
  // No memory, valid/ready or live-data qualification is changed here.
  write("bound.fir",firrtl.passes.RemoveValidIf.runTransform(linked).circuit.serialize)
  owner.widgets.zipWithIndex.foreach {case(w,i)=>
    write(s"abi-$i.txt",w.module.crRegistry.getAllRegs.map{case(n,a)=>s"$n=$a"}.mkString("\n")+"\n")
  }
  if(globalControl){val header=new StringBuilder;owner.genWidgetHeaders(header,Map.empty);write("control.h",header.toString)}
  println(s"PASS unchanged SFC source -> FAME -> actual channel-bound CSV hosts: ${bridges.size} widgets")
}
