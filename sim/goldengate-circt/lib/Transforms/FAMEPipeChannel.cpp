// See LICENSE for license details.
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include <climits>
#include <functional>
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
constexpr llvm::StringLiteral wrapperName = "GGFAMEPipeWrapper";

// PipeChannel(gen) stores integer leaves without changing their types. In
// particular UInt<0> still carries valid/ready tokens (present in the SFC U250
// oracle). Clock/reset/analog leaves, flips, consts and unknown widths cannot
// be transported by this data queue.
bool isPayloadType(FIRRTLBaseType type) {
  if (!type || !type.isRegisterType())
    return false;
  auto width = getBitWidth(type);
  if (!width || *width > INT32_MAX)
    return false;
  if (auto integer = dyn_cast<IntType>(type))
    return integer.getWidthOrSentinel() >= 0;
  if (auto bundle = dyn_cast<BundleType>(type))
    return llvm::all_of(bundle.getElements(), [](BundleType::BundleElement e) {
      return isPayloadType(e.type);
    });
  if (auto vector = dyn_cast<FVectorType>(type))
    return isPayloadType(vector.getElementType());
  return false;
}

std::string pipeModuleName(FIRRTLBaseType type, unsigned latency) {
  std::string name = "GGFAMEPipe";
  // Preserve legacy UInt module names. Type, not packed width, identifies a
  // definition: equal-width SInt/UInt and differently shaped aggregates must
  // not share incompatible ports. Hash the full printed type deterministically.
  if (auto integer = dyn_cast<UIntType>(type))
    name += std::to_string(integer.getWidthOrSentinel());
  else if (auto integer = dyn_cast<SIntType>(type))
    name += "S" + std::to_string(integer.getWidthOrSentinel());
  else {
    std::string spelling;
    llvm::raw_string_ostream stream(spelling);
    stream << type;
    llvm::SHA256 hash;
    hash.update(spelling);
    name += "T";
    for (uint8_t byte : hash.final()) {
      name += "0123456789abcdef"[byte >> 4];
      name += "0123456789abcdef"[byte & 15];
    }
  }
  return name + (latency == 0 ? "_L0" : "");
}

struct BoundaryPipe {
  std::string name;
  unsigned port;
  FIRRTLBaseType payload;
  unsigned latency;
};

