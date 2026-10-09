// See LICENSE for license details.
// Independent BridgeTopWiring oracle for the shared-module native unit fixture.
package midas.passes

import firrtl._
import firrtl.annotations._
import firrtl.stage.Forms
import firrtl.stage.transforms.Compiler

object PrintSourceGroupingOracle extends App {
  val input = """circuit Top :
    |  module Top :
    |    input clock : Clock
    |    wire local_wire : UInt<1>
    |    local_wire <= UInt<1>(0)
    |    inst left of A
    |    inst right of B
    |    inst direct of Leaf
    |    left.clock <= clock
    |    right.clock <= clock
    |    direct.clock <= clock
    |  module A :
    |    input clock : Clock
    |    inst l of Leaf
    |    l.clock <= clock
    |  module B :
    |    input clock : Clock
    |    inst l of Leaf
    |    l.clock <= clock
    |  module Leaf :
    |    input clock : Clock
    |    wire message_wire : UInt<1>
    |    wire empty_wire : UInt<1>
    |    message_wire <= UInt<1>(0)
    |    empty_wire <= UInt<1>(0)
    |""".stripMargin
  val selected = Seq("Top" -> "local_wire", "Leaf" -> "message_wire", "Leaf" -> "empty_wire")
  val annotations = selected.map { case (module, wire) =>
    val target = ModuleTarget("Top", module)
    BridgeTopWiringAnnotation(target.ref(wire), target.ref("clock"))
  }
  val lowered = new Compiler(Forms.MidForm).execute(CircuitState(Parser.parse(input), ChirrtlForm, annotations))
  val result = new BridgeTopWiring("synthesizedPrintf_").runTransform(lowered)
  val outputs = result.annotations.collect { case a: BridgeTopWiringOutputAnnotation => a }
  require(outputs.size == 7)
  for (source <- annotations.map(_.target)) {
    val indices = outputs.zipWithIndex.collect { case (a, i) if a.pathlessSource == source => i }
    require(indices == (indices.head to indices.last), "source replicas interleaved")
    val ports = outputs.filter(_.pathlessSource == source).map(_.topSink.ref)
    val paths = if (source.module == "Top") Seq("") else Seq("left_l_", "right_l_", "direct_")
    require(ports == paths.map("synthesizedPrintf_" + _ + source.ref), "instance traversal differs")
  }
  val out = new java.io.PrintWriter(args(0))
  try out.write(JsonProtocol.serialize(outputs)) finally out.close()
  println("PASS SFC source groups: Top.local_wire; Leaf.message_wire/empty_wire replicas left/l, right/l, direct")
}
