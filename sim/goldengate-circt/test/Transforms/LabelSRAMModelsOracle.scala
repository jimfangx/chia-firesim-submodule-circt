// See LICENSE for license details.
// Run the unchanged SFC LabelSRAMModels on typed inputs, before LowForm.
import firrtl._
import firrtl.annotations._
import midas.passes.fame._
import midas.targetutils.FirrtlMemModelAnnotation

object LabelSRAMModelsOracle extends App {
  val destination = new java.io.File(args(0)); destination.mkdirs()
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(destination, name))
    try out.write(text) finally out.close()
  }
  for ((name, collision, twoClocks, reads, depth, width, latency) <- Seq(
      ("mixed", false, false, Seq("r"), 4, 8, 1),
      ("collision", true, true, Seq("r"), 4, 8, 1),
      ("rocket-rf", false, false, Seq("id_rs_MPORT", "id_rs_MPORT_1"), 31, 64, 0))) {
    val memName = if (name == "rocket-rf") "rf" else "ram"
    val writeName = if (name == "rocket-rf") "MPORT" else "w"
    val addrWidth = if (depth == 31) 5 else 2
    val readConnections = reads.map(r => s"""$memName.$r.clk <= clock
      $memName.$r.en <= UInt<1>(0)
      $memName.$r.addr <= UInt<$addrWidth>(0)""").mkString("\n")
    val rwDeclaration = if (name == "rocket-rf") "" else "readwriter => rw"
    val rwConnections = if (name == "rocket-rf") "" else s"""$memName.rw.clk <= clock
      $memName.rw.en <= UInt<1>(0)
      $memName.rw.addr <= UInt<$addrWidth>(0)
      $memName.rw.wmode <= UInt<1>(0)
      $memName.rw.wdata is invalid
      $memName.rw.wmask is invalid"""
    val properties = Seq(s"data-type => UInt<$width>", s"depth => $depth",
      s"read-latency => $latency", "write-latency => 1") ++
      reads.map(r => s"reader => $r") ++ Seq(s"writer => $writeName") ++
      (if (rwDeclaration.isEmpty) Nil else Seq(rwDeclaration)) ++ Seq("read-under-write => undefined")
    val connections = (readConnections + "\n" + s"""$memName.$writeName.clk <= ${if (twoClocks) "other" else "clock"}
      $memName.$writeName.en <= UInt<1>(0)
      $memName.$writeName.addr <= UInt<$addrWidth>(0)
      $memName.$writeName.data is invalid
      $memName.$writeName.mask is invalid
      $rwConnections""").linesIterator.map(_.trim).filter(_.nonEmpty).toSeq
    val lines = Seq("circuit Top :") ++
      (if (collision) Seq(s"  module $memName :", "    input unused : Clock") else Nil) ++
      Seq("  module Top :", "    input clock : Clock", "    input other : Clock", s"    mem $memName :") ++
      properties.map("      " + _) ++ connections.map("    " + _)
    val circuit = Parser.parse(lines.mkString("\n") + "\n")
    val typed = new HighFirrtlCompiler().compile(CircuitState(circuit, HighForm, Nil), Nil)
    val label = FirrtlMemModelAnnotation(ReferenceTarget("Top", "Top", Nil, memName, Nil))
    val input = typed.copy(annotations = Seq(label, label))
    write(name+".fir", typed.circuit.serialize)
    write(name+".json", JsonProtocol.serialize(input.annotations))
    val labeled = new LabelSRAMModels().execute(input)
    write(name+".sfc.fir", labeled.circuit.serialize)
    write(name+".sfc.json", JsonProtocol.serialize(labeled.annotations))
    require(labeled.circuit.modules.size == typed.circuit.modules.size + 1)
    require(!labeled.annotations.exists(_.isInstanceOf[FirrtlMemModelAnnotation]))
    println(s"PASS SFC $name one wrapper per duplicate SRAM identity")
  }
  if (args.length > 1) {
    val source = scala.io.Source.fromFile(args(1))
    val circuit = try Parser.parse(source.mkString) finally source.close()
    val typed = new HighFirrtlCompiler().compile(CircuitState(circuit, HighForm, Nil), Nil)
    val label = FirrtlMemModelAnnotation(ReferenceTarget(circuit.main, "Rocket", Nil, "rf", Nil))
    val labeled = new LabelSRAMModels().execute(typed.copy(annotations = Seq(label, label)))
    write("golden-rocket.sfc.fir", labeled.circuit.serialize)
    write("golden-rocket.sfc.json", JsonProtocol.serialize(labeled.annotations))
    println("PASS SFC optional extraction on the immutable Rocket FIRRTL input")
  }
}