// Resolve the post-FAME payload target to a live CIRCT port. Channel clock
// metadata belongs to the target clock domain; these queues use hostClock.
LogicalResult findBoundaryPipes(CircuitOp circuit, FModuleOp &top,
                                SmallVectorImpl<BoundaryPipe> &pipes,
                                std::string &error) {
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!top || !raw) {
    error = "PipeChannel construction needs a top module and retained annotations";
    return failure();
  }
  std::set<unsigned> usedPorts;
  std::set<std::string> usedNames;
  auto bit = UIntType::get(circuit.getContext(), 1, false);
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
      continue;
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") !=
                     goldengate::AnnotationClasses::PipeChannel)
      continue;
    auto name = annotation.getMember<StringAttr>("globalName");
    auto latency = info.getAs<IntegerAttr>("latency");
    if (!name || name.getValue().empty() || !latency ||
        latency.getInt() < 0 || latency.getInt() > 1) {
      error = "PipeChannel requires a name and latency zero or one";
      return failure();
    }
    auto sources = annotation.getMember<ArrayAttr>("sources");
    auto sinks = annotation.getMember<ArrayAttr>("sinks");
    bool hasSource = sources && !sources.empty();
    bool hasSink = sinks && !sinks.empty();
    if (hasSource == hasSink) {
      error = "PipeChannel " + name.getValue().str() +
              " is not a single boundary connection; loopbacks are not implemented";
      return failure();
    }
    auto endpoints = hasSource ? sources : sinks;
    if (endpoints.size() != 1) {
      error = "PipeChannel " + name.getValue().str() +
              " has multiple endpoints; aggregated pipes are not implemented";
      return failure();
    }
    auto spelling = dyn_cast<StringAttr>(endpoints[0]);
    auto target = spelling ? goldengate::resolveAnnotationTarget(
                                 circuit, spelling.getValue(), error)
                           : std::nullopt;
    if (!target || target->module != top || !target->port) {
      error = "PipeChannel " + name.getValue().str() +
              " payload must resolve to a top-level port: " + error;
      return failure();
    }
    unsigned port = *target->port;
    auto type = dyn_cast<BundleType>(top.getPortType(port));
    auto ready = type ? type.getElementIndex("ready") : std::nullopt;
    auto valid = type ? type.getElementIndex("valid") : std::nullopt;
    auto bits = type ? type.getElementIndex("bits") : std::nullopt;
    auto payload = bits ? type.getElements()[*bits].type : FIRRTLBaseType();
    if (!type || type.getElements().size() != 3 || !ready || !valid ||
        !bits || !isPayloadType(payload) || !getBitWidth(payload) ||
        !type.getElements()[*ready].isFlip ||
        type.getElements()[*valid].isFlip || type.getElements()[*bits].isFlip ||
        type.getElements()[*ready].type != bit ||
        type.getElements()[*valid].type != bit ||
        target->fieldID != type.getFieldID(*bits) ||
        top.getPortDirection(port) != (hasSource ? Direction::Out : Direction::In)) {
      error = "PipeChannel " + name.getValue().str() +
              " requires a passive integer Decoupled payload target with matching direction";
      return failure();
    }
    if (!usedPorts.insert(port).second ||
        !usedNames.insert(name.getValue().str()).second) {
      error = "PipeChannel " + name.getValue().str() +
              " shares a port or name; channel fanout is not implemented";
      return failure();
    }
    pipes.push_back({name.getValue().str(), port, payload,
                     static_cast<unsigned>(latency.getUInt())});
  }
  return success();
}
} // namespace

LogicalResult goldengate::addFAMEPipeChannel(CircuitOp circuit,
                                             unsigned payloadWidth,
                                             unsigned latency,
                                             std::string &error) {
  return addFAMEPipeChannel(circuit,
      UIntType::get(circuit.getContext(), payloadWidth, false), latency, error);
}

