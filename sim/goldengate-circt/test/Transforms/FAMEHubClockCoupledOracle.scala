// See LICENSE for license details.
// Execute the preserved rational producer and FAME hub together under stalls.
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
import firesim.lib.bridges.ClockParameters
import firesim.lib.nasti.NastiParameters
import midas.widgets.{ClockBridgeModule, CtrlNastiKey}
import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.diplomacy.LazyModule
object FAMEHubClockCoupledOracle extends App {
  val input = """circuit Top :
  module Model :
    input bridge_clocks_1 : Clock
    input bridge_clocks_0 : Clock
    input in0 : UInt<16>
    input in1 : UInt<16>
    output out0 : UInt<16>
    output out1 : UInt<16>
    output targetClock0 : Clock
    output targetClock1 : Clock
    reg state0 : UInt<16>, bridge_clocks_0
    reg state1 : UInt<16>, bridge_clocks_1
    state0 <= add(state0, in0)
    state1 <= add(state1, in1)
    out0 <= add(state0, in0)
    out1 <= add(state1, in1)
    targetClock0 <= bridge_clocks_0
    targetClock1 <= bridge_clocks_1
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input bridge_clocks_0 : Clock
    input bridge_clocks_1 : Clock
    input in0 : UInt<16>
    input in1 : UInt<16>
    output out0 : UInt<16>
    output out1 : UInt<16>
    output targetClock0 : Clock
    output targetClock1 : Clock
    inst model of Model
    model.bridge_clocks_0 <= bridge_clocks_0
    model.bridge_clocks_1 <= bridge_clocks_1
    model.in0 <= in0
    model.in1 <= in1
    out0 <= model.out0
    out1 <= model.out1
    targetClock0 <= model.targetClock0
    targetClock1 <= model.targetClock1
"""
  val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
  val top = ModuleTarget("Top", "Top")
  val model = ModuleTarget("Top", "Model")
  val clockNames = if (args.length > 1) Seq("bridge_clocks_1", "bridge_clocks_0")
                   else Seq("bridge_clocks_0", "bridge_clocks_1")
  val annos = Seq(
    FAMEHostClock(top.ref("hostClock")), FAMEHostReset(top.ref("hostReset")),
    FAMETransformAnnotation(model),
    FAMEChannelPortsAnnotation("bridge_clocks", None, clockNames.map(model.ref)),
    FAMEChannelConnectionAnnotation("bridge_clocks", TargetClockChannel(Seq(RationalClock("domain0", 1, 2), RationalClock("domain1", 1, 3)), Seq(1, 2)), None, None, Some(clockNames.map(top.ref)))
  ) ++ (0 until 2).flatMap { index =>
    val out = "out" + index
    val clk = "targetClock" + index
    Seq(
      FAMEChannelPortsAnnotation(out, Some(model.ref(clk)), Seq(model.ref(out))),
      FAMEChannelConnectionAnnotation(out, PipeChannel(0), Some(top.ref(clk)), Some(Seq(top.ref(out))), None),
      FAMEChannelPortsAnnotation("in" + index, Some(model.ref(clk)), Seq(model.ref("in" + index))),
      FAMEChannelConnectionAnnotation("in" + index, PipeChannel(0), Some(top.ref(clk)), None, Some(Seq(top.ref("in" + index))))
    )
  }
  val result = new FAMETransform().execute(low.copy(annotations = annos))

