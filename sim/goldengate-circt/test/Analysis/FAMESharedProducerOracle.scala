// See LICENSE for license details.
// Exercise preserved Scala model-port deduplication and host source renames.
package midas.passes.fame

import firrtl._
import firrtl.annotations._

object FAMESharedProducerOracle extends App {
  val input = """circuit Top :
  module Model :
    input clock0 : Clock
    input clock1 : Clock
    input data : UInt<8>
    output printfB : UInt<8>
    output other : UInt<8>
    output alias0 : Clock
    output alias1 : Clock
    printfB <= data
    other <= data
    alias0 <= clock0
    alias1 <= clock1
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input clock0 : Clock
    input clock1 : Clock
    input data : UInt<8>
    output printfB : UInt<8>
    output other : UInt<8>
    output alias0 : Clock
    output alias1 : Clock
    inst model of Model
    model.clock0 <= clock0
    model.clock1 <= clock1
    model.data <= data
    printfB <= model.printfB
    other <= model.other
    alias0 <= model.alias0
    alias1 <= model.alias1
"""
  val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
  def rt(ref: String) = ReferenceTarget("Top", "Top", Nil, ref, Nil)
  def source(name: String, ports: Seq[String], clock: String = "alias0", latency: Int = 0) =
    FAMEChannelConnectionAnnotation(name, PipeChannel(latency), Some(rt(clock)),
      Some(ports.map(rt)), None)
  def state(first: Seq[String], second: Seq[String], secondClock: String = "alias0",
      base: CircuitState = low) =
    base.copy(annotations = Seq(
      FAMEHostClock(rt("hostClock")), FAMEHostReset(rt("hostReset")),
      FAMETransformAnnotation(ModuleTarget("Top", "Model")),
      FAMEChannelConnectionAnnotation("input", PipeChannel(0), Some(rt("alias0")),
        None, Some(Seq(rt("data")))),
      source("arbitrary_print_second", first),
      source("second_claim_on_print_b", second, secondClock, 1)))
  val inferred = new InferModelPorts().execute(state(Seq("printfB"), Seq("printfB")))
  val analysis = new FAMEChannelAnalysis(inferred)
  val outputs = analysis.modelOutputChannelPortMap(ModuleTarget("Top", "Model"))
  require(outputs.size == 1 && outputs("printfB")._2.map(_.name) == Seq("printfB"))
  require(outputs("printfB")._1.map(_.name) == Some("alias0"))
  val transform = new FAMETransform
  val renames = transform.hostDecouplingRenames(analysis)
  for (branch <- Seq("arbitrary_print_second", "second_claim_on_print_b")) {
    require(analysis.chNameToModelSourcePortName(branch) == "printfB_source")
    require(transform.topSourcePortName(branch, analysis) == "model_printfB_source")
    println(s"BRANCH $branch printfB")
  }
  require(RTRenamer.exact(renames)(rt("printfB")) == rt("model_printfB_source").field("bits"))
  println("PRODUCER printfB branches 2")
  // An aggregate has one producer too, independently of representative name.
  val aggregate = new InferModelPorts().execute(state(Seq("printfB", "other"), Seq("printfB", "other")))
  val aggregateOutputs = new FAMEChannelAnalysis(aggregate)
    .modelOutputChannelPortMap(ModuleTarget("Top", "Model"))
  require(aggregateOutputs.size == 1 &&
    aggregateOutputs.values.head._2.map(_.name) == Seq("printfB", "other"))
  for (bad <- Seq(
      state(Seq("printfB"), Seq("printfB"), "alias1"),
      state(Seq("printfB", "other"), Seq("other", "printfB")),
      state(Seq("printfB", "other"), Seq("printfB")))) {
    var rejected = false
    try new InferModelPorts().execute(bad)
    catch { case e: RuntimeException if e.getMessage.contains("partially overlapping") => rejected = true }
    require(rejected, "Scala accepted incompatible shared producer")
  }
  // Distinct wrapper ports driven by the same scalar model producer must
  // share its local port and both move to the same host payload target.
  val aliasInput = input.replace("  module Top :", "  module Top :\n    output secondPrintf : UInt<8>") +
    "    secondPrintf <= model.printfB\n"
  val aliasLow = new LowFirrtlCompiler().compile(
    CircuitState(Parser.parse(aliasInput), ChirrtlForm), Nil)
  val aliasInferred = new InferModelPorts().execute(
    state(Seq("printfB"), Seq("secondPrintf"), base = aliasLow))
  val aliasAnalysis = new FAMEChannelAnalysis(aliasInferred)
  require(aliasAnalysis.modelOutputChannelPortMap(ModuleTarget("Top", "Model")).size == 1)
  val aliasRenames = transform.hostDecouplingRenames(aliasAnalysis)
  for (port <- Seq("printfB", "secondPrintf")) {
    require(RTRenamer.exact(aliasRenames)(rt(port)) ==
      rt("model_printfB_source").field("bits"))
    require(aliasAnalysis.staleTopPorts.contains(rt(port)))
  }
  println("PRODUCER printfB physical aliases 2 one token target")
  // Aggregate producer names are the representative global channel name in
  // SFC's ModulePortDeduper, not the common prefix of the model's leaf ports.
  // Keep model leaves independent of either branch prefix so both possible
  // representatives have the same host bundle field names.
  val multiAliasInput = """circuit Top :
  module Model :
    input clock0 : Clock
    input inData : UInt<8>
    output data : UInt<8>
    output valid : UInt<1>
    output alias0 : Clock
    data <= inData
    valid <= UInt<1>(1)
    alias0 <= clock0
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input clock0 : Clock
    input inData : UInt<8>
    output left_data : UInt<8>
    output left_valid : UInt<1>
    output right_data : UInt<8>
    output right_valid : UInt<1>
    output alias0 : Clock
    inst model of Model
    model.clock0 <= clock0
    model.inData <= inData
    left_data <= model.data
    left_valid <= model.valid
    right_data <= model.data
    right_valid <= model.valid
    alias0 <= model.alias0
"""
  val multiAliasLow = new LowFirrtlCompiler().compile(
    CircuitState(Parser.parse(multiAliasInput), ChirrtlForm), Nil)
  def multiAliasState(rightPorts: Seq[String]) = multiAliasLow.copy(annotations = Seq(
    FAMEHostClock(rt("hostClock")), FAMEHostReset(rt("hostReset")),
    FAMETransformAnnotation(ModuleTarget("Top", "Model")),
    FAMEChannelConnectionAnnotation("input", PipeChannel(0), Some(rt("alias0")),
      None, Some(Seq(rt("inData")))),
    source("left_", Seq("left_data", "left_valid")),
    source("right_", rightPorts, latency = 1)))
  val multiAliasInferred = new InferModelPorts().execute(
    multiAliasState(Seq("right_data", "right_valid")))
  val multiAliasAnalysis = new FAMEChannelAnalysis(multiAliasInferred)
  val multiOutputs = multiAliasAnalysis.modelOutputChannelPortMap(ModuleTarget("Top", "Model"))
  require(multiOutputs.size == 1 &&
    multiOutputs.values.head._2.map(_.name) == Seq("data", "valid"))
  val multiProducer = multiOutputs.keys.head
  require(Set("left_", "right_").contains(multiProducer))
  val multiAliasRenames = transform.hostDecouplingRenames(multiAliasAnalysis)
  for (branch <- Seq("left_", "right_")) {
    require(multiAliasAnalysis.chNameToModelSourcePortName(branch) == s"${multiProducer}_source")
    require(transform.topSourcePortName(branch, multiAliasAnalysis) == s"model_${multiProducer}_source")
    for (leaf <- Seq("data", "valid")) {
      require(RTRenamer.exact(multiAliasRenames)(rt(s"$branch$leaf")) ==
        rt(s"model_${multiProducer}_source").field("bits").field(leaf))
      require(multiAliasAnalysis.staleTopPorts.contains(rt(s"$branch$leaf")))
      require(RTRenamer.exact(multiAliasRenames)(ModuleTarget("Top", "Model").ref(leaf)) ==
        ModuleTarget("Top", "Model").ref(s"${multiProducer}_source").field("bits").field(leaf))
    }
  }
  var reversedAliasesRejected = false
  try new InferModelPorts().execute(multiAliasState(Seq("right_valid", "right_data")))
  catch {
    case e: RuntimeException if e.getMessage.contains("partially overlapping") =>
      reversedAliasesRejected = true
  }
  require(reversedAliasesRejected, "Scala accepted a reordered aggregate alias branch")
  println(s"PRODUCER $multiProducer physical aliases 4 fields data valid one token target")
  def writeBoundary(directory: java.io.File, boundary: CircuitState, renames: RenameMap): Unit = {
    directory.mkdirs()
    val fir = new java.io.PrintWriter(new java.io.File(directory, "post-infer-model-ports.sfc.fir"))
    try fir.write(boundary.circuit.serialize) finally fir.close()
    val anno = new java.io.PrintWriter(new java.io.File(directory, "post-infer-model-ports.sfc.json"))
    try anno.write(JsonProtocol.serialize(boundary.annotations)) finally anno.close()
    val renamed = new java.io.PrintWriter(new java.io.File(directory, "post-host-renames.sfc.json"))
    try renamed.write(JsonProtocol.serialize(boundary.annotations.flatMap(_.update(renames))))
    finally renamed.close()
  }
  if (args.nonEmpty) {
    writeBoundary(new java.io.File(args(0)), inferred, renames)
    writeBoundary(new java.io.File(args(0), "distinct-top-aliases"), aliasInferred, aliasRenames)
    writeBoundary(new java.io.File(args(0), "distinct-multiport-aliases"),
      multiAliasInferred, multiAliasRenames)
  }
  println("PASS production shared scalar/aggregate producer; common bits rename; four incompatible groups rejected")
}