LogicalResult goldengate::addFAMEPipeChannel(CircuitOp circuit,
                                             FIRRTLBaseType data,
                                             unsigned latency,
                                             std::string &error) {
  auto width = data ? getBitWidth(data) : std::nullopt;
  if (!isPayloadType(data) || !width || latency > 1) {
    error = "PipeChannel requires a passive integer payload with known widths and latency zero or one";
    return failure();
  }
  auto *context = circuit.getContext();
  std::string name = pipeModuleName(data, latency);
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleLike>(&op);
        module && module.getModuleName() == name) {
      error = "PipeChannel module already exists";
      return failure();
    }
  auto bit = UIntType::get(context, 1, false);
  auto clock = ClockType::get(context);
  SmallVector<PortInfo> ports{
      {StringAttr::get(context, "clock"), clock, Direction::In},
      {StringAttr::get(context, "reset"), bit, Direction::In},
      {StringAttr::get(context, "io_in_ready"), bit, Direction::Out},
      {StringAttr::get(context, "io_in_valid"), bit, Direction::In},
      {StringAttr::get(context, "io_in_bits"), data, Direction::In},
      {StringAttr::get(context, "io_out_ready"), bit, Direction::In},
      {StringAttr::get(context, "io_out_valid"), bit, Direction::Out},
      {StringAttr::get(context, "io_out_bits"), data, Direction::Out}};
  OpBuilder builder(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  auto pipe = builder.create<FModuleOp>(
      circuit.getLoc(), StringAttr::get(context, name),
      ConventionAttr::get(context, Convention::Internal), ports);
  builder.setInsertionPointToStart(pipe.getBodyBlock());
  auto arg = [&](unsigned index) { return pipe.getBodyBlock()->getArgument(index); };
  Location loc = pipe.getLoc();
  Value zero = builder.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value initializing, zeroData;
  if (latency == 1) {
    // Match 0.U.asTypeOf(gen), including signed and aggregate payloads.
    auto zeroType = UIntType::get(context, *width, false);
    Value packedZero = builder.create<ConstantOp>(
        loc, zeroType, APInt(*width, 0));
    zeroData = data == zeroType
                   ? packedZero
                   : builder.create<BitCastOp>(loc, data, packedZero).getResult();
    initializing = builder.create<RegOp>(loc, bit, arg(0), "initializing").getResult();
  }
  Value valid0 = builder.create<RegResetOp>(loc, bit, arg(0), arg(1), zero,
                                           "valid_0").getResult();
  Value valid1 = builder.create<RegResetOp>(loc, bit, arg(0), arg(1), zero,
                                           "valid_1").getResult();
  Value bits0 = builder.create<RegOp>(loc, data, arg(0), "bits_0").getResult();
  Value bits1 = builder.create<RegOp>(loc, data, arg(0), "bits_1").getResult();

  // ShiftQueue(2) does not accept an enqueue when its second slot is full.
  Value enqReady = builder.create<NotPrimOp>(loc, valid1);
  Value enqValid = arg(3);
  Value inReady = enqReady;
  Value enqBits = arg(4);
  if (latency == 1) {
    enqValid = builder.create<OrPrimOp>(loc, initializing, arg(3));
    Value notInit = builder.create<NotPrimOp>(loc, initializing);
    inReady = builder.create<AndPrimOp>(loc, notInit, enqReady);
    enqBits = builder.create<MuxPrimOp>(loc, initializing, zeroData, arg(4));
    builder.create<StrictConnectOp>(loc, initializing, arg(1));
  }
  Value fire = builder.create<AndPrimOp>(loc, enqReady, enqValid);
  builder.create<StrictConnectOp>(loc, arg(2), inReady);
  builder.create<StrictConnectOp>(loc, arg(6), valid0);
  builder.create<StrictConnectOp>(loc, arg(7), bits0);
  Value fireOrValid1 = builder.create<OrPrimOp>(loc, fire, valid1);
  Value notValid0 = builder.create<NotPrimOp>(loc, valid0);
  Value write0NoDeq = builder.create<AndPrimOp>(loc, fire, notValid0);
  Value write0 = builder.create<MuxPrimOp>(loc, arg(5), fireOrValid1,
                                          write0NoDeq);
  Value nextValid0 = builder.create<MuxPrimOp>(
      loc, arg(5), fireOrValid1,
      builder.create<OrPrimOp>(loc, fire, valid0).getResult());
  builder.create<StrictConnectOp>(loc, valid0, nextValid0);
  Value shiftedBits = builder.create<MuxPrimOp>(loc, valid1, bits1, enqBits);
  builder.create<StrictConnectOp>(loc, bits0,
                                  builder.create<MuxPrimOp>(loc, write0,
                                                            shiftedBits, bits0));

  Value fireAndValid0 = builder.create<AndPrimOp>(loc, fire, valid0);
  Value fireAndValid1 = builder.create<AndPrimOp>(loc, fire, valid1);
  Value notValid1 = builder.create<NotPrimOp>(loc, valid1);
  Value write1NoDeq = builder.create<AndPrimOp>(loc, fireAndValid0, notValid1);
  Value write1 = builder.create<MuxPrimOp>(loc, arg(5), fireAndValid1,
                                          write1NoDeq);
  Value nextValid1 = builder.create<MuxPrimOp>(
      loc, arg(5), fireAndValid1,
      builder.create<OrPrimOp>(loc, fireAndValid0, valid1).getResult());
  builder.create<StrictConnectOp>(loc, valid1, nextValid1);
  builder.create<StrictConnectOp>(loc, bits1,
                                  builder.create<MuxPrimOp>(loc, write1,
                                                            enqBits, bits1));
  return success();
}

