// See LICENSE for license details.
// Execute actual Scala PipeChannel/ShiftQueue IR for typed token payloads.
import chisel3._
import firrtl._
import firrtl.ir.{Mux => FMux, _}
import org.chipsalliance.cde.config.Parameters
import midas.core.PipeChannel

object FAMEPipePayloadOracle extends App {
  implicit val p: Parameters = Parameters.empty
  class Inner extends Bundle {
    val signed = SInt(7.W)
    val unsigned = UInt(9.W)
  }
  class Nested extends Bundle {
    val flag = UInt(1.W)
    val inner = new Inner
    val lanes = Vec(3, UInt(5.W))
  }
  case class Leaf(name: String, width: Int, offset: Int)
  case class Shape(name: String, gen: () => Data, leaves: Seq[Leaf]) {
    val width: Int = leaves.map(_.width).sum
  }
  val shapes = Seq(
    Shape("zero", () => UInt(0.W), Seq(Leaf("", 0, 0))),
    Shape("signed", () => SInt(13.W), Seq(Leaf("", 13, 0))),
    Shape("vector", () => Vec(3, UInt(5.W)), (0 until 3).map(i => Leaf(s"_$i", 5, i * 5))),
    Shape("nested", () => new Nested,
      Seq(Leaf("_flag", 1, 31), Leaf("_inner_signed", 7, 24),
          Leaf("_inner_unsigned", 9, 15)) ++
          (0 until 3).map(i => Leaf(s"_lanes_$i", 5, i * 5)))
  )
  val destination = new java.io.File(args(0))
  destination.mkdirs()
  def flatten(s: Statement): Seq[Statement] = s match {
    case Block(stmts) => stmts.flatMap(flatten)
    case other => Seq(other)
  }
  def mask(width: Int): BigInt = (BigInt(1) << width) - 1
  for (shape <- shapes; latency <- 0 to 1) {
    val input = chisel3.stage.ChiselStage.emitChirrtl(new PipeChannel(shape.gen(), latency))
    val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
    val writer = new java.io.PrintWriter(new java.io.File(destination, s"${shape.name}-L$latency.sfc.fir"))
    try writer.write(low.circuit.serialize) finally writer.close()
    val modules = low.circuit.modules.collect { case m: firrtl.ir.Module => m.name -> m }.toMap
    var nodes = Map.empty[String, (String, Expression)]
    var drivers = Map.empty[String, (String, Expression)]
    var regs = Seq.empty[(String, String, DefRegister)]
    def visit(name: String, prefix: String): Unit = {
      val body = flatten(modules(name).body)
      body.foreach {
        case n: DefNode => nodes += (prefix + n.name) -> (prefix -> n.value)
        case c: Connect => drivers += (prefix + c.loc.serialize) -> (prefix -> c.expr)
        case r: DefRegister => regs :+= ((prefix + r.name, prefix, r))
        case i: DefInstance => visit(i.module, prefix + i.name + ".")
        case _ => ()
      }
    }
    visit(low.circuit.main, "")
    var state = regs.map { case (name, _, _) => name -> BigInt(0) }.toMap
    var inputs = Map.empty[String, BigInt]
    var memo = Map.empty[String, BigInt]
    def get(name: String): BigInt = memo.getOrElse(name, {
      val value = state.getOrElse(name, inputs.getOrElse(name, {
        val (prefix, expr) = nodes.getOrElse(name, drivers.getOrElse(name, sys.error(s"missing $name")))
        eval(expr, prefix)
      }))
      memo += name -> value
      value
    })
    // Queue primitives only inspect bit patterns; signed leaves retain their
    // two's-complement bits while SFC owns the actual typed registers/muxes.
    def eval(e: Expression, prefix: String): BigInt = {
      val value: BigInt = e match {
        case UIntLiteral(v, _) => v
        case SIntLiteral(v, _) => v
        case FMux(s, h, l, _) => eval(if (eval(s, prefix) != 0) h else l, prefix)
        case DoPrim(op, a, c, _) => op match {
          case PrimOps.And => eval(a(0), prefix) & eval(a(1), prefix)
          case PrimOps.Or => eval(a(0), prefix) | eval(a(1), prefix)
          case PrimOps.Xor => eval(a(0), prefix) ^ eval(a(1), prefix)
          case PrimOps.Not => ~eval(a(0), prefix)
          case PrimOps.Eq => if (eval(a(0), prefix) == eval(a(1), prefix)) BigInt(1) else BigInt(0)
          case PrimOps.Add => eval(a(0), prefix) + eval(a(1), prefix)
          case PrimOps.Cat => (eval(a(0), prefix) << a(1).tpe.asInstanceOf[UIntType].width.asInstanceOf[IntWidth].width.toInt) | eval(a(1), prefix)
          case PrimOps.Bits => (eval(a(0), prefix) >> c(1).toInt) & mask((c(0) - c(1) + 1).toInt)
          case PrimOps.AsUInt | PrimOps.AsSInt | PrimOps.Pad => eval(a(0), prefix)
          case other => sys.error(s"unsupported oracle primitive $other")
        }
        case other => get(prefix + other.serialize)
      }
      e.tpe match {
        case UIntType(IntWidth(w)) => value & mask(w.toInt)
        case SIntType(IntWidth(w)) => value & mask(w.toInt)
        case _ => value
      }
    }
    for (cycle <- 0 until 512) {
      val reset = cycle < 3 || cycle % 97 < 2
      val inputValid = cycle % 7 != 0
      val outputReady = cycle % 11 >= 4
      val payload = ((BigInt(cycle) * 0x143 + 0x1234) ^
        (if (cycle % 2 != 0) BigInt("a5a50000", 16) else BigInt(0))) & mask(shape.width)
      inputs = Map("clock" -> BigInt(0), "reset" -> BigInt(if (reset) 1 else 0),
                   "io_in_valid" -> BigInt(if (inputValid) 1 else 0),
                   "io_out_ready" -> BigInt(if (outputReady) 1 else 0)) ++
        shape.leaves.filter(_.width > 0).map(leaf =>
          ("io_in_bits" + leaf.name) -> ((payload >> leaf.offset) & mask(leaf.width)))
      memo = Map.empty
      val ready = get("io_in_ready")
      val valid = get("io_out_valid")
      val bits = if (valid == 0) BigInt(0) else shape.leaves.filter(_.width > 0).map(leaf =>
        (get("io_out_bits" + leaf.name) & mask(leaf.width)) << leaf.offset).foldLeft(BigInt(0))(_ | _)
      println(s"TRACE ${shape.name} $latency $cycle $ready $valid $bits")
      state = regs.map { case (name, prefix, reg) =>
        name -> (if (eval(reg.reset, prefix) != 0) eval(reg.init, prefix)
                 else eval(drivers(name)._2, drivers(name)._1))
      }.toMap
    }
  }
}
