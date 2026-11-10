// See LICENSE for license details.
// Parse independently emitted SFC and native FIRRTL; neither candidate is
// transformed here. This bounded semantic check is not a formal proof.
import firrtl._
import firrtl.ir._
object FAMEModelGraphCompare extends App {
  require(args.length==2,"usage: GraphCompare sfc-fame.fir native-fame.fir")
  def read(path:String)=Parser.parse(scala.io.Source.fromFile(path).mkString)
  val circuits=args.map(read)
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
          case PrimOps.Not => if (eval(args(0)) == 0) BigInt(1) else BigInt(0)
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

  def module(c:Circuit,n:String)=c.modules.collectFirst{case m:Module if m.name==n=>m}.get
  def norm(s:String)=s.replaceAll("_fired_[0-9]+","_fired")
  def interface(m:Module)=m.ports.map(p=>(p.name,p.direction.serialize,p.tpe.serialize)).sortBy(_._1)
  def signature(m:Module):Map[String,String]={
    val body=flatten(m.body)
    val nodes=body.collect{case n:DefNode=>n.name->n.value}.toMap
    def canonical(e:Expression):String=e match {
      case r:WRef if nodes.contains(r.name)=>canonical(nodes(r.name))
      case r:WRef=>norm(r.name)
      case f:WSubField=>norm(f.serialize)
      case UIntLiteral(v,_)=>v.toString
      case DoPrim(PrimOps.AsUInt,args,_,_)=>canonical(args.head)
      case DoPrim(op,args,cs,_)=>s"$op(${args.map(canonical).mkString(",")};${cs.mkString(",")})"
      case Mux(s,h,l,_)=>s"mux(${canonical(s)},${canonical(h)},${canonical(l)})"
      case _=>norm(e.serialize)
    }
    body.collect{case r:DefRegister if r.name.startsWith("state")=>
      norm(r.name)->Seq(r.tpe.serialize,canonical(r.clock),canonical(r.reset),canonical(r.init)).mkString("|")}.toMap
  }
  for(name<-Seq("Hub","Worker")){
    val a=module(circuits(0),name);val b=module(circuits(1),name)
    require(interface(a)==interface(b),s"$name complete channel interface mismatch ${interface(a)} vs ${interface(b)}")
    require(signature(a)==signature(b),s"$name target register clock/reset/init mismatch ${signature(a)} vs ${signature(b)}")
  }
  var comparisons=0
  val hs=circuits.map(c=>new Interpreter(module(c,"Hub")))
  val ws=circuits.map(c=>Array(new Interpreter(module(c,"Worker")),new Interpreter(module(c,"Worker"))))
  val tokens=Array(0,0,0,0);var clockIndex=0
  def bit(b:Boolean)=BigInt(if(b)1 else 0)
  def payload(n:Int,stream:Int)=BigInt(if(n==0||n%11==7)128 else(n*17+stream*9)%127)
  def same(values:Array[Seq[BigInt]],where:String):Unit={require(values(0)==values(1),s"emitted behavior mismatch at $where: ${values.toSeq}");comparisons+=1}
  for(cycle<-0 until 192){
    val reset=cycle<2||cycle==40||cycle==41||cycle==111
    if(reset)clockIndex=0
    val clockValid=cycle%9!=5
    for(h<-hs){
      h.inputs=Map("hostReset"->bit(reset),"tick_sink.valid"->bit(clockValid),
        "tick_sink.bits.c0"->bit(clockIndex%4!=2),"tick_sink.bits.c1"->bit(clockIndex%4==0||clockIndex%4==2))
      for(stream<-0 until 2){
        val in=if(stream==0)"input_sink"else"right_sink";val out=if(stream==0)"output_source"else"other_source"
        h.inputs++=Map(in+".valid"->bit((cycle+3*stream)%7!=3),in+".bits"->payload(tokens(stream),stream),out+".ready"->bit((cycle+2*stream)%5!=2))
      }
      if(reset)h.resetHost()
    }
    same(hs.map(h=>Seq(h.ref("input_sink.ready"),h.ref("right_sink.ready"),h.ref("output_source.valid"),h.ref("other_source.valid"),
      h.ref("output_source.bits"),h.ref("other_source.bits"),h.ref("targetCycleFinishing"),h.ref("tick_sink.ready"),h.gate("c0"),h.gate("c1"),
      h.enabled("c0"),h.enabled("c1"),h.fired("input"),h.fired("right"),h.fired("output"),h.fired("other"),h.state("state0"),h.state("state1"))),s"Hub cycle$cycle")
    val ready=Seq(hs(0).ref("input_sink.ready"),hs(0).ref("right_sink.ready"));val clockReady=hs(0).ref("tick_sink.ready")
    hs.foreach(_.step())
    for(stream<-0 until 2)if((cycle+3*stream)%7!=3&&ready(stream)!=0)tokens(stream)+=1
    if(clockValid&&clockReady!=0)clockIndex+=1
    for(index<-0 until 2){
      val stream=index+2;val valid=(cycle+3*stream)%7!=3;val readyOut=(cycle+2*stream)%5!=2
      val pair=ws.map(_(index))
      for(w<-pair){w.inputs=Map("hostReset"->bit(reset),"input_sink.valid"->bit(valid),"input_sink.bits"->payload(tokens(stream),stream),"output_source.ready"->bit(readyOut));if(reset)w.resetHost()}
      same(pair.map(w=>Seq(w.ref("input_sink.ready"),w.ref("output_source.valid"),w.ref("output_source.bits"),w.ref("targetCycleFinishing"),w.gate("clock"),w.enabled("clock"),w.fired("input"),w.fired("output"),w.state("state"))),s"Worker$index cycle$cycle")
      val inputReady=pair(0).ref("input_sink.ready");pair.foreach(_.step());if(valid&&inputReady!=0)tokens(stream)+=1
    }
  }
  println(s"PASS $comparisons matched emitted-model rows: full channel interfaces, target state clock/reset/init, independent stalls, virtual-clock state and reset schedules; graph transport checked separately")
}
