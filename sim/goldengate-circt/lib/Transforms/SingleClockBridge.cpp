// See LICENSE for license details.
// Required input invariants: active GGFAMEPipeWrapper with Boolean clock tokens,
// one retained ClockBridgeModule constructor and one matching clock channel.
// Annotations consumed: none; BridgeIO identity transfers to the producer hPort.
// Annotations produced: clock channel source at the producer; copied boundary
// ports transfer to the new top, while the clock sink remains on the inner top.
// IR mutations: add an ordered rational-clock producer, its decoded MCR bank,
// and an outer wrapper exposing that bank for subsequent control-bus mapping.
// Analyses required: retained target resolution. Analyses preserved: inner model,
// clock domain and channel identities, including clockInfo and perClockMFMR.
// Output invariants: always-valid ordered Vec[Bool] clock tokens, sync reset of
// 64-bit hCycle/tCycle; target progress counted only on accepted clock tokens.
// Unreset snapshots capture pre-edge counters on latch writes at words 2/5;
// words 0/1 and 3/4 read saved low/high halves. Permissions match MCRIO.bindReg.
// Scope: ClockBridge.scala rational scheduling and Widget.genWideRORegInit with
// 32-bit MCR words. Transport and driver emission are subsequent mapping steps.
#include "goldengate/SingleClockBridge.h"
#include "goldengate/RationalClockTokenGenerator.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addClockBridge(CircuitOp circuit,
                                              std::string &error) {
  constexpr llvm::StringLiteral producerName = "GGSingleClockBridge";
  constexpr llvm::StringLiteral wrapperName = "GGClockBridgeWrapper";
  constexpr llvm::StringLiteral controlName = "clockBridge_mcr";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGFAMEPipeWrapper")
    return reject("clock bridge requires the active FAME PipeChannel wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto module = dyn_cast<FModuleLike>(&op);
    if (!module) continue;
    if (module.getModuleName() == producerName || module.getModuleName() == wrapperName)
      return reject("clock bridge module or wrapper already exists");
    if (module.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("clock bridge requires a top and retained annotations");
  DictionaryAttr bridge, channel;
  unsigned bridgeIndex = 0, channelIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::BridgeIO) &&
        anno.getMember<StringAttr>("widgetClass") == "midas.widgets.ClockBridgeModule") {
      if (bridge) return reject("multiple ClockBridgeModule constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
    auto info = anno.getMember<DictionaryAttr>("channelInfo");
    if (anno.isClass(AnnotationClasses::ChannelConnection) && info &&
        info.getAs<StringAttr>("class") == AnnotationClasses::TargetClockChannel) {
      if (channel) return reject("multiple target clock channels are unsupported");
      channel = cast<DictionaryAttr>(attr); channelIndex = i;
    }
  }
  if (!bridge || !channel) return reject("clock bridge constructor or clock channel is missing");
  auto key = bridge.getAs<DictionaryAttr>("widgetConstructorKey");
  auto clocks = key ? key.getAs<ArrayAttr>("clocks") : ArrayAttr();
  auto info = channel.getAs<DictionaryAttr>("channelInfo");
  auto ratios = info.getAs<ArrayAttr>("perClockMFMR");
  auto mapping = bridge.getAs<DictionaryAttr>("channelMapping");
  auto name = channel.getAs<StringAttr>("globalName");
  if (!key || key.getAs<StringAttr>("class") != "firesim.lib.bridges.ClockParameters" ||
      !clocks || clocks.empty() || clocks != info.getAs<ArrayAttr>("clockInfo") ||
      !ratios || ratios.size() != clocks.size() || !name || !mapping ||
      mapping.size() != 1 || mapping.getAs<StringAttr>("clocks") != name)
    return reject("clock bridge needs matching ordered constructor clocks, channel and MFMRs");
  SmallVector<RationalClockInfo> clockInfo;
  unsigned fastestIndex = 0;
  double fastestFrequency = 0;
  for (auto [i, attr] : llvm::enumerate(clocks)) {
    auto clock = dyn_cast<DictionaryAttr>(attr);
    auto mult = clock ? clock.getAs<IntegerAttr>("multiplier") : IntegerAttr();
    auto div = clock ? clock.getAs<IntegerAttr>("divisor") : IntegerAttr();
    auto clockName = clock ? clock.getAs<StringAttr>("name") : StringAttr();
    auto mfmr = dyn_cast<IntegerAttr>(ratios[i]);
    if (!mult || !div || mult.getInt() <= 0 || div.getInt() <= 0 ||
        !clockName || !mfmr || mfmr.getInt() <= 0)
      return reject("clock bridge needs named positive rational clocks and MFMRs");
    clockInfo.push_back({clockName.getValue().str(), uint64_t(mult.getInt()),
                         uint64_t(div.getInt()), uint64_t(mfmr.getInt())});
    // Match ClockBridgeModule's Double frequency sort, including its last-lane
    // tie selection. Exact period comparison would change rounded Double ties.
    double frequency = double(mult.getInt()) / double(div.getInt());
    if (frequency >= fastestFrequency) {
      fastestFrequency = frequency;
      fastestIndex = i;
    }
  }
  auto schedule = analyzeRationalClockSchedule(clockInfo, error);
  if (!schedule) return failure();
  unsigned minPeriod = *llvm::min_element(schedule->periods);
  for (auto [i, period] : llvm::enumerate(schedule->periods))
    if (clockInfo[i].mfmr != (period + minPeriod - 1) / minPeriod)
      return reject("clock bridge MFMR does not match the ordered rational clock periods");
  auto sinks = channel.getAs<ArrayAttr>("sinks");
  auto sources = channel.getAs<ArrayAttr>("sources");
  if (!sinks || sinks.size() != clocks.size() || (sources && !sources.empty()) || channel.get("clock"))
    return reject("clock bridge needs one ordered boundary sink per clock");
  auto spelling = dyn_cast<StringAttr>(sinks[0]);
  auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
  if (!target || target->module != inner || !target->port)
    return reject("clock bridge sink must resolve to the active wrapper port");
  unsigned clockPort = *target->port;
  auto *context = circuit.getContext();
  auto bit = UIntType::get(context, 1, false);
  auto token = dyn_cast<BundleType>(inner.getPortType(clockPort));
  auto bits = token ? token.getElementIndex("bits") : std::nullopt;
  auto vector = bits ? dyn_cast<FVectorType>(token.getElements()[*bits].type) : FVectorType();
  if (!token || token.getElements().size() != 3 || !vector || vector.getNumElements() != clocks.size() ||
      vector.getElementType() != bit ||
      !token.getElement("ready") || !token.getElement("valid") ||
      token.getElement("ready")->type != bit || !token.getElement("ready")->isFlip ||
      token.getElement("valid")->type != bit || token.getElement("valid")->isFlip ||
      token.getElements()[*bits].isFlip || inner.getPortDirection(clockPort) != Direction::In)
    return reject("clock bridge needs a Decoupled Vec[Bool] sink matching clock count");
  for (auto [i, attr] : llvm::enumerate(sinks)) {
    auto spelling = dyn_cast<StringAttr>(attr);
    auto lane = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    if (!lane || lane->module != inner || lane->port != clockPort ||
        lane->fieldID != token.getFieldID(*bits) + vector.getFieldID(i))
      return reject("clock bridge sinks must resolve to bits[i] in constructor clock order");
  }
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name.getValue() == controlName)
      return reject("clock bridge decoded MCR boundary port already exists");
    if (port.name.getValue() == "hostClock" && port.direction == Direction::In && isa<ClockType>(port.type)) hostClock = i;
    if (port.name.getValue() == "hostReset" && port.direction == Direction::In && port.type == bit) hostReset = i;
  }
  if (!hostClock || !hostReset) return reject("clock bridge needs hostClock and synchronous hostReset inputs");
  bool instantiated = false;
  circuit.walk([&](InstanceOp i) { instantiated |= i.getModuleName() == inner.getName(); });
  if (instantiated) return reject("clock bridge requires an uninstantiated top wrapper");

  // Validate all schedule, metadata and endpoint contracts before mutation.
  // Keep the existing producer symbol for control/header mapping compatibility.
  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  Location loc = circuit.getLoc();
  auto wide = UIntType::get(context, 64, false);
  auto word = UIntType::get(context, 32, false);
  auto wordToken = BundleType::get(context, {
      {b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, word}});
  // The bridge sees the reverse of Lib.scala MCRFile.io.mcr: read sources,
  // write sinks, and incoming strobes (bindReg deliberately ignores wstrb).
  auto mcr = BundleType::get(context, {
      {b.getStringAttr("read"), false, FVectorType::get(wordToken, 6)},
      {b.getStringAttr("write"), true, FVectorType::get(wordToken, 6)},
      {b.getStringAttr("wstrb"), true, UIntType::get(context, 4, false)}});
  SmallVector<PortInfo> producerPorts{
      {b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("hPort"), token, Direction::Out},
      {b.getStringAttr("hCycle_value"), wide, Direction::Out},
      {b.getStringAttr("tCycle_value"), wide, Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto producer = b.create<FModuleOp>(loc, b.getStringAttr(producerName),
      ConventionAttr::get(context, Convention::Internal), producerPorts);
  producer->setAttr("goldengate.bridgeConstructor", key);
  // MCRFileMap.allocate assigns one four-byte word per entry in this order.
  SmallVector<Attribute> registerMap;
  for (auto [i, name] : llvm::enumerate(ArrayRef<llvm::StringRef>{
           "hCycle_0", "hCycle_1", "hCycle_latch", "tCycle_0", "tCycle_1", "tCycle_latch"})) {
    bool latch = i % 3 == 2;
    registerMap.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(name)),
        b.getNamedAttr("offset", b.getI32IntegerAttr(i * 4)),
        b.getNamedAttr("readable", b.getBoolAttr(!latch)),
        b.getNamedAttr("writeable", b.getBoolAttr(latch))}));
  }
  producer->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registerMap));
  b.setInsertionPointToStart(producer.getBodyBlock());
  auto arg = [&](unsigned i) { return producer.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) { return b.create<SubfieldOp>(loc, v, n).getResult(); };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  b.create<StrictConnectOp>(loc, field(arg(2), "valid"), one);
  auto edgeBits = buildRationalClockTokens(b, loc, arg(0), arg(1),
      field(arg(2), "ready"), *schedule);
  Value payload = field(arg(2), "bits");
  for (auto [i, edge] : llvm::enumerate(edgeBits))
    b.create<StrictConnectOp>(loc, b.create<SubindexOp>(loc, payload, i), edge);
  Value zero = b.create<ConstantOp>(loc, wide, APInt(64, 0));
  Value hCycle = b.create<RegResetOp>(loc, wide, arg(0), arg(1), zero, "hCycle").getResult();
  Value tCycle = b.create<RegResetOp>(loc, wide, arg(0), arg(1), zero, "tCycleFastest").getResult();
  Value increment = b.create<ConstantOp>(loc, wide, APInt(64, 1));
  auto next = [&](Value v) -> Value {
    Value sum = b.create<AddPrimOp>(loc, v, increment);
    return b.create<BitsPrimOp>(loc, sum, 63, 0);
  };
  Value fire = b.create<AndPrimOp>(loc, field(arg(2), "ready"), field(arg(2), "valid"));
  Value fastestFire = b.create<AndPrimOp>(loc, fire, edgeBits[fastestIndex]);
  b.create<StrictConnectOp>(loc, hCycle, next(hCycle));
  b.create<StrictConnectOp>(loc, tCycle, b.create<MuxPrimOp>(loc, fastestFire, next(tCycle), tCycle));
  b.create<StrictConnectOp>(loc, arg(3), hCycle);
  b.create<StrictConnectOp>(loc, arg(4), tCycle);

  Value assertionEnable = b.create<NotPrimOp>(loc, arg(1));
  Value wordZero = b.create<ConstantOp>(loc, word, APInt(32, 0));
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(5), group), i);
  };
  for (unsigned group = 0; group < 2; ++group) {
    Value counter = group == 0 ? hCycle : tCycle;
    Value shadow = b.create<RegOp>(loc, wide, arg(0),
        group == 0 ? "hCycle_mmreg" : "tCycle_mmreg").getResult();
    Value write = slot("write", group * 3 + 2);
    Value lowBit = b.create<BitsPrimOp>(loc, field(write, "bits"), 0, 0);
    Value latch = b.create<AndPrimOp>(loc, field(write, "valid"), lowBit);
    b.create<StrictConnectOp>(loc, shadow, b.create<MuxPrimOp>(loc, latch, counter, shadow));
    for (unsigned half = 0; half < 3; ++half) {
      unsigned i = group * 3 + half;
      Value read = slot("read", i), write = slot("write", i);
      b.create<StrictConnectOp>(loc, field(read, "valid"), one);
      b.create<StrictConnectOp>(loc, field(write, "ready"), one);
      Value data = half == 2 ? wordZero :
          b.create<BitsPrimOp>(loc, shadow, half * 32 + 31, half * 32).getResult();
      b.create<StrictConnectOp>(loc, field(read, "bits"), data);
      Value forbidden = field(half == 2 ? read : write, half == 2 ? "ready" : "valid");
      Value permitted = b.create<NotPrimOp>(loc, forbidden);
      b.create<AssertOp>(loc, arg(0), permitted, assertionEnable,
          half == 2 ? "ClockBridge register is write only" : "ClockBridge register is read only",
          ValueRange{}, "");
    }
  }

  SmallVector<PortInfo> ports;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != clockPort) ports.push_back(p);
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto clockBridge = b.create<InstanceOp>(loc, producer, "ClockBridgeModule_0");
  unsigned outerIndex = 0;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (i == clockPort) continue;
    Value external = wrapper.getBodyBlock()->getArgument(outerIndex++);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto externalHost = [&](unsigned index) {
    return wrapper.getBodyBlock()->getArgument(index - unsigned(index > clockPort));
  };
  b.create<StrictConnectOp>(loc, clockBridge.getResult(0), externalHost(*hostClock));
  b.create<StrictConnectOp>(loc, clockBridge.getResult(1), externalHost(*hostReset));
  b.create<ConnectOp>(loc, sim.getResult(clockPort), clockBridge.getResult(2));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), clockBridge.getResult(5));

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
          copiedPort |= i != clockPort && localName == p.name.getValue();
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
  SmallVector<Attribute> clockSources;
  for (unsigned i = 0; i < clocks.size(); ++i)
    clockSources.push_back(b.getStringAttr(newPrefix + "|" + producerName.str() +
                                          ">hPort.bits[" + std::to_string(i) + "]"));
  newChannel.set("sources", b.getArrayAttr(clockSources));
  annotations[channelIndex] = newChannel.getDictionary(context);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  circuit.setName(wrapperName);
  return success();
}
