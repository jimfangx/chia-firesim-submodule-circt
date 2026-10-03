// See LICENSE for license details.
#pragma once

#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace goldengate {
enum class ChannelKind { Pipe, DecoupledForward, DecoupledReverse, TargetClock };

struct RationalClockInfo {
  std::string name;
  uint64_t multiplier;
  uint64_t divisor;
  uint64_t mfmr;
};

struct GGChannelConnection {
  std::string name;
  ChannelKind kind;
  std::optional<GGTarget> clock;
  llvm::SmallVector<GGTarget> sources;
  llvm::SmallVector<GGTarget> sinks;
  std::map<std::string, GGTarget> handshake;
  std::optional<uint64_t> latency;
  std::vector<RationalClockInfo> targetClocks;
};

// Interpret the pre-FAME connection annotation using CIRCT attributes and
// resolve every reference into a FIRRTL module operation and port index.
std::optional<GGChannelConnection>
analyzeChannelConnection(circt::firrtl::CircuitOp circuit,
                         circt::firrtl::Annotation annotation,
                         std::string &error);

// A forward channel's ready reference must resolve to the matching reverse
// channel endpoint. Check this after all connection annotations are resolved.
bool validateDecoupledChannelPairs(
    llvm::ArrayRef<GGChannelConnection> channels, std::string &error);
} // namespace goldengate
