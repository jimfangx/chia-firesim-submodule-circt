// See LICENSE for license details.
// Requires: uninstantiated TSI bound top, one-tracker BlockDev constructor,
// nine single-clock channels with exact endpoint types and retained targets.
// Consumes: nine channel-facing top ports; no annotation classes are consumed.
// Produces: constructor metadata on the engine, transferred bridge/channel
// targets and ordinary FIRRTL HostPort gates, cycle and write-tracker state.
// Mutates: adds engine/wrapper, replaces channel boundaries, exposes functional
// queues, timing controls and committed read/write latency inputs for subsequent passes.
// Analyses required: target resolution. Hierarchy/port analyses are invalidated;
// clock, channel and constructor metadata are preserved with explicit transfer.
// Output: valid FIRRTL; beat count is unreset and updates only on target fire,
// allocation resets on host or qualified target reset. Completion has priority
// over a same-cycle request and is not reset- or latency-pipe-ready-gated.
#include "goldengate/BlockDevTokenEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <tuple>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addBlockDevTokenEngine(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral engineName = "GGBlockDevTokenEngine";
  constexpr llvm::StringLiteral wrapperName = "GGBlockDevTokenWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGTSIBridgeBoundWrapper")
    return reject("BlockDev token mapping requires the active TSI binding wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == engineName || m.getModuleName() == wrapperName)
      return reject("BlockDev token engine or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("BlockDev needs a top and retained annotations");
  DictionaryAttr bridge; unsigned bridgeIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation a(attr); auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "firechip.goldengateimplementations.BlockDevBridgeModule") {
      if (bridge) return reject("multiple BlockDev constructors are unsupported");
      bridge = cast<DictionaryAttr>(attr); bridgeIndex = i;
    }
  }
  auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr();
  auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr();
  auto keyClass = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto trackers = key ? key.getAs<IntegerAttr>("nTrackers") : IntegerAttr();
  if (!keyClass || keyClass.getValue() != "firechip.bridgeinterfaces.BlockDeviceConfig" ||
      !trackers || trackers.getInt() != 1 || !mapping || mapping.size() != 9)
    return reject("BlockDev requires the Rocket one-tracker constructor and nine channels");
  auto *ctx = circuit.getContext();
  auto bit = UIntType::get(ctx, 1, false), word = UIntType::get(ctx, 32, false);
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  OpBuilder b(ctx);
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> e) { return BundleType::get(ctx, e); };
  // The Rocket one-tracker handoff carries a one-bit tag.
  auto request = bundle({{b.getStringAttr("tag"), false, bit}, {b.getStringAttr("len"), false, word},
      {b.getStringAttr("offset"), false, word}, {b.getStringAttr("write"), false, bit}});
  auto data = bundle({{b.getStringAttr("tag"), false, bit}, {b.getStringAttr("data"), false, uint(64)}});
  auto infoType = bundle({{b.getStringAttr("nsectors"), false, word}, {b.getStringAttr("max_req_len"), false, word}});
  const llvm::StringRef locals[]{"bdev_req_fwd", "bdev_data_fwd", "bdev_resp_rev", "reset",
      "bdev_req_rev", "bdev_data_rev", "bdev_resp_fwd", "bdev_info_nsectors", "bdev_info_max_req_len"};
  const std::vector<std::string> paths[]{
      {"bdev.req.bits.tag", "bdev.req.bits.len", "bdev.req.bits.offset", "bdev.req.bits.write", "bdev.req.valid"},
      {"bdev.data.bits.tag", "bdev.data.bits.data", "bdev.data.valid"}, {"bdev.resp.ready"}, {"reset"},
      {"bdev.req.ready"}, {"bdev.data.ready"}, {"bdev.resp.bits.tag", "bdev.resp.bits.data", "bdev.resp.valid"},
      {"bdev.info.nsectors"}, {"bdev.info.max_req_len"}};
  unsigned tokenPorts[9], channelIndices[9];
  SmallVector<SmallVector<std::string>> payloadFields;
  Attribute clock;
  for (unsigned j = 0; j < 9; ++j) {
    auto name = mapping.getAs<StringAttr>(locals[j]);
    if (!name) return reject("incomplete BlockDev channel mapping");
    DictionaryAttr channel;
    for (auto [i, attr] : llvm::enumerate(raw)) {
      Annotation a(attr);
      if (a.isClass(AnnotationClasses::ChannelConnection) && a.getMember<StringAttr>("globalName") == name) {
        if (channel) return reject("duplicate BlockDev channel");
        channel = cast<DictionaryAttr>(attr); channelIndices[j] = i;
      }
    }
    bool forward = j == 0 || j == 1 || j == 6, pipe = j == 3 || j > 6;
    auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
    auto ends = channel ? channel.getAs<ArrayAttr>(j < 4 ? "sources" : "sinks") : ArrayAttr();
    auto opposite = channel ? channel.getAs<ArrayAttr>(j < 4 ? "sinks" : "sources") : ArrayAttr();
    auto cls = pipe ? AnnotationClasses::PipeChannel : forward ? AnnotationClasses::DecoupledForwardChannel : AnnotationClasses::DecoupledReverseChannel;
    auto infoClass = info ? info.getAs<StringAttr>("class") : StringAttr();
    if (!infoClass || infoClass.getValue() != cls || !ends || ends.size() != paths[j].size() || (opposite && !opposite.empty()))
      return reject("BlockDev needs nine single-boundary Pipe/Decoupled channels");
    if (pipe) {
      auto latency = info.getAs<IntegerAttr>("latency");
      if (!latency || latency.getInt() != 1) return reject("BlockDev pipes need latency one");
    }
    auto channelClock = channel.get("clock");
    if (!channelClock || (clock && clock != channelClock)) return reject("BlockDev channels must share one clock");
    clock = channelClock;
    auto spelling = dyn_cast<StringAttr>(ends[0]);
    auto t = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    if (!t || t->module != inner || !t->port) return reject("BlockDev endpoint must resolve to an active wrapper port");
    unsigned port = *t->port;
    FIRRTLBaseType payload = j > 6 ? FIRRTLBaseType(word) : FIRRTLBaseType(bit);
    SmallVector<std::string> fields;
    if (forward) {
      auto bits = j == 0 ? request : data;
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
      auto bits = j == 0 ? request : data;
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
    if (inner.getPortType(port) != token || inner.getPortDirection(port) != (j < 4 ? Direction::Out : Direction::In))
      return reject("BlockDev endpoint has an unsupported payload or direction");
    // Preserve endpoint order: each descriptor leaf identifies its corresponding
    // target field, including the one-bit tag placeholder.
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
        return reject("BlockDev payload endpoints are missing, shared or reordered");
    }
    tokenPorts[j] = port; payloadFields.push_back(fields);
    for (unsigned k = 0; k < j; ++k) if (tokenPorts[k] == port) return reject("BlockDev token ports must be distinct");
  }
  for (unsigned j : {0u, 1u, 6u}) {
    auto info = cast<DictionaryAttr>(raw[channelIndices[j]]).getAs<DictionaryAttr>("channelInfo");
    unsigned reverse = j == 0 ? 4 : j == 1 ? 5 : 2;
    for (bool valid : {false, true}) {
      auto spelling = info.getAs<StringAttr>(valid ? (j < 4 ? "validSource" : "validSink") : (j < 4 ? "readySink" : "readySource"));
      unsigned port = tokenPorts[valid ? j : reverse];
      auto t = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
      auto token = cast<BundleType>(inner.getPortType(port)); unsigned id = token.getFieldID(2);
      if (valid) { auto p = cast<BundleType>(token.getElement("bits")->type); id += p.getFieldID(*p.getElementIndex("valid")); }
      if (!t || t->module != inner || t->port != port || t->fieldID != id)
        return reject("BlockDev forward descriptor disagrees with its handshake endpoints");
    }
  }
  const llvm::StringRef boundary[]{"blockdev_req_enq", "blockdev_data_enq", "blockdev_rresp_deq", "blockdev_wack_deq",
      "blockdev_timing", "blockdev_info", "blockdev_tfire", "blockdev_queue_reset",
      "blockdev_write_latency_enq_valid", "blockdev_read_latency_enq", "blockdev_resp_ready"};
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (llvm::is_contained(boundary, p.name.getValue())) return reject("BlockDev boundary already exists");
    if (p.name == "hostClock" && p.direction == Direction::In && isa<ClockType>(p.type)) hostClock = i;
    if (p.name == "hostReset" && p.direction == Direction::In && p.type == bit) hostReset = i;
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (!hostClock || !hostReset || used) return reject("BlockDev needs host controls and an uninstantiated top");
  Location loc = circuit.getLoc(); b.setInsertionPointToStart(circuit.getBodyBlock());
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto path = [&](Value v, llvm::StringRef n) { SmallVector<llvm::StringRef> parts; n.split(parts, '.'); for (auto part : parts) v = field(v, part); return v; };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto queue = [&](FIRRTLBaseType payload) { return bundle({{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}}); };
  auto decoupled = [&](BundleType payload, bool flipped) { return bundle({{b.getStringAttr("ready"), !flipped, bit},
      {b.getStringAttr("valid"), flipped, bit}, {b.getStringAttr("bits"), flipped, payload}}); };
  auto bdev = bundle({{b.getStringAttr("req"), false, decoupled(request, false)}, {b.getStringAttr("data"), false, decoupled(data, false)},
      {b.getStringAttr("resp"), false, decoupled(data, true)}, {b.getStringAttr("info"), true, infoType}});
  auto hBits = bundle({{b.getStringAttr("bdev"), false, bdev}, {b.getStringAttr("reset"), false, bit}});
  auto hPort = bundle({{b.getStringAttr("hBits"), false, hBits},
      {b.getStringAttr("toHost"), false, bundle({{b.getStringAttr("hReady"), true, bit}, {b.getStringAttr("hValid"), false, bit}})},
      {b.getStringAttr("fromHost"), false, bundle({{b.getStringAttr("hReady"), false, bit}, {b.getStringAttr("hValid"), true, bit}})}});
  auto timing = bundle({{b.getStringAttr("returnWrite"), false, bit}, {b.getStringAttr("readRespBusy"), false, bit},
      {b.getStringAttr("wAckStallN"), true, bit}, {b.getStringAttr("rRespStallN"), true, bit}, {b.getStringAttr("tCycle"), true, uint(24)}});
  SmallVector<PortInfo> enginePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("hPort"), hPort, Direction::In},
      {b.getStringAttr("reqBuf_io_enq"), queue(request), Direction::Out}, {b.getStringAttr("dataBuf_io_enq"), queue(data), Direction::Out},
      {b.getStringAttr("rRespBuf_io_deq"), queue(data), Direction::In}, {b.getStringAttr("wAckBuf_io_deq"), queue(bit), Direction::In},
      {b.getStringAttr("timing"), timing, Direction::In}, {b.getStringAttr("info"), infoType, Direction::In},
      {b.getStringAttr("tFire"), bit, Direction::Out}, {b.getStringAttr("queueReset"), bit, Direction::Out},
      {b.getStringAttr("writeLatencyEnqValid"), bit, Direction::Out},
      {b.getStringAttr("readLatencyEnq"), bundle({{b.getStringAttr("valid"), false, bit},
          {b.getStringAttr("bits"), false, word}}), Direction::Out},
      {b.getStringAttr("respReady"), bit, Direction::Out}};
  auto engine = b.create<FModuleOp>(loc, b.getStringAttr(engineName), ConventionAttr::get(ctx, Convention::Internal), enginePorts);
  engine->setAttr("goldengate.bridgeConstructor", key);
  b.setInsertionPointToStart(engine.getBodyBlock());
  auto arg = [&](unsigned i) { return engine.getBodyBlock()->getArgument(i); };
  Value hb = field(arg(2), "hBits"), th = field(arg(2), "toHost"), fh = field(arg(2), "fromHost");
  Value hostValid = field(th, "hValid"), hostReady = field(fh, "hReady"), reqReady = field(arg(3), "ready"), dataReady = field(arg(4), "ready");
  Value returningWrite = field(arg(7), "returnWrite"), readBusy = field(arg(7), "readRespBusy");
  Value wAckStallN = either(neg(returningWrite), field(arg(6), "valid")), rRespStallN = either(neg(readBusy), field(arg(5), "valid"));
  Value predicates[]{hostValid, hostReady, reqReady, dataReady, rRespStallN, wAckStallN};
  auto fireExcept = [&](int excluded) { Value v = constant(1, 1); for (unsigned i = 0; i < 6; ++i) if (int(i) != excluded) v = both(v, predicates[i]); return v; };
  Value fire = fireExcept(-1);
  connect(arg(9), fire); connect(field(th, "hReady"), fire); connect(field(fh, "hValid"), fire);
  // Reset uses channel predicates only; queue/timing stalls must not suppress it.
  Value queueReset = either(arg(1), both(both(hostValid, hostReady), field(hb, "reset")));
  connect(arg(10), queueReset);
  connect(field(arg(7), "wAckStallN"), wAckStallN); connect(field(arg(7), "rRespStallN"), rRespStallN);
  Value cycle = b.create<RegResetOp>(loc, uint(24), arg(0), arg(1), constant(24, 0), "tCycle").getResult();
  Value increment = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, cycle, constant(24, 1)), 23, 0);
  connect(cycle, b.create<MuxPrimOp>(loc, fire, increment, cycle)); connect(field(arg(7), "tCycle"), cycle);
  for (auto [queueIndex, name, excluded] : {std::tuple<unsigned, llvm::StringRef, int>{3, "req", 2}, {4, "data", 3}}) {
    auto target = path(hb, "bdev." + name.str());
    connect(field(arg(queueIndex), "valid"), both(field(target, "valid"), fireExcept(excluded)));
    connect(field(arg(queueIndex), "bits"), field(target, "bits")); connect(field(target, "ready"), constant(1, 1));
  }
  connect(field(arg(6), "ready"), both(both(fireExcept(5), returningWrite), path(hb, "bdev.resp.ready")));
  connect(field(arg(5), "ready"), both(both(fireExcept(4), readBusy), path(hb, "bdev.resp.ready")));
  connect(arg(13), path(hb, "bdev.resp.ready"));
  connect(path(hb, "bdev.resp.valid"), either(returningWrite, readBusy));
  Value useRead = both(field(arg(5), "valid"), readBusy), useWrite = both(field(arg(6), "valid"), returningWrite);
  auto select = [&](Value condition, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, condition, yes, no); };
  connect(path(hb, "bdev.resp.bits.data"), select(useRead, path(arg(5), "bits.data"), constant(64, 0)));
  connect(path(hb, "bdev.resp.bits.tag"), select(useRead, path(arg(5), "bits.tag"), select(useWrite, field(arg(6), "bits"), constant(1, 0))));
  connect(path(hb, "bdev.info"), arg(8));

  // BlockDevBridgeModule.scala's one-tracker write timing input. Requests and
  // data use target ready (constant true), not individual functional queue
  // valid signals: the common tFire commits both only once per target cycle.
  Value count = b.create<RegOp>(loc, word, arg(0), "wBeatCounters_0").getResult();
  Value allocated = b.create<RegResetOp>(loc, bit, arg(0), queueReset,
                                        constant(1, 0), "wValid_0").getResult();
  Value requestValid = path(hb, "bdev.req.valid");
  // Read timing uses committed target req.fire (target ready is constant one),
  // not reqBuf enqueue valid with its own capacity predicate excluded.
  connect(field(arg(12), "valid"), both(both(fire, requestValid), neg(path(hb, "bdev.req.bits.write"))));
  connect(field(arg(12), "bits"), path(hb, "bdev.req.bits.len"));
  Value writeRequest = both(both(requestValid, path(hb, "bdev.req.bits.write")),
                            neg(path(hb, "bdev.req.bits.tag")));
  Value writeData = both(path(hb, "bdev.data.valid"), neg(path(hb, "bdev.data.bits.tag")));
  Value done = both(writeData, b.create<EQPrimOp>(loc, count, constant(32, 1)));
  Value completed = both(fire, done);
  connect(arg(11), completed);
  // 512-byte sectors / 64-bit beats = 64 beats per sector. Preserve the full
  // 39-bit Scala product for the assertion; register assignments truncate.
  Value beats = b.create<PadPrimOp>(loc,
      b.create<CatPrimOp>(loc, path(hb, "bdev.req.bits.len"), constant(6, 0)), 39);
  auto low32 = [&](Value v) -> Value { return b.create<BitsPrimOp>(loc, v, 31, 0); };
  Value newCount = low32(b.create<SubPrimOp>(loc, beats,
      select(writeData, constant(39, 1), constant(39, 0))));
  Value decrement = low32(b.create<SubPrimOp>(loc, count, constant(32, 1)));
  Value nextCount = select(done, count, select(writeRequest, newCount,
                                              select(writeData, decrement, count)));
  connect(count, select(fire, nextCount, count));
  connect(allocated, select(fire, select(done, constant(1, 0),
                                       select(writeRequest, constant(1, 1), allocated)), allocated));
  Value assertionsEnabled = both(fire, neg(queueReset));
  Value permittedLength = either(neg(requestValid),
      b.create<LTPrimOp>(loc, beats, constant(39, 0xFFFFFFFFull)));
  b.create<AssertOp>(loc, arg(0), permittedLength, assertionsEnabled,
      "Transaction length exceeds timing model maximum supported length", ValueRange{}, "");
  b.create<AssertOp>(loc, arg(0), allocated, both(completed, neg(queueReset)),
      "Write data received for unallocated tracker: 0", ValueRange{}, "");
  b.create<AssertOp>(loc, arg(0), allocated,
      both(assertionsEnabled, both(both(neg(done), neg(writeRequest)), writeData)),
      "Write data received for unallocated tracker: 0", ValueRange{}, "");

  auto removed = [&](unsigned i) { return llvm::is_contained(tokenPorts, i); };
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!removed(i)) { copied.push_back(i); ports.push_back(p); }
  for (unsigned j = 0; j < 11; ++j) { auto p = enginePorts[j + 3]; p.name = b.getStringAttr(boundary[j]); ports.push_back(p); }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), widget = b.create<InstanceOp>(loc, engine, "BlockDevBridgeModule_0");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external, p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(widget.getResult(0), outer(*hostClock)); connect(widget.getResult(1), outer(*hostReset));
  hb = field(widget.getResult(2), "hBits"); th = field(widget.getResult(2), "toHost"); fh = field(widget.getResult(2), "fromHost");
  Value allValid = constant(1, 1), allReady = constant(1, 1);
  for (unsigned j = 0; j < 4; ++j) allValid = both(allValid, field(sim.getResult(tokenPorts[j]), "valid"));
  for (unsigned j = 4; j < 9; ++j) allReady = both(allReady, field(sim.getResult(tokenPorts[j]), "ready"));
  connect(field(th, "hValid"), allValid); connect(field(fh, "hReady"), allReady);
  for (unsigned j = 0; j < 9; ++j) {
    Value token = sim.getResult(tokenPorts[j]), gate = field(j < 4 ? th : fh, j < 4 ? "hReady" : "hValid");
    for (unsigned k = j < 4 ? 0 : 4; k < (j < 4 ? 4 : 9); ++k) if (j != k) gate = both(gate, field(sim.getResult(tokenPorts[k]), j < 4 ? "valid" : "ready"));
    connect(field(token, j < 4 ? "ready" : "valid"), gate);
    Value bits = field(token, "bits");
    for (unsigned k = 0; k < paths[j].size(); ++k) {
      Value leaf = payloadFields[j][k].empty() ? bits : path(bits, payloadFields[j][k]);
      Value target = path(hb, paths[j][k]); connect(j < 4 ? target : leaf, j < 4 ? leaf : target);
    }
  }
  for (unsigned j = 0; j < 11; ++j) {
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
  for (unsigned j = 0; j < 9; ++j) {
    NamedAttrList channel(cast<DictionaryAttr>(annotations[channelIndices[j]]));
    SmallVector<Attribute> endpoints;
    for (auto &path : paths[j]) endpoints.push_back(b.getStringAttr(newPrefix + "|" + engineName.str() + ">hPort.hBits." + path));
    channel.set(j < 4 ? "sinks" : "sources", b.getArrayAttr(endpoints));
    annotations[channelIndices[j]] = channel.getDictionary(ctx);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
