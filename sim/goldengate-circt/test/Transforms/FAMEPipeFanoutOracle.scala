// See LICENSE for license details.
// Differential oracle for SimWrapper.genPipeChannel's atomic enqueue fork.
// Elaborate production PipeChannel and DecoupledHelper, then execute their
// emitted Low FIRRTL. Each queue register is keyed by its complete instance
// path; an independent FIFO scoreboard checks delivery and reset seed tokens.
import firrtl._
import firrtl.ir._
import org.chipsalliance.cde.config.Parameters

object FAMEPipeFanoutOracle extends App {
  implicit val p: Parameters = Parameters.empty
  object Elaboration {
    import chisel3._
    import freechips.rocketchip.util.DecoupledHelper
    class Queues extends chisel3.Module {
      val io = IO(new Bundle {
        val inValid = Input(Bool())
        val inBits = Input(UInt(16.W))
        val inReady = Output(Bool())
        val outReady = Input(Vec(3, Bool()))
        val outValid = Output(Vec(3, Bool()))
        val outBits = Output(Vec(3, UInt(16.W)))
        val enqueueValid = Output(Vec(3, Bool()))
        val enqueueReady = Output(Vec(3, Bool()))
      })
      val queues = Seq(0, 1, 0).zipWithIndex.map { case (latency, index) =>
        chisel3.Module(new midas.core.PipeChannel(UInt(16.W), latency))
          .suggestName(s"queue$index")
      }
      // This is the production SimWrapper.genPipeChannel grouping rule.
      // Excluding each queue's own ready avoids combinational feedback while
      // ensuring every physical enqueue happens on the same source transfer.
      val helper = DecoupledHelper((io.inValid +: queues.map(_.io.in.ready)): _*)
      for ((queue, index) <- queues.zipWithIndex) {
        queue.io.in.bits := io.inBits
        queue.io.in.valid := helper.fire(queue.io.in.ready)
        queue.io.out.ready := io.outReady(index)
        io.outValid(index) := queue.io.out.valid
        io.outBits(index) := queue.io.out.bits
        io.enqueueValid(index) := queue.io.in.valid
        io.enqueueReady(index) := queue.io.in.ready
      }
      io.inReady := helper.fire(io.inValid)
    }
  }
  val destination = new java.io.File(args(0))
  destination.mkdirs()
  val input = chisel3.stage.ChiselStage.emitChirrtl(new Elaboration.Queues)
  val circuit = new LowFirrtlCompiler()
    .compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil).circuit
  val writer = new java.io.PrintWriter(new java.io.File(destination, "queues.sfc.fir"))
  try writer.write(circuit.serialize) finally writer.close()

  def flatten(statement: Statement): Seq[Statement] = statement match {
    case Block(statements) => statements.flatMap(flatten)
    case other => Seq(other)
  }
  class Interpreter(module: Module, definitions: Map[String, Module]) {
    var nodes = Map.empty[String, (String, Expression)]
    var drivers = Map.empty[String, (String, Expression)]
    var registers = Seq.empty[(String, String, DefRegister)]
    def visit(current: Module, prefix: String): Unit = flatten(current.body).foreach {
      case n: DefNode => nodes += (prefix + n.name) -> (prefix -> n.value)
      case c: Connect => drivers += (prefix + c.loc.serialize) -> (prefix -> c.expr)
      case r: DefRegister => registers :+= ((prefix + r.name, prefix, r))
      case i: DefInstance if definitions.contains(i.module) =>
        visit(definitions(i.module), prefix + i.name + ".")
      case _ => ()
    }
    visit(module, "")
    var state = registers.map { case (name, _, _) => name -> BigInt(0) }.toMap
    var inputs: PartialFunction[String, BigInt] = PartialFunction.empty
    var memo = Map.empty[String, BigInt]
    def get(name: String): BigInt = memo.getOrElse(name, {
      val value = state.getOrElse(name,
        if (inputs.isDefinedAt(name)) inputs(name)
        else {
          val (prefix, expression) = nodes.getOrElse(name,
            drivers.getOrElse(name, sys.error(s"missing $name in ${module.name}")))
          eval(expression, prefix)
        })
      memo += name -> value
      value
    })
    def eval(expression: Expression, prefix: String = ""): BigInt = {
      def e(a: Expression): BigInt = eval(a, prefix)
      val value = expression match {
        case UIntLiteral(v, _) => v
        case Mux(s, h, l, _) => e(if (e(s) != 0) h else l)
        case DoPrim(op, arguments, constants, _) => op match {
          case PrimOps.Eq => if (e(arguments(0)) == e(arguments(1))) BigInt(1) else BigInt(0)
          case PrimOps.And => e(arguments(0)) & e(arguments(1))
          case PrimOps.Or => e(arguments(0)) | e(arguments(1))
          case PrimOps.Not => ~e(arguments(0))
          case PrimOps.Bits => (e(arguments(0)) >> constants(1).toInt) &
            ((BigInt(1) << (constants(0) - constants(1) + 1).toInt) - 1)
          case PrimOps.Tail | PrimOps.Pad | PrimOps.AsUInt => e(arguments(0))
          case other => sys.error(s"unsupported oracle primitive $other")
        }
        case other => get(prefix + other.serialize)
      }
      expression.tpe match {
        case UIntType(IntWidth(width)) => value & ((BigInt(1) << width.toInt) - 1)
        case _ => value
      }
    }
    def next: Map[String, BigInt] = registers.map { case (name, prefix, register) =>
      require(register.clock.serialize == "clock", s"unexpected register clock ${register.clock}")
      val (driverPrefix, driver) = drivers(name)
      val value = if (eval(register.reset, prefix) != 0) eval(register.init, prefix)
                  else eval(driver, driverPrefix)
      val width = register.tpe.asInstanceOf[UIntType].width.asInstanceOf[IntWidth].width.toInt
      name -> (value & ((BigInt(1) << width) - 1))
    }.toMap
  }
  val modules = circuit.modules.collect { case module: Module => module.name -> module }.toMap
  val top = modules(circuit.main)
  require(flatten(top.body).count(_.isInstanceOf[DefInstance]) == 3)
  val queues = new Interpreter(top, modules)
  require(queues.registers.map(_._1).distinct.size == queues.registers.size)
  val fifos = Array.fill(3)(scala.collection.mutable.Queue.empty[BigInt])
  val deliveries = Array.fill(3)(0)
  val highWater = Array.fill(3)(0)
  var pending = false
  var tokenCounter = 0
  var previousReset = false
  var transfers = 0
  var stalled = 0
  var differingReadiness = 0
  var seeds = 0
  def bit(value: Boolean): BigInt = if (value) BigInt(1) else BigInt(0)
  def mask(values: Seq[BigInt]): BigInt = values.zipWithIndex
    .map { case (value, index) => value << index }.foldLeft(BigInt(0))(_ | _)
  for (cycle <- 0 until 4096) {
    val reset = cycle < 3 || cycle == 2048 || cycle == 2049
    if (reset) { pending = false; tokenCounter = 0 }
    else if (!pending && cycle % 7 != 0) pending = true
    val payload = BigInt((tokenCounter * 73 + 19) & 65535)
    val ready = Seq(cycle % 97 >= 11, cycle % 83 >= 7, cycle % 61 >= 13)
    queues.memo = Map.empty
    queues.inputs = {
      case "clock" => BigInt(0)
      case "reset" => bit(reset)
      case "io_inValid" => bit(pending)
      case "io_inBits" => payload
      case name if name.startsWith("io_outReady_") => bit(ready(name.stripPrefix("io_outReady_").toInt))
    }
    val inReady = queues.get("io_inReady")
    val outValid = mask((0 until 3).map(i => queues.get(s"io_outValid_$i")))
    val enqueueValid = mask((0 until 3).map(i => queues.get(s"io_enqueueValid_$i")))
    val enqueueReady = mask((0 until 3).map(i => queues.get(s"io_enqueueReady_$i")))
    require(inReady == bit(enqueueReady == 7), s"fork readiness mismatch at cycle $cycle")
    for (i <- 0 until 3) {
      val initializing = i == 1 && previousReset
      require(queues.get(s"io_enqueueReady_$i") == bit(!initializing && fifos(i).size < 2),
        s"queue $i capacity mismatch at cycle $cycle")
      require(queues.get(s"io_outValid_$i") == bit(fifos(i).nonEmpty),
        s"queue $i valid mismatch at cycle $cycle")
      if (fifos(i).nonEmpty) require(queues.get(s"io_outBits_$i") == fifos(i).front,
        s"queue $i payload mismatch at cycle $cycle")
      val otherReady = (0 until 3).filterNot(_ == i).forall(j => ((enqueueReady >> j) & 1) != 0)
      require(queues.get(s"io_enqueueValid_$i") == bit(pending && otherReady),
        s"fork enqueue-valid mismatch at cycle $cycle, queue $i")
      highWater(i) = highWater(i).max(fifos(i).size)
    }
    def outBits(i: Int): BigInt = if (((outValid >> i) & 1) == 0) BigInt(0)
                                 else queues.get(s"io_outBits_$i")
    println(s"TRACE $cycle $inReady $outValid ${outBits(0)} ${outBits(1)} ${outBits(2)} $enqueueValid $enqueueReady")
    val next = queues.next
    val transfer = pending && inReady != 0
    if (!reset) {
      if (pending && !transfer) stalled += 1
      if (enqueueReady != 0 && enqueueReady != 7) differingReadiness += 1
      if (transfer) transfers += 1
    }
    for (i <- 0 until 3) {
      val fifo = fifos(i)
      if (reset) fifo.clear()
      else {
        if (fifo.nonEmpty && ready(i)) { fifo.dequeue(); deliveries(i) += 1 }
        if (i == 1 && previousReset) { fifo.enqueue(BigInt(0)); seeds += 1 }
        else if (transfer) fifo.enqueue(payload)
        require(fifo.size <= 2, s"queue $i overflow at cycle $cycle")
      }
    }
    if (!reset && transfer) { pending = false; tokenCounter += 1 }
    previousReset = reset
    queues.state = next
  }
  require(transfers > 1000 && stalled > 500 && differingReadiness > 500,
    "fanout must exercise deliveries and independent queue backpressure")
  require(deliveries.forall(_ > 1000) && highWater.forall(_ == 2),
    "every queue must deliver tokens and reach depth two")
  require(seeds == 2, "latency-one queue must insert one seed per reset epoch")
  println(s"CHECK transfers $transfers; stalls $stalled; differing readiness $differingReadiness; " +
    s"deliveries ${deliveries.mkString(",")}; high-water ${highWater.mkString(",")}; seeds $seeds")
}
