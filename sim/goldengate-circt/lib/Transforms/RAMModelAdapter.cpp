// See LICENSE for license details.
#include "goldengate/RAMModelAdapter.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseSet.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;

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
} // namespace

LogicalResult goldengate::wrapRAMModel(
    CircuitOp circuit, FModuleOp wrapper, FModuleOp implementation,
    RAMModelParameters &parameters, std::string &error) {
  parameters = {};
  if (!wrapper || !implementation || wrapper == implementation ||
      wrapper->getParentOp() != circuit || implementation->getParentOp() != circuit) {
    error = "RAM adapter needs distinct internal modules in the same circuit";
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
  // Multiple fields of one command can share a channel. A channel shared by
  // different commands would make SFC's annotation-set iteration decide which
  // ready/valid connect wins; require a defined arbitration policy first.
  llvm::DenseSet<unsigned> ownedChannels;
  for (const auto &port : memoryPorts) {
    llvm::DenseSet<unsigned> local{port.addr.port, port.en.port, port.data.port};
    if (!port.read) local.insert(port.mask.port);
    for (unsigned channel : local)
      if (!ownedChannels.insert(channel).second) {
        error = "RAM channel shared by different commands requires arbitration";
        return failure();
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
  bool innerSymbols = false;
  wrapper.walk([&](Operation *op) { innerSymbols |= op->hasAttr("inner_sym"); });
  if (innerSymbols) {
    error = "RAM wrapper body has inner symbols requiring a rename policy";
    return failure();
  }
  std::string prefix = "~" + circuit.getName().str() + "|" + wrapper.getName().str() + ">";
  std::function<bool(Attribute)> referencesBody = [&](Attribute attr) {
    if (auto text = dyn_cast<StringAttr>(attr)) {
      if (!text.getValue().starts_with(prefix)) return false;
      std::string ignored;
      auto target = resolveAnnotationTarget(circuit, text.getValue(), ignored);
      return !target || !target->port;
    }
    if (auto array = dyn_cast<ArrayAttr>(attr)) return llvm::any_of(array, referencesBody);
    if (auto dictionary = dyn_cast<DictionaryAttr>(attr))
      return llvm::any_of(dictionary, [&](NamedAttribute member) { return referencesBody(member.getValue()); });
    return false;
  };
  if (referencesBody(raw)) {
    error = "RAM wrapper body has retained targets requiring a rename policy";
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
  auto bits = [&](const Signal &s) {
    Value result = sub(block->getArgument(s.port), "bits");
    return s.field.empty() ? result : sub(result, s.field);
  };
  auto valid = [&](const Signal &s) { return sub(block->getArgument(s.port), "valid"); };
  auto ready = [&](const Signal &s) { return sub(block->getArgument(s.port), "ready"); };
  auto andAll = [&](Value first, ArrayRef<Signal> signals, const Signal *excluded) {
    for (const auto &s : signals)
      if (!excluded || s.target != excluded->target)
        first = b.create<AndPrimOp>(loc, first, valid(s));
    return first;
  };
  connect(model.getResult(*implClock), block->getArgument(*clock));
  connect(model.getResult(*implReset), block->getArgument(*reset));
  Value channel = model.getResult(*implChannels), resetChannel = sub(channel, "reset");
  connect(sub(resetChannel, "valid"), constant(true));
  connect(sub(resetChannel, "bits"), constant(false));
  unsigned readIndex = 0, writeIndex = 0;
  for (const auto &port : memoryPorts) {
    auto index = port.read ? readIndex++ : writeIndex++;
    Value cmd = b.create<SubindexOp>(loc,
        sub(channel, port.read ? "read_cmds" : "write_cmds"), index);
    SmallVector<Signal> inputs;
    if (!port.read) { inputs.push_back(port.data); inputs.push_back(port.mask); }
    inputs.push_back(port.addr); inputs.push_back(port.en);
    for (const auto &s : inputs)
      connect(ready(s), andAll(sub(cmd, "ready"), inputs, &s));
    connect(sub(cmd, "valid"), andAll(constant(true), inputs, nullptr));
    Value payload = sub(cmd, "bits");
    connect(sub(payload, "addr"), bits(port.addr));
    connect(sub(payload, "en"), bits(port.en));
    if (port.read) {
      Value resp = b.create<SubindexOp>(loc, sub(channel, "read_resps"), index);
      connect(bits(port.data), sub(resp, "bits"));
      connect(valid(port.data), sub(resp, "valid"));
      connect(sub(resp, "ready"), ready(port.data));
    } else {
      connect(sub(payload, "data"), bits(port.data));
      connect(sub(payload, "mask"), bits(port.mask));
    }
  }
  if (failed(verify(*candidate))) {
    error = "RAM adapter produced invalid CIRCT FIRRTL";
    return failure();
  }
  wrapper.getBody().takeBody(candidate->getBody());
  parameters = planned;
  return success();
}
