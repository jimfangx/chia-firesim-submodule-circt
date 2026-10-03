// See LICENSE for license details.
// Port ResetPulseBridgeModule and Widget.genWOReg{Init} through FIRRTL ops.
// Inputs: active simulator wrapper, one retained reset bridge constructor and
// zero-latency PipeChannel sink. The constructor and channel class survive.
// Output: bridge token source replaces the external reset-token input; a new
// outer wrapper exposes two decoded 32-bit MCR words for later bus mapping.
// pulseLength is unreset. doneInit resets synchronously. Tokens alone decrement
// the remaining length; an MCR write wins over a simultaneous accepted token.
// Despite their Scala helper names, genWOReg{Init} attach ReadWrite registers.
// Driver headers and Nasti transport for this bridge remain subsequent steps.
#include "goldengate/ResetPulseBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addResetPulseBridge(CircuitOp circuit,
                                             std::string &error) {
  constexpr llvm::StringLiteral producerName = "GGResetPulseBridge";
  constexpr llvm::StringLiteral wrapperName = "GGResetPulseBridgeWrapper";
  constexpr llvm::StringLiteral controlName = "resetBridge_mcr";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGClockBridgeControlWrapper")
    return reject("reset pulse bridge requires the active ClockBridge control wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto module = dyn_cast<FModuleLike>(&op);
    if (!module) continue;
    if (module.getModuleName() == producerName || module.getModuleName() == wrapperName)
      return reject("reset pulse bridge module or wrapper already exists");
    if (module.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("reset pulse bridge requires a top and retained annotations");
  DictionaryAttr bridge, channel;
  unsigned bridgeIndex = 0, channelIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::BridgeIO) &&
        anno.getMember<StringAttr>("widgetClass") == "midas.widgets.ResetPulseBridgeModule") {
      if (bridge) return reject("multiple ResetPulseBridgeModule constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  if (!bridge) return reject("reset pulse bridge constructor is missing");
  auto key = bridge.getAs<DictionaryAttr>("widgetConstructorKey");
  auto activeHigh = key ? key.getAs<BoolAttr>("activeHigh") : BoolAttr();
  auto maximum = key ? key.getAs<IntegerAttr>("maxPulseLength") : IntegerAttr();
  auto initial = key ? key.getAs<IntegerAttr>("defaultPulseLength") : IntegerAttr();
  auto mapping = bridge.getAs<DictionaryAttr>("channelMapping");
  auto channelName = mapping ? mapping.getAs<StringAttr>("reset") : StringAttr();
  // Restrict to representable positive Scala Int parameters and a nonzero
  // storage width; no clamp is applied to runtime writes, as in genWOReg.
  if (!key || key.getAs<StringAttr>("class") != "firesim.lib.bridges.ResetPulseBridgeParameters" ||
      !activeHigh || !maximum || !initial || maximum.getInt() <= 0 ||
      maximum.getInt() >= INT32_MAX || initial.getInt() < 0 ||
      initial.getInt() > maximum.getInt() || !mapping || mapping.size() != 1 || !channelName)
    return reject("reset pulse bridge needs valid Boolean polarity, pulse lengths and one reset channel");
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::ChannelConnection) &&
        anno.getMember<StringAttr>("globalName") == channelName) {
      if (channel) return reject("duplicate reset pulse bridge channel");
      channel = cast<DictionaryAttr>(attr); channelIndex = i;
    }
  }
  auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
  auto latency = info ? info.getAs<IntegerAttr>("latency") : IntegerAttr();
  auto sinks = channel ? channel.getAs<ArrayAttr>("sinks") : ArrayAttr();
  auto sources = channel ? channel.getAs<ArrayAttr>("sources") : ArrayAttr();
  if (!info || info.getAs<StringAttr>("class") != AnnotationClasses::PipeChannel ||
      !latency || latency.getInt() != 0 || !sinks || sinks.size() != 1 ||
      (sources && !sources.empty()))
    return reject("reset pulse bridge needs one zero-latency boundary PipeChannel sink");
  auto spelling = dyn_cast<StringAttr>(sinks[0]);
  auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
  if (!target || target->module != inner || !target->port)
    return reject("reset pulse bridge sink must resolve to the active wrapper port");
  unsigned resetPort = *target->port;
  auto *context = circuit.getContext();
  auto bit = UIntType::get(context, 1, false);
  auto token = dyn_cast<BundleType>(inner.getPortType(resetPort));
  auto payload = token ? token.getElementIndex("bits") : std::nullopt;
  if (!token || token.getElements().size() != 3 || !payload ||
      token.getElements()[*payload].type != bit || token.getElements()[*payload].isFlip ||
      target->fieldID != token.getFieldID(*payload) ||
      !token.getElement("ready") || !token.getElement("valid") ||
      token.getElement("ready")->type != bit || !token.getElement("ready")->isFlip ||
      token.getElement("valid")->type != bit || token.getElement("valid")->isFlip ||
      inner.getPortDirection(resetPort) != Direction::In)
    return reject("reset pulse bridge needs a Decoupled Bool input sink");
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name.getValue() == controlName)
      return reject("reset pulse bridge decoded MCR boundary port already exists");
    if (port.name.getValue() == "hostClock" && port.direction == Direction::In && isa<ClockType>(port.type)) hostClock = i;
    if (port.name.getValue() == "hostReset" && port.direction == Direction::In && port.type == bit) hostReset = i;
  }
  if (!hostClock || !hostReset) return reject("reset pulse bridge needs hostClock and synchronous hostReset inputs");
  bool instantiated = false;
  circuit.walk([&](InstanceOp i) { instantiated |= i.getModuleName() == inner.getName(); });
  if (instantiated) return reject("reset pulse bridge requires an uninstantiated top wrapper");

  // All invariants above are checked before mutation.
  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  Location loc = circuit.getLoc();
  auto word = UIntType::get(context, 32, false);
  auto wordToken = BundleType::get(context, {
      {b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, word}});
  auto mcr = BundleType::get(context, {
      {b.getStringAttr("read"), false, FVectorType::get(wordToken, 2)},
      {b.getStringAttr("write"), true, FVectorType::get(wordToken, 2)},
      {b.getStringAttr("wstrb"), true, UIntType::get(context, 4, false)}});
  auto hPort = BundleType::get(context, {{b.getStringAttr("reset"), false, token}});
  SmallVector<PortInfo> producerPorts{
      {b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("hPort"), hPort, Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto producer = b.create<FModuleOp>(loc, b.getStringAttr(producerName),
      ConventionAttr::get(context, Convention::Internal), producerPorts);
  producer->setAttr("goldengate.bridgeConstructor", key);
  SmallVector<Attribute> registers;
  for (auto [i, name] : llvm::enumerate(ArrayRef<llvm::StringRef>{"pulseLength", "doneInit"}))
    registers.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(name)),
        b.getNamedAttr("offset", b.getI32IntegerAttr(i * 4)),
        b.getNamedAttr("readable", b.getBoolAttr(true)),
        b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  producer->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(producer.getBodyBlock());
  auto arg = [&](unsigned i) { return producer.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) { return b.create<SubfieldOp>(loc, v, n).getResult(); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(3), group), i);
  };
  unsigned width = llvm::Log2_64_Ceil(uint64_t(maximum.getInt()) + 1);
  auto pulseType = UIntType::get(context, width, false);
  Value pulse = b.create<RegOp>(loc, pulseType, arg(0), "pulseLength").getResult();
  Value zeroBit = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value oneBit = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value initialized = b.create<RegResetOp>(loc, bit, arg(0), arg(1), zeroBit, "doneInit").getResult();
  Value resetToken = field(arg(2), "reset");
  Value zero = b.create<ConstantOp>(loc, pulseType, APInt(width, 0));
  Value complete = b.create<EQPrimOp>(loc, pulse, zero);
  Value polarity = b.create<ConstantOp>(loc, bit, APInt(1, activeHigh.getValue()));
  b.create<StrictConnectOp>(loc, field(resetToken, "valid"), initialized);
  b.create<StrictConnectOp>(loc, field(resetToken, "bits"), b.create<XorPrimOp>(loc, complete, polarity));
  Value fire = b.create<AndPrimOp>(loc, field(resetToken, "ready"), initialized);
  Value one = b.create<ConstantOp>(loc, pulseType, APInt(width, 1));
  Value difference = b.create<SubPrimOp>(loc, pulse, one);
  Value decrement = b.create<BitsPrimOp>(loc, difference, width - 1, 0);
  Value counted = b.create<MuxPrimOp>(loc, complete, zero, decrement);
  Value advanced = b.create<MuxPrimOp>(loc, fire, counted, pulse);
  Value pulseWrite = slot("write", 0), initWrite = slot("write", 1);
  Value writtenPulse = b.create<BitsPrimOp>(loc, field(pulseWrite, "bits"), width - 1, 0);
  Value writtenInit = b.create<BitsPrimOp>(loc, field(initWrite, "bits"), 0, 0);
  b.create<StrictConnectOp>(loc, pulse, b.create<MuxPrimOp>(loc, field(pulseWrite, "valid"), writtenPulse, advanced));
  b.create<StrictConnectOp>(loc, initialized, b.create<MuxPrimOp>(loc, field(initWrite, "valid"), writtenInit, initialized));
  for (unsigned i = 0; i < 2; ++i) {
    Value read = slot("read", i), write = slot("write", i);
    b.create<StrictConnectOp>(loc, field(read, "valid"), oneBit);
    b.create<StrictConnectOp>(loc, field(write, "ready"), oneBit);
    Value data = b.create<PadPrimOp>(loc, i == 0 ? pulse : initialized, 32);
    b.create<StrictConnectOp>(loc, field(read, "bits"), data);
  }

  SmallVector<PortInfo> ports;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != resetPort) ports.push_back(p);
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto resetBridge = b.create<InstanceOp>(loc, producer, "ResetPulseBridgeModule_0");
  unsigned outerIndex = 0;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (i == resetPort) continue;
    Value external = wrapper.getBodyBlock()->getArgument(outerIndex++);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto externalHost = [&](unsigned index) {
    return wrapper.getBodyBlock()->getArgument(index - unsigned(index > resetPort));
  };
  b.create<StrictConnectOp>(loc, resetBridge.getResult(0), externalHost(*hostClock));
  b.create<StrictConnectOp>(loc, resetBridge.getResult(1), externalHost(*hostReset));
  b.create<ConnectOp>(loc, sim.getResult(resetPort), field(resetBridge.getResult(2), "reset"));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), resetBridge.getResult(3));

  std::string oldPrefix = "~" + circuit.getName().str();
  std::string newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue();
      if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str();
      llvm::StringRef ref(suffix);
      bool copiedPort = false;
      if (ref.consume_front(modulePrefix)) {
        auto localName = ref.take_front(ref.find_first_of(".["));
        for (auto [i, p] : llvm::enumerate(inner.getPorts()))
          copiedPort |= i != resetPort && localName == p.name.getValue();
      }
      if (copiedPort)
        suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">");
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v));
      return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue()));
      return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations;
  for (auto attr : raw) annotations.push_back(retarget(attr));
  NamedAttrList newBridge(cast<DictionaryAttr>(annotations[bridgeIndex]));
  newBridge.set("target", b.getStringAttr(newPrefix + "|" + producerName.str() + ">hPort"));
  annotations[bridgeIndex] = newBridge.getDictionary(context);
  NamedAttrList newChannel(cast<DictionaryAttr>(annotations[channelIndex]));
  newChannel.set("sources", b.getArrayAttr({b.getStringAttr(newPrefix + "|" + producerName.str() + ">hPort.reset.bits")}));
  annotations[channelIndex] = newChannel.getDictionary(context);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  circuit.setName(wrapperName);
  return success();
}
