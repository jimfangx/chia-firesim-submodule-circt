// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_COMMAND_SCHEDULER_H
#define GOLDENGATE_FASED_FRFCFS_COMMAND_SCHEDULER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Compose the selected FIFO MAS or FirstReadyFCFSModel scheduler: four rank
// trackers and refresh arbitration, with one pipelined FIFO head for FCFS or
// an eight-entry collapsing reference window for FRFCFS.
// nextBits is decoded metadata {id:4,len:8,isWrite:1,rowAddr:26,bankAddrOH:8,
// bankAddr:3,rankAddrOH:4,rankAddr:2}. The core computes incoming
// isReady/mayPRE itself, including broadcasts from commands issued in the
// enqueue cycle. State/reset advance only on targetFire. The three-bit
// schedulerWindowSize retains the oracle's truncation of default eight to zero
// (full window). This boundary excludes transaction arbitration, backend
// response queues, LLC, functional capacities, power counters and MMIO/platform
// acceptance. Core, rank, bank, refresh and collapsing-buffer symbols must all
// be free; collisions are checked before any dependency is materialized.
mlir::LogicalResult materializeFASEDFRFCFSCommandScheduler(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
