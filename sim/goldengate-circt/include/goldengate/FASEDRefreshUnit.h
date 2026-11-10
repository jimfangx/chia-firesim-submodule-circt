// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_REFRESH_UNIT_H
#define GOLDENGATE_FASED_REFRESH_UNIT_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Materialize Scala RefreshUnit for the selected four-rank/eight-bank DRAM
// organization. Pure combinational arbitration, independent REF/PRE proposals,
// and lowest-index priority are preserved. Encoders with no selected input
// return the last index; a no-match PRE bank mux returns rank 3's encoded bank,
// exactly as Chisel PriorityMux lowers, even while suggestPRE is false.
// Inputs: wantREF[4], canREF[4], canPRE[4], bankCanPRE_0..3[8], ranksInUse[4].
// Outputs: suggestREF, refRankAddr[2], suggestPRE, preRankAddr[2], preBankAddr[3].
// The GGFASEDRefreshUnit symbol must be free. Failure leaves IR/result unchanged.
// This primitive does not establish FRFCFS scheduler or platform support.
mlir::LogicalResult materializeFASEDRefreshUnit(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
