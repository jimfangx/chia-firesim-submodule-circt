// See LICENSE for license details.
#include "goldengate/ChannelAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include <algorithm>
#include <vector>

using namespace circt::firrtl;

static std::optional<goldengate::GGTarget>
resolvePort(CircuitOp circuit, mlir::StringAttr spelling, std::string &error) {
  if (!spelling) {
    error = "missing port reference";
    return std::nullopt;
  }
  auto target = goldengate::resolveAnnotationTarget(circuit,
                                                     spelling.getValue(), error);
  if (target && !target->port) {
    error = "expected a port reference";
    return std::nullopt;
  }
  return target;
}

std::optional<goldengate::GGChannelConnection>
goldengate::analyzeChannelConnection(CircuitOp circuit, Annotation annotation,
                                     std::string &error) {
  auto name = annotation.getMember<mlir::StringAttr>("globalName");
  auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo");
  if (!name || !info) {
    error = "channel has no name or channelInfo";
    return std::nullopt;
  }
  auto type = info.getAs<mlir::StringAttr>("class");
  if (!type) {
    error = "channelInfo has no class";
    return std::nullopt;
  }
  GGChannelConnection result;
  result.name = name.getValue().str();
  auto kind = type.getValue();
  if (kind == AnnotationClasses::PipeChannel) {
    result.kind = ChannelKind::Pipe;
    auto latency = info.getAs<mlir::IntegerAttr>("latency");
    if (!latency || latency.getInt() < 0) {
      error = "pipe channel has invalid latency";
      return std::nullopt;
    }
    result.latency = latency.getUInt();
  } else if (kind == AnnotationClasses::DecoupledForwardChannel) {
    result.kind = ChannelKind::DecoupledForward;
    for (llvm::StringRef field : {"readySink", "validSource", "readySource",
                                  "validSink"}) {
      if (auto value = info.getAs<mlir::StringAttr>(field)) {
        auto target = resolvePort(circuit, value, error);
        if (!target)
          return std::nullopt;
        result.handshake.emplace(field.str(), *target);
      }
    }
  } else if (kind == AnnotationClasses::DecoupledReverseChannel) {
    result.kind = ChannelKind::DecoupledReverse;
  } else if (kind == AnnotationClasses::TargetClockChannel) {
    result.kind = ChannelKind::TargetClock;
    auto clocks = info.getAs<mlir::ArrayAttr>("clockInfo");
    auto mfmrs = info.getAs<mlir::ArrayAttr>("perClockMFMR");
    if (!clocks || !mfmrs || clocks.size() != mfmrs.size()) {
      error = "target clock metadata lengths do not match";
      return std::nullopt;
    }
    for (unsigned i = 0; i < clocks.size(); ++i) {
      auto record = mlir::dyn_cast<mlir::DictionaryAttr>(clocks[i]);
      auto mfmr = mlir::dyn_cast<mlir::IntegerAttr>(mfmrs[i]);
      if (!record || !mfmr) {
        error = "invalid target clock record";
        return std::nullopt;
      }
      auto clockName = record.getAs<mlir::StringAttr>("name");
      auto multiplier = record.getAs<mlir::IntegerAttr>("multiplier");
      auto divisor = record.getAs<mlir::IntegerAttr>("divisor");
      if (!clockName || !multiplier || !divisor || multiplier.getInt() <= 0 ||
          divisor.getInt() <= 0 || mfmr.getInt() <= 0) {
        error = "invalid target clock ratio or MFMR";
        return std::nullopt;
      }
      result.targetClocks.push_back({clockName.getValue().str(),
                                     multiplier.getUInt(), divisor.getUInt(),
                                     mfmr.getUInt()});
    }
  } else {
    error = "unsupported channel kind: " + kind.str();
    return std::nullopt;
  }

  if (auto clock = annotation.getMember<mlir::StringAttr>("clock")) {
    result.clock = resolvePort(circuit, clock, error);
    if (!result.clock)
      return std::nullopt;
  }
  for (auto field : {"sources", "sinks"}) {
    if (auto endpoints = annotation.getMember<mlir::ArrayAttr>(field)) {
      for (auto attr : endpoints) {
        auto spelling = mlir::dyn_cast<mlir::StringAttr>(attr);
        auto target = resolvePort(circuit, spelling, error);
        if (!target)
          return std::nullopt;
        if (llvm::StringRef(field) == "sources")
          result.sources.push_back(*target);
        else
          result.sinks.push_back(*target);
      }
    }
  }
  if (result.sources.empty() && result.sinks.empty()) {
    error = "channel has neither sources nor sinks";
    return std::nullopt;
  }
  if (result.kind == ChannelKind::DecoupledForward) {
    bool sourcePair = result.handshake.count("readySink") &&
                      result.handshake.count("validSource");
    bool sinkPair = result.handshake.count("readySource") &&
                    result.handshake.count("validSink");
    if (result.handshake.size() != 2 || sourcePair == sinkPair) {
      error = "forward channel needs exactly one ready/valid endpoint pair";
      return std::nullopt;
    }
    const auto &valid = result.handshake.at(sourcePair ? "validSource"
                                                  : "validSink");
    const auto &endpoints = sourcePair ? result.sources : result.sinks;
    bool found = std::any_of(endpoints.begin(), endpoints.end(),
                             [&](const GGTarget &endpoint) {
                               return endpoint.module == valid.module &&
                                      endpoint.port == valid.port;
                             });
    if (!found) {
      error = "forward channel valid is not one of its data endpoints";
      return std::nullopt;
    }
  }
  if (result.kind == ChannelKind::TargetClock &&
      result.targetClocks.size() != result.sources.size() + result.sinks.size()) {
    error = "target clock count differs from endpoint count";
    return std::nullopt;
  }
  return result;
}

bool goldengate::validateDecoupledChannelPairs(
    llvm::ArrayRef<GGChannelConnection> channels, std::string &error) {
  std::vector<unsigned> reverseUses(channels.size(), 0);
  for (const auto &forward : channels) {
    if (forward.kind != ChannelKind::DecoupledForward)
      continue;
    const bool sourceSide = forward.handshake.count("readySink");
    const auto &ready = forward.handshake.at(sourceSide ? "readySink"
                                                    : "readySource");
    unsigned matches = 0;
    for (auto [index, reverse] : llvm::enumerate(channels)) {
      if (reverse.kind != ChannelKind::DecoupledReverse)
        continue;
      const auto &endpoints = sourceSide ? reverse.sinks : reverse.sources;
      const bool hasReady = std::any_of(
          endpoints.begin(), endpoints.end(), [&](const GGTarget &endpoint) {
            return endpoint.module == ready.module && endpoint.port == ready.port;
          });
      if (hasReady) {
        ++matches;
        ++reverseUses[index];
      }
    }
    if (matches != 1) {
      error = "forward channel " + forward.name + " has " +
              std::to_string(matches) + " matching reverse ready channels";
      return false;
    }
  }
  for (auto [index, reverse] : llvm::enumerate(channels)) {
    if (reverse.kind == ChannelKind::DecoupledReverse &&
        reverseUses[index] != 1) {
      error = "reverse channel " + reverse.name + " belongs to " +
              std::to_string(reverseUses[index]) + " forward channels";
      return false;
    }
  }
  return true;
}
