// See LICENSE for license details.
// Supporting compiler case: execute the unchanged SFC FAMETransform for one
// two-domain hub and two instances of a virtual-clock stateful definition.
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
object FAMEModelControlsOracle extends App {
  val input = """circuit Top :
  module Hub :
    input c0 : Clock
    input c1 : Clock
    input data : UInt<8>
    input right : UInt<8>
    output out : UInt<8>
    output otherOut : UInt<8>
    output alias0 : Clock
    output alias1 : Clock
    reg state0 : UInt<8>, c0 with :
      reset => (bits(data, 7, 7), UInt<8>(0))
    reg state1 : UInt<8>, c1 with :
      reset => (bits(right, 7, 7), UInt<8>(0))
    state0 <= bits(add(state0, bits(data, 6, 0)), 7, 0)
    state1 <= bits(add(state1, bits(right, 6, 0)), 7, 0)
    out <= bits(add(state0, bits(data, 6, 0)), 7, 0)
    otherOut <= bits(add(state1, bits(right, 6, 0)), 7, 0)
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
    input h_right : UInt<8>
    output h_out : UInt<8>
    output h_other : UInt<8>
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
    h.right <= h_right
    h_out <= h.out
    h_other <= h.otherOut
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
  def flatten(stmt: Statement): Seq[Statement] = stmt match {
    case Block(stmts) => stmts.flatMap(flatten)
    case other => Seq(other)
  }
  // Evaluate only expressions and register transitions emitted by SFC. The
  // host schedule supplies rational clock tokens and independent peer stalls.
  class Interpreter(module: Module) {
    val body = flatten(module.body)
    val nodes = body.collect { case n: DefNode => n.name -> n.value }.toMap
    val drivers = body.collect { case c: Connect => c.loc.serialize -> c.expr }.toMap
    val registers = body.collect { case r: DefRegister => r }
    val gates = body.collect { case i: WDefInstance if i.module == "AbstractClockGate" => i.name }
    var inputs = Map.empty[String, BigInt]
    var state = registers.map(r => r.name -> (if (r.name.startsWith("state")) BigInt(37) else BigInt(0))).toMap
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
    def enabled(clock: String): BigInt = ref(registers.find(_.name.startsWith(clock + "_enabled")).get.name)
    def gate(clock: String): BigInt = ref(gates.find(_.startsWith(clock + "_buffer")).get + ".CE")
    def resetHost(): Unit = { state = state ++ registers.filter(_.clock.serialize == "hostClock").map(r => r.name -> eval(r.init)) }
    def step(): Unit = {
      state = registers.map { r =>
        val advance = r.clock.serialize == "hostClock" || gates.find(g => r.clock.serialize == g + ".O").exists(g => ref(g + ".CE") != 0)
        val value = if (!advance) state(r.name) else if (eval(r.reset) != 0) eval(r.init) else eval(drivers(r.name))
        r.name -> value
      }.toMap
    }
  }
  val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
  val top = ModuleTarget("Top", "Top"); val hub = ModuleTarget("Top", "Hub"); val worker = ModuleTarget("Top", "Worker")
  def payload(n: Int, stream: Int): Int = if (n == 0 || n % 11 == 7) 128 else (n * 17 + stream * 9) % 127
  for (associated <- Seq(false, true)) {
    def pipe(name: String, p: String, clock: String, input: Boolean) =
      FAMEChannelConnectionAnnotation(name, PipeChannel(0), Some(top.ref(clock)),
        if (input) None else Some(Seq(top.ref(p))), if (input) Some(Seq(top.ref(p))) else None)
    val annotations = Seq(
      FAMEHostClock(top.ref("hostClock")), FAMEHostReset(top.ref("hostReset")),
      FAMETransformAnnotation(hub), FAMETransformAnnotation(worker),
      FAMEChannelPortsAnnotation("tick", None, Seq(hub.ref("c0"), hub.ref("c1"))),
      FAMEChannelConnectionAnnotation("tick", TargetClockChannel(
        Seq(RationalClock("d0",1,2), RationalClock("d1",1,3)), Seq(2,3)), None, None, Some(Seq(top.ref("h_c0"),top.ref("h_c1")))),
      FAMEChannelPortsAnnotation("input", Some(hub.ref("alias0")), Seq(hub.ref("data"))),
      FAMEChannelPortsAnnotation("right", Some(hub.ref("alias1")), Seq(hub.ref("right"))),
      FAMEChannelPortsAnnotation("output", Some(hub.ref("alias0")), Seq(hub.ref("out"))),
      FAMEChannelPortsAnnotation("other", Some(hub.ref("alias1")), Seq(hub.ref("otherOut"))),
      FAMEChannelPortsAnnotation("input", if (associated) Some(worker.ref("alias")) else None, Seq(worker.ref("data"))),
      FAMEChannelPortsAnnotation("output", if (associated) Some(worker.ref("alias")) else None, Seq(worker.ref("out"))),
      pipe("h_input","h_data","h_alias0",true),pipe("h_right","h_right","h_alias1",true),
      pipe("h_output","h_out","h_alias0",false),pipe("h_other","h_other","h_alias1",false)
    ) ++ Seq("w0","w1").flatMap { instance => Seq(
      pipe(instance+"_input",instance+"_data",if(associated) instance+"_alias" else "h_alias0",true),
      pipe(instance+"_output",instance+"_out",if(associated) instance+"_alias" else "h_alias1",false)) }
    val transformed = new FAMETransform().execute(low.copy(annotations=annotations))
    def find(name: String) = transformed.circuit.modules.collectFirst { case m: Module if m.name==name => m }.get
    val h = new Interpreter(find("Hub")); val workers = Array(new Interpreter(find("Worker")),new Interpreter(find("Worker")))
    val tokens = Array(0,0,0,0); var clockIndex=0
    for(cycle <- 0 until 96) {
      val reset=cycle<2 || cycle==40 || cycle==41
      if(reset) clockIndex=0
      val clockValid=cycle%9 !=5
      h.inputs=Map("hostReset"->BigInt(if(reset)1 else 0),"tick_sink.valid"->BigInt(if(clockValid)1 else 0),
        "tick_sink.bits.c0"->BigInt(if(clockIndex%4 !=2)1 else 0),
        "tick_sink.bits.c1"->BigInt(if(clockIndex%4==0 || clockIndex%4==2)1 else 0))
      for(stream <- 0 until 2) {
        val in=if(stream==0) "input_sink" else "right_sink"; val out=if(stream==0) "output_source" else "other_source"
        h.inputs ++= Map(in+".valid"->BigInt(if((cycle+3*stream)%7 !=3)1 else 0),
          in+".bits"->BigInt(payload(tokens(stream),stream)),out+".ready"->BigInt(if((cycle+2*stream)%5 !=2)1 else 0))
      }
      if(reset) h.resetHost()
      val leftReady=h.ref("input_sink.ready"); val rightReady=h.ref("right_sink.ready"); val clockReady=h.ref("tick_sink.ready")
      val hValues=Seq(leftReady,rightReady,h.ref("output_source.valid"),h.ref("other_source.valid"),
        h.ref("output_source.bits"),h.ref("other_source.bits"),h.ref("targetCycleFinishing"),clockReady,h.gate("c0"),h.gate("c1"),
        h.enabled("c0"),h.enabled("c1"),h.fired("input"),h.fired("right"),h.fired("output"),h.fired("other"),h.state("state0"),h.state("state1"))
      println(s"MSTATE ${if(associated)1 else 0} $cycle Hub 0 ${hValues.mkString(" ")}")
      h.step()
      if(h.inputs("input_sink.valid")!=0 && leftReady!=0)tokens(0)+=1
      if(h.inputs("right_sink.valid")!=0 && rightReady!=0)tokens(1)+=1
      if(clockValid && clockReady!=0)clockIndex+=1
      for(index <- 0 until 2) {
        val w=workers(index); val stream=index+2; val valid=(cycle+3*stream)%7 !=3; val ready=(cycle+2*stream)%5 !=2
        w.inputs=Map("hostReset"->BigInt(if(reset)1 else 0),"input_sink.valid"->BigInt(if(valid)1 else 0),
          "input_sink.bits"->BigInt(payload(tokens(stream),stream)),"output_source.ready"->BigInt(if(ready)1 else 0))
        if(reset)w.resetHost();val inputReady=w.ref("input_sink.ready")
        val values=Seq(inputReady,w.ref("output_source.valid"),w.ref("output_source.bits"),w.ref("targetCycleFinishing"),
          w.gate("clock"),w.enabled("clock"),w.fired("input"),w.fired("output"),w.state("state"))
        println(s"MSTATE ${if(associated)1 else 0} $cycle Worker $index ${values.mkString(" ")}")
        w.step();if(valid && inputReady!=0)tokens(stream)+=1
      }
    }
  }
}