LogicalResult goldengate::addFAMEBoundaryPipeChannels(CircuitOp circuit,
                                                      std::string &error) {
  FModuleOp target;
  SmallVector<BoundaryPipe> channels;
  if (failed(findBoundaryPipes(circuit, target, channels, error)))
    return failure();
  std::map<std::string, std::pair<FIRRTLBaseType, unsigned>> definitions;
  for (const auto &channel : channels)
    definitions.emplace(pipeModuleName(channel.payload, channel.latency),
                        std::make_pair(channel.payload, channel.latency));
  // Check all symbol collisions before changing the circuit.
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleLike>(&op))
      for (const auto &[name, definition] : definitions)
        if (module.getModuleName() == name) {
          error = "PipeChannel module already exists: " +
                  module.getModuleName().str();
          return failure();
        }
  for (const auto &[name, definition] : definitions)
    if (failed(addFAMEPipeChannel(circuit, definition.first, definition.second,
                                  error)))
      return failure();
  return success();
}

LogicalResult goldengate::addFAMEPipeWrapper(CircuitOp circuit,
                                            std::string &error) {
  FModuleOp target;
  SmallVector<BoundaryPipe> channels;
  if (failed(findBoundaryPipes(circuit, target, channels, error)))
    return failure();
  SmallVector<FModuleOp> pipes;
  for (const auto &channel : channels) {
    FModuleOp pipe;
    for (Operation &op : circuit.getBodyBlock()->getOperations())
      if (auto module = dyn_cast<FModuleOp>(&op);
          module && module.getName() ==
                        pipeModuleName(channel.payload, channel.latency))
        pipe = module;
    if (!pipe) {
      error = "wrapper is missing a PipeChannel module for " + channel.name;
      return failure();
    }
    pipes.push_back(pipe);
  }
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleLike>(&op);
        module && module.getModuleName() == wrapperName) {
      error = "PipeChannel wrapper already exists";
      return failure();
    }
  std::optional<unsigned> clockPort, resetPort;
  for (unsigned i = 0, n = target.getPorts().size(); i < n; ++i) {
    auto name = target.getPortName(i);
    if (name == "hostClock")
      clockPort = i;
    else if (name == "hostReset")
      resetPort = i;
  }
  if (!clockPort || !resetPort ||
      target.getPortDirection(*clockPort) != Direction::In ||
      target.getPortDirection(*resetPort) != Direction::In ||
      !isa<ClockType>(target.getPortType(*clockPort)) ||
      target.getPortType(*resetPort) !=
          UIntType::get(circuit.getContext(), 1, false)) {
    error = "PipeChannel wrapper lacks host clock or reset controls";
    return failure();
  }
  auto targetPorts = target.getPorts();
  SmallVector<PortInfo> ports(targetPorts.begin(), targetPorts.end());
  OpBuilder builder(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  auto wrapper = builder.create<FModuleOp>(
      circuit.getLoc(), StringAttr::get(circuit.getContext(), wrapperName),
      target.getConventionAttr(), ports);
  builder.setInsertionPointToStart(wrapper.getBodyBlock());
  Location loc = wrapper.getLoc();
  auto child = builder.create<InstanceOp>(loc, target, "target_FAMETop");
  for (unsigned i = 0, n = ports.size(); i < n; ++i) {
    bool isChannelPort = false;
    for (const auto &channel : channels)
      isChannelPort |= i == channel.port;
    if (isChannelPort)
      continue;
    Value external = wrapper.getBodyBlock()->getArgument(i);
    Value internal = child.getResult(i);
    if (target.getPortDirection(i) == Direction::In)
      builder.create<ConnectOp>(loc, internal, external);
    else
      builder.create<ConnectOp>(loc, external, internal);
  }
  auto field = [&](Value bundle, llvm::StringRef name) {
    return builder.create<SubfieldOp>(loc, bundle, name).getResult();
  };
  for (auto [index, channel] : llvm::enumerate(channels)) {
    auto queue = builder.create<InstanceOp>(loc, pipes[index],
                                             "PipeChannel_" + channel.name);
    Value internal = child.getResult(channel.port);
    Value external = wrapper.getBodyBlock()->getArgument(channel.port);
    // A target source enqueues into the host queue. A bridge source enqueues
    // from the wrapper input, and the target sink dequeues the same queue.
    bool targetSource = target.getPortDirection(channel.port) == Direction::Out;
    Value source = targetSource ? internal : external;
    Value sink = targetSource ? external : internal;
    builder.create<ConnectOp>(loc, queue.getResult(0),
                              wrapper.getBodyBlock()->getArgument(*clockPort));
    builder.create<ConnectOp>(loc, queue.getResult(1),
                              wrapper.getBodyBlock()->getArgument(*resetPort));
    builder.create<ConnectOp>(loc, field(source, "ready"), queue.getResult(2));
    builder.create<ConnectOp>(loc, queue.getResult(3), field(source, "valid"));
    builder.create<ConnectOp>(loc, queue.getResult(4), field(source, "bits"));
    builder.create<ConnectOp>(loc, queue.getResult(5), field(sink, "ready"));
    builder.create<ConnectOp>(loc, field(sink, "valid"), queue.getResult(6));
    builder.create<ConnectOp>(loc, field(sink, "bits"), queue.getResult(7));
  }
  return success();
}

