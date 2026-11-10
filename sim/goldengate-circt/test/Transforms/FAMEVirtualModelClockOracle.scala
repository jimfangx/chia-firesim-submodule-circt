// See LICENSE for license details.
// Execute the preserved SFC transform on a real two-clock hub and two instances
// of one virtual-clock model. These are supporting cases, not feature acceptance.
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
object FAMEVirtualModelClockOracle extends App {
  val input = """circuit Top :
  module Hub :
    input c0 : Clock
    input c1 : Clock
    input data : UInt<8>
    output out : UInt<8>
    output alias0 : Clock
    output alias1 : Clock
    out <= data
    alias0 <= c0
    alias1 <= c1
  module Worker :
    input clock : Clock
    input data : UInt<8>
    output out : UInt<8>
    output alias : Clock
    reg state : UInt<8>, clock with :
      reset => (bits(data, 7, 7), UInt<8>(0))
    state <= bits(add(state, bits(data, 6, 0)), 7, 0)
    out <= bits(add(state, bits(data, 6, 0)), 7, 0)
    alias <= clock
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input h_c0 : Clock
    input h_c1 : Clock
    input h_data : UInt<8>
    output h_out : UInt<8>
    output h_alias0 : Clock
    output h_alias1 : Clock
    input w0_clock : Clock
    input w0_data : UInt<8>
    output w0_out : UInt<8>
    output w0_alias : Clock
    input w1_clock : Clock
    input w1_data : UInt<8>
    output w1_out : UInt<8>
    output w1_alias : Clock
    inst h of Hub
    inst w0 of Worker
    inst w1 of Worker
    h.c0 <= h_c0
    h.c1 <= h_c1
    h.data <= h_data
    h_out <= h.out
    h_alias0 <= h.alias0
    h_alias1 <= h.alias1
    w0.clock <= w0_clock
    w0.data <= w0_data
    w0_out <= w0.out
    w0_alias <= w0.alias
    w1.clock <= w1_clock
    w1.data <= w1_data
    w1_out <= w1.out
    w1_alias <= w1.alias
"""
  val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
  val top = ModuleTarget("Top", "Top")
  val hub = ModuleTarget("Top", "Hub")
  val worker = ModuleTarget("Top", "Worker")
  def flatten(stmt: Statement): Seq[Statement] = stmt match {
    case Block(stmts) => stmts.flatMap(flatten)
    case other => Seq(other)
  }
  // Read only expressions emitted by FAMETransform, not a separate behavioral
  // model of its handshake rules. Each interpreter owns one instance's state.
  class Interpreter(module: Module) {
    val body = flatten(module.body)
    val nodes = body.collect { case n: DefNode => n.name -> n.value }.toMap
    val drivers = body.collect { case c: Connect => c.loc.serialize -> c.expr }.toMap
    val registers = body.collect { case r: DefRegister => r }
    val gates = body.collect { case i: WDefInstance if i.module == "AbstractClockGate" => i.name }
    require(gates.size == 1)
    val gate = gates.head
    var inputs = Map.empty[String, BigInt]
    var state = registers.map(r => r.name -> (if (r.name == "state") BigInt(37) else BigInt(0))).toMap
    def ref(name: String): BigInt = inputs.getOrElse(name, state.getOrElse(name,
      eval(nodes.getOrElse(name, drivers.getOrElse(name, sys.error(s"missing $name"))))))
    def eval(expr: Expression): BigInt = {
      val result: BigInt = expr match {
        case UIntLiteral(value, _) => value
        case r: WRef => ref(r.name)
        case field: WSubField => ref(field.serialize)
        case Mux(sel, high, low, _) => eval(if (eval(sel) != 0) high else low)
        case ValidIf(_, value, _) => eval(value)
        case DoPrim(op, args, constants, _) => op match {
          case PrimOps.And => eval(args(0)) & eval(args(1))
          case PrimOps.Or => eval(args(0)) | eval(args(1))
          case PrimOps.Not => ~eval(args(0))
          case PrimOps.Add => eval(args(0)) + eval(args(1))
          case PrimOps.Bits => (eval(args(0)) >> constants(1).toInt) & ((BigInt(1) << (constants(0)-constants(1)+1).toInt)-1)
          case PrimOps.Tail | PrimOps.Pad | PrimOps.AsUInt => eval(args(0))
          case _ => sys.error(s"unsupported emitted primitive $op")
        }
        case _ => sys.error(s"unsupported emitted expression $expr")
      }
      expr.tpe match {
        case UIntType(IntWidth(width)) => result & ((BigInt(1) << width.toInt)-1)
        case _ => result
      }
    }
    def fired(channel: String): BigInt = ref(registers.find(_.name.startsWith(channel + "_fired")).get.name)
    def enabled: BigInt = ref(registers.find(_.name.startsWith("clock_enabled")).get.name)
    def resetHost(): Unit = {
      state = state ++ registers.filter(_.clock.serialize == "hostClock").map(r => r.name -> eval(r.init))
    }
    def step(): Unit = {
      state = registers.map { r =>
        val advance = r.clock.serialize == "hostClock" || ref(gate + ".CE") != 0
        val value = if (!advance) state(r.name) else if (eval(r.reset) != 0) eval(r.init) else eval(drivers(r.name))
        r.name -> value
      }.toMap
    }
  }

