// See LICENSE for license details.
// Requires: selected post-FAME wrapper, local MemPortAnnotation channel targets,
// known UInt payload widths, read and write commands, no replaced-body identities.
// Consumes/produces annotations: none; preserves wrapper and port metadata.
// Analyses: typed target resolution; invalidates wrapper body/instance analyses.
// Mutates: wrapper body; materialization also appends a unique native host module.
// Output: verified FIRRTL adapter and optionally native async host timing model.
// Failed analysis/emission leaves the circuit unchanged.
#include "goldengate/RAMModelAdapter.h"
#include "goldengate/AsyncRAMModel.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "goldengate/RetainedAnnotationTargets.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "circt/Support/Namespace.h"
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;
using goldengate::RAMModelParameters;
using goldengate::AnnotationClasses;
using goldengate::resolveAnnotationTarget;

namespace {
struct Signal {
  unsigned port;
  std::string field, target;
  unsigned width;
};
struct MemoryPort {
  bool read;
  Signal addr, en, data, mask;
};
BundleType bundle(MLIRContext *context,
                  std::initializer_list<BundleType::BundleElement> fields) {
  return BundleType::get(context, ArrayRef<BundleType::BundleElement>(fields));
}
struct RAMModelPlan {
  RAMModelParameters parameters;
  SmallVector<MemoryPort, 2> memoryPorts;
  unsigned clock, reset;
  BundleType channels;
  // One physical input token may carry fields for multiple serialized memory
  // commands. Each consumer must see it exactly once before the source advances.
  std::map<unsigned, SmallVector<unsigned>> inputConsumers;
  std::map<unsigned, SmallVector<unsigned>> responseProducers;
};
LogicalResult analyzeRAMModel(CircuitOp circuit, FModuleOp wrapper,
                              RAMModelPlan &plan, std::string &error) {
  if (!wrapper || wrapper->getParentOp() != circuit) {
    error = "RAM adapter needs an internal wrapper in this circuit";
    return failure();
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "RAM adapter needs retained memory port annotations";
    return failure();
  }
  auto *context = circuit.getContext();
  auto bit = UIntType::get(context, 1);
  auto ports = wrapper.getPorts();
  auto findPort = [](FModuleOp module, StringRef name) -> std::optional<unsigned> {
    for (unsigned i = 0; i < module.getNumPorts(); ++i)
      if (module.getPortName(i) == name) return i;
    return std::nullopt;
  };
  auto clock = findPort(wrapper, "hostClock"), reset = findPort(wrapper, "hostReset");
  if (!clock || !reset || ports[*clock].direction != Direction::In ||
      ports[*reset].direction != Direction::In ||
      !isa<ClockType>(ports[*clock].type) || ports[*reset].type != bit) {
    error = "RAM adapter needs FAME hostClock and one-bit hostReset inputs";
    return failure();
  }
  auto resolve = [&](Annotation annotation, StringRef member, bool input,
                     Signal &signal) {
    auto spelling = annotation.getMember<StringAttr>(member);
    auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error)
                           : std::nullopt;
    if (!target || target->module.getOperation() != wrapper || !target->port) {
      error = "RAM " + member.str() + " must target a local wrapper port";
      return failure();
    }
    auto port = ports[*target->port];
    auto channel = dyn_cast<BundleType>(port.type);
    auto bitsIndex = channel ? channel.getElementIndex("bits") : std::nullopt;
    auto validIndex = channel ? channel.getElementIndex("valid") : std::nullopt;
    auto readyIndex = channel ? channel.getElementIndex("ready") : std::nullopt;
    if (!bitsIndex || !validIndex || !readyIndex || channel.getElements().size() != 3 ||
        port.direction != (input ? Direction::In : Direction::Out)) {
      error = "RAM " + member.str() + " is not a directed Decoupled channel";
      return failure();
    }
    auto fields = channel.getElements();
    if (fields[*bitsIndex].isFlip || fields[*validIndex].isFlip ||
        !fields[*readyIndex].isFlip || fields[*validIndex].type != bit ||
        fields[*readyIndex].type != bit) {
      error = "RAM channel has an invalid ready/valid ABI";
      return failure();
    }
    FIRRTLBaseType payload = fields[*bitsIndex].type;
    std::string field;
    uint64_t bitsID = channel.getFieldID(*bitsIndex);
    if (auto aggregate = dyn_cast<BundleType>(payload)) {
      bool found = false;
      for (unsigned i = 0; i < aggregate.getElements().size(); ++i) {
        auto leaf = aggregate.getElements()[i];
        if (!leaf.isFlip && target->fieldID == bitsID + aggregate.getFieldID(i)) {
          payload = leaf.type;
          field = leaf.name.getValue().str();
          found = true;
          break;
        }
      }
      if (!found) {
        error = "RAM target needs one ground field below channel bits";
        return failure();
      }
    } else if (target->fieldID != bitsID) {
      error = "RAM target must select channel bits";
      return failure();
    }
    auto uint = dyn_cast<UIntType>(payload);
    if (!uint || !uint.getWidth() || *uint.getWidth() <= 0) {
      error = "RAM payload requires a positive known UInt width";
      return failure();
    }
    signal = {*target->port, field, spelling.getValue().str(), unsigned(*uint.getWidth())};
    return success();
  };
  SmallVector<MemoryPort, 2> memoryPorts;
  llvm::DenseSet<Attribute> seen;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    bool read = annotation.isClass(AnnotationClasses::ModelReadPort);
    bool write = annotation.isClass(AnnotationClasses::ModelWritePort);
    bool readwrite = annotation.isClass(AnnotationClasses::ModelReadWritePort);
    if (!read && !write && !readwrite) continue;
    auto address = annotation.getMember<StringAttr>("addr");
    auto target = address ? resolveAnnotationTarget(circuit, address.getValue(), error)
                          : std::nullopt;
    if (!target) return failure();
    if (target->module.getOperation() != wrapper) continue;
    if (readwrite) {
      error = "EmitAndWrapRAMModels does not support readwrite ports";
      return failure();
    }
    if (!seen.insert(attr).second) continue; // SFC's annotation multimap is a set.
    MemoryPort port{};
    port.read = read;
    if (failed(resolve(annotation, "addr", true, port.addr)) ||
        failed(resolve(annotation, "en", true, port.en)) ||
        failed(resolve(annotation, "data", !read, port.data)) ||
        (write && failed(resolve(annotation, "mask", true, port.mask))))
      return failure();
    if (port.en.width != 1 || (write && port.mask.width != 1)) {
      error = "RAM enable and mask must be one bit";
      return failure();
    }
    if (!memoryPorts.empty() &&
        (port.addr.width != memoryPorts.front().addr.width ||
         port.data.width != memoryPorts.front().data.width)) {
      error = "All memory port widths must be equal";
      return failure();
    }
    memoryPorts.push_back(port);
  }
  if (memoryPorts.empty()) {
    error = "RAM adapter has no selected memory ports";
    return failure();
  }
  RAMModelParameters planned{memoryPorts.front().addr.width, memoryPorts.front().data.width, 0, 0};
  for (const auto &port : memoryPorts) port.read ? ++planned.reads : ++planned.writes;
  std::map<unsigned, SmallVector<unsigned>> inputConsumers;
  std::map<unsigned, SmallVector<unsigned>> responseProducers;
  std::set<std::pair<unsigned, std::string>> responseFields;
  for (auto [consumer, port] : llvm::enumerate(memoryPorts)) {
    llvm::DenseSet<unsigned> inputs{port.addr.port, port.en.port};
    if (!port.read) { inputs.insert(port.data.port); inputs.insert(port.mask.port); }
    for (unsigned channel : inputs)
      inputConsumers[channel].push_back(consumer);
    if (port.read) {
      if (!responseFields.insert({port.data.port, port.data.field}).second) {
        error = "RAM response payload field shared by different producers requires arbitration";
        return failure();
      }
      responseProducers[port.data.port].push_back(consumer);
    }
  }
  // The current AsyncMemChiselModel indexes both command vectors at zero.
  if (!planned.reads || !planned.writes) {
    error = "Async RAM implementation requires read and write ports";
    return failure();
  }
  OpBuilder b(context);
  auto field = [&](StringRef name, bool flip, FIRRTLBaseType type) {
    return BundleType::BundleElement{b.getStringAttr(name), flip, type};
  };
  auto decoupled = [&](FIRRTLBaseType payload) {
    return bundle(context, {field("ready", true, bit), field("valid", false, bit),
                            field("bits", false, payload)});
  };
  auto addr = UIntType::get(context, planned.addressWidth);
  auto data = UIntType::get(context, planned.dataWidth);
  auto readCmd = bundle(context, {field("en", false, bit), field("addr", false, addr)});
  auto writeCmd = bundle(context, {field("en", false, bit), field("mask", false, bit),
                                   field("addr", false, addr), field("data", false, data)});
  auto channels = bundle(context, {
      field("reset", true, decoupled(bit)),
      field("read_cmds", true, FVectorType::get(decoupled(readCmd), planned.reads)),
      field("read_resps", false, FVectorType::get(decoupled(data), planned.reads)),
      field("write_cmds", true, FVectorType::get(decoupled(writeCmd), planned.writes))});
  bool bodyIdentities = false;
  wrapper.walk([&](Operation *op) {
    if (op == wrapper) return;
    auto annotations = op->getAttrOfType<ArrayAttr>("annotations");
    bodyIdentities |= op->hasAttr("inner_sym") || (annotations && !annotations.empty());
  });
  if (bodyIdentities) {
    error = "RAM wrapper body has annotation identities requiring a rename policy";
    return failure();
  }
  std::string prefix = "~" + circuit.getName().str() + "|";
  auto referencesBody = [&](StringAttr text) {
    auto ref = text.getValue();
    if (!ref.consume_front(prefix)) return false;
    auto [scope, reference] = ref.split('>');
    SmallVector<StringRef> path;
    scope.split(path, '/');
    for (auto [i, step] : llvm::enumerate(path)) {
      auto name = i ? step.split(':').second : step;
      if (name != wrapper.getName()) continue;
      if (i + 1 != path.size()) return true; // Erased instance in this body.
      if (reference.empty()) continue; // Module/instance identity survives.
      std::string ignored;
      auto target = resolveAnnotationTarget(circuit,
          prefix + wrapper.getName().str() + ">" + reference.str(), ignored);
      if (!target || !target->port) return true;
    }
    return false;
  };
  bool retainedBodyTarget = false;
  for (auto annotation : raw)
    goldengate::mapRetainedAnnotationTargets(annotation, [&](StringAttr target) {
      retainedBodyTarget |= referencesBody(target);
      return target;
    });
  if (retainedBodyTarget) {
    error = "RAM wrapper body has retained targets requiring a rename policy";
    return failure();
  }
  plan = {planned, std::move(memoryPorts), *clock, *reset, channels,
          std::move(inputConsumers), std::move(responseProducers)};
  return success();
}
} // namespace

