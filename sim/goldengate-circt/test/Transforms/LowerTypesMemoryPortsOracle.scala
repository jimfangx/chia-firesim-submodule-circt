// See LICENSE for license details.
// Exercise the actual LabelSRAMModels -> LowFirrtlCompiler annotation boundary.
import firrtl._
import firrtl.annotations._
import midas.passes.fame._
import midas.targetutils.FirrtlMemModelAnnotation

object LowerTypesMemoryPortsOracle extends App {
  val destination = new java.io.File(args(0)); destination.mkdirs()
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(destination, name))
    try out.write(text) finally out.close()
  }
  for ((name, dataType) <- Seq("scalar" -> "UInt<8>", "zero" -> "UInt<0>",
                             "singleton" -> "{only : UInt<8>}",
                             "singleton-zero" -> "{pad : UInt<0>, only : UInt<8>}",
                             "singleton-vector" -> "UInt<8>[1]",
                             "aggregate" -> "{a : UInt<8>, b : UInt<4>}")) {
    val circuit = Parser.parse(s"""circuit Top :
      module Top :
        input clock : Clock
        mem ram :
          data-type => $dataType
          depth => 4
          read-latency => 1
          write-latency => 1
          reader => r
          writer => w
          readwriter => rw
          read-under-write => undefined
        ram.r.clk <= clock
        ram.r.en <= UInt<1>(0)
        ram.r.addr <= UInt<2>(0)
        ram.w.clk <= clock
        ram.w.en <= UInt<1>(0)
        ram.w.addr <= UInt<2>(0)
        ram.w.data is invalid
        ram.w.mask is invalid
        ram.rw.clk <= clock
        ram.rw.en <= UInt<1>(0)
        ram.rw.addr <= UInt<2>(0)
        ram.rw.wmode <= UInt<1>(0)
        ram.rw.wdata is invalid
        ram.rw.wmask is invalid
    """)
    val typed = new HighFirrtlCompiler().compile(CircuitState(circuit, HighForm, Nil), Nil)
    val labeled = new LabelSRAMModels().execute(CircuitState(typed.circuit, HighForm,
      Seq(FirrtlMemModelAnnotation(ReferenceTarget("Top", "Top", Nil, "ram", Nil)))))
    val ports = labeled.annotations.collect { case a: MemPortAnnotation => a }
    require(ports.size == 3)
    write(name+".fir", labeled.circuit.serialize)
    write(name+".json", JsonProtocol.serialize(ports))
    if (name == "scalar" || name.startsWith("singleton")) {
      val lowered = new LowFirrtlCompiler().compile(labeled.copy(annotations = ports), Nil)
      val retained = lowered.annotations.collect { case a: MemPortAnnotation => a }
      require(retained.size == 3 && retained.flatMap(_.getTargets).size == 13)
      require(retained.flatMap(_.getTargets).forall(t => !t.serialize.split(">").last.contains(".")))
      write(name+".sfc.fir", lowered.circuit.serialize)
      write(name+".sfc.json", JsonProtocol.serialize(retained))
      println(s"PASS SFC preserves all 13 $name SRAM port members through LowerTypes")
    } else {
      for (port <- ports) {
        var rejected = false
        try new LowFirrtlCompiler().compile(labeled.copy(annotations = Seq(port)), Nil)
        catch { case e: AssertionError if e.getMessage.contains("renameMatches") => rejected = true }
        require(rejected, s"SFC accepted $name ${port.getClass.getSimpleName}")
        write(name+"-"+port.getClass.getSimpleName+".json", JsonProtocol.serialize(Seq(port)))
        println(s"PASS SFC rejects $name ${port.getClass.getSimpleName}")
      }
    }
  }
}
