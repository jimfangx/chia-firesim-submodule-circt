// See LICENSE for license details.
// Supporting internal-link transport oracle. The unchanged production SimWrapper
// cannot elaborate target-to-target RV channels (see ReadyValid graph oracle).
// Instantiate its exact unchanged ReadyValidChannel primitive independently;
// this establishes only the transport boundary, never full SimWrapper parity.
import chisel3._
import firrtl._
import midas.core.ReadyValidChannel
import org.chipsalliance.cde.config.Parameters
object FAMEInternalReadyValidOracle extends App {
  implicit val p: Parameters = Parameters.empty
  val directory = new java.io.File(args(0)); directory.mkdirs()
  val chirrtl = (new chisel3.stage.ChiselStage).emitChirrtl(new ReadyValidChannel(UInt(8.W)),
    Array("--target-dir",directory.getAbsolutePath))
  val circuit = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl),ChirrtlForm),Nil).circuit
  val out = new java.io.PrintWriter(new java.io.File(directory,"sfc-queue.fir"))
  try out.write(circuit.serialize) finally out.close()
  println("PASS unchanged SFC ReadyValidChannel 8-bit primitive; internal full SimWrapper remains unsupported")
}
