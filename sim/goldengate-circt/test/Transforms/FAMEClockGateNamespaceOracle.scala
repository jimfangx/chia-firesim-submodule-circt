// See LICENSE for license details.
import firrtl.Namespace

// FAMETransform allocates the buffer exactly once after its enable register.
object FAMEClockGateNamespaceOracle extends App {
  val ns = Namespace(Seq("target_buffer", "target_buffer_0", "other_buffer", "third_buffer"))
  Seq("target", "other", "third").foreach { clock =>
    ns.newName(s"${clock}_enabled")
    println(s"GATE $clock ${ns.newName(s"${clock}_buffer")}")
  }
}
