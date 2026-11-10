// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_RANK_STATE_TRACKER_H
#define GOLDENGATE_FASED_RANK_STATE_TRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Materialize Scala RankStateTracker for the selected 8-bank, 4-rank, 16-GiB
// organization, including eight bank tracker instances and the four-ACT window.
// All state and synchronous resets advance only on targetFire. CAS commands for
// other ranks still update rank-to-rank timing. This does not implement the
// refresh arbiter, scheduler, backend, LLC or platform configuration acceptance.
// Both GGFASEDRankStateTracker and GGFASEDBankStateTracker symbols must be free.
// Failure leaves the circuit and result unchanged.
mlir::LogicalResult materializeFASEDRankStateTracker(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
