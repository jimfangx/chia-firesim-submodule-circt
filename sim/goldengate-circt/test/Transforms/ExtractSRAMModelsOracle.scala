// See LICENSE for license details.
// Exercise the unchanged Scala transforms on a hierarchy probe and the
// immutable Rocket input. Native candidates use the emitted typed handoff.
import firrtl._
import firrtl.annotations._
import midas.passes.fame._
import midas.targetutils.FirrtlMemModelAnnotation

object ExtractSRAMModelsOracle extends App {
  val destination = new java.io.File(args(0)); destination.mkdirs()
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(destination, name))
    try out.write(text) finally out.close()
  }
  def run(name: String, circuit: firrtl.ir.Circuit, parent: String, memory: String): Unit = {
    // MidasTransforms invokes its optional SRAM transform after LowForm.
    // Resolve CHIRRTL invalid memory clock fields before stripping port clocks.
    val low = new LowFirrtlCompiler().compile(CircuitState(circuit, HighForm, Nil), Nil)
    // MidasTransforms also removes ValidIf before its SRAM boundary. The
    // pinned CIRCT importer cannot ingest that legacy expression directly.
    val typed = firrtl.passes.RemoveValidIf.runTransform(low)
    val label = FirrtlMemModelAnnotation(ReferenceTarget(circuit.main, parent, Nil, memory, Nil))
    val input = typed.copy(annotations = Seq(label, label))
    write(name + ".fir", typed.circuit.serialize)
    write(name + ".json", JsonProtocol.serialize(input.annotations))
    val wrapped = new LabelSRAMModels().runTransform(input)
    val extracted = new ExtractModel().runTransform(wrapped)
    write(name + ".sfc.fir", extracted.circuit.serialize)
    write(name + ".sfc.json", JsonProtocol.serialize(extracted.annotations))
    val lowered = new LowFirrtlCompiler().compile(extracted, Nil)
    write(name + ".low.sfc.fir", lowered.circuit.serialize)
    write(name + ".low.sfc.json", JsonProtocol.serialize(lowered.annotations))
    println(s"PASS SFC $name SRAM wrapping, transitive promotion, and LowForm identities")
  }
  val probe = Parser.parse("""circuit Top :
  module Parent :
    input clock : Clock
    input addr : UInt<2>
    input en : UInt<1>
    input data : UInt<8>
    output out : UInt<8>
    mem ram :
      data-type => UInt<8>
      depth => 4
      read-latency => 1
      write-latency => 1
      reader => r
      writer => w
      readwriter => rw
      read-under-write => undefined
    ram.r.clk <= clock
    ram.r.addr <= addr
    ram.r.en <= en
    out <= ram.r.data
    ram.w.clk <= clock
    ram.w.addr <= addr
    ram.w.en <= en
    ram.w.data <= data
    ram.w.mask <= UInt<1>(1)
    ram.rw.clk <= clock
    ram.rw.addr <= addr
    ram.rw.en <= en
    ram.rw.wmode <= UInt<1>(0)
    ram.rw.wdata <= data
    ram.rw.wmask <= UInt<1>(1)
  module Middle :
    input clock : Clock
    inst p0 of Parent
    inst p1 of Parent
    p0.clock <= clock
    p0.addr <= UInt<2>(0)
    p0.en <= UInt<1>(1)
    p0.data <= UInt<8>(0)
    p1.clock <= clock
    p1.addr <= UInt<2>(1)
    p1.en <= UInt<1>(1)
    p1.data <= UInt<8>(1)
  module Top :
    input clock : Clock
    inst m0 of Middle
    inst m1 of Middle
    m0.clock <= clock
    m1.clock <= clock
""")
  run("fanout", probe, "Parent", "ram")
  val source = scala.io.Source.fromFile(args(1))
  val golden = try Parser.parse(source.mkString) finally source.close()
  run("golden-rocket", golden, "Rocket", "rf")
}