  for (associated <- Seq(false, true)) {
    def pipe(name: String, port: String, clock: String, input: Boolean) =
      FAMEChannelConnectionAnnotation(name, PipeChannel(0), Some(top.ref(clock)),
        if (input) None else Some(Seq(top.ref(port))), if (input) Some(Seq(top.ref(port))) else None)
    val annotations = Seq(
      FAMEHostClock(top.ref("hostClock")), FAMEHostReset(top.ref("hostReset")),
      FAMETransformAnnotation(hub), FAMETransformAnnotation(worker),
      FAMEChannelPortsAnnotation("tick", None, Seq(hub.ref("c0"), hub.ref("c1"))),
      FAMEChannelConnectionAnnotation("tick", TargetClockChannel(
        Seq(RationalClock("d0", 1, 2), RationalClock("d1", 2, 3)), Seq(2, 3)),
        None, None, Some(Seq(top.ref("h_c0"), top.ref("h_c1")))),
      FAMEChannelPortsAnnotation("input", Some(hub.ref("alias0")), Seq(hub.ref("data"))),
      FAMEChannelPortsAnnotation("output", Some(hub.ref("alias1")), Seq(hub.ref("out"))),
      FAMEChannelPortsAnnotation("input", if (associated) Some(worker.ref("alias")) else None, Seq(worker.ref("data"))),
      FAMEChannelPortsAnnotation("output", if (associated) Some(worker.ref("alias")) else None, Seq(worker.ref("out"))),
      pipe("h_input", "h_data", "h_alias0", true), pipe("h_output", "h_out", "h_alias1", false)
    ) ++ Seq("w0", "w1").flatMap { instance =>
      Seq(pipe(s"${instance}_input", s"${instance}_data", if (associated) s"${instance}_alias" else "h_alias0", true),
          pipe(s"${instance}_output", s"${instance}_out", if (associated) s"${instance}_alias" else "h_alias1", false))
    }
    val result = new FAMETransform().execute(low.copy(annotations = annotations))
    val transformed = result.circuit.modules.collectFirst { case m: Module if m.name == "Worker" => m }.get
    val registers = flatten(transformed.body).collect { case r: DefRegister => r }
    for ((name, reset) <- Seq("input" -> (if (associated) 1 else 0), "output" -> 0)) {
      val fired = registers.filter(_.name.startsWith(name + "_fired"))
      require(fired.size == 1, s"repeated virtual model has duplicate $name FSMs")
      require(fired.head.init == UIntLiteral(reset), s"incorrect $name fired reset ${fired.head.init}")
      println(s"VIRTUAL ${if (associated) "associated" else "unassociated"} $name ${if (associated) "clock" else "none"} $reset")
    }
    require(transformed.ports.exists(_.name == "input_sink") && transformed.ports.exists(_.name == "output_source"))
    val wrapper = result.circuit.modules.collectFirst { case m: Module if m.name == "Top" => m }.get
    val instances = flatten(wrapper.body).collect { case i: WDefInstance if i.module == "Worker" => i.name }
    require(instances.toSet == Set("w0_", "w1_"), s"lost repeated virtual instances: $instances")
    val workers = Array(new Interpreter(transformed), new Interpreter(transformed))
    val tokenIndex = Array(0, 0)
    for (cycle <- 0 until 96; index <- 0 until 2) {
      val sim = workers(index)
      val reset = cycle < 2 || cycle == 40 || cycle == 41
      val valid = (cycle + 3 * index) % 7 != 3
      val ready = (cycle + 2 * index) % 5 != 2
      val n = tokenIndex(index)
      val data = if (n == 0 || n % 11 == 7) 128 else (n * 17 + index * 9) % 127
      sim.inputs = Map("hostReset" -> BigInt(if (reset) 1 else 0),
        "input_sink.valid" -> BigInt(if (valid) 1 else 0), "input_sink.bits" -> BigInt(data),
        "output_source.ready" -> BigInt(if (ready) 1 else 0))
      if (reset) sim.resetHost()
      val inputReady = sim.ref("input_sink.ready")
      println(s"VSTATE ${if (associated) 1 else 0} $cycle $index $inputReady ${sim.ref("output_source.valid")} ${sim.ref("output_source.bits")} ${sim.ref("targetCycleFinishing")} ${sim.ref(sim.gate + ".CE")} ${sim.enabled} ${sim.fired("input")} ${sim.fired("output")} ${sim.state("state")}")
      sim.step()
      if (valid && inputReady != 0) tokenIndex(index) += 1
    }

  }
}
