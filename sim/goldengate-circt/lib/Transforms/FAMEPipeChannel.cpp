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
  unsigned sourcePort; // Shared target output or primary bridge input.
  bool targetSource;
  bool loopback;
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
  std::set<unsigned> usedSinks;
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
    if (!hasSource && !hasSink) {
      error = "PipeChannel " + name.getValue().str() + " has no endpoint";
      return failure();
    }
    auto resolvePort = [&](ArrayAttr endpoints, Direction direction,
                           unsigned &port, FIRRTLBaseType &payload) -> LogicalResult {
      if (endpoints.size() != 1) {
        error = "PipeChannel " + name.getValue().str() +
                " requires one complete payload per endpoint";
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
      port = *target->port;
      auto type = dyn_cast<BundleType>(top.getPortType(port));
      auto ready = type ? type.getElementIndex("ready") : std::nullopt;
      auto valid = type ? type.getElementIndex("valid") : std::nullopt;
      auto bits = type ? type.getElementIndex("bits") : std::nullopt;
      payload = bits ? type.getElements()[*bits].type : FIRRTLBaseType();
      if (!type || type.getElements().size() != 3 || !ready || !valid ||
          !bits || !isPayloadType(payload) || !getBitWidth(payload) ||
          !type.getElements()[*ready].isFlip ||
          type.getElements()[*valid].isFlip || type.getElements()[*bits].isFlip ||
          type.getElements()[*ready].type != bit ||
          type.getElements()[*valid].type != bit ||
          target->fieldID != type.getFieldID(*bits) ||
          top.getPortDirection(port) != direction) {
        error = "PipeChannel " + name.getValue().str() +
                " requires a passive integer Decoupled payload target with matching direction";
        return failure();
      }
      return success();
    };
    unsigned sourcePort = 0, sinkPort = 0;
    FIRRTLBaseType sourceType, sinkType;
    if ((hasSource && failed(resolvePort(sources, Direction::Out, sourcePort, sourceType))) ||
        (hasSink && failed(resolvePort(sinks, Direction::In, sinkPort, sinkType))))
      return failure();
    if ((hasSource && hasSink && sourceType != sinkType) ||
        (hasSink && !usedSinks.insert(sinkPort).second) ||
        !usedNames.insert(name.getValue().str()).second) {
      error = "PipeChannel " + name.getValue().str() +
              " has incompatible endpoints or repeats a sink or name";
      return failure();
    }
    unsigned port = hasSink ? sinkPort : sourcePort;
    pipes.push_back({name.getValue().str(), port, hasSource ? sourceType : sinkType,
                     static_cast<unsigned>(latency.getUInt()),
                     hasSource ? sourcePort : port, hasSource, hasSource && hasSink});
  }
  // SimWrapper forks one source into independent queues. Bridge sources
  // select the first sink's external input; target sources share a live port.
  std::map<std::string, unsigned> byName;
  for (auto [i, pipe] : llvm::enumerate(pipes)) byName.emplace(pipe.name, i);
  std::set<unsigned> grouped;
  std::map<unsigned, unsigned> groupPrimary;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::ChannelFanout))
      continue;
    auto names = annotation.getMember<ArrayAttr>("channelNames");
    if (!names || names.empty()) {
      error = "PipeChannel fanout needs nonempty channelNames";
      return failure();
    }
    SmallVector<unsigned> members;
    for (Attribute name : names) {
      auto spelling = dyn_cast<StringAttr>(name);
      auto found = spelling ? byName.find(spelling.getValue().str()) : byName.end();
      if (found == byName.end() || !grouped.insert(found->second).second) {
        error = "PipeChannel fanout has a missing or repeated channel";
        return failure();
      }
      members.push_back(found->second);
    }
    auto &primary = pipes[members.front()];
    for (unsigned index : members) {
      auto &pipe = pipes[index];
      if (pipe.targetSource != primary.targetSource ||
          pipe.payload != primary.payload ||
          (pipe.targetSource && pipe.sourcePort != primary.sourcePort)) {
        error = "PipeChannel fanout needs one source direction, identical payloads and a shared target source";
        return failure();
      }
      groupPrimary[index] = members.front();
      if (!pipe.targetSource) pipe.sourcePort = primary.port;
    }
  }
  std::map<unsigned, SmallVector<unsigned>> targetGroups;
  for (auto [index, pipe] : llvm::enumerate(pipes))
    if (pipe.targetSource) targetGroups[pipe.sourcePort].push_back(index);
  for (const auto &[port, members] : targetGroups) {
    unsigned externalSinks = 0;
    for (unsigned index : members) {
      const auto &pipe = pipes[index];
      externalSinks += !pipe.loopback;
      if (members.size() > 1 &&
          (!grouped.count(index) || !grouped.count(members.front()) ||
           groupPrimary.at(index) != groupPrimary.at(members.front()))) {
        error = "shared target PipeChannel source needs a fanout annotation";
        return failure();
      }
    }
    // Scala ChannelizedWrapperIO deduplicates identical source targets. Only
    // one queue may drive that external output; other queues must feed models.
    if (externalSinks > 1) {
      error = "target PipeChannel fanout supports at most one bridge sink";
      return failure();
    }
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
    // The wrapper's positional connects are the SimWrapper PipeChannel ABI.
    // A symbol match alone is insufficient: preflight the complete typed
    // interface before creating any wrapper or instance operation.
    auto *context = circuit.getContext();
    auto bit = UIntType::get(context, 1, false);
    SmallVector<Type> types{ClockType::get(context), bit, bit, bit,
                           channel.payload, bit, bit, channel.payload};
    StringRef names[] = {"clock", "reset", "io_in_ready", "io_in_valid",
                         "io_in_bits", "io_out_ready", "io_out_valid",
                         "io_out_bits"};
    bool compatible = pipe.getNumPorts() == types.size();
    for (unsigned i = 0; compatible && i < types.size(); ++i)
      compatible = pipe.getPortName(i) == names[i] &&
          pipe.getPortType(i) == types[i] &&
          pipe.getPortDirection(i) ==
              ((i == 2 || i == 6 || i == 7) ? Direction::Out : Direction::In);
    if (!compatible) {
      error = "PipeChannel interface differs from boundary payload for " +
              channel.name;
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
  std::set<unsigned> secondaryPorts;
  std::set<unsigned> exposedSources;
  for (const auto &channel : channels) {
    if (channel.targetSource && !channel.loopback)
      exposedSources.insert(channel.sourcePort);
    if (channel.loopback ||
        (!channel.targetSource && channel.port != channel.sourcePort))
      secondaryPorts.insert(channel.port);
  }
  for (const auto &channel : channels)
    if (channel.targetSource && !exposedSources.count(channel.sourcePort))
      secondaryPorts.insert(channel.sourcePort);
  SmallVector<PortInfo> ports;
  std::map<unsigned, unsigned> wrapperPorts;
  for (auto [i, port] : llvm::enumerate(targetPorts)) {
    if (secondaryPorts.count(i)) continue;
    wrapperPorts.emplace(i, ports.size());
    ports.push_back(port);
  }
  OpBuilder builder(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  auto wrapper = builder.create<FModuleOp>(
      circuit.getLoc(), StringAttr::get(circuit.getContext(), wrapperName),
      target.getConventionAttr(), ports);
  builder.setInsertionPointToStart(wrapper.getBodyBlock());
  Location loc = wrapper.getLoc();
  auto child = builder.create<InstanceOp>(loc, target, "target_FAMETop");
  for (unsigned i = 0, n = targetPorts.size(); i < n; ++i) {
    bool isChannelPort = false;
    for (const auto &channel : channels)
      isChannelPort |= i == channel.port || i == channel.sourcePort;
    if (isChannelPort)
      continue;
    Value external = wrapper.getBodyBlock()->getArgument(wrapperPorts.at(i));
    Value internal = child.getResult(i);
    if (target.getPortDirection(i) == Direction::In)
      builder.create<ConnectOp>(loc, internal, external);
    else
      builder.create<ConnectOp>(loc, external, internal);
  }
  auto field = [&](Value bundle, llvm::StringRef name) {
    return builder.create<SubfieldOp>(loc, bundle, name).getResult();
  };
  SmallVector<InstanceOp> queues;
  std::map<unsigned, SmallVector<unsigned>> groups;
  for (auto [index, channel] : llvm::enumerate(channels)) {
    auto queue = builder.create<InstanceOp>(loc, pipes[index],
                                             "PipeChannel_" + channel.name);
    queues.push_back(queue);
    groups[channel.sourcePort].push_back(index);
    Value internal = child.getResult(channel.port);
    Value sink = channel.targetSource && !channel.loopback
                     ? wrapper.getArgument(wrapperPorts.at(channel.port))
                     : internal;
    builder.create<ConnectOp>(loc, queue.getResult(0),
                              wrapper.getArgument(wrapperPorts.at(*clockPort)));
    builder.create<ConnectOp>(loc, queue.getResult(1),
                              wrapper.getArgument(wrapperPorts.at(*resetPort)));
    builder.create<ConnectOp>(loc, queue.getResult(5), field(sink, "ready"));
    builder.create<ConnectOp>(loc, field(sink, "valid"), queue.getResult(6));
    builder.create<ConnectOp>(loc, field(sink, "bits"), queue.getResult(7));
  }
  for (const auto &[port, members] : groups) {
    Value source = target.getPortDirection(port) == Direction::Out
                       ? child.getResult(port)
                       : wrapper.getArgument(wrapperPorts.at(port));
    Value sourceValid = field(source, "valid");
    Value sourceBits = field(source, "bits");
    Value ready;
    for (unsigned index : members) {
      Value laneReady = queues[index].getResult(2);
      ready = ready ? builder.create<AndPrimOp>(loc, ready, laneReady).getResult()
                    : laneReady;
    }
    builder.create<ConnectOp>(loc, field(source, "ready"), ready);
    for (unsigned index : members) {
      // DecoupledHelper.fire(q.ready) excludes that queue's own ready.
      // Every enqueue handshake is consequently the same atomic broadcast,
      // without creating a ready-to-valid feedback dependency on that queue.
      Value valid = sourceValid;
      for (unsigned peer : members)
        if (peer != index)
          valid = builder.create<AndPrimOp>(loc, valid, queues[peer].getResult(2));
      builder.create<ConnectOp>(loc, queues[index].getResult(3), valid);
      builder.create<ConnectOp>(loc, queues[index].getResult(4), sourceBits);
    }
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
  FModuleLike wrapper;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleLike>(&op);
        module && module.getModuleName() == wrapperName)
      wrapper = module;
  if (!wrapper) {
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
  auto externalRenames = wrapper->getAttrOfType<DictionaryAttr>("goldengate.externalTargetRenames");
  std::set<std::string> innerOnlyPorts;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == oldName)
      for (auto port : module.getPorts())
        if (!llvm::any_of(wrapper.getPorts(), [&](const PortInfo &external) {
              return external.name == port.name;
            })) innerOnlyPorts.insert(port.name.getValue().str());
  std::set<std::string> modelSources;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    auto sources = annotation.getMember<ArrayAttr>("sources");
    auto sinks = annotation.getMember<ArrayAttr>("sinks");
    if (annotation.isClass(AnnotationClasses::ChannelConnection) &&
        sources && sinks && !sources.empty() && !sinks.empty())
      for (Attribute source : sources)
        if (auto spelling = dyn_cast<StringAttr>(source))
          modelSources.insert(spelling.getValue().str());
  }
  std::function<Attribute(Attribute, bool)> retarget =
      [&](Attribute attr, bool targetDomain) -> Attribute {
    if (auto string = dyn_cast<StringAttr>(attr)) {
      if (!targetDomain && externalRenames)
        if (auto replacement = externalRenames.getAs<StringAttr>(string.getValue()))
          string = replacement;
      llvm::StringRef value = string.getValue();
      if (value == oldCircuit)
        return StringAttr::get(context, newCircuit);
      if (!value.starts_with(oldCircuit + "|"))
        return attr;
      std::string suffix = value.drop_front(oldName.size() + 1).str();
      if (suffix == oldModule)
        return StringAttr::get(context, newCircuit + newModule);
      if (!targetDomain && llvm::StringRef(suffix).starts_with(oldModule + ">")) {
        auto reference = llvm::StringRef(suffix).drop_front(oldModule.size() + 1);
        auto portName = reference.take_front(reference.find_first_of(".["));
        bool exposed = !innerOnlyPorts.count(portName.str());
        // Secondary fanout sinks stay on the retained inner target. They are
        // fed by independent queues, not external wrapper inputs.
        if (exposed) suffix.replace(0, oldModule.size() + 1, newModule + ">");
      }
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
      auto sources = annotation.getMember<ArrayAttr>("sources");
      auto sinks = annotation.getMember<ArrayAttr>("sinks");
      bool loopback = annotation.isClass(AnnotationClasses::ChannelConnection) &&
                      sources && !sources.empty() && sinks && !sinks.empty();
      bool sharedModelSource = sources && llvm::any_of(sources, [&](Attribute source) {
        auto spelling = dyn_cast<StringAttr>(source);
        return spelling && modelSources.count(spelling.getValue().str());
      });
      for (NamedAttribute value : dict)
        // A channel's associated clock names the retained target domain.
        // SimWrapper.genClockChannel changes the wrapper payload to Vec[Bool],
        // while the target still owns its scalar Clock / ClockRecord leaves.
        // Shared model sources stay upstream of queues on the retained target;
        // their exposed wrapper port is a queue output, a different endpoint.
        // Moving a clock leaf loses its Clock type or invalidates a record path.
        values.emplace_back(value.getName(), retarget(value.getValue(),
            targetDomain || (annotation.isClass(AnnotationClasses::ChannelConnection) &&
                (value.getName().getValue() == "clock" ||
                 (loopback && value.getName().getValue() == "sinks") ||
                 (sharedModelSource && value.getName().getValue() == "sources")))));
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
