// See LICENSE for license details.
// UARTBridge.scala serial state machine and FPGATop.connectHostPort handshake.
// Serial registers use host reset; accepted target reset resets only the byte
// queues. Preserve the oracle's enqueue of pre-edge txData on the final sample.
#include "goldengate/UARTSerialEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addUARTSerialEngine(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral engineName = "GGUARTSerialEngine";
  constexpr llvm::StringLiteral wrapperName = "GGUARTSerialWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGResetPulseBridgeControlWrapper")
    return reject("UART serial mapping requires the active ResetPulseBridge control wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (!m) continue;
    if (m.getModuleName() == engineName || m.getModuleName() == wrapperName)
      return reject("UART serial engine or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("UART serial mapping needs retained annotations and a top module");
  DictionaryAttr bridge;
  unsigned bridgeIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation a(attr);
    if (a.isClass(AnnotationClasses::BridgeIO) &&
        a.getMember<StringAttr>("widgetClass") == "firechip.goldengateimplementations.UARTBridgeModule") {
      if (bridge) return reject("multiple UARTBridgeModule constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr();
  auto divisor = key ? key.getAs<IntegerAttr>("div") : IntegerAttr();
  auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr();
  if (!key || key.getAs<StringAttr>("class") != "firechip.bridgeinterfaces.UARTKey" ||
      !divisor || divisor.getInt() <= 0 || divisor.getInt() > INT32_MAX ||
      !mapping || mapping.size() != 3)
    return reject("UART serial mapping needs one UARTKey with positive baud divisor and three channels");
  auto *context = circuit.getContext();
  auto bit = UIntType::get(context, 1, false);
  const llvm::StringRef localNames[]{"reset", "uart_txd", "uart_rxd"};
  unsigned channelIndices[3], tokenPorts[3];
  for (unsigned j = 0; j < 3; ++j) {
    auto name = mapping.getAs<StringAttr>(localNames[j]);
    if (!name) return reject("UART reset, TX and RX channel mapping is incomplete");
    DictionaryAttr channel;
    for (auto [i, attr] : llvm::enumerate(raw)) {
      Annotation a(attr);
      if (a.isClass(AnnotationClasses::ChannelConnection) && a.getMember<StringAttr>("globalName") == name) {
        if (channel) return reject("duplicate UART boundary channel");
        channel = cast<DictionaryAttr>(attr); channelIndices[j] = i;
      }
    }
    auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
    auto latency = info ? info.getAs<IntegerAttr>("latency") : IntegerAttr();
    auto endpoints = channel ? channel.getAs<ArrayAttr>(j == 2 ? "sinks" : "sources") : ArrayAttr();
    auto opposite = channel ? channel.getAs<ArrayAttr>(j == 2 ? "sources" : "sinks") : ArrayAttr();
    if (!info || info.getAs<StringAttr>("class") != AnnotationClasses::PipeChannel ||
        !latency || latency.getInt() != 1 || !endpoints || endpoints.size() != 1 ||
        (opposite && !opposite.empty()))
      return reject("UART channels need single latency-one boundary PipeChannel endpoints");
    auto spelling = dyn_cast<StringAttr>(endpoints[0]);
    auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    if (!target || target->module != inner || !target->port)
      return reject("UART channel endpoint must resolve to the active wrapper port");
    unsigned port = *target->port;
    auto token = dyn_cast<BundleType>(inner.getPortType(port));
    auto bits = token ? token.getElementIndex("bits") : std::nullopt;
    if (!token || token.getElements().size() != 3 || !bits ||
        target->fieldID != token.getFieldID(*bits) ||
        token.getElement("bits")->type != bit || token.getElement("bits")->isFlip ||
        !token.getElement("ready") || !token.getElement("valid") ||
        token.getElement("ready")->type != bit || !token.getElement("ready")->isFlip ||
        token.getElement("valid")->type != bit || token.getElement("valid")->isFlip ||
        inner.getPortDirection(port) != (j == 2 ? Direction::In : Direction::Out))
      return reject("UART channel needs a Decoupled Bool port with correct direction");
    tokenPorts[j] = port;
    for (unsigned k = 0; k < j; ++k)
      if (tokenPorts[k] == port) return reject("UART channel endpoints must be distinct");
  }
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "uart_tx" || p.name == "uart_rx" || p.name == "uart_fifoReset")
      return reject("UART byte boundary port already exists");
    if (p.name == "hostClock" && p.direction == Direction::In && isa<ClockType>(p.type)) hostClock = i;
    if (p.name == "hostReset" && p.direction == Direction::In && p.type == bit) hostReset = i;
  }
  if (!hostClock || !hostReset) return reject("UART serial mapping needs hostClock and hostReset");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("UART serial mapping needs an uninstantiated top");

  // Mutation starts only after the constructor, endpoints and boundary validate.
  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  Location loc = circuit.getLoc();
  auto byte = UIntType::get(context, 8, false);
  auto byteToken = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, byte}});
  auto uart = BundleType::get(context, {{b.getStringAttr("txd"), false, bit}, {b.getStringAttr("rxd"), true, bit}});
  auto hBits = BundleType::get(context, {{b.getStringAttr("reset"), false, bit}, {b.getStringAttr("uart"), false, uart}});
  auto toHost = BundleType::get(context, {{b.getStringAttr("hReady"), true, bit}, {b.getStringAttr("hValid"), false, bit}});
  auto fromHost = BundleType::get(context, {{b.getStringAttr("hReady"), false, bit}, {b.getStringAttr("hValid"), true, bit}});
  auto hPort = BundleType::get(context, {{b.getStringAttr("hBits"), false, hBits},
      {b.getStringAttr("toHost"), false, toHost}, {b.getStringAttr("fromHost"), false, fromHost}});
  SmallVector<PortInfo> enginePorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("hPort"), hPort, Direction::In},
      {b.getStringAttr("txfifo"), byteToken, Direction::Out}, {b.getStringAttr("rxfifo"), byteToken, Direction::In},
      {b.getStringAttr("fifoReset"), bit, Direction::Out}};
  auto engine = b.create<FModuleOp>(loc, b.getStringAttr(engineName), ConventionAttr::get(context, Convention::Internal), enginePorts);
  engine->setAttr("goldengate.bridgeConstructor", key);
  b.setInsertionPointToStart(engine.getBodyBlock());
  auto arg = [&](unsigned i) { return engine.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto constant = [&](unsigned width, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, UIntType::get(context, width, false), APInt(width, n));
  };
  auto reg = [&](unsigned width, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, UIntType::get(context, width, false), arg(0), arg(1), constant(width, 0), name).getResult();
  };
  auto mux = [&](Value cond, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, cond, yes, no); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto eq = [&](Value v, uint64_t n) -> Value {
    return b.create<EQPrimOp>(loc, v, constant(cast<UIntType>(v.getType()).getWidthOrSentinel(), n));
  };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value hb = field(arg(2), "hBits"), th = field(arg(2), "toHost"), fh = field(arg(2), "fromHost");
  Value txd = field(field(hb, "uart"), "txd");
  Value fire = both(both(field(th, "hValid"), field(fh, "hReady")), field(arg(3), "ready"));
  connect(field(th, "hReady"), fire); connect(field(fh, "hValid"), fire);
  connect(arg(5), b.create<OrPrimOp>(loc, arg(1), both(fire, field(hb, "reset"))));
  Value txState = reg(2, "txState"), txData = b.create<RegOp>(loc, byte, arg(0), "txData").getResult();
  Value rxState = reg(2, "rxState");
  Value txIdle = eq(txState, 0), txWait = eq(txState, 1), txSample = eq(txState, 2), txBreak = eq(txState, 3);
  Value rxIdle = eq(rxState, 0), rxStart = eq(rxState, 1), rxDataState = eq(rxState, 2);
  auto counter = [&](unsigned limit, Value enable, llvm::StringRef name) -> std::pair<Value, Value> {
    if (limit == 1) return {constant(1, 0), enable};
    unsigned width = llvm::Log2_64_Ceil(limit);
    Value count = reg(width, name), terminal = eq(count, limit - 1);
    Value sum = b.create<AddPrimOp>(loc, count, constant(width, 1));
    Value inc = b.create<BitsPrimOp>(loc, sum, width - 1, 0);
    connect(count, mux(enable, mux(terminal, constant(width, 0), inc), count));
    return {count, both(enable, terminal)};
  };
  auto [txIdx, txWrap] = counter(8, both(txSample, fire), "txDataIdx");
  auto [txBaud, txBaudWrap] = counter(divisor.getInt(), both(txWait, fire), "txBaudCount");
  auto [txSlack, txSlackWrap] = counter(4, both(both(txIdle, eq(txd, 0)), fire), "txSlackCount");
  Value nextTx = mux(txIdle, mux(txSlackWrap, constant(2, 1), txState),
      mux(txWait, mux(txBaudWrap, constant(2, 2), txState),
      mux(txSample, mux(txWrap, mux(txd, constant(2, 0), constant(2, 3)), mux(fire, constant(2, 1), txState)),
      mux(both(both(txBreak, txd), fire), constant(2, 0), txState))));
  connect(txState, nextTx);
  Value sampled = b.create<OrPrimOp>(loc, txData, b.create<DShlPrimOp>(loc, txd, txIdx));
  connect(txData, mux(txSlackWrap, constant(8, 0), mux(both(txSample, fire), sampled, txData)));
  connect(field(arg(3), "valid"), txWrap); connect(field(arg(3), "bits"), txData);
  auto [rxBaud, rxBaudWrap] = counter(divisor.getInt(), fire, "rxBaudCount");
  auto [rxIdx, rxWrap] = counter(8, both(both(rxDataState, fire), rxBaudWrap), "rxDataIdx");
  connect(rxState, mux(rxIdle, mux(both(rxBaudWrap, field(arg(4), "valid")), constant(2, 1), rxState),
      mux(rxStart, mux(rxBaudWrap, constant(2, 2), rxState),
      mux(both(both(rxDataState, rxWrap), rxBaudWrap), constant(2, 0), rxState))));
  Value shifted = b.create<DShrPrimOp>(loc, field(arg(4), "bits"), rxIdx);
  Value rxBit = b.create<BitsPrimOp>(loc, shifted, 0, 0);
  connect(field(field(hb, "uart"), "rxd"), mux(rxStart, constant(1, 0), mux(rxDataState, rxBit, constant(1, 1))));
  connect(field(arg(4), "ready"), both(both(both(rxDataState, rxWrap), rxBaudWrap), fire));

  auto removed = [&](unsigned i) { return llvm::is_contained(tokenPorts, i); };
  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!removed(i)) { copied.push_back(i); ports.push_back(p); }
  ports.push_back({b.getStringAttr("uart_tx"), byteToken, Direction::Out});
  ports.push_back({b.getStringAttr("uart_rx"), byteToken, Direction::In});
  ports.push_back({b.getStringAttr("uart_fifoReset"), bit, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto serial = b.create<InstanceOp>(loc, engine, "UARTBridgeSerial_0");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outerArg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(serial.getResult(0), outerArg(*hostClock)); connect(serial.getResult(1), outerArg(*hostReset));
  Value host = serial.getResult(2), incoming = field(host, "toHost"), outgoing = field(host, "fromHost");
  Value resetToken = sim.getResult(tokenPorts[0]), txToken = sim.getResult(tokenPorts[1]), rxToken = sim.getResult(tokenPorts[2]);
  connect(field(incoming, "hValid"), both(field(resetToken, "valid"), field(txToken, "valid")));
  // FPGATop's all-channel helpers collapse to fire here: fire already includes
  // both incoming valid bits and the single outgoing channel's ready bit.
  connect(field(resetToken, "ready"), field(incoming, "hReady"));
  connect(field(txToken, "ready"), field(incoming, "hReady"));
  connect(field(outgoing, "hReady"), field(rxToken, "ready"));
  connect(field(rxToken, "valid"), field(outgoing, "hValid"));
  connect(field(rxToken, "bits"), field(field(field(host, "hBits"), "uart"), "rxd"));
  connect(field(field(host, "hBits"), "reset"), field(resetToken, "bits"));
  connect(field(field(field(host, "hBits"), "uart"), "txd"), field(txToken, "bits"));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(copied.size()), serial.getResult(3));
  b.create<ConnectOp>(loc, serial.getResult(4), wrapper.getBodyBlock()->getArgument(copied.size() + 1));
  connect(wrapper.getBodyBlock()->getArgument(copied.size() + 2), serial.getResult(5));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue();
      if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  NamedAttrList newBridge(cast<DictionaryAttr>(annotations[bridgeIndex]));
  newBridge.set("target", b.getStringAttr(newPrefix + "|" + engineName.str() + ">hPort"));
  annotations[bridgeIndex] = newBridge.getDictionary(context);
  const llvm::StringRef endpointNames[]{"reset", "uart.txd", "uart.rxd"};
  for (unsigned j = 0; j < 3; ++j) {
    NamedAttrList channel(cast<DictionaryAttr>(annotations[channelIndices[j]]));
    channel.set(j == 2 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr(newPrefix + "|" + engineName.str() + ">hPort.hBits." + endpointNames[j].str())}));
    annotations[channelIndices[j]] = channel.getDictionary(context);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
