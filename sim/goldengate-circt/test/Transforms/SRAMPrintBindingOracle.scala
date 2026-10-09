// See LICENSE for license details.
// Independent SFC pipeline on the immutable Rocket handoff, with explicit rf
// and printf selections. The candidate is never transformed by Scala.
package midas.passes.fame

import firrtl._
import firrtl.annotations._
import firrtl.ir._
import firrtl.stage.Forms
import firrtl.stage.transforms.Compiler
import midas.stage.phases.ConfigParametersAnnotation

object SRAMPrintBindingOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(path)
    try source.mkString finally source.close()
  }
  def write(name: String, body: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, name))
    try out.write(body) finally out.close()
  }
  val parameters = new midas.SimConfig().toInstance.alterPartial {
    case midas.SynthPrints => true
  }
  val config = ConfigParametersAnnotation(parameters)
  var state = CircuitState(Parser.parse(read(args(1))), ChirrtlForm,
    midas.ConvertExternalToInternalAnnotations(JsonProtocol.deserialize(read(args(2)), true)) :+ config)
  def privatePass(name: String): Transform = Class.forName("midas.passes." + name)
    .getDeclaredConstructor().newInstance().asInstanceOf[Transform]
  def emit(name: String): Unit = {
    write(name + ".fir", state.circuit.serialize)
    write(name + ".json", JsonProtocol.serialize(state.annotations
      .filterNot(_.isInstanceOf[ConfigParametersAnnotation])))
  }
  state = new Compiler(Forms.LowForm).execute(state)
  state = midas.passes.HoistStopAndPrintfEnables.runTransform(state)
  state = midas.passes.CoerceAsyncToSyncReset.runTransform(state)
  state = privatePass("BridgeExtraction").runTransform(state)
  state = new Compiler(Forms.LowForm).execute(state)
  state = privatePass("PrintSynthesis").runTransform(state)
  state = new ResolveAndCheck().runTransform(state)
  val trigger = Class.forName("midas.passes.TriggerWiring$")
    .getField("MODULE$").get(null).asInstanceOf[Transform]
  state = trigger.runTransform(state)
  state = midas.passes.GlobalResetConditionWiring.runTransform(state)
  state = midas.passes.ChannelClockInfoAnalysis.runTransform(state)
  state = midas.passes.UpdateBridgeClockInfo.runTransform(state)
  state = WrapTop.runTransform(state)
  state = new LabelSRAMModels().runTransform(state)
  state = new ExtractModel().runTransform(state)
  state = new Compiler(Forms.LowForm).execute(state)
  state = PromotePassthroughConnections.runTransform(state)
  state = new ResolveAndCheck().runTransform(state)
  state = new FAMEDefaults().runTransform(state)
  state = FindDefaultClocks.runTransform(state)
  state = new ChannelExcision().runTransform(state)
  state = new InferModelPorts().runTransform(state)
  emit("sfc-prepared")
  state = new FAMETransform().runTransform(new ResolveAndCheck().runTransform(state))
  state = AddRemainingFanoutAnnotations.runTransform(state)
  state = state.copy(circuit = state.circuit.copy(modules =
    state.circuit.modules :+ midas.passes.DefineAbstractClockGate.blackbox))
  state = new EmitAndWrapRAMModels().runTransform(state)
  emit("sfc-post-ram")
  // Pinned firtool no longer imports legacy validif expressions. Apply only
  // the SFC backend's existing normalization to the independent reference.
  write("sfc-post-ram-backend.fir",
    firrtl.passes.RemoveValidIf.runTransform(state).circuit.serialize)
  require(state.circuit.modules.exists(_.name == "rf"), "selected SRAM adapter missing")
  val prints = state.annotations.collect {
    case a: firesim.lib.bridgeutils.BridgeIOAnnotation if a.widgetClass == "midas.widgets.PrintBridgeModule" => a
  }
  require(prints.size == 1, "expected one domain of selected prints")
  println("PASS independent SFC selected Rocket.rf + PrintSynthesis/FAME/RAM boundary")
}
