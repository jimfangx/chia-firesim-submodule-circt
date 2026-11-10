// See LICENSE for license details.
package midas.widgets

import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.RationalClock
import freechips.rocketchip.diplomacy.LazyModule
import midas.core.{StreamAllocator, ToCPUStreamAllocatorKey}
import org.chipsalliance.cde.config.Parameters

/** Elaborate the unchanged SFC PrintBridgeModule with the actual F2 control
  * parameters. This is supporting ABI/interface evidence, not a replacement
  * for the enabled Linux/formal representative.
  */
object PrintF2ControlOracle extends App {
  require(args.length == 1, "output-directory")
  val output = new java.io.File(args(0))
  require(output.mkdirs() || output.isDirectory)
  implicit val p: Parameters = new midas.F2Config().toInstance.alterPartial {
    case TargetClockInfo          => Some(RationalClock("clock\"\\?\n", 1, 3))
    case ToCPUStreamAllocatorKey => new StreamAllocator
  }
  val key = PrintBridgeParameters(
    "reset0",
    Seq(PrintPort("record0", Seq("enable" -> "UInt<1>", "argument" -> "SInt<5>"), "signed=%d\n")),
  )
  val widget = LazyModule(new PrintBridgeModule(key))
  val firrtl = (new ChiselStage).emitChirrtl(widget.module, Array("--target-dir", output.getPath))
  val header = new StringBuilder
  // A high F2 base detects accidental address truncation in emitted driver ABI.
  val base = BigInt("80000000", 16)
  widget.module.genHeader(base, Map.empty, header)
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(output, name))
    try out.write(text) finally out.close()
  }
  write("print-f2.fir", firrtl)
  write("print-f2.h", header.toString)
  val registers = Seq("startCycleL", "startCycleH", "endCycleL", "endCycleH", "doneInit", "flushNarrowPacket")
  val addresses = registers.map(name => s"$name=${base + widget.getCRAddr(name)}").mkString("\n")
  write("print-f2-abi.txt", s"addressBits=${p(CtrlNastiKey).addrBits}\nregisterCount=${widget.module.numRegs}\nregionBytes=${widget.memRegionSize}\nstreamBits=${widget.toHostStreamWidthBits}\nstreamIndex=${widget.toHostStreamIdx}\nqueueDepth=${widget.toHostCPUQueueDepth}\n$addresses\n")
  println("PASS unchanged SFC PrintBridgeModule F2 control/driver ABI")
}
