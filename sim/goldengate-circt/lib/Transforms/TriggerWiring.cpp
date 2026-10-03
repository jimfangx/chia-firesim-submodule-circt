// See LICENSE for license details.
#include "goldengate/TriggerWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

using namespace circt::firrtl;
using namespace mlir;
using A = goldengate::AnnotationClasses;

namespace {
bool source(Annotation a) {
  return a.isClass(A::TriggerSource) || a.isClass(A::InternalTriggerSource);
}
bool sink(Annotation a) {
  return a.isClass(A::TriggerSink) || a.isClass(A::InternalTriggerSink);
}
// Hardware support starts with local, ground references in the circuit top.
// Resolve through target identity utilities rather than interpreting FIRRTL text.
Value resolve(CircuitOp circuit, FModuleOp top, StringAttr target,
              std::string &error) {
  if (!target) { error = "trigger annotation has a missing reference"; return {}; }
  std::string portError;
  if (auto port = goldengate::resolveAnnotationTarget(circuit, target.getValue(), portError)) {
    if (port->module == top && port->port && port->fieldID.value_or(0) == 0)
      return top.getBodyBlock()->getArgument(*port->port);
  }
  auto *op = goldengate::resolveInternalAnnotationTarget(circuit, target.getValue(), error);
  if (op && isa<WireOp, NodeOp, RegOp, RegResetOp>(op) &&
      op->getBlock() == top.getBodyBlock())
    return op->getResult(0);
  error = "trigger hardware currently needs local ground references in the circuit top: " +
          target.getValue().str();
  return {};
}
bool boolean(Value v) {
  auto type = v ? dyn_cast<UIntType>(v.getType()) : UIntType();
  return type && type.getWidth() == 1;
}
struct Source { Value event, reset; bool credit; std::string name; };
// BridgeTopWiring groups source events by the upstream input Clock port.
// Prove electrical aliases using transparent FIRRTL operations only. A cone
// with one input clock is insufficient: a mux or gate may change its edges.
class LocalClockAliases {
public:
  explicit LocalClockAliases(FModuleOp top) : top(top) {
    top.walk([&](Operation *op) {
      Value dest, src;
      if (auto connect = dyn_cast<ConnectOp>(op)) {
        dest = connect.getDest(); src = connect.getSrc();
      } else if (auto connect = dyn_cast<StrictConnectOp>(op)) {
        dest = connect.getDest(); src = connect.getSrc();
      }
      if (dest && isa<ClockType>(dest.getType()))
        drivers[dest].push_back({src, op->getBlock() == top.getBodyBlock()});
    });
  }
  Value root(Value value) {
    llvm::DenseSet<Value> visited;
    while (value && isa<ClockType>(value.getType()) && visited.insert(value).second) {
      if (auto arg = dyn_cast<BlockArgument>(value)) {
        if (arg.getOwner() == top.getBodyBlock() &&
            top.getPortDirection(arg.getArgNumber()) == Direction::In &&
            drivers[value].empty()) return value;
        return {};
      }
      auto *op = value.getDefiningOp();
      if (!op || op->getBlock() != top.getBodyBlock()) return {};
      if (auto node = dyn_cast<NodeOp>(op)) {
        if (!drivers[value].empty()) return {};
        value = node.getInput();
      } else if (isa<WireOp>(op)) {
        auto &assigned = drivers[value];
        if (assigned.size() != 1 || !assigned.front().second) return {};
        value = assigned.front().first;
      } else return {};
    }
    return {};
  }
private:
  FModuleOp top;
  llvm::DenseMap<Value, SmallVector<std::pair<Value, bool>>> drivers;
};
} // namespace

