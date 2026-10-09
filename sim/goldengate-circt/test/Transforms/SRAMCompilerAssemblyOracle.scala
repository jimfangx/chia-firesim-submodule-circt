// See LICENSE for license details.
// Export the independently generated SFC SRAM adapter for comparison after
// native FireSim host/platform assembly. No Scala pass transforms the candidate.
package midas.passes.fame

import firrtl._
import firrtl.ir._

object SRAMCompilerAssemblyOracle extends App {
  val source = scala.io.Source.fromFile(args(0))
  val reference = try Parser.parse(source.mkString) finally source.close()
  val adapter = reference.modules.find(_.name == "rf").get
  val implementation = reference.modules.find(_.name == "RamModel").get
  // Keep the SFC port contract while excluding its implementation from this
  // adapter comparison. Native RAM state has a separate transition comparator.
  val blackbox = ExtModule(NoInfo, implementation.name, implementation.ports,
    implementation.name, Nil)
  val output = new java.io.PrintWriter(args(1))
  try output.write(Circuit(NoInfo, Seq(adapter, blackbox), adapter.name).serialize)
  finally output.close()
}
