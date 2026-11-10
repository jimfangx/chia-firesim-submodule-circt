// See LICENSE for license details.
package midas.passes
import firrtl._
import firrtl.Mappers._
import firrtl.annotations.{JsonProtocol,ModuleTarget,NoTargetAnnotation,HasSerializationHints}
import firrtl.options.TargetDirAnnotation
import chisel3._
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{BridgeIOAnnotation,RationalClock}
import freechips.rocketchip.diplomacy.{LazyModule,LazyModuleImp,IdRange}
import freechips.rocketchip.amba.axi4._
import midas.core._
import midas.widgets.{AutoCounterBridgeModule,AutoCounterParameters,EventMetadata,TargetClockInfo,WidgetMMIO,HasWidgets,PrintBridgeModule,PrintBridgeParameters,PrintPort}
import midas.targetutils.PerfCounterOps
import org.chipsalliance.cde.config.Parameters

/** Execute unchanged SFC target passes and actual host/channel constructor
  * binding on the same input. Only the verification top is new: target channels
  * not consumed by CSV remain externally driven/observed. No target outputs
  * bypass FAME to reach a host and no expected counter values are substituted.
  */
object EnabledInstrumentationPipelineOracle extends App {
  require(args.length==3,"input.fir input.json output-directory")
  val globalControl=true
  def read(p:String):String={val s=scala.io.Source.fromFile(p);try s.mkString finally s.close()}
  val output=new java.io.File(args(2));require(output.mkdirs()||output.isDirectory)
  def write(name:String,text:String):Unit={val p=new java.io.PrintWriter(new java.io.File(output,name));try p.write(text) finally p.close()}
  write("input.fir",read(args(0)))
  write("input.anno.json",read(args(1)))
  var state=CircuitState(Parser.parse(read(args(0))),LowForm,
    JsonProtocol.deserialize(read(args(1))) :+ TargetDirAnnotation(output.getPath))
  state=new AutoCounterTransform().doTransform(state,usePrintfImplementation=false)
  // Run the actual enabled transform; remove only its nonserializable config.
  val printConfig=midas.stage.phases.ConfigParametersAnnotation(
    new midas.F2Config().toInstance.alterPartial {case midas.SynthPrints=>true})
  state=new PrintSynthesis().runTransform(state.copy(annotations=state.annotations:+printConfig))
  state=state.copy(annotations=state.annotations.filterNot(_==printConfig))
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
    def typeHints=Seq(classOf[EventMetadata],classOf[PrintPort],PerfCounterOps.Accumulate.getClass,PerfCounterOps.Identity.getClass)
  }
  write("sfc-fame.fir",state.circuit.serialize)
  write("sfc-fame.json",JsonProtocol.serialize(state.annotations :+ ConstructorHints()))
  write("AbstractClockGate.v",DefineAbstractClockGate.impl)
  val bridges=state.annotations.collect {case b:BridgeIOAnnotation if b.widgetClass==classOf[AutoCounterBridgeModule].getName=>b}
  val printBridges=state.annotations.collect {case b:BridgeIOAnnotation if b.widgetClass==classOf[PrintBridgeModule].getName=>b}
  require(printBridges.size==1,"expected one enabled Print domain")
  require(bridges.nonEmpty,"no CSV bridge generated")
  val target=state.circuit.modules.find(_.name==state.circuit.main).get
  val leafTypes=target.ports.map(p=>ModuleTarget(state.circuit.main,target.name).ref(p.name)->p).toMap
  val config=SimWrapperConfig(state.annotations,leafTypes)
  val boundNames=(bridges++printBridges).flatMap(_.channelMapping.values).toSet
  val external=SimWrapperConfig(state.annotations.filter {
    case c:fame.FAMEChannelConnectionAnnotation=> !boundNames(c.globalName)
    case _=>true
  },leafTypes)
  implicit val p:Parameters=new midas.F2Config().toInstance.alterPartial {
    case ToCPUStreamAllocatorKey=>new StreamAllocator
    case FromCPUStreamAllocatorKey=>new StreamAllocator
  }
  class Bound extends LazyModule with HasWidgets {
    val widgets=bridges.map {b=>addWidget(new AutoCounterBridgeModule(b.widgetConstructorKey.get.asInstanceOf[AutoCounterParameters])(
      p.alterPartial{case TargetClockInfo=>b.clockInfo}))}
    val prints=printBridges.map {b=>addWidget(new PrintBridgeModule(b.widgetConstructorKey.get.asInstanceOf[PrintBridgeParameters])(
      p.alterPartial{case TargetClockInfo=>b.clockInfo}))}
    val engine=addWidget(new CPUManagedStreamEngine(p,StreamEngineParameters(prints.map(_.streamSourceParams),Seq.empty)))
    val cpuParams=p(CPUManagedAXI4Key).get
    val cpuNode=AXI4MasterNode(Seq(AXI4MasterPortParameters(masters=Seq(AXI4MasterParameters(
      name="cpu-managed-axi4",id=IdRange(0,1<<cpuParams.idBits),aligned=false,maxFlight=cpuParams.maxFlight)))))
    engine.cpuManagedAXI4NodeOpt.get := AXI4Buffer() := cpuNode
    lazy val module=new LazyModuleImp(this) {
      val cpu=IO(Flipped(AXI4Bundle(cpuParams.axi4BundleParams))).suggestName("cpu_managed_axi4")
      cpuNode.out.head._1 <> cpu
      engine.streamsToHostCPU.zip(prints).foreach{case(e,w)=>e <> w.streamEnq}
      val target=Module(new TargetBox(config))
      val channels=IO(new TargetBoxIO(external)).suggestName("channels")
      target.io.hostClock:=clock
      target.io.hostReset:=reset.asBool
      target.io.clockElement._2 <> channels.clockElement._2
      target.io.wireInputPortMap.foreach {case(n,t)=> t <> channels.wireInputPortMap(n)}
      channels.wireOutputPortMap.foreach {case(n,t)=> t <> target.io.wireOutputPortMap(n)}
      if(globalControl){val ctrl=IO(Flipped(WidgetMMIO())).suggestName("ctrl");genCtrlIO(ctrl)}
      prints.zip(printBridges).foreach {case(w,b)=>HostPortIOConnectChannels2Port(w.module.hPort,b,target.io)}
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
  val peer=new StringBuilder;owner.genWidgetPeerToPeerAddrMap(peer);write("peer.yaml",peer.toString)
  println(s"PASS unchanged SFC source -> CSV + Print + dynamic triggers -> FAME -> actual shared control/CPU hosts: ${bridges.size} CSV / ${printBridges.size} Print")
}
