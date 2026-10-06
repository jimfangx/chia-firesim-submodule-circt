// See LICENSE for license details.
import firrtl.Namespace

// Execute the namespace contract in FAMETransform.genMetadata/hostFlagReg.
// Collisions model legal pre-FAME target declarations; do not edit the oracle.
object FAMEFiredNamespaceOracle extends App {
  val ns = Namespace(Seq("input_fired_0", "output_fired", "output_fired_0",
                         "virtualInput_fired_0"))
  Seq("input", "output", "virtualInput", "virtualOutput").foreach { channel =>
    val suggestion = ns.newName(s"${channel}_fired")
    val fired = ns.newName(suggestion)
    println(s"IDENTITY $channel $fired")
  }
}
