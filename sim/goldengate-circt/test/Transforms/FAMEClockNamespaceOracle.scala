// See LICENSE for license details.
import firrtl.Namespace

// FAMETransformer.hostFlagReg performs exactly one allocation per model clock.
object FAMEClockNamespaceOracle extends App {
  val ns = Namespace(Seq("target_enabled", "target_enabled_0", "other_enabled"))
  Seq("target", "other").foreach { clock =>
    println(s"IDENTITY $clock ${ns.newName(s"${clock}_enabled")}")
  }
}