LogicalResult goldengate::activateFAMEPipeWrapper(CircuitOp circuit,
                                                 std::string &error) {
  std::string oldName = circuit.getName().str();
  if (oldName == wrapperName) {
    error = "PipeChannel wrapper is already active";
    return failure();
  }
  bool foundWrapper = false;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleLike>(&op);
        module && module.getModuleName() == wrapperName)
      foundWrapper = true;
  if (!foundWrapper) {
    error = "PipeChannel wrapper module is missing";
    return failure();
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "PipeChannel activation requires retained annotations";
    return failure();
  }
  auto *context = circuit.getContext();
  std::string oldCircuit = "~" + oldName;
  std::string newCircuit = "~" + wrapperName.str();
  std::string oldModule = "|" + oldName;
  std::string newModule = "|" + wrapperName.str();
  std::function<Attribute(Attribute, bool)> retarget =
      [&](Attribute attr, bool targetDomain) -> Attribute {
    if (auto string = dyn_cast<StringAttr>(attr)) {
      llvm::StringRef value = string.getValue();
      if (value == oldCircuit)
        return StringAttr::get(context, newCircuit);
      if (!value.starts_with(oldCircuit + "|"))
        return attr;
      std::string suffix = value.drop_front(oldName.size() + 1).str();
      if (suffix == oldModule)
        return StringAttr::get(context, newCircuit + newModule);
      if (!targetDomain && llvm::StringRef(suffix).starts_with(oldModule + ">"))
        suffix.replace(0, oldModule.size() + 1, newModule + ">");
      return StringAttr::get(context, newCircuit + suffix);
    }
    if (auto array = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values;
      for (Attribute value : array)
        values.push_back(retarget(value, targetDomain));
      return ArrayAttr::get(context, values);
    }
    if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
      SmallVector<NamedAttribute> values;
      Annotation annotation(dict);
      for (NamedAttribute value : dict)
        // A channel's associated clock names the retained target domain.
        // SimWrapper.genClockChannel changes the wrapper payload to Vec[Bool],
        // while the target still owns its scalar Clock / ClockRecord leaves.
        // Only boundary endpoints move to wrapper ports; moving a clock leaf
        // there either loses its Clock type or leaves an invalid record path.
        values.emplace_back(value.getName(), retarget(value.getValue(),
            targetDomain || (annotation.isClass(AnnotationClasses::ChannelConnection) &&
                             value.getName().getValue() == "clock")));
      return DictionaryAttr::get(context, values);
    }
    return attr;
  };
  SmallVector<Attribute> annotations;
  for (Attribute annotation : raw)
    annotations.push_back(retarget(annotation, false));
  circuit->setAttr("rawAnnotations", ArrayAttr::get(context, annotations));
  circuit.setName(wrapperName);
  return success();
}
