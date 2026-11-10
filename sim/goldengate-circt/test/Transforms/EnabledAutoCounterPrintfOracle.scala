// See LICENSE for license details.
package midas.passes

import firrtl._
import firrtl.annotations.JsonProtocol
import firrtl.options.TargetDirAnnotation

/** Execute the unchanged SFC AutoCounterTransform on the CLI fixture's exact
  * pre-transform FIRRTL/annotations. This is supporting compiler evidence;
  * enabled representative/Print/F2/formal acceptance remains separate.
  */
object EnabledAutoCounterPrintfOracle extends App {
  require(args.length == 3, "input.fir input.json output-directory")
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(path)
    try source.mkString finally source.close()
  }
  val output = new java.io.File(args(2))
  require(output.mkdirs() || output.isDirectory)
  val state = CircuitState(
    Parser.parse(read(args(0))),
    LowForm,
    JsonProtocol.deserialize(read(args(1))) :+ TargetDirAnnotation(output.getPath),
  )
  val instrumented = new AutoCounterTransform().doTransform(state, usePrintfImplementation = true)
  val result = new ResolveAndCheck().runTransform(instrumented)
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(output, name))
    try out.write(text) finally out.close()
  }
  write("post-autocounter.fir", result.circuit.serialize)
  write("post-autocounter.json", JsonProtocol.serialize(result.annotations))
  println("PASS unchanged SFC enabled AutoCounterUsePrintfImpl transform")
}