LogicalResult goldengate::wrapRAMModel(
    CircuitOp circuit, FModuleOp wrapper, FModuleOp implementation,
    RAMModelParameters &parameters, std::string &error) {
  parameters = {};
  if (!implementation || wrapper == implementation ||
      implementation->getParentOp() != circuit) {
    error = "RAM adapter needs distinct internal modules in the same circuit";
    return failure();
  }
  RAMModelPlan plan;
  if (failed(analyzeRAMModel(circuit, wrapper, plan, error))) return failure();
  auto *context = circuit.getContext();
  OpBuilder b(context);
  auto bit = UIntType::get(context, 1);
  auto findPort = [](FModuleOp module, StringRef name) -> std::optional<unsigned> {
    for (unsigned i = 0; i < module.getNumPorts(); ++i)
      if (module.getPortName(i) == name) return i;
    return std::nullopt;
  };
  auto channels = plan.channels;
  auto implClock = findPort(implementation, "clock"), implReset = findPort(implementation, "reset"),
       implChannels = findPort(implementation, "channels");
  auto implPorts = implementation.getPorts();
  if (!implClock || !implReset || !implChannels || implPorts.size() != 3 ||
      implPorts[*implClock].direction != Direction::In ||
      implPorts[*implClock].type != ClockType::get(context) ||
      implPorts[*implReset].direction != Direction::In || implPorts[*implReset].type != bit ||
      implPorts[*implChannels].direction != Direction::Out || implPorts[*implChannels].type != channels) {
    error = "RAM implementation ABI differs from resolved memory parameters";
    return failure();
  }
  // Build and verify a temporary body before committing. Existing instances,
  // module metadata, port identities and retained annotations are untouched.
  OwningOpRef<FModuleOp> candidate(cast<FModuleOp>(wrapper->clone()));
  b.setInsertionPoint(wrapper);
  b.insert(candidate->getOperation());
  auto block = candidate->getBodyBlock();
  block->dropAllReferences();
  block->getOperations().clear();
  b.setInsertionPointToEnd(block);
  Location loc = wrapper.getLoc();
  auto model = b.create<InstanceOp>(loc, implementation, "model");
  auto sub = [&](Value value, StringRef name) -> Value {
    return b.create<SubfieldOp>(loc, value, name);
  };
  auto connect = [&](Value destination, Value source) {
    b.create<StrictConnectOp>(loc, destination, source);
  };
  auto constant = [&](bool value) -> Value {
    return b.create<ConstantOp>(loc, bit, APInt(1, value));
  };
  auto both = [&](Value left, Value right) -> Value {
    return b.create<AndPrimOp>(loc, left, right);
  };
  auto either = [&](Value left, Value right) -> Value {
    return b.create<OrPrimOp>(loc, left, right);
  };
  auto neg = [&](Value value) -> Value { return b.create<NotPrimOp>(loc, value); };
  using Delivery = std::pair<unsigned, unsigned>; // Physical channel, command.
  std::map<Delivery, Value> delivered, consumerReady;
  std::map<unsigned, Value> responses;
  auto bits = [&](const Signal &s) {
    Value result = sub(block->getArgument(s.port), "bits");
    return s.field.empty() ? result : sub(result, s.field);
  };
  auto valid = [&](const Signal &s, unsigned consumer) {
    Value result = sub(block->getArgument(s.port), "valid");
    auto seen = delivered.find({s.port, consumer});
    return seen == delivered.end() ? result : both(result, neg(seen->second));
  };
  auto ready = [&](const Signal &s) { return sub(block->getArgument(s.port), "ready"); };
  auto andAll = [&](Value first, ArrayRef<Signal> signals, const Signal *excluded,
                    unsigned consumer) {
    for (const auto &s : signals)
      if (!excluded || s.target != excluded->target)
        first = b.create<AndPrimOp>(loc, first, valid(s, consumer));
    return first;
  };
  connect(model.getResult(*implClock), block->getArgument(plan.clock));
  connect(model.getResult(*implReset), block->getArgument(plan.reset));
  Value channel = model.getResult(*implChannels), resetChannel = sub(channel, "reset");
  connect(sub(resetChannel, "valid"), constant(true));
  connect(sub(resetChannel, "bits"), constant(false));
  circt::Namespace stateNames;
  for (auto port : wrapper.getPorts()) stateNames.newName(port.getName());
  stateNames.newName(model.getName());
  for (const auto &[port, consumers] : plan.inputConsumers) {
    if (consumers.size() < 2) continue;
    for (unsigned consumer : consumers) {
      auto name = stateNames.newName("ram_shared_" + std::to_string(port) + "_" +
                                     std::to_string(consumer) + "_delivered");
      delivered[{port, consumer}] = b.create<RegOp>(
          loc, bit, block->getArgument(plan.clock), name).getResult();
    }
  }
  unsigned readIndex = 0, writeIndex = 0;
  for (auto [consumer, port] : llvm::enumerate(plan.memoryPorts)) {
    auto index = port.read ? readIndex++ : writeIndex++;
    Value cmd = b.create<SubindexOp>(loc,
        sub(channel, port.read ? "read_cmds" : "write_cmds"), index);
    SmallVector<Signal> inputs;
    if (!port.read) { inputs.push_back(port.data); inputs.push_back(port.mask); }
    inputs.push_back(port.addr); inputs.push_back(port.en);
    for (const auto &s : inputs) {
      auto commandReady = andAll(sub(cmd, "ready"), inputs, &s, consumer);
      if (delivered.count({s.port, consumer}))
        consumerReady[{s.port, consumer}] = commandReady;
      else connect(ready(s), commandReady);
    }
    connect(sub(cmd, "valid"), andAll(constant(true), inputs, nullptr, consumer));
    Value payload = sub(cmd, "bits");
    connect(sub(payload, "addr"), bits(port.addr));
    connect(sub(payload, "en"), bits(port.en));
    if (port.read) {
      Value resp = b.create<SubindexOp>(loc, sub(channel, "read_resps"), index);
      connect(bits(port.data), sub(resp, "bits"));
      responses[consumer] = resp;
      if (plan.responseProducers.at(port.data.port).size() == 1) {
        connect(valid(port.data, consumer), sub(resp, "valid"));
        connect(sub(resp, "ready"), ready(port.data));
      }
    } else {
      connect(sub(payload, "data"), bits(port.data));
      connect(sub(payload, "mask"), bits(port.mask));
    }
  }
  // Distinct response payload fields may share one physical channel. Join
  // their tokens: none of the independently buffered model responses may be
  // released until the complete output packet can transfer. SFC's last-connect
  // valid happens to work for ordered arrivals, but can expose stale fields
  // when a later-priority read command arrives before an earlier one.
  for (const auto &[port, producers] : plan.responseProducers) {
    if (producers.size() < 2) continue;
    Value output = block->getArgument(port), allValid = constant(true);
    for (unsigned producer : producers)
      allValid = both(allValid, sub(responses.at(producer), "valid"));
    connect(sub(output, "valid"), allValid);
    for (unsigned producer : producers) {
      Value responseReady = sub(output, "ready");
      for (unsigned other : producers)
        if (producer != other)
          responseReady = both(responseReady, sub(responses.at(other), "valid"));
      connect(sub(responses.at(producer), "ready"), responseReady);
    }
  }
  // A shared source must hold its token/payload until every consumer has
  // accepted it. Remember early deliveries instead of AND-ing ready signals:
  // host RAM commands are serialized, so their ready pulses need not overlap.
  // Suppress repeats for already-served commands and clear only on the source
  // handshake (or host reset). Storage is never initialized by this protocol.
  for (const auto &[port, consumers] : plan.inputConsumers) {
    if (consumers.size() < 2) continue;
    Value source = block->getArgument(port);
    Value sourceValid = sub(source, "valid"), allDone = constant(true);
    SmallVector<Value> done;
    for (unsigned consumer : consumers) {
      Value seen = delivered.at({port, consumer});
      Value fire = both(both(sourceValid, neg(seen)),
                        consumerReady.at({port, consumer}));
      done.push_back(either(seen, fire));
      allDone = both(allDone, done.back());
    }
    connect(sub(source, "ready"), allDone);
    Value flush = either(block->getArgument(plan.reset), both(sourceValid, allDone));
    for (auto [index, consumer] : llvm::enumerate(consumers))
      connect(delivered.at({port, consumer}), b.create<MuxPrimOp>(
          loc, flush, constant(false), done[index]));
  }
  if (failed(verify(*candidate))) {
    error = "RAM adapter produced invalid CIRCT FIRRTL";
    return failure();
  }
  wrapper.getBody().takeBody(candidate->getBody());
  parameters = plan.parameters;
  return success();
}

