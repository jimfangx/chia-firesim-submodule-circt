// See LICENSE for license details.
// Inspect the same CheckCombLoops/FAMEChannelAnalysis graph used by the
// unchanged FAMETransform; never transform the native candidate with Scala.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import org.json4s._
import org.json4s.native.JsonMethods._
import scala.collection.mutable

object SRAMChannelDependenciesOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  for (name <- Seq("fanout", "golden-rocket")) {
    val circuit = Parser.parse(read(s"oracle/$name.channels.sfc.fir"))
    val annotations = JsonProtocol.deserialize(read(s"oracle/$name.channels.sfc.json"))
    val analysis = new FAMEChannelAnalysis(CircuitState(circuit, LowForm, annotations))
    val rows = analysis.transformedModules.toSeq.flatMap { target =>
      val inputs = analysis.modelInputChannelPortMap(target).toSeq.flatMap {
        case (channel, (_, ports)) => ports.map(_.name -> channel)
      }.toMap
      val outputs = analysis.modelOutputChannelPortMap(target)
      val outputNames = outputs.toSeq.flatMap {
        case (channel, (_, ports)) => ports.map(_.name -> channel)
      }.toMap
      val dependencies = mutable.LinkedHashMap[String, mutable.LinkedHashSet[String]]()
      // FAMETransform's LI-BDN step 2: output RHS aliases may occur in the
      // simplified graph, so only references in the input map become tokens.
      analysis.connectivity(target.module).getEdgeMap.foreach {
        case (port, references) if outputNames.contains(port) =>
          dependencies.getOrElseUpdate(outputNames(port), mutable.LinkedHashSet()) ++=
            references.flatMap(inputs.get(_))
        case _ =>
      }
      outputs.keys.toSeq.sorted.map { output =>
        require(dependencies.contains(output), s"missing SFC output vertex: $target/$output")
        JObject(List("module" -> JString(target.module),
          "output_channel" -> JString(output),
          "input_channels" -> JArray(dependencies(output).toList.map(JString))))
      }
    }
    val oracle = new java.io.PrintWriter(new java.io.File(directory, s"oracle/$name.dependencies.sfc.json"))
    try oracle.write(pretty(render(JArray(rows.toList)))) finally oracle.close()
    def semantic(values: List[JValue]) = values.map { row =>
      val module = (row \ "module").asInstanceOf[JString].s
      val output = (row \ "output_channel").asInstanceOf[JString].s
      val inputs = (row \ "input_channels").asInstanceOf[JArray].arr.map(_.asInstanceOf[JString].s)
      require(inputs.distinct.size == inputs.size, s"duplicate dependency: $module/$output")
      (module, output) -> inputs.toSet
    }
    val nativeRows = parse(read(s"$name-candidate/post-sram-channel-dependencies.json")).asInstanceOf[JArray].arr
    val expected = semantic(rows.toList)
    val actual = semantic(nativeRows)
    require(actual.map(_._1).distinct.size == actual.size, "repeated model instance duplicated local output FSM")
    require(expected.toMap == actual.toMap,
      s"$name dependencies differ: missing=${expected.toSet -- actual.toSet}, extra=${actual.toSet -- expected.toSet}")
    val edges = expected.map(_._2.size).sum
    println(s"PASS $name: ${expected.map(_._1._1).distinct.size} model definitions, ${expected.size} local outputs, $edges dependency edges match SFC FAMETransform")
  }
}
