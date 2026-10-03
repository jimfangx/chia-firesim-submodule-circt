// See LICENSE for license details.
// TracerVBridge.scala HostPort consumption, stream packing and trigger history.
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addTracerVTokenEngine(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral engineName = "GGTracerVTokenEngine";
  constexpr llvm::StringLiteral wrapperName = "GGTracerVTokenWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGPeekPokeBridgeControlWrapper")
    return reject("TracerV token mapping requires the active PeekPoke control wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (!m) continue;
    if (m.getModuleName() == engineName || m.getModuleName() == wrapperName)
      return reject("TracerV token engine or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("TracerV token mapping needs retained annotations and a top module");
  DictionaryAttr bridge;
  unsigned bridgeIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation a(attr);
    if (a.isClass(AnnotationClasses::BridgeIO) &&
        a.getMember<StringAttr>("widgetClass") == "firechip.goldengateimplementations.TracerVBridgeModule") {
      if (bridge) return reject("multiple TracerVBridgeModule constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr();
  auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr();
  auto keyWidth = [&](llvm::StringRef name, int64_t expected) {
    auto width = key ? key.getAs<IntegerAttr>(name) : IntegerAttr();
    return width && width.getInt() == expected;
  };
  if (!key || key.getAs<StringAttr>("class") != "firechip.bridgeinterfaces.TraceBundleWidths" ||
      !keyWidth("retireWidth", 1) || !keyWidth("iaddrWidth", 40) ||
      !keyWidth("insnWidth", 32) || !keyWidth("causeWidth", 64) || !keyWidth("tvalWidth", 40) ||
      !mapping || mapping.size() != 12)
    return reject("TracerV token engine requires the Rocket TraceBundleWidths constructor and twelve channels");
  auto *context = circuit.getContext();
  auto bit = UIntType::get(context, 1, false);
  const llvm::StringRef localNames[]{"tiletrace_reset", "tiletrace_trace_retiredinsns_0_valid",
      "tiletrace_trace_retiredinsns_0_iaddr", "tiletrace_trace_retiredinsns_0_insn",
      "tiletrace_trace_retiredinsns_0_priv", "tiletrace_trace_retiredinsns_0_exception",
      "tiletrace_trace_retiredinsns_0_interrupt", "tiletrace_trace_retiredinsns_0_cause",
      "tiletrace_trace_retiredinsns_0_tval", "tiletrace_trace_time", "triggerCredit", "triggerDebit"};
  const unsigned widths[]{1, 1, 40, 32, 3, 1, 1, 64, 40, 64, 1, 1};
  unsigned channelIndices[12], tokenPorts[12];
  for (unsigned j = 0; j < 12; ++j) {
    auto name = mapping.getAs<StringAttr>(localNames[j]);
    if (!name) return reject("TracerV channel mapping is incomplete");
    DictionaryAttr channel;
    for (auto [i, attr] : llvm::enumerate(raw)) {
      Annotation a(attr);
      if (a.isClass(AnnotationClasses::ChannelConnection) && a.getMember<StringAttr>("globalName") == name) {
        if (channel) return reject("duplicate TracerV boundary channel");
        channel = cast<DictionaryAttr>(attr); channelIndices[j] = i;
      }
    }
    auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
    auto latency = info ? info.getAs<IntegerAttr>("latency") : IntegerAttr();
    auto endpoints = channel ? channel.getAs<ArrayAttr>(j >= 10 ? "sinks" : "sources") : ArrayAttr();
    auto opposite = channel ? channel.getAs<ArrayAttr>(j >= 10 ? "sources" : "sinks") : ArrayAttr();
    if (!info || info.getAs<StringAttr>("class") != AnnotationClasses::PipeChannel ||
        !latency || latency.getInt() != 1 || !endpoints || endpoints.size() != 1 ||
        (opposite && !opposite.empty()))
      return reject("TracerV channels need single latency-one boundary PipeChannel endpoints");
    auto spelling = dyn_cast<StringAttr>(endpoints[0]);
    auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    if (!target || target->module != inner || !target->port)
      return reject("TracerV channel endpoint must resolve to the active wrapper port");
    unsigned port = *target->port;
    auto token = dyn_cast<BundleType>(inner.getPortType(port));
    auto bits = token ? token.getElementIndex("bits") : std::nullopt;
    if (!token || token.getElements().size() != 3 || !bits ||
        target->fieldID != token.getFieldID(*bits) ||
        token.getElement("bits")->type != UIntType::get(context, widths[j], false) || token.getElement("bits")->isFlip ||
        !token.getElement("ready") || !token.getElement("valid") ||
        token.getElement("ready")->type != bit || !token.getElement("ready")->isFlip ||
        token.getElement("valid")->type != bit || token.getElement("valid")->isFlip ||
        inner.getPortDirection(port) != (j >= 10 ? Direction::In : Direction::Out))
      return reject("TracerV channel needs a Decoupled UInt port with correct direction");
    tokenPorts[j] = port;
    for (unsigned k = 0; k < j; ++k)
      if (tokenPorts[k] == port) return reject("TracerV channel endpoints must be distinct");
  }
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "tracerv_control" || p.name == "tracerv_stream")
      return reject("TracerV control or stream boundary port already exists");
    if (p.name == "hostClock" && p.direction == Direction::In && isa<ClockType>(p.type)) hostClock = i;
    if (p.name == "hostReset" && p.direction == Direction::In && p.type == bit) hostReset = i;
  }
  if (!hostClock || !hostReset) return reject("TracerV token mapping needs hostClock and hostReset");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("TracerV token mapping needs an uninstantiated top");

  // Constructor and all channel endpoints validate before any mutation.
  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  Location loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto notv = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value pred, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, pred, yes, no); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  SmallVector<BundleType::BundleElement> payload;
  for (unsigned j = 0; j < 12; ++j)
    payload.push_back({b.getStringAttr(localNames[j]), j >= 10, uint(widths[j])});
  auto toHost = BundleType::get(context, {{b.getStringAttr("hReady"), true, bit}, {b.getStringAttr("hValid"), false, bit}});
  auto fromHost = BundleType::get(context, {{b.getStringAttr("hReady"), false, bit}, {b.getStringAttr("hValid"), true, bit}});
  auto hPort = BundleType::get(context, {{b.getStringAttr("hBits"), false, BundleType::get(context, payload)},
      {b.getStringAttr("toHost"), false, toHost}, {b.getStringAttr("fromHost"), false, fromHost}});
  auto control = BundleType::get(context, {{b.getStringAttr("initDone"), true, bit},
      {b.getStringAttr("traceEnable"), true, bit}, {b.getStringAttr("trigger"), true, bit},
      {b.getStringAttr("tCycle"), false, uint(64)}});
  auto stream = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(512)}});
  SmallVector<PortInfo> enginePorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("hPort"), hPort, Direction::In},
      {b.getStringAttr("control"), control, Direction::Out}, {b.getStringAttr("stream"), stream, Direction::Out}};
  auto engine = b.create<FModuleOp>(loc, b.getStringAttr(engineName), ConventionAttr::get(context, Convention::Internal), enginePorts);
  engine->setAttr("goldengate.bridgeConstructor", key);
  b.setInsertionPointToStart(engine.getBodyBlock());
  auto arg = [&](unsigned i) { return engine.getBodyBlock()->getArgument(i); };
  auto reg = [&](unsigned w, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), constant(w, 0), name).getResult();
  };
  Value cycle = reg(64, "trace_cycle_counter"), counter = reg(1, "counter"), previous = reg(1, "triggerReg");
  Value hb = field(arg(2), "hBits"), th = field(arg(2), "toHost"), fh = field(arg(2), "fromHost");
  Value trigger = field(arg(3), "trigger"), init = field(arg(3), "initDone");
  Value traceValid = both(field(hb, localNames[1]), notv(field(hb, localNames[0])));
  // Keep the one-bit arm register as in SFC, including its unreachable value.
  // The stream payload always selects the sole arm; an invalid arm suppresses
  // stream-valid and still permits HostPort consumption.
  Value armZero = b.create<EQPrimOp>(loc, counter, constant(1, 0));
  Value remain = both(armZero, traceValid);
  Value maybeFire = b.create<OrPrimOp>(loc, notv(remain), armZero);
  Value inputValid = field(th, "hValid"), outputReady = field(fh, "hReady");
  Value streamReady = field(arg(4), "ready");
  Value common = both(both(both(inputValid, outputReady), streamReady), init);
  Value fire = both(maybeFire, common);
  Value enq = both(both(remain, field(arg(3), "traceEnable")), common);
  Value increment = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, counter, constant(1, 1)), 0, 0);
  connect(counter, mux(fire, constant(1, 0), mux(enq, increment, counter)));
  connect(previous, mux(fire, trigger, previous));
  // DecoupledHelper.fire excludes the predicate for the endpoint being driven.
  connect(field(th, "hReady"), both(both(both(maybeFire, outputReady), streamReady), init));
  connect(field(fh, "hValid"), both(both(both(maybeFire, inputValid), streamReady), init));
  connect(field(arg(4), "valid"), both(both(both(both(both(remain,
      field(arg(3), "traceEnable")), inputValid), outputReady), init), trigger));
  connect(field(hb, "triggerCredit"), both(trigger, notv(previous)));
  connect(field(hb, "triggerDebit"), both(notv(trigger), previous));
  Value advance = both(field(th, "hReady"), inputValid);
  Value nextCycle = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, cycle, constant(64, 1)), 63, 0);
  connect(cycle, mux(advance, nextCycle, cycle));
  connect(field(arg(3), "tCycle"), cycle);
  Value pc = b.create<PadPrimOp>(loc, field(hb, localNames[2]), 63);
  Value record = b.create<CatPrimOp>(loc, traceValid, pc);
  Value packet = b.create<CatPrimOp>(loc, record, cycle);
  connect(field(arg(4), "bits"), b.create<PadPrimOp>(loc, packet, 512));

  auto removed = [&](unsigned i) { return llvm::is_contained(tokenPorts, i); };
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!removed(i)) { copied.push_back(i); ports.push_back(p); }
  ports.push_back({b.getStringAttr("tracerv_control"), control, Direction::Out});
  ports.push_back({b.getStringAttr("tracerv_stream"), stream, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto tracer = b.create<InstanceOp>(loc, engine, "TracerVBridgeModule_0");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outerArg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(tracer.getResult(0), outerArg(*hostClock)); connect(tracer.getResult(1), outerArg(*hostReset));
  hb = field(tracer.getResult(2), "hBits"); th = field(tracer.getResult(2), "toHost"); fh = field(tracer.getResult(2), "fromHost");
  Value allValid = constant(1, 1), allReady = constant(1, 1);
  for (unsigned j = 0; j < 10; ++j) allValid = both(allValid, field(sim.getResult(tokenPorts[j]), "valid"));
  for (unsigned j = 10; j < 12; ++j) allReady = both(allReady, field(sim.getResult(tokenPorts[j]), "ready"));
  connect(field(th, "hValid"), allValid); connect(field(fh, "hReady"), allReady);
  // FPGATop HostPortIOConnectChannels2Port: exclude each channel's own valid
  // or ready predicate so a channel can advertise readiness/validity first.
  for (unsigned j = 0; j < 12; ++j) {
    Value token = sim.getResult(tokenPorts[j]);
    Value gate = field(j < 10 ? th : fh, j < 10 ? "hReady" : "hValid");
    for (unsigned k = j < 10 ? 0 : 10; k < (j < 10 ? 10 : 12); ++k)
      if (j != k) gate = both(gate, field(sim.getResult(tokenPorts[k]), j < 10 ? "valid" : "ready"));
    connect(field(token, j < 10 ? "ready" : "valid"), gate);
    connect(j < 10 ? field(hb, localNames[j]) : field(token, "bits"),
            j < 10 ? field(token, "bits") : field(hb, localNames[j]));
  }
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(copied.size()), tracer.getResult(3));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(copied.size() + 1), tracer.getResult(4));
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
  for (unsigned j = 0; j < 12; ++j) {
    NamedAttrList channel(cast<DictionaryAttr>(annotations[channelIndices[j]]));
    channel.set(j >= 10 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr(newPrefix + "|" + engineName.str() + ">hPort.hBits." + localNames[j].str())}));
    annotations[channelIndices[j]] = channel.getDictionary(context);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
