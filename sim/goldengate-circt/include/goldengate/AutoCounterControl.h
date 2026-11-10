// See LICENSE for license details.
#pragma once
#include "goldengate/AutoCounterBridge.h"
namespace goldengate {
// Assemble the complete bound CSV host collection behind one real AXI control
// port. The caller supplies every region explicitly; no placeholder or unbound
// slave is accepted. Regions may be relocated but must exactly size each actual
// bank. Uses the production decode, AW/W queues, per-ID read/write tracking,
// response arbiters and error slave. All local AXI ports become internal.
// Header constructors use these same regions. Retained annotations transfer.
// Clone/verify/commit preserves IR, header and child handles on failure; child
// handles change on success. Full platform registration and Print are separate.
mlir::LogicalResult assembleAutoCounterControl(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    llvm::ArrayRef<ControlMMIORegion> regions,
    std::string &header, std::string &error);
}
