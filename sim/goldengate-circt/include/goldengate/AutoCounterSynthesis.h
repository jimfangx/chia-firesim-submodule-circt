// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>
namespace goldengate {
// Enabled AutoCounterTransform's CSV strategy. Input is LowerTypes/ExpandWhens
// normalized target FIRRTL with typed retained AutoCounter annotations and
// synchronous UInt<1> resets. Select/gate events, export every reachable instance,
// resolve input clock roots, and emit per-domain CSV constructors, trigger/reset
// sinks and FAME source channels. Hardware and metadata commit together only
// after successful verification. Existing child handles may change on success;
// circuit identity is preserved. Generated constructors preserve concrete event
// operation types; no guessing from standalone untyped JSON is required.
// Event positions match unchanged SFC BridgeTopWiring's Scala 2.13.10
// ComponentName/ReferenceTarget immutable-map iteration, including hash trie
// branches and full-hash collisions. No event-order normalization is needed.
// Clock-info inference, Trigger/GlobalReset wiring, FAME tokenization, host/MMIO
// assembly and the final enabled platform remain subsequent compiler stages.
mlir::LogicalResult synthesizeEnabledAutoCounterCSV(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<llvm::StringRef> coverModuleNames,
    unsigned &synthesized, std::string &error);
}
