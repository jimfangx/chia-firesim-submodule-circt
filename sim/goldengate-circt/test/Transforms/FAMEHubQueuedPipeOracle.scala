// See LICENSE for license details.
// Execute preserved PipeChannel instances, rational producer, and FAME hub.
// Every queue register is keyed by its instance path, even when definitions share.
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
object FAMEHubQueuedPipeOracle extends App {
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
  class Interpreter(module: Module, definitions: Map[String, Module] = Map.empty) {
    var nodes = Map.empty[String, (String, Expression)]
    var drivers = Map.empty[String, (String, Expression)]
    var registers = Seq.empty[(String, String, DefRegister)]
    def visit(current: Module, prefix: String): Unit = flatten(current.body).foreach {
      case n: DefNode => nodes += (prefix+n.name) -> (prefix -> n.value)
      case c: Connect => drivers += (prefix+c.loc.serialize) -> (prefix -> c.expr)
      case r: DefRegister => registers :+= ((prefix+r.name, prefix, r))
      case i: DefInstance if definitions.contains(i.module) =>
        visit(definitions(i.module), prefix+i.name+".")
      case _ => ()
    }
    visit(module, "")
    val regs = registers.map(_._3)
    var state = registers.map { case (name,_,_) => name -> BigInt(0) }.toMap
    var inputs: PartialFunction[String,BigInt] = PartialFunction.empty
    var memo = Map.empty[String,BigInt]
    def get(name: String): BigInt = memo.getOrElse(name, {
      val value = state.getOrElse(name,
        if(inputs.isDefinedAt(name)) inputs(name)
        else {
          val (prefix,e) = nodes.getOrElse(name, drivers.getOrElse(name,
            sys.error(s"missing $name in ${module.name}")))
          eval(e,prefix)
        })
      memo += name -> value
      value
    })
    def eval(expr: Expression, prefix: String = ""): BigInt = {
      def e(a: Expression): BigInt = eval(a,prefix)
      val value = expr match {
        case UIntLiteral(v,_) => v
        case Mux(s,h,l,_) => e(if(e(s)!=0) h else l)
        case DoPrim(op,a,c,_) => op match {
          case PrimOps.Eq => if(e(a(0))==e(a(1))) BigInt(1) else BigInt(0)
          case PrimOps.Lt => if(e(a(0))<e(a(1))) BigInt(1) else BigInt(0)
          case PrimOps.Add => e(a(0))+e(a(1))
          case PrimOps.Sub => e(a(0))-e(a(1))
          case PrimOps.And => e(a(0)) & e(a(1))
          case PrimOps.Or => e(a(0)) | e(a(1))
          case PrimOps.Not => ~e(a(0))
          case PrimOps.Bits => (e(a(0)) >> c(1).toInt) & ((BigInt(1) << (c(0)-c(1)+1).toInt)-1)
          case PrimOps.Tail | PrimOps.Pad | PrimOps.AsUInt => e(a(0))
          case other => sys.error(s"unsupported oracle primitive $other")
        }
        case other => get(prefix+other.serialize)
      }
      expr.tpe match {
        case UIntType(IntWidth(w)) => value & ((BigInt(1) << w.toInt)-1)
        case _ => value
      }
    }
    def next: Map[String,BigInt] = registers.map { case (name,prefix,r) =>
      val clock = r.clock.serialize
      val advance = clock == "clock" || clock == "hostClock" || {
        require(clock.endsWith(".O"), s"unexpected register clock $clock")
        get(prefix+clock.stripSuffix(".O")+".CE") != 0
      }
      val (driverPrefix,driver) = drivers(name)
      val value = if(!advance) state(name) else if(eval(r.reset,prefix)!=0)
        eval(r.init,prefix) else eval(driver,driverPrefix)
      val width = r.tpe.asInstanceOf[UIntType].width.asInstanceOf[IntWidth].width.toInt
      name -> (value & ((BigInt(1)<<width)-1))
    }.toMap
  }
  val hubModule = result.circuit.modules.collectFirst { case m: Module if m.name=="Model" => m }.get
  val producerModule = generator.circuit.modules.collectFirst { case m: Module if m.name==generator.circuit.main => m }.get
  val hub = new Interpreter(hubModule)
  val producer = new Interpreter(producerModule)
  object QueueElaboration {
    import chisel3._
    class Queues extends chisel3.Module {
      val io = IO(new Bundle {
        val externalInValid = Input(Vec(2,Bool()))
        val externalInBits = Input(Vec(2,UInt(16.W)))
        val externalInReady = Output(Vec(2,Bool()))
        val hubInValid = Output(Vec(2,Bool()))
        val hubInBits = Output(Vec(2,UInt(16.W)))
        val hubInReady = Input(Vec(2,Bool()))
        val hubOutValid = Input(Vec(2,Bool()))
        val hubOutBits = Input(Vec(2,UInt(16.W)))
        val hubOutReady = Output(Vec(2,Bool()))
        val externalOutValid = Output(Vec(2,Bool()))
        val externalOutBits = Output(Vec(2,UInt(16.W)))
        val externalOutReady = Input(Vec(2,Bool()))
      })
      for(i <- 0 until 2) {
        val in = chisel3.Module(new midas.core.PipeChannel(UInt(16.W),0)).suggestName(s"input$i")
        in.io.in.valid := io.externalInValid(i)
        in.io.in.bits := io.externalInBits(i)
        io.externalInReady(i) := in.io.in.ready
        io.hubInValid(i) := in.io.out.valid
        io.hubInBits(i) := in.io.out.bits
        in.io.out.ready := io.hubInReady(i)
        val out = chisel3.Module(new midas.core.PipeChannel(UInt(16.W),0)).suggestName(s"output$i")
        out.io.in.valid := io.hubOutValid(i)
        out.io.in.bits := io.hubOutBits(i)
        io.hubOutReady(i) := out.io.in.ready
        io.externalOutValid(i) := out.io.out.valid
        io.externalOutBits(i) := out.io.out.bits
        out.io.out.ready := io.externalOutReady(i)
      }
    }
  }
  val queueInput = chisel3.stage.ChiselStage.emitChirrtl(new QueueElaboration.Queues)
  val queueCircuit = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(queueInput),ChirrtlForm),Nil).circuit
  save("queues.sfc.fir",queueCircuit)
  val queueModules = queueCircuit.modules.collect { case m: Module => m.name -> m }.toMap
  val queueModule = queueModules(queueCircuit.main)
  val queues = new Interpreter(queueModule,queueModules)
  require(flatten(queueModule.body).count(_.isInstanceOf[DefInstance])==4)
  require(queues.registers.map(_._1).distinct.size==queues.registers.size)
  val channelNames = Seq("in0", "in1", "out0", "out1")
  val firedNames = channelNames.map(name => hub.regs.find(_.name.startsWith(name+"_fired")).get.name)
  val payload = hubModule.ports.find(_.name=="bridge_clocks_sink").get.tpe.asInstanceOf[BundleType]
    .fields.find(_.name=="bits").get.tpe.asInstanceOf[BundleType]
  require(payload.fields.map(_.name)==clockNames.map(_.stripPrefix("bridge_clocks")))
  // An independent FIFO scoreboard checks each physical queue's token stream.
  // The emitted IR, rather than this scoreboard, still drives every hub signal.
  val scoreboards = Array.fill(4)(scala.collection.mutable.Queue.empty[BigInt])
  val deliveries = Array.fill(4)(0)
  val highWater = Array.fill(4)(0)
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
    queues.memo=Map.empty
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
      case "in0_sink.valid" => queues.get("io_hubInValid_0")
      case "in1_sink.valid" => queues.get("io_hubInValid_1")
      case "in0_sink.bits" => queues.get("io_hubInBits_0")
      case "in1_sink.bits" => queues.get("io_hubInBits_1")
      case "out0_source.ready" => queues.get("io_hubOutReady_0")
      case "out1_source.ready" => queues.get("io_hubOutReady_1")
    }
    queues.inputs = {
      case "clock" => BigInt(0)
      case "reset" => bit(reset)
      case "io_externalInValid_0" => bit(pending(0))
      case "io_externalInValid_1" => bit(pending(1))
      case "io_externalInBits_0" => inputPayload(0)
      case "io_externalInBits_1" => inputPayload(1)
      case "io_hubInReady_0" => hub.get("in0_sink.ready")
      case "io_hubInReady_1" => hub.get("in1_sink.ready")
      case "io_hubOutValid_0" => hub.get("out0_source.valid")
      case "io_hubOutValid_1" => hub.get("out1_source.valid")
      case "io_hubOutBits_0" => hub.get("out0_source.bits")
      case "io_hubOutBits_1" => hub.get("out1_source.bits")
      case "io_externalOutReady_0" => bit(cycle%97>=29 && cycle%7!=0)
      case "io_externalOutReady_1" => bit(cycle%83>=37 && cycle%11!=0)
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
    val externalInputReady=mask((0 until 2).map(i=>queues.get(s"io_externalInReady_$i")))
    val externalOutputValid=mask((0 until 2).map(i=>queues.get(s"io_externalOutValid_$i")))
    val hubInputValid=mask((0 until 2).map(i=>queues.get(s"io_hubInValid_$i")))
    def outBits(i: Int): BigInt = if(((externalOutputValid>>i)&1)==0) BigInt(0) else queues.get(s"io_externalOutBits_$i")
    for(i <- 0 until 4) {
      val lane=i%2
      val sink=if(i<2) s"io_hubInValid_$lane" else s"io_externalOutValid_$lane"
      val bits=if(i<2) s"io_hubInBits_$lane" else s"io_externalOutBits_$lane"
      val ready=if(i<2) s"io_externalInReady_$lane" else s"io_hubOutReady_$lane"
      val fifo=scoreboards(i)
      require(queues.get(sink)==bit(fifo.nonEmpty), s"queue $i valid mismatch at cycle $cycle")
      require(queues.get(ready)==bit(fifo.size<2), s"queue $i capacity mismatch at cycle $cycle")
      if(fifo.nonEmpty) require(queues.get(bits)==fifo.front, s"queue $i payload mismatch at cycle $cycle")
      highWater(i)=highWater(i).max(fifo.size)
    }
    println(s"TRACE $cycle $tokens $finishing $enabled $fired $inputReady $outputValid $ce ${hub.get("state0")} ${hub.get("state1")} ${hub.get("out0_source.bits")} ${hub.get("out1_source.bits")} $externalInputReady $externalOutputValid ${outBits(0)} ${outBits(1)} $hubInputValid ${queues.get("io_hubInBits_0")} ${queues.get("io_hubInBits_1")}")
    for(i <- 0 until 2) if(((ce>>i)&1)!=0)
      expectedState(i)=(expectedState(i)+queues.get(s"io_hubInBits_$i"))&65535
    val nextHub=hub.next
    val nextProducer=producer.next
    val nextQueues=queues.next
    for(i <- 0 until 2) if(reset || (pending(i) && ((externalInputReady>>i)&1)!=0)) pending(i)=false
    for(i <- 0 until 4) {
      val lane=i%2
      val fifo=scoreboards(i)
      if(reset) fifo.clear()
      else {
        val inValid=if(i<2) queues.get(s"io_externalInValid_$lane") else queues.get(s"io_hubOutValid_$lane")
        val inReady=if(i<2) queues.get(s"io_externalInReady_$lane") else queues.get(s"io_hubOutReady_$lane")
        val outValid=if(i<2) queues.get(s"io_hubInValid_$lane") else queues.get(s"io_externalOutValid_$lane")
        val outReady=if(i<2) queues.get(s"io_hubInReady_$lane") else queues.get(s"io_externalOutReady_$lane")
        val payload=if(i<2) queues.get(s"io_externalInBits_$lane") else queues.get(s"io_hubOutBits_$lane")
        if(outValid!=0 && outReady!=0) { fifo.dequeue(); deliveries(i)+=1 }
        if(inValid!=0 && inReady!=0) fifo.enqueue(payload)
        require(fifo.size<=2, s"queue $i overflow at cycle $cycle")
      }
    }
    hub.state=nextHub
    producer.state=nextProducer
    queues.state=nextQueues
  }
  require(deliveries.forall(_>0), "every physical queue must deliver tokens")
  require(deliveries.distinct.size>1, "independent queues must exercise different delivery counts")
  require(highWater.forall(_==2), "all queues must reach depth two under independent stalls")
  println(s"CHECK queue deliveries ${deliveries.mkString(",")}; high-water ${highWater.mkString(",")}")
}