LogicalResult goldengate::materializeRAMModel(
    CircuitOp circuit, FModuleOp wrapper, FModuleOp &implementation,
    RAMModelParameters &parameters, std::string &error) {
  implementation = {};
  parameters = {};
  RAMModelPlan plan;
  if (failed(analyzeRAMModel(circuit, wrapper, plan, error))) return failure();
  // SFC allocates one RamModel per wrapper using the circuit namespace. Include
  // external modules too; never reuse another wrapper's storage implementation.
  llvm::StringSet<> names;
  for (auto &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleLike>(&op)) names.insert(module.getModuleName());
  std::string name = "RamModel";
  for (unsigned suffix = 1; names.count(name); ++suffix)
    name = "RamModel_" + std::to_string(suffix);
  OpBuilder b(circuit.getContext());
  auto loc = wrapper.getLoc();
  SmallVector<PortInfo> ports{
      {b.getStringAttr("clock"), ClockType::get(b.getContext()), Direction::In},
      {b.getStringAttr("reset"), UIntType::get(b.getContext(), 1), Direction::In},
      {b.getStringAttr("channels"), plan.channels, Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  OwningOpRef<FModuleOp> host(b.create<FModuleOp>(
      loc, b.getStringAttr(name), wrapper.getConventionAttr(), ports));
  RAMModelParameters emitted;
  // The temporary host is erased on either failure. wrapRAMModel commits only
  // after its replacement body verifies, so failure cannot orphan a host or
  // partially replace the selected wrapper.
  if (failed(emitAsyncRAMModel(*host, emitted, error)) ||
      failed(wrapRAMModel(circuit, wrapper, *host, parameters, error)))
    return failure();
  implementation = host.release();
  return success();
}

LogicalResult goldengate::materializeSelectedRAMModels(
    CircuitOp circuit, unsigned &materialized, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "Selected RAM materialization needs retained memory port annotations";
    return failure();
  }
  SmallVector<std::string> selected;
  llvm::DenseSet<Operation *> seen;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ModelReadPort) &&
        !annotation.isClass(AnnotationClasses::ModelWritePort) &&
        !annotation.isClass(AnnotationClasses::ModelReadWritePort))
      continue;
    auto address = annotation.getMember<StringAttr>("addr");
    auto target = address ? resolveAnnotationTarget(circuit, address.getValue(), error)
                          : std::nullopt;
    if (!target || !target->port ||
        !isa<FModuleOp>(target->module.getOperation())) {
      if (error.empty())
        error = "Selected RAM addr annotation must target an internal wrapper port";
      return failure();
    }
    if (seen.insert(target->module.getOperation()).second)
      selected.push_back(target->module.getName().str());
  }
  if (selected.empty()) {
    materialized = 0;
    return success();
  }

  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  llvm::StringMap<FModuleOp> originalWrappers, stagedWrappers;
  llvm::StringSet<> originalModules;
  for (auto module : circuit.getOps<FModuleLike>()) {
    originalModules.insert(module.getModuleName());
    if (auto internal = dyn_cast<FModuleOp>(module.getOperation()))
      originalWrappers[internal.getName()] = internal;
  }
  for (auto wrapper : staged->getOps<FModuleOp>())
    stagedWrappers[wrapper.getName()] = wrapper;
  for (const auto &name : selected) {
    FModuleOp implementation;
    RAMModelParameters parameters;
    if (failed(materializeRAMModel(*staged, stagedWrappers.lookup(name),
                                   implementation, parameters, error)))
      return failure();
  }
  if (failed(verify(*staged))) {
    error = "Selected RAM materialization produced invalid FIRRTL";
    return failure();
  }
  // Each replacement changes only a selected body and adds a fresh module.
  // Preserve untouched simulator operations and selected module identities;
  // callers may retain their FAME selection sets across this boundary.
  for (const auto &name : selected)
    originalWrappers.lookup(name).getBody().takeBody(
        stagedWrappers.lookup(name).getBody());
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto module = dyn_cast<FModuleLike>(&op))
      if (!originalModules.count(module.getModuleName()))
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  materialized = selected.size();
  return success();
}