LogicalResult goldengate::wireTriggers(CircuitOp circuit, unsigned &consumed,
                                      std::string &error) {
  consumed = 0;
  error.clear();
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = "TriggerWiring needs retained annotations"; return failure(); }
  SmallVector<Attribute> retained;
  SmallVector<Annotation> sources, sinks;
  unsigned credits = 0, debits = 0;
  for (Attribute attr : raw) {
    Annotation a(attr);
    if (sink(a)) { sinks.push_back(a); continue; }
    if (!source(a)) { retained.push_back(attr); continue; }
    auto kind = a.getMember<BoolAttr>("sourceType");
    if (!kind) { error = "trigger source has no sourceType"; return failure(); }
    kind.getValue() ? ++credits : ++debits;
    sources.push_back(a);
  }
  if ((credits == 0) != (debits == 0)) {
    error = "trigger credits and debits must both be present"; return failure();
  }
  // Scala skips target resolution when either side is absent, then consumes
  // trigger annotations. This includes sinks with no sources.
  if (sources.empty() || sinks.empty()) {
    consumed = sources.size();
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
    return success();
  }
  FModuleOp top;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == circuit.getName()) top = module;
  if (!top) { error = "trigger hardware needs a circuit top"; return failure(); }
  StringAttr baseTarget;
  for (auto attr : raw) {
    Annotation a(attr);
    auto info = a.getMember<DictionaryAttr>("channelInfo");
    if (!a.isClass(A::ChannelConnection) || !info ||
        info.getAs<StringAttr>("class") != A::TargetClockChannel) continue;
    auto targets = a.getMember<ArrayAttr>("sinks");
    if (targets && !targets.empty()) {
      baseTarget = dyn_cast<StringAttr>(targets[0]);
      break;
    }
  }
  Value clock = resolve(circuit, top, baseTarget, error);
  if (!clock || !isa<ClockType>(clock.getType())) {
    if (clock) error = "trigger base reference must be Clock";
    return failure();
  }
  LocalClockAliases aliases(top);
  // Local accounting uses BridgeTopWiring's root; the synchronizers and
  // global counters retain the annotated base clock, as in Scala.
  Value baseClock = clock;
  clock = aliases.root(clock);
  if (!clock) {
    error = "trigger base clock needs an unconditional local alias of an input Clock port";
    return failure();
  }
  SmallVector<Source> events;
  llvm::DenseSet<Value> creditTargets, debitTargets;
  SmallVector<std::pair<NodeOp, Value>> nodes;
  llvm::DenseSet<Operation *> seen;
  for (auto a : sources) {
    auto target = a.getMember<StringAttr>("target");
    Value event = resolve(circuit, top, target, error);
    if (!event) return failure();
    Value eventClock = resolve(circuit, top, a.getMember<StringAttr>("clock"), error);
    if (!eventClock) return failure();
    if (!boolean(event) || aliases.root(eventClock) != clock) {
      error = "trigger sources must be UInt<1> on the local base clock"; return failure();
    }
    bool credit = a.getMember<BoolAttr>("sourceType").getValue();
    if (!(credit ? creditTargets : debitTargets).insert(event).second) {
      error = "trigger hardware currently needs distinct source targets per sourceType";
      return failure();
    }
    Value reset;
    if (auto resetTarget = a.getMember<StringAttr>("reset")) {
      reset = resolve(circuit, top, resetTarget, error);
      if (!reset) return failure();
      if (!boolean(reset)) { error = "trigger reset must be UInt<1>"; return failure(); }
    } else if (a.getDict().get("reset")) {
      error = "trigger reset must be a reference when present"; return failure();
    }
    auto name = target.getValue().split('>').second.str();
    events.push_back({event, reset, credit, name});
  }
  DominanceInfo dominance(circuit);
  for (auto a : sinks) {
    Value value = resolve(circuit, top, a.getMember<StringAttr>("target"), error);
    if (!value) return failure();
    Value sinkClock = resolve(circuit, top, a.getMember<StringAttr>("clock"), error);
    if (!sinkClock) return failure();
    auto node = value.getDefiningOp<NodeOp>();
    if (!node || !boolean(value) || aliases.root(sinkClock) != clock) {
      error = "trigger sinks must be UInt<1> nodes on the local base clock"; return failure();
    }
    if (!dominance.properlyDominates(sinkClock, node.getOperation())) {
      error = "trigger sink clock must dominate its node declaration"; return failure();
    }
    if (seen.insert(node).second) nodes.push_back({node, sinkClock});
  }
  // All unsupported scope/type/clock cases have been rejected before mutation.
  circt::Namespace names;
  for (auto name : top.getPortNamesAttr()) names.newName(cast<StringAttr>(name).getValue());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
  });
  OpBuilder b(circuit.getContext());
  b.setInsertionPointToEnd(top.getBodyBlock());
  auto loc = top.getLoc();
  auto named = [&](Value value, StringRef name) -> Value {
    return b.create<NodeOp>(loc, value, b.getStringAttr(names.newName(name))).getResult();
  };
  auto reg = [&](unsigned width, StringRef name, Value domain) -> Value {
    // RegZeroPreset in Scala has reset=0/init=self, with no preset annotation.
    return b.create<RegOp>(loc, UIntType::get(b.getContext(), width), domain,
                           names.newName(name)).getResult();
  };
  std::string clockName = top.getPortName(cast<BlockArgument>(clock).getArgNumber()).str();
  SmallVector<Value> creditSignals, debitSignals;
  for (auto event : events) {
    Value signal = event.event;
    if (event.reset) {
      Value active = b.create<NotPrimOp>(loc, event.reset);
      signal = named(b.create<AndPrimOp>(loc, active, signal), event.name + "_masked");
    }
    (event.credit ? creditSignals : debitSignals).push_back(signal);
  }
  auto reduce = [&](ArrayRef<Value> signals, StringRef stem) -> Value {
    // Scala DensePrefixSum uses the previous layer for every addition at an
    // offset. A ripple sum would grow FIRRTL widths differently for N > 3.
    SmallVector<Value> layer(signals);
    for (size_t offset = 1; offset < layer.size(); offset *= 2) {
      SmallVector<Value> next(layer);
      for (size_t i = offset; i < layer.size(); ++i)
        next[i] = named(b.create<AddPrimOp>(loc, layer[i - offset], layer[i]),
                        stem.str() + "_sum");
      layer.swap(next);
    }
    return layer.back();
  };
  auto local = [&](ArrayRef<Value> signals, StringRef suffix) -> Value {
    std::string stem = clockName + suffix.str();
    Value signal = reduce(signals, stem);
    Value count = reg(16, stem, clock);
    Value next = named(b.create<AddPrimOp>(loc, count, signal), stem + "_next");
    Value truncated = b.create<BitsPrimOp>(loc, next, 15, 0);
    b.create<StrictConnectOp>(loc, count, truncated);
    Value s1 = reg(16, stem + "_next_count_sync_s1", baseClock);
    Value s2 = reg(16, stem + "_next_count_sync_s2", baseClock);
    b.create<StrictConnectOp>(loc, s1, truncated);
    b.create<StrictConnectOp>(loc, s2, s1);
    // SFC infers UInt<17> subtraction, including the underflow bit at wrap.
    return named(b.create<SubPrimOp>(loc, s1, s2), stem + "_next_diff");
  };
  Value creditDiff = local(creditSignals, "_credits");
  Value debitDiff = local(debitSignals, "_debits");
  auto total = [&](Value diff, StringRef name) -> Value {
    Value count = reg(32, name, baseClock);
    Value next = named(b.create<AddPrimOp>(loc, count, diff), name.str() + "_next");
    b.create<StrictConnectOp>(loc, count, b.create<BitsPrimOp>(loc, next, 31, 0));
    return next;
  };
  Value creditNext = total(creditDiff, "totalCredits");
  Value debitNext = total(debitDiff, "totalDebits");
  // Compare full UInt<33> NEXT values; comparing truncated state changes wrap semantics.
  Value enable = named(b.create<NEQPrimOp>(loc, creditNext, debitNext), "trigger_source");
  for (auto [node, sinkClock] : nodes) {
    b.setInsertionPoint(node);
    Value sync = reg(1, "trigger_sync", sinkClock);
    b.setInsertionPointToEnd(top.getBodyBlock());
    b.create<StrictConnectOp>(loc, sync, enable);
    node->setOperand(0, sync);
  }
  consumed = sources.size();
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
  return success();
}
