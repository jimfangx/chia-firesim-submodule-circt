// See LICENSE for license details.
#include "goldengate/TSITokenEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <set>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addTSITokenEngine(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral engineName = "GGTSITokenEngine";
  constexpr llvm::StringLiteral wrapperName = "GGTSITokenWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGSimulationMasterBoundWrapper")
    return reject("TSI token mapping requires the active SimulationMaster binding wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == engineName || m.getModuleName() == wrapperName)
      return reject("TSI token engine or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("TSI token mapping needs a top and retained annotations");
  DictionaryAttr bridge; unsigned bridgeIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation a(attr);
    auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "firechip.goldengateimplementations.TSIBridgeModule") {
      if (bridge) return reject("multiple TSI constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr();
  auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr();
  auto keyClass = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto region = key ? key.getAs<StringAttr>("memoryRegionNameOpt") : StringAttr();
  if (!keyClass || keyClass.getValue() != "firechip.bridgeinterfaces.TSIBridgeParams" ||
      !region || region.getValue() != "MainMemory_0" || !mapping || mapping.size() != 5)
    return reject("TSI token mapping requires the Rocket MainMemory_0 constructor and five channels");
  auto *ctx = circuit.getContext();
  auto bit = UIntType::get(ctx, 1, false), word = UIntType::get(ctx, 32, false);
  const llvm::StringRef locals[]{"tsi_in_rev", "tsi_out_fwd", "reset", "tsi_in_fwd", "tsi_out_rev"};
  const llvm::StringRef fields[]{"tsi_in_ready", "", "reset", "", "tsi_out_ready"};
  unsigned tokenPorts[5], channelIndices[5];
  Attribute clock;
  for (unsigned j = 0; j < 5; ++j) {
    auto name = mapping.getAs<StringAttr>(locals[j]);
    if (!name) return reject("incomplete TSI channel mapping");
    DictionaryAttr channel;
    for (auto [i, attr] : llvm::enumerate(raw)) {
      Annotation a(attr);
      if (a.isClass(AnnotationClasses::ChannelConnection) && a.getMember<StringAttr>("globalName") == name) {
        if (channel) return reject("duplicate TSI channel");
        channel = cast<DictionaryAttr>(attr); channelIndices[j] = i;
      }
    }
    auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
    auto endpoints = channel ? channel.getAs<ArrayAttr>(j < 3 ? "sources" : "sinks") : ArrayAttr();
    auto opposite = channel ? channel.getAs<ArrayAttr>(j < 3 ? "sinks" : "sources") : ArrayAttr();
    bool forward = j == 1 || j == 3;
    auto cls = j == 2 ? AnnotationClasses::PipeChannel : forward ? AnnotationClasses::DecoupledForwardChannel : AnnotationClasses::DecoupledReverseChannel;
    auto infoClass = info ? info.getAs<StringAttr>("class") : StringAttr();
    if (!infoClass || infoClass.getValue() != cls || !endpoints ||
        endpoints.size() != (forward ? 2 : 1) || (opposite && !opposite.empty()))
      return reject("TSI needs five single-boundary Pipe/Decoupled channels");
    if (j == 2) {
      auto latency = info.getAs<IntegerAttr>("latency");
      if (!latency || latency.getInt() != 1) return reject("TSI reset needs a latency-one pipe");
    }
    auto channelClock = channel.get("clock");
    if (!channelClock || (clock && clock != channelClock)) return reject("TSI channels must share one clock");
    clock = channelClock;
    auto spelling = dyn_cast<StringAttr>(endpoints[0]);
    auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    if (!target || target->module != inner || !target->port)
      return reject("TSI endpoint must resolve to an active wrapper port");
    unsigned port = *target->port;
    auto token = dyn_cast<BundleType>(inner.getPortType(port));
    auto payload = forward ? BundleType::get(ctx, {{StringAttr::get(ctx, "bits"), false, word},
                                                 {StringAttr::get(ctx, "valid"), false, bit}}) : FIRRTLBaseType(bit);
    auto expected = BundleType::get(ctx, {{StringAttr::get(ctx, "ready"), true, bit},
                                         {StringAttr::get(ctx, "valid"), false, bit},
                                         {StringAttr::get(ctx, "bits"), false, payload}});
    // Valid(buildChannelType) places target-valid before the scalar word.
    // The earlier flat handoff remains useful for standalone engine tests.
    if (forward && token != expected) {
      payload = BundleType::get(ctx, {{StringAttr::get(ctx, "valid"), false, bit},
                                    {StringAttr::get(ctx, "bits"), false, word}});
      expected = BundleType::get(ctx, {{StringAttr::get(ctx, "ready"), true, bit},
                                      {StringAttr::get(ctx, "valid"), false, bit},
                                      {StringAttr::get(ctx, "bits"), false, payload}});
    }
    if (token != expected || inner.getPortDirection(port) != (j < 3 ? Direction::Out : Direction::In))
      return reject("TSI endpoint needs a correctly directed 32-bit Decoupled or Bool token");
    std::set<unsigned> expectedIDs;
    unsigned base = token.getFieldID(*token.getElementIndex("bits"));
    if (forward) {
      auto p = cast<BundleType>(payload);
      expectedIDs.insert(base + p.getFieldID(*p.getElementIndex("bits")));
      expectedIDs.insert(base + p.getFieldID(*p.getElementIndex("valid")));
    } else expectedIDs.insert(base);
    for (auto endpoint : endpoints) {
      auto s = dyn_cast<StringAttr>(endpoint);
      auto t = s ? resolveAnnotationTarget(circuit, s.getValue(), error) : std::nullopt;
      if (!t || t->module != inner || t->port != port || !t->fieldID || !expectedIDs.erase(*t->fieldID))
        return reject("TSI payload endpoints are missing, shared or inconsistent");
    }
    tokenPorts[j] = port;
    for (unsigned k = 0; k < j; ++k) if (tokenPorts[k] == port) return reject("TSI token ports must be distinct");
  }
  // The forward descriptors must identify the same valid/ready fields as their
  // paired reverse channels. Validate these before changing any operation.
  for (unsigned j : {1u, 3u}) {
    auto info = cast<DictionaryAttr>(raw[channelIndices[j]]).getAs<DictionaryAttr>("channelInfo");
    unsigned r = j == 1 ? 4 : 0;
    for (auto [member, port, valid] : {std::tuple<llvm::StringRef, unsigned, bool>{j == 1 ? "validSource" : "validSink", tokenPorts[j], true},
                                      {j == 1 ? "readySink" : "readySource", tokenPorts[r], false}}) {
      auto spelling = info.getAs<StringAttr>(member);
      auto t = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
      auto token = cast<BundleType>(inner.getPortType(port));
      unsigned id = token.getFieldID(*token.getElementIndex("bits"));
      if (valid) {
        auto payload = cast<BundleType>(token.getElement("bits")->type);
        id += payload.getFieldID(*payload.getElementIndex("valid"));
      }
      if (!t || t->module != inner || t->port != port || t->fieldID != id)
        return reject("TSI forward descriptor disagrees with its handshake endpoints");
    }
  }
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "tsi_control" || p.name == "tsi_in_deq" || p.name == "tsi_out_enq" || p.name == "tsi_queue_reset")
      return reject("TSI boundary already exists");
    if (p.name == "hostClock" && p.direction == Direction::In && isa<ClockType>(p.type)) hostClock = i;
    if (p.name == "hostReset" && p.direction == Direction::In && p.type == bit) hostReset = i;
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (!hostClock || !hostReset || used) return reject("TSI needs host controls and an uninstantiated top");

  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin()); Location loc = circuit.getLoc();
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, UIntType::get(ctx, w, false), APInt(w, n)); };
  auto hBits = BundleType::get(ctx, {{b.getStringAttr("tsi_in_ready"), false, bit},
      {b.getStringAttr("tsi_in_valid"), true, bit}, {b.getStringAttr("tsi_in_bits"), true, word},
      {b.getStringAttr("tsi_out_ready"), true, bit}, {b.getStringAttr("tsi_out_valid"), false, bit},
      {b.getStringAttr("tsi_out_bits"), false, word}, {b.getStringAttr("reset"), false, bit}});
  auto hPort = BundleType::get(ctx, {{b.getStringAttr("hBits"), false, hBits},
      {b.getStringAttr("toHost"), false, BundleType::get(ctx, {{b.getStringAttr("hReady"), true, bit}, {b.getStringAttr("hValid"), false, bit}})},
      {b.getStringAttr("fromHost"), false, BundleType::get(ctx, {{b.getStringAttr("hReady"), false, bit}, {b.getStringAttr("hValid"), true, bit}})}});
  auto control = BundleType::get(ctx, {{b.getStringAttr("step_size"), true, word},
      {b.getStringAttr("start"), true, bit}, {b.getStringAttr("done"), false, bit}});
  auto queue = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, word}});
  SmallVector<PortInfo> enginePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("hPort"), hPort, Direction::In},
      {b.getStringAttr("control"), control, Direction::Out}, {b.getStringAttr("inBuf_io_deq"), queue, Direction::In},
      {b.getStringAttr("outBuf_io_enq"), queue, Direction::Out}, {b.getStringAttr("queueReset"), bit, Direction::Out}};
  auto engine = b.create<FModuleOp>(loc, b.getStringAttr(engineName), ConventionAttr::get(ctx, Convention::Internal), enginePorts);
  engine->setAttr("goldengate.bridgeConstructor", key);
  b.setInsertionPointToStart(engine.getBodyBlock());
  auto arg = [&](unsigned i) { return engine.getBodyBlock()->getArgument(i); };
  Value remaining = b.create<RegResetOp>(loc, word, arg(0), arg(1), constant(32, 0), "tokensToEnqueue").getResult();
  Value hb = field(arg(2), "hBits"), th = field(arg(2), "toHost"), fh = field(arg(2), "fromHost");
  Value empty = b.create<EQPrimOp>(loc, remaining, constant(32, 0));
  Value fire = both(both(field(th, "hValid"), field(fh, "hReady")), b.create<NotPrimOp>(loc, empty));
  Value next = b.create<BitsPrimOp>(loc, b.create<SubPrimOp>(loc, remaining, constant(32, 1)), 31, 0);
  Value decrement = b.create<MuxPrimOp>(loc, fire, next, remaining);
  connect(remaining, b.create<MuxPrimOp>(loc, field(arg(3), "start"), field(arg(3), "step_size"), decrement));
  // Raw done predicate: the later MMIO bank must preserve the SFC sampled
  // done register and its software-write override, rather than read this wire.
  connect(field(arg(3), "done"), empty);
  // Unlike DecoupledHelper bridges, TSI deliberately drives both host signals
  // with tFire including their own predicate (as in the immutable SFC RTL).
  connect(field(th, "hReady"), fire); connect(field(fh, "hValid"), fire);
  connect(arg(6), b.create<OrPrimOp>(loc, arg(1), both(fire, field(hb, "reset"))));
  connect(field(hb, "tsi_in_valid"), field(arg(4), "valid"));
  connect(field(hb, "tsi_in_bits"), field(arg(4), "bits"));
  connect(field(arg(4), "ready"), both(field(hb, "tsi_in_ready"), fire));
  connect(field(hb, "tsi_out_ready"), field(arg(5), "ready"));
  connect(field(arg(5), "valid"), both(field(hb, "tsi_out_valid"), fire));
  connect(field(arg(5), "bits"), field(hb, "tsi_out_bits"));

  auto removed = [&](unsigned i) { return llvm::is_contained(tokenPorts, i); };
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!removed(i)) { copied.push_back(i); ports.push_back(p); }
  ports.push_back({b.getStringAttr("tsi_control"), control, Direction::Out});
  ports.push_back({b.getStringAttr("tsi_in_deq"), queue, Direction::In});
  ports.push_back({b.getStringAttr("tsi_out_enq"), queue, Direction::Out});
  ports.push_back({b.getStringAttr("tsi_queue_reset"), bit, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), tsi = b.create<InstanceOp>(loc, engine, "TSIBridgeModule_0");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(tsi.getResult(0), outer(*hostClock)); connect(tsi.getResult(1), outer(*hostReset));
  hb = field(tsi.getResult(2), "hBits"); th = field(tsi.getResult(2), "toHost"); fh = field(tsi.getResult(2), "fromHost");
  Value allValid = constant(1, 1), allReady = constant(1, 1);
  for (unsigned j = 0; j < 3; ++j) allValid = both(allValid, field(sim.getResult(tokenPorts[j]), "valid"));
  for (unsigned j = 3; j < 5; ++j) allReady = both(allReady, field(sim.getResult(tokenPorts[j]), "ready"));
  connect(field(th, "hValid"), allValid); connect(field(fh, "hReady"), allReady);
  // FPGATop HostPortIOConnectChannels2Port excludes a channel's own predicate.
  for (unsigned j = 0; j < 5; ++j) {
    Value token = sim.getResult(tokenPorts[j]);
    Value gate = field(j < 3 ? th : fh, j < 3 ? "hReady" : "hValid");
    for (unsigned k = j < 3 ? 0 : 3; k < (j < 3 ? 3 : 5); ++k)
      if (j != k) gate = both(gate, field(sim.getResult(tokenPorts[k]), j < 3 ? "valid" : "ready"));
    connect(field(token, j < 3 ? "ready" : "valid"), gate);
    Value bits = field(token, "bits");
    if (j == 1 || j == 3) {
      connect(j == 1 ? field(hb, "tsi_out_bits") : field(bits, "bits"), j == 1 ? field(bits, "bits") : field(hb, "tsi_in_bits"));
      connect(j == 1 ? field(hb, "tsi_out_valid") : field(bits, "valid"), j == 1 ? field(bits, "valid") : field(hb, "tsi_in_valid"));
    } else connect(j < 3 ? field(hb, fields[j]) : bits, j < 3 ? bits : field(hb, fields[j]));
  }
  for (unsigned j = 0; j < 4; ++j) {
    Value external = wrapper.getBodyBlock()->getArgument(copied.size() + j);
    Value internal = tsi.getResult(3 + j);
    b.create<ConnectOp>(loc, j == 1 ? internal : external, j == 1 ? external : internal);
  }
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
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(ctx);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  NamedAttrList newBridge(cast<DictionaryAttr>(annotations[bridgeIndex]));
  newBridge.set("target", b.getStringAttr(newPrefix + "|" + engineName.str() + ">hPort"));
  annotations[bridgeIndex] = newBridge.getDictionary(ctx);
  for (unsigned j = 0; j < 5; ++j) {
    NamedAttrList channel(cast<DictionaryAttr>(annotations[channelIndices[j]]));
    SmallVector<Attribute> endpoints;
    std::string prefix = newPrefix + "|" + engineName.str() + ">hPort.hBits.";
    if (j == 1 || j == 3) {
      endpoints.push_back(b.getStringAttr(prefix + (j == 1 ? "tsi_out_bits" : "tsi_in_bits")));
      endpoints.push_back(b.getStringAttr(prefix + (j == 1 ? "tsi_out_valid" : "tsi_in_valid")));
    } else endpoints.push_back(b.getStringAttr(prefix + fields[j].str()));
    channel.set(j < 3 ? "sinks" : "sources", b.getArrayAttr(endpoints));
    annotations[channelIndices[j]] = channel.getDictionary(ctx);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
