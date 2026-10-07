// See LICENSE for license details.
// Requires: uninstantiated BlockDev response scheduler top, Rocket FASED widths,
// eleven single-clock channels with exact types and retained endpoint targets.
// Consumes: eleven channel-facing ports; transfers bridge/channel endpoints.
// Produces: ordinary FIRRTL HostPort, ingress snoop, timing and reset gates.
// Mutates hierarchy/ports; preserves annotation classes and constructor metadata.
// Functional ingress, egress, host outstanding counters and timing state remain
// explicit boundaries for subsequent passes. The model enable is targetFire.
// Oracle: FASEDMemoryTimingModel.scala 354-402; ingress excludes its own capacity.
#include "goldengate/FASEDTokenEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <algorithm>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDTokenEngine(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral engineName = "GGFASEDTokenEngine";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDTokenWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevResponseSchedulerWrapper")
    return reject("FASED token mapping requires the active BlockDev response scheduler wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == engineName || m.getModuleName() == wrapperName)
      return reject("FASED token engine or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED needs a top and retained annotations");
  DictionaryAttr bridge; unsigned bridgeIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation a(attr); auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "midas.models.FASEDMemoryTimingModel") {
      if (bridge) return reject("multiple FASED constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr();
  auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr();
  auto keyClass = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto hasWidth = [&](llvm::StringRef n, int w) { auto a = widths ? widths.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == w; };
  if (!keyClass || keyClass.getValue() != "firesim.lib.bridges.CompleteConfig" ||
      !hasWidth("addrBits", 35) || !hasWidth("dataBits", 64) || !hasWidth("idBits", 4) || !mapping || mapping.size() != 11)
    return reject("FASED requires the Rocket 35/64/4-bit constructor and eleven channels");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> e) { return BundleType::get(ctx, e); };
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> e;
    for (auto [n, w] : fields) e.push_back({b.getStringAttr(n), false, uint(w)});
    return BundleType::get(ctx, e);
  };
  auto address = payload({{"user",1}, {"id",4}, {"region",4}, {"qos",4}, {"prot",3}, {"cache",4},
      {"lock",1}, {"burst",2}, {"size",3}, {"len",8}, {"addr",35}});
  auto data = payload({{"user",1}, {"strb",8}, {"id",4}, {"last",1}, {"data",64}});
  auto response = payload({{"user",1}, {"id",4}, {"resp",2}});
  auto read = payload({{"user",1}, {"id",4}, {"last",1}, {"data",64}, {"resp",2}});
  const llvm::StringRef locals[]{"axi4_aw_fwd", "axi4_w_fwd", "axi4_b_rev", "axi4_ar_fwd", "axi4_r_rev", "reset",
      "axi4_aw_rev", "axi4_w_rev", "axi4_b_fwd", "axi4_ar_rev", "axi4_r_fwd"};
  const bool forwards[]{true,true,false,true,false,false,false,false,true,false,true};
  const unsigned pairs[]{6,7,8,9,10,5,0,1,2,3,4};
  const BundleType payloads[]{address,data,{},address,{},{},{},{},response,{},read};
  std::vector<std::string> paths[11];
  for (unsigned j = 0; j < 11; ++j) {
    std::string channel = locals[j].take_front(locals[j].rfind('_')).str();
    std::replace(channel.begin(), channel.end(), '_', '.');
    if (forwards[j]) {
      for (auto e : payloads[j].getElements()) paths[j].push_back(channel + ".bits." + e.name.str());
      paths[j].push_back(channel + ".valid");
    } else paths[j].push_back(j == 5 ? "reset" : channel + ".ready");
  }
  unsigned tokenPorts[11], channelIndices[11];
  SmallVector<SmallVector<std::string>> payloadFields;
  Attribute clock;
  for (unsigned j = 0; j < 11; ++j) {
    auto name = mapping.getAs<StringAttr>(locals[j]);
    if (!name) return reject("incomplete FASED channel mapping");
    DictionaryAttr channel;
    for (auto [i, attr] : llvm::enumerate(raw)) {
      Annotation a(attr);
      if (a.isClass(AnnotationClasses::ChannelConnection) && a.getMember<StringAttr>("globalName") == name) {
        if (channel) return reject("duplicate FASED channel");
        channel = cast<DictionaryAttr>(attr); channelIndices[j] = i;
      }
    }
    bool forward = forwards[j], pipe = j == 5;
    auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
    auto ends = channel ? channel.getAs<ArrayAttr>(j < 6 ? "sources" : "sinks") : ArrayAttr();
    auto opposite = channel ? channel.getAs<ArrayAttr>(j < 6 ? "sinks" : "sources") : ArrayAttr();
    auto cls = pipe ? AnnotationClasses::PipeChannel : forward ? AnnotationClasses::DecoupledForwardChannel : AnnotationClasses::DecoupledReverseChannel;
    auto infoClass = info ? info.getAs<StringAttr>("class") : StringAttr();
    if (!infoClass || infoClass.getValue() != cls || !ends || ends.size() != paths[j].size() || (opposite && !opposite.empty()))
      return reject("FASED needs eleven single-boundary Pipe/Decoupled channels");
    if (pipe) {
      auto latency = info.getAs<IntegerAttr>("latency");
      if (!latency || latency.getInt() != 1) return reject("FASED pipes need latency one");
    }
    auto channelClock = channel.get("clock");
    if (!channelClock || (clock && clock != channelClock)) return reject("FASED channels must share one clock");
    clock = channelClock;
    auto spelling = dyn_cast<StringAttr>(ends[0]);
    auto t = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    if (!t || t->module != inner || !t->port) return reject("FASED endpoint must resolve to an active wrapper port");
    unsigned port = *t->port;
    FIRRTLBaseType payload = bit;
    SmallVector<std::string> fields;
    if (forward) {
      auto bits = payloads[j];
      SmallVector<BundleType::BundleElement> flattened;
      for (auto e : bits.getElements()) {
        fields.push_back("bits_" + e.name.str());
        flattened.push_back({b.getStringAttr(fields.back()), false, e.type});
      }
      fields.push_back("valid"); flattened.push_back({b.getStringAttr("valid"), false, bit});
      payload = BundleType::get(ctx, flattened);
    } else fields.push_back("");
    auto token = bundle({{b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
        {b.getStringAttr("bits"), false, payload}});
    // Canonical SimWrapper ports carry Valid(buildChannelType(payload)).
    // Accept the earlier flat boundary as well for standalone token-engine use.
    if (forward && inner.getPortType(port) != token) {
      auto bits = payloads[j];
      bool scalar = bits.getElements().size() == 1;
      auto normalized = scalar ? bits.getElements()[0].type : FIRRTLBaseType(bits);
      payload = bundle({{b.getStringAttr("valid"), false, bit},
                        {b.getStringAttr("bits"), false, normalized}});
      token = bundle({{b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
                      {b.getStringAttr("bits"), false, payload}});
      fields.clear();
      for (auto e : bits.getElements()) fields.push_back(scalar ? "bits" : "bits." + e.name.str());
      fields.push_back("valid");
    }
    if (inner.getPortType(port) != token || inner.getPortDirection(port) != (j < 6 ? Direction::Out : Direction::In))
      return reject("FASED endpoint has an unsupported payload or direction");
    // Preserve endpoint order: each descriptor leaf identifies its corresponding
    // AXI field, including placeholder user/ID fields.
    unsigned base = token.getFieldID(2);
    for (auto [k, endpoint] : llvm::enumerate(ends)) {
      auto s = dyn_cast<StringAttr>(endpoint);
      auto e = s ? resolveAnnotationTarget(circuit, s.getValue(), error) : std::nullopt;
      unsigned id = base;
      if (forward) {
        FIRRTLBaseType type = payload;
        SmallVector<llvm::StringRef> components; llvm::StringRef(fields[k]).split(components, '.');
        for (auto component : components) {
          auto record = cast<BundleType>(type);
          unsigned index = *record.getElementIndex(component);
          id += record.getFieldID(index); type = record.getElements()[index].type;
        }
      }
      if (!e || e->module != inner || e->port != port || e->fieldID != id)
        return reject("FASED payload endpoints are missing, shared or reordered");
    }
    tokenPorts[j] = port; payloadFields.push_back(fields);
    for (unsigned k = 0; k < j; ++k) if (tokenPorts[k] == port) return reject("FASED token ports must be distinct");
  }
  for (unsigned j : {0u, 1u, 3u, 8u, 10u}) {
    auto info = cast<DictionaryAttr>(raw[channelIndices[j]]).getAs<DictionaryAttr>("channelInfo");
    unsigned reverse = pairs[j];
    for (bool valid : {false, true}) {
      auto spelling = info.getAs<StringAttr>(valid ? (j < 6 ? "validSource" : "validSink") : (j < 6 ? "readySink" : "readySource"));
      unsigned port = tokenPorts[valid ? j : reverse];
      auto t = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
      auto token = cast<BundleType>(inner.getPortType(port)); unsigned id = token.getFieldID(2);
      if (valid) { auto p = cast<BundleType>(token.getElement("bits")->type); id += p.getFieldID(*p.getElementIndex("valid")); }
      if (!t || t->module != inner || t->port != port || t->fieldID != id)
        return reject("FASED forward descriptor disagrees with its handshake endpoints");
    }
  }
  const llvm::StringRef boundary[]{"fased_timing", "fased_ingress", "fased_readiness", "fased_tfire",
      "fased_ingress_reset", "fased_egress_reset", "fased_model_reset"};
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (llvm::is_contained(boundary, p.name.getValue())) return reject("FASED boundary already exists");
    if (p.name == "hostClock" && p.direction == Direction::In && isa<ClockType>(p.type)) hostClock = i;
    if (p.name == "hostReset" && p.direction == Direction::In && p.type == bit) hostReset = i;
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (!hostClock || !hostReset || used) return reject("FASED needs host controls and an uninstantiated top");
  Location loc = circuit.getLoc(); b.setInsertionPointToStart(circuit.getBodyBlock());
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto path = [&](Value v, llvm::StringRef n) { SmallVector<llvm::StringRef> parts; n.split(parts, '.'); for (auto part : parts) v = field(v, part); return v; };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto decoupled = [&](BundleType bits, bool flipped) { return bundle({{b.getStringAttr("ready"), !flipped, bit},
      {b.getStringAttr("valid"), flipped, bit}, {b.getStringAttr("bits"), flipped, bits}}); };
  auto axi4 = bundle({{b.getStringAttr("aw"), false, decoupled(address, false)},
      {b.getStringAttr("w"), false, decoupled(data, false)}, {b.getStringAttr("b"), false, decoupled(response, true)},
      {b.getStringAttr("ar"), false, decoupled(address, false)}, {b.getStringAttr("r"), false, decoupled(read, true)}});
  auto hBits = bundle({{b.getStringAttr("axi4"), false, axi4}, {b.getStringAttr("reset"), false, bit}});
  auto hPort = bundle({{b.getStringAttr("hBits"), false, hBits},
      {b.getStringAttr("toHost"), false, bundle({{b.getStringAttr("hReady"), true, bit}, {b.getStringAttr("hValid"), false, bit}})},
      {b.getStringAttr("fromHost"), false, bundle({{b.getStringAttr("hReady"), false, bit}, {b.getStringAttr("hValid"), true, bit}})}});
  auto valid = [&](BundleType bits) { return bundle({{b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bits}}); };
  auto ingress = bundle({{b.getStringAttr("hReady"), true, bit}, {b.getStringAttr("hValid"), false, bit},
      {b.getStringAttr("hBits"), false, bundle({{b.getStringAttr("aw"), false, valid(address)},
          {b.getStringAttr("w"), false, valid(data)}, {b.getStringAttr("ar"), false, valid(address)}})}});
  auto readiness = bundle({{b.getStringAttr("readValid"), false, bit}, {b.getStringAttr("writeValid"), false, bit},
      {b.getStringAttr("hostMemIdle"), false, bit}});
  SmallVector<PortInfo> enginePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("hPort"), hPort, Direction::In},
      {b.getStringAttr("timing"), axi4, Direction::Out}, {b.getStringAttr("ingress"), ingress, Direction::Out},
      {b.getStringAttr("readiness"), readiness, Direction::In}, {b.getStringAttr("targetFire"), bit, Direction::Out},
      {b.getStringAttr("ingressReset"), bit, Direction::Out}, {b.getStringAttr("egressReset"), bit, Direction::Out},
      {b.getStringAttr("modelReset"), bit, Direction::Out}};
  auto engine = b.create<FModuleOp>(loc, b.getStringAttr(engineName), ConventionAttr::get(ctx, Convention::Internal), enginePorts);
  engine->setAttr("goldengate.bridgeConstructor", key);
  b.setInsertionPointToStart(engine.getBodyBlock());
  auto arg = [&](unsigned i) { return engine.getBodyBlock()->getArgument(i); };
  Value hb = field(arg(2), "hBits"), th = field(arg(2), "toHost"), fh = field(arg(2), "fromHost");
  Value targetReset = field(hb, "reset");
  Value resetReady = either(neg(targetReset), field(arg(5), "hostMemIdle"));
  Value ingressValid = both(both(both(both(field(th, "hValid"), field(fh, "hReady")),
      field(arg(5), "writeValid")), field(arg(5), "readValid")), resetReady);
  Value fire = both(ingressValid, field(arg(4), "hReady"));
  connect(field(th, "hReady"), fire); connect(field(fh, "hValid"), fire);
  connect(field(arg(4), "hValid"), ingressValid);
  connect(arg(6), fire); connect(arg(7), either(arg(1), both(targetReset, ingressValid)));
  connect(arg(8), either(arg(1), both(targetReset, fire))); connect(arg(9), targetReset);
  // The later timing-model pass must clock its state only when targetFire is
  // true. Host reset is not added to the SFC targetFire or model-reset gates.
  b.create<ConnectOp>(loc, arg(3), field(hb, "axi4"));
  for (auto n : {"aw", "w", "ar"}) {
    Value source = path(hb, std::string("axi4.") + n), dest = path(arg(4), std::string("hBits.") + n);
    connect(field(dest, "valid"), both(field(source, "ready"), field(source, "valid")));
    connect(field(dest, "bits"), field(source, "bits"));
  }

  auto removed = [&](unsigned i) { return llvm::is_contained(tokenPorts, i); };
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!removed(i)) { copied.push_back(i); ports.push_back(p); }
  for (unsigned j = 0; j < 7; ++j) {
    auto p = enginePorts[j + 3]; p.name = b.getStringAttr(boundary[j]); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), widget = b.create<InstanceOp>(loc, engine, "FASEDMemoryTimingModel_0");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external, p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(widget.getResult(0), outer(*hostClock)); connect(widget.getResult(1), outer(*hostReset));
  hb = field(widget.getResult(2), "hBits"); th = field(widget.getResult(2), "toHost"); fh = field(widget.getResult(2), "fromHost");
  Value allValid = constant(1, 1), allReady = constant(1, 1);
  for (unsigned j = 0; j < 6; ++j) allValid = both(allValid, field(sim.getResult(tokenPorts[j]), "valid"));
  for (unsigned j = 6; j < 11; ++j) allReady = both(allReady, field(sim.getResult(tokenPorts[j]), "ready"));
  connect(field(th, "hValid"), allValid); connect(field(fh, "hReady"), allReady);
  for (unsigned j = 0; j < 11; ++j) {
    Value token = sim.getResult(tokenPorts[j]), gate = field(j < 6 ? th : fh, j < 6 ? "hReady" : "hValid");
    for (unsigned k = j < 6 ? 0 : 6; k < (j < 6 ? 6 : 11); ++k) if (j != k) gate = both(gate, field(sim.getResult(tokenPorts[k]), j < 6 ? "valid" : "ready"));
    connect(field(token, j < 6 ? "ready" : "valid"), gate);
    Value bits = field(token, "bits");
    for (unsigned k = 0; k < paths[j].size(); ++k) {
      Value leaf = payloadFields[j][k].empty() ? bits : path(bits, payloadFields[j][k]);
      Value target = path(hb, paths[j][k]); connect(j < 6 ? target : leaf, j < 6 ? leaf : target);
    }
  }
  for (unsigned j = 0; j < 7; ++j) {
    Value external = wrapper.getBodyBlock()->getArgument(copied.size() + j), internal = widget.getResult(3 + j);
    b.create<ConnectOp>(loc, enginePorts[j + 3].direction == Direction::In ? internal : external, enginePorts[j + 3].direction == Direction::In ? external : internal);
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
  for (unsigned j = 0; j < 11; ++j) {
    NamedAttrList channel(cast<DictionaryAttr>(annotations[channelIndices[j]]));
    SmallVector<Attribute> endpoints;
    for (auto &path : paths[j]) endpoints.push_back(b.getStringAttr(newPrefix + "|" + engineName.str() + ">hPort.hBits." + path));
    channel.set(j < 6 ? "sinks" : "sources", b.getArrayAttr(endpoints));
    annotations[channelIndices[j]] = channel.getDictionary(ctx);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
