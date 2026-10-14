// See LICENSE for license details.
#pragma once
#include "goldengate/CPUStreamRead.h"
namespace goldengate {
/// Consume buffered Decoupled512 inputs into a U250 AXI AW/W/B interface.
/// Allocate incoming windows independently of outgoing streams. AW and B
/// complete on the last beat, all writes require B readiness, and only the
/// selected sink counter advances. Reject unsupported inputs before mutation.
mlir::LogicalResult addCPUStreamWrite(circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<CPUStreamSourcePort> sinks, std::string &error);
}
