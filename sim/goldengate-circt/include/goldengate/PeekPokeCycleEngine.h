// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Connect Rocket PeekPoke reset tokens and cycle scheduling to the simulator.
mlir::LogicalResult addPeekPokeCycleEngine(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
// Bind the STEP queue and seven decoded MCR words to the cycle engine.
mlir::LogicalResult addPeekPokeMMIOBank(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
} // namespace goldengate
