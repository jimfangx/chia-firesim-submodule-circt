// See LICENSE for license details.
#pragma once
#include "goldengate/RAMModelAdapter.h"
namespace goldengate {
// Replace a host timing-model body with the AsyncMemChiselModel contract.
// Requires the exact aggregate RegfileModelIO ABI; infers depth=2^addrWidth.
// Preserves module/port identities and annotations. No target memory is reset.
// Rejects unsupported ABI or body identities before mutation.
mlir::LogicalResult emitAsyncRAMModel(circt::firrtl::FModuleOp module,
    RAMModelParameters &parameters, std::string &error);
}