  val destination = new java.io.File(args(0))
  destination.mkdirs()
  def save(name: String, circuit: Circuit): Unit = {
    val writer = new java.io.PrintWriter(new java.io.File(destination, name))
    try writer.write(circuit.serialize) finally writer.close()
  }
  save("hub.sfc.fir", result.circuit)
  implicit val p: Parameters = Parameters.empty.alterPartial {
    case CtrlNastiKey => NastiParameters(32,32,12)
  }
  val ratios = Seq(1 -> 2, 1 -> 3)
  val clocks = ratios.zipWithIndex.map { case ((n,d),i) => RationalClock(s"domain$i",n,d) }
  val owner = LazyModule(new ClockBridgeModule(ClockParameters(clocks)))
  val generatorInput = chisel3.stage.ChiselStage.emitChirrtl(new owner.RationalClockTokenGenerator(ratios))
  val generator = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(generatorInput),ChirrtlForm),Nil)
  save("producer.sfc.fir", generator.circuit)

  def flatten(stmt: Statement): Seq[Statement] = stmt match {
    case Block(stmts) => stmts.flatMap(flatten)
    case other => Seq(other)
  }
  // Reference lookup follows actual nodes and connects lazily. The only
  // environment inputs are the external channel stimulus and producer/hub
  // interconnect; all handshake and state equations come from emitted IR.
  class Interpreter(module: Module) {
    val body = flatten(module.body)
    val nodes = body.collect { case n: DefNode => n.name -> n.value }.toMap
    val drivers = body.collect { case c: Connect => c.loc.serialize -> c.expr }.toMap
    val regs = body.collect { case r: DefRegister => r }
    var state = regs.map(r => r.name -> BigInt(0)).toMap
    var inputs: PartialFunction[String,BigInt] = PartialFunction.empty
    var memo = Map.empty[String,BigInt]
    def get(name: String): BigInt = memo.getOrElse(name, {
      val value = state.getOrElse(name,
        if(inputs.isDefinedAt(name)) inputs(name)
        else eval(nodes.getOrElse(name, drivers.getOrElse(name, sys.error(s"missing $name in ${module.name}")))))
      memo += name -> value
      value
    })
    def eval(expr: Expression): BigInt = {
      val value = expr match {
        case UIntLiteral(v,_) => v
        case Mux(s,h,l,_) => eval(if(eval(s)!=0) h else l)
        case DoPrim(op,a,c,_) => op match {
          case PrimOps.Eq => if(eval(a(0))==eval(a(1))) BigInt(1) else BigInt(0)
          case PrimOps.Lt => if(eval(a(0))<eval(a(1))) BigInt(1) else BigInt(0)
          case PrimOps.Add => eval(a(0))+eval(a(1))
          case PrimOps.Sub => eval(a(0))-eval(a(1))
          case PrimOps.And => eval(a(0)) & eval(a(1))
          case PrimOps.Or => eval(a(0)) | eval(a(1))
          case PrimOps.Not => ~eval(a(0))
          case PrimOps.Bits => (eval(a(0)) >> c(1).toInt) & ((BigInt(1) << (c(0)-c(1)+1).toInt)-1)
          case PrimOps.Tail | PrimOps.Pad | PrimOps.AsUInt => eval(a(0))
          case other => sys.error(s"unsupported oracle primitive $other")
        }
        case other => get(other.serialize)
      }
      expr.tpe match {
        case UIntType(IntWidth(w)) => value & ((BigInt(1) << w.toInt)-1)
        case _ => value
      }
    }
    def next: Map[String,BigInt] = regs.map { r =>
      val clock = r.clock.serialize
      val advance = clock == "clock" || clock == "hostClock" || {
        require(clock.endsWith(".O"), s"unexpected register clock $clock")
        get(clock.stripSuffix(".O")+".CE") != 0
      }
      val value = if(!advance) state(r.name) else if(eval(r.reset)!=0) eval(r.init) else eval(drivers(r.name))
      val width = r.tpe.asInstanceOf[UIntType].width.asInstanceOf[IntWidth].width.toInt
      r.name -> (value & ((BigInt(1)<<width)-1))
    }.toMap
  }
  val hubModule = result.circuit.modules.collectFirst { case m: Module if m.name=="Model" => m }.get
  val producerModule = generator.circuit.modules.collectFirst { case m: Module if m.name==generator.circuit.main => m }.get
  val hub = new Interpreter(hubModule)
  val producer = new Interpreter(producerModule)
  val channelNames = Seq("in0", "in1", "out0", "out1")
  val firedNames = channelNames.map(name => hub.regs.find(_.name.startsWith(name+"_fired")).get.name)
  val payload = hubModule.ports.find(_.name=="bridge_clocks_sink").get.tpe.asInstanceOf[BundleType]
    .fields.find(_.name=="bits").get.tpe.asInstanceOf[BundleType]
  require(payload.fields.map(_.name)==clockNames.map(_.stripPrefix("bridge_clocks")))
  val pending = Array(false,false)
  val inputPayload = Array(BigInt(0),BigInt(0))
  val expectedState = Array(BigInt(0),BigInt(0))
  def bit(value: Boolean): BigInt = if(value) BigInt(1) else BigInt(0)
  def mask(values: Seq[BigInt]): BigInt = values.zipWithIndex.map { case (v,i) => v<<i }.foldLeft(BigInt(0))(_|_)
  for(cycle <- 0 until 8192) {
    val reset = cycle<3 || cycle==4096 || cycle==4097
    if(reset) {
      for(i <- 0 until 2) { pending(i)=false; inputPayload(i)=BigInt(0) }
    } else {
      if(!pending(0) && (cycle%11==3 || cycle%43>37)) {
        pending(0)=true
        inputPayload(0)=BigInt((cycle*73+19)&65535)
      }
      if(!pending(1) && (cycle%17==5 || cycle%61>53)) {
        pending(1)=true
        inputPayload(1)=BigInt((cycle*151+41)&65535)
      }
    }
    hub.memo=Map.empty
    producer.memo=Map.empty
    producer.inputs = {
      case "clock" => BigInt(0)
      case "reset" => bit(reset)
      case "io_ready" => hub.get("bridge_clocks_sink.ready")
    }
    hub.inputs = {
      case "hostClock" => BigInt(0)
      case "hostReset" => bit(reset)
      case "bridge_clocks_sink.valid" => producer.get("io_valid")
      case name if name.startsWith("bridge_clocks_sink.bits.") =>
        val suffix=name.stripPrefix("bridge_clocks_sink.bits.")
        val lane=payload.fields.indexWhere(_.name==suffix)
        require(lane>=0)
        producer.get(s"io_bits_$lane")
      case "in0_sink.valid" => bit(pending(0))
      case "in1_sink.valid" => bit(pending(1))
      case "in0_sink.bits" => inputPayload(0)
      case "in1_sink.bits" => inputPayload(1)
      case "out0_source.ready" => bit(cycle%97>=29 && cycle%7!=0)
      case "out1_source.ready" => bit(cycle%83>=37 && cycle%11!=0)
    }
    val tokens=mask((0 until 2).map(i => producer.get(s"io_bits_$i")))
    val finishing=hub.get("targetCycleFinishing")
    val enabled=mask((0 until 2).map(i => hub.get(s"bridge_clocks_${i}_enabled")))
    val fired=mask(firedNames.map(hub.get))
    val inputReady=mask((0 until 2).map(i => hub.get(s"in${i}_sink.ready")))
    val outputValid=mask((0 until 2).map(i => hub.get(s"out${i}_source.valid")))
    val ce=mask((0 until 2).map(i => hub.get(s"bridge_clocks_${i}_buffer.CE")))
    for(i <- 0 until 2)
      require(hub.get(s"state$i")==expectedState(i), s"target state mismatch at cycle $cycle, lane $i")
    println(s"TRACE $cycle $tokens $finishing $enabled $fired $inputReady $outputValid $ce ${hub.get("state0")} ${hub.get("state1")} ${hub.get("out0_source.bits")} ${hub.get("out1_source.bits")}")
    for(i <- 0 until 2) if(((ce>>i)&1)!=0)
      expectedState(i)=(expectedState(i)+inputPayload(i))&65535
    val nextHub=hub.next
    val nextProducer=producer.next
    for(i <- 0 until 2) if(reset || (pending(i) && ((inputReady>>i)&1)!=0)) pending(i)=false
    hub.state=nextHub
    producer.state=nextProducer
  }
}
