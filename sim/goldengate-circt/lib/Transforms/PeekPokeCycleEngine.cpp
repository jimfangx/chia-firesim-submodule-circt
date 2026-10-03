// See LICENSE for license details.
// PeekPokeIO.bindInputs and cycle-step scheduling through native FIRRTL ops.
// Rocket scope: one reset poke, no peeks, maxChannelDecoupling=2. Control
// boundary is the STEP queue dequeue, poke snoop and unreset reset-value
// register. These remain explicit until the queue and MMIO bank are ported.
#include "goldengate/PeekPokeCycleEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addPeekPokeCycleEngine(CircuitOp circuit,
                                                std::string &error) {
  constexpr llvm::StringLiteral producerName = "GGPeekPokeCycleEngine";
  constexpr llvm::StringLiteral wrapperName = "GGPeekPokeCycleWrapper";
  constexpr llvm::StringLiteral controlName = "peekPokeBridge_cycle";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGUARTBridgeControlWrapper")
    return reject("PeekPoke cycle engine requires the active UART control wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto module = dyn_cast<FModuleLike>(&op);
    if (!module) continue;
    if (module.getModuleName() == producerName || module.getModuleName() == wrapperName)
      return reject("PeekPoke cycle engine module or wrapper already exists");
    if (module.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("PeekPoke cycle engine requires a top and retained annotations");
  DictionaryAttr bridge, channel;
  unsigned bridgeIndex = 0, channelIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::BridgeIO) &&
        anno.getMember<StringAttr>("widgetClass") == "midas.widgets.PeekPokeBridgeModule") {
      if (bridge) return reject("multiple PeekPokeBridgeModule constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  if (!bridge) return reject("PeekPoke cycle engine constructor is missing");
  auto key = bridge.getAs<DictionaryAttr>("widgetConstructorKey");
  auto maximum = key ? key.getAs<IntegerAttr>("maxChannelDecoupling") : IntegerAttr();
  auto pokes = key ? key.getAs<ArrayAttr>("pokes") : ArrayAttr();
  auto peeks = key ? key.getAs<ArrayAttr>("peeks") : ArrayAttr();
  auto poke = pokes && pokes.size() == 1 ? dyn_cast<DictionaryAttr>(pokes[0]) : DictionaryAttr();
  auto width = poke ? poke.getAs<IntegerAttr>("fieldWidth") : IntegerAttr();
  auto type = poke ? poke.getAs<DictionaryAttr>("tpe") : DictionaryAttr();
  auto mapping = bridge.getAs<DictionaryAttr>("channelMapping");
  auto channelName = mapping ? mapping.getAs<StringAttr>("reset") : StringAttr();
  // This stage implements the Rocket constructor. More channels need the
  // reduction across their decoupling flags, rather than a single flag.
  if (!key || key.getAs<StringAttr>("class") != "firesim.lib.bridges.PeekPokeKey" ||
      !maximum || maximum.getInt() != 2 || !peeks || !peeks.empty() ||
      !poke || poke.getAs<StringAttr>("name") != "reset" || !width || width.getInt() != 1 ||
      !type || type.getAs<StringAttr>("typeString") != "UInt" ||
      !mapping || mapping.size() != 1 || !channelName)
    return reject("PeekPoke cycle engine requires one UInt1 reset poke, no peeks, and decoupling 2");
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::ChannelConnection) &&
        anno.getMember<StringAttr>("globalName") == channelName) {
      if (channel) return reject("duplicate PeekPoke cycle engine channel");
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
    return reject("PeekPoke cycle engine needs one zero-latency boundary PipeChannel sink");
  auto spelling = dyn_cast<StringAttr>(sinks[0]);
  auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
  if (!target || target->module != inner || !target->port)
    return reject("PeekPoke cycle engine sink must resolve to the active wrapper port");
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
    return reject("PeekPoke cycle engine needs a Decoupled Bool input sink");
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name.getValue() == controlName)
      return reject("PeekPoke cycle engine cycle boundary port already exists");
    if (port.name.getValue() == "hostClock" && port.direction == Direction::In && isa<ClockType>(port.type)) hostClock = i;
    if (port.name.getValue() == "hostReset" && port.direction == Direction::In && port.type == bit) hostReset = i;
  }
  if (!hostClock || !hostReset) return reject("PeekPoke cycle engine needs hostClock and synchronous hostReset inputs");
  bool instantiated = false;
  circuit.walk([&](InstanceOp i) { instantiated |= i.getModuleName() == inner.getName(); });
  if (instantiated) return reject("PeekPoke cycle engine requires an uninstantiated top wrapper");

  // All invariants above are checked before mutation.
  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  Location loc = circuit.getLoc();
  auto word = UIntType::get(context, 32, false);
  auto wordToken = BundleType::get(context, {
      {b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, word}});
  // STEP is the dequeue side of Widget.genAndAttachQueue. Poke and bits
  // are the write-valid snoop and unreset data register of bindInputs.
  // The MMIO bank and STEP queue are subsequent porting stages.
  auto control = BundleType::get(context, {
      {b.getStringAttr("step"), true, wordToken},
      {b.getStringAttr("poke"), true, bit},
      {b.getStringAttr("resetValue"), true, bit},
      {b.getStringAttr("tCycle"), false, UIntType::get(context, 64, false)},
      {b.getStringAttr("done"), false, bit},
      {b.getStringAttr("precisePeekable"), false, bit}});
  auto hPort = BundleType::get(context, {{b.getStringAttr("reset"), false, token}});
  SmallVector<PortInfo> producerPorts{
      {b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("hPort"), hPort, Direction::Out},
      {b.getStringAttr("control"), control, Direction::Out}};
  auto producer = b.create<FModuleOp>(loc, b.getStringAttr(producerName),
      ConventionAttr::get(context, Convention::Internal), producerPorts);
  producer->setAttr("goldengate.bridgeConstructor", key);
  b.setInsertionPointToStart(producer.getBodyBlock());
  auto arg = [&](unsigned i) { return producer.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto constant = [&](unsigned w, uint64_t value) -> Value {
    return b.create<ConstantOp>(loc, uint(w), APInt(w, value));
  };
  auto reg = [&](unsigned w, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), constant(w, 0), name).getResult();
  };
  auto mux = [&](Value cond, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, cond, yes, no); };
  auto andv = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto notv = [&](Value a) -> Value { return b.create<NotPrimOp>(loc, a); };
  auto truncated = [&](Value v, unsigned w) -> Value { return b.create<BitsPrimOp>(loc, v, w - 1, 0); };
  Value horizon = reg(32, "cycleHorizon"), cycle = reg(64, "tCycle");
  Value ahead = reg(2, "cyclesAhead"), words = reg(1, "wordsReceived");
  Value resetToken = field(arg(2), "reset"), step = field(arg(3), "step");
  Value done = b.create<EQPrimOp>(loc, horizon, constant(32, 0));
  Value full = b.create<GEQPrimOp>(loc, ahead, constant(2, 2));
  Value empty = b.create<EQPrimOp>(loc, ahead, constant(2, 0));
  Value withinHorizon = b.create<LTPrimOp>(loc, ahead, horizon);
  Value valid = b.create<OrPrimOp>(loc, andv(notv(full), withinHorizon), words);
  Value fire = andv(field(resetToken, "ready"), valid);
  Value isAhead = b.create<OrPrimOp>(loc, notv(empty), fire);
  Value advance = andv(isAhead, notv(done));
  // Saturation is independent of the poke bypass. At full, a poked token may
  // still fire, but inc+dec together hold the counter even in that case.
  Value inc = andv(andv(fire, notv(advance)), notv(full));
  Value dec = andv(andv(notv(fire), advance), notv(empty));
  Value increment = truncated(b.create<AddPrimOp>(loc, ahead, constant(2, 1)), 2);
  Value decrement = truncated(b.create<SubPrimOp>(loc, ahead, constant(2, 1)), 2);
  connect(ahead, mux(inc, increment, mux(dec, decrement, ahead)));
  Value received = truncated(b.create<AddPrimOp>(loc, words, constant(1, 1)), 1);
  connect(words, mux(field(arg(3), "poke"), received, mux(fire, constant(1, 0), words)));
  Value remaining = truncated(b.create<SubPrimOp>(loc, horizon, constant(32, 1)), 32);
  connect(horizon, mux(andv(done, field(step, "valid")), field(step, "bits"), mux(advance, remaining, horizon)));
  Value nextCycle = truncated(b.create<AddPrimOp>(loc, cycle, constant(64, 1)), 64);
  connect(cycle, mux(advance, nextCycle, cycle));
  connect(field(step, "ready"), done);
  connect(field(resetToken, "valid"), valid);
  connect(field(resetToken, "bits"), field(arg(3), "resetValue"));
  connect(field(arg(3), "tCycle"), cycle);
  connect(field(arg(3), "done"), done);
  connect(field(arg(3), "precisePeekable"), done);

  SmallVector<PortInfo> ports;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != resetPort) ports.push_back(p);
  ports.push_back({b.getStringAttr(controlName), control, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto resetBridge = b.create<InstanceOp>(loc, producer, "PeekPokeBridgeModule_0");
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
