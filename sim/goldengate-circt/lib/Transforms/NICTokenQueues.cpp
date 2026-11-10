// See LICENSE for license details.
// Oracle: SimpleNICBridgeModule's HostPort and two
// Queue(..., 10), flow=false, pipe=false. Each constructor-free NIC has
// fifteen latency-one pipe endpoints sharing its own target clock. Widgets
// have independent queue instances, channel gates and external bindings.
// Consumes channel-facing ports; preserves all annotation classes and bodies.
// Targets on consumed ports remain on the retained inner module; copied port
// and host identities transfer to the wrapper. Circuit identities transfer.
// Adds ordinary FIRRTL memories, pointer/reset state and channel gates.
// Hierarchy/port analyses must be rebuilt. Reset flushes queue occupancy only
// on host reset. Incoming keep is constant FF; outgoing keep is synchronized
// with the other source channels although the CPU packet adapter discards it.
// Normal stream packet adapters and MMIO state are separate transforms. The
// explicit compile-time loopback boundary adds the SFC ten-token generator,
// keeps packet keep bits, and cannot enter the normal CPU/MMIO assembly path.
#include "goldengate/NICTokenQueues.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
using goldengate::AnnotationClasses;
using goldengate::resolveAnnotationTarget;

static LogicalResult addNICTokenQueuesImpl(CircuitOp circuit, std::string &error,
                                          bool loopback) {
  StringRef queueName = loopback ? "GGNICLoopbackTokenQueue10" : "GGNICTokenQueue10";
  StringRef wrapperName = loopback ? "GGNICLoopbackTokenQueuesWrapper" : "GGNICTokenQueuesWrapper";
  unsigned tokenWidth = loopback ? 74 : 66;
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC queues require retained annotations");
  SmallVector<DictionaryAttr> bridges;
  for (auto attr : raw) {
    Annotation a(attr); auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      auto bridge = dyn_cast<DictionaryAttr>(attr);
      if (bridge.get("widgetConstructorKey"))
        return reject("SimpleNIC requires its constructor-free BridgeIO annotation");
      bridges.push_back(bridge);
    }
  }
  if (bridges.empty()) return success();
  if (circuit.getName() != "GGCPUStreamPlatformWrapper" &&
      circuit.getName() != "GGTracerVStreamQueueWrapper")
    return reject("NIC queues require the active buffered-stream or CPU platform boundary");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == queueName || m.getModuleName() == wrapperName)
      return reject("NIC queue or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  if (!inner) return reject("NIC queues require an internal top module");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = uint(1);
  auto token = [&](unsigned w) { return BundleType::get(ctx, {
      {b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, uint(w)}}); };
  // The first four are target-to-host; the remaining eleven host-to-target.
  const llvm::StringRef locals[]{"nic_out_bits_data", "nic_out_bits_keep", "nic_out_bits_last", "nic_out_valid",
      "nic_in_bits_data", "nic_in_bits_keep", "nic_in_bits_last", "nic_in_valid", "nic_macAddr",
      "nic_rlimit_inc", "nic_rlimit_period", "nic_rlimit_size", "nic_pauser_threshold", "nic_pauser_quanta", "nic_pauser_refresh"};
  const unsigned widths[]{64, 8, 1, 1, 64, 8, 1, 1, 48, 8, 8, 8, 16, 16, 16};
  struct Widget {
    unsigned channels[15];
    unsigned hostIn, hostOut, controls, done;
  };
  SmallVector<Widget> widgets(bridges.size());
  // Disjointness is global; a widget clock is local. Different NIC widgets
  // may be in different rational target domains and may stall independently.
  std::set<unsigned> consumed;
  std::set<std::string> names;
  for (auto [index, bridge] : llvm::enumerate(bridges)) {
    auto mapping = bridge.getAs<DictionaryAttr>("channelMapping");
    if (!mapping || mapping.size() != 15) return reject("NIC requires fifteen pipe channels");
    Attribute clock;
    auto &widget = widgets[index];
    for (unsigned j = 0; j < 15; ++j) {
      auto global = mapping.getAs<StringAttr>(locals[j]);
      if (!global || !names.insert(global.getValue().str()).second)
        return reject("missing or aliased NIC channel mapping");
      DictionaryAttr channel;
      for (auto attr : raw) {
        Annotation a(attr);
        if (a.isClass(AnnotationClasses::ChannelConnection) && a.getMember<StringAttr>("globalName") == global) {
          if (channel) return reject("duplicate NIC channel annotation");
          channel = dyn_cast<DictionaryAttr>(attr);
        }
      }
      auto info = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr();
      auto cls = info ? info.getAs<StringAttr>("class") : StringAttr();
      auto latency = info ? info.getAs<IntegerAttr>("latency") : IntegerAttr();
      auto ends = channel ? channel.getAs<ArrayAttr>(j < 4 ? "sources" : "sinks") : ArrayAttr();
      auto opposite = channel ? channel.getAs<ArrayAttr>(j < 4 ? "sinks" : "sources") : ArrayAttr();
      if (!cls || cls.getValue() != AnnotationClasses::PipeChannel || !latency || latency.getInt() != 1 ||
          !ends || ends.size() != 1 || (opposite && !opposite.empty()))
        return reject("NIC requires single-boundary latency-one pipe channels");
      auto c = channel.get("clock");
      if (!c || (clock && c != clock)) return reject("NIC channels must share one target clock");
      clock = c;
      auto spelling = dyn_cast<StringAttr>(ends[0]);
      auto t = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
      if (!t || t->module != inner || !t->port ||
          inner.getPortType(*t->port) != token(widths[j]) ||
          inner.getPorts()[*t->port].direction != (j < 4 ? Direction::Out : Direction::In) ||
          t->fieldID != token(widths[j]).getFieldID(2) || !consumed.insert(*t->port).second)
        return reject("NIC endpoint must resolve to a distinct typed active pipe payload");
      widget.channels[j] = *t->port;
    }
    // Channel clock analysis already resolved this identity before FAME. At
    // this boundary FAME has removed the physical target-clock port; retain the
    // common historical identity rather than interpreting it as a host clock.
    if (!isa<StringAttr>(clock)) return reject("NIC needs a retained target clock identity");
  }
  auto hostPort = [&](llvm::StringRef name, Type type) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == Direction::In) return i;
    return std::nullopt;
  };
  auto hostClock = hostPort("hostClock", ClockType::get(ctx)), hostReset = hostPort("hostReset", bit);
  if (!hostClock || !hostReset) return reject("NIC needs host clock and reset inputs");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("NIC queues require an uninstantiated top");
  for (auto p : inner.getPorts())
    if (p.name.getValue().starts_with("nic_") ||
        (p.name.getValue().starts_with("nic") && p.name.getValue().size() > 3 &&
         llvm::StringRef("0123456789").contains(p.name.getValue()[3]))) return reject("NIC host boundary already exists");

  // All boundary rejections precede mutation. Bits[63:0] data and [64] last are common. Normal packets force
  // incoming keep=FF; loopback must retain outgoing keep[72:65], valid[73].
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> qp{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("enq"), token(tokenWidth), Direction::In},
      {b.getStringAttr("deq"), token(tokenWidth), Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(queueName), ConventionAttr::get(ctx, Convention::Internal), qp);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value c, Value a, Value d) -> Value { return b.create<MuxPrimOp>(loc, c, a, d); };
  auto qa = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto reg = [&](unsigned w, llvm::StringRef n) -> Value {
    return b.create<RegResetOp>(loc, uint(w), qa(0), qa(1), constant(w, 0), n).getResult();
  };
  Value enqPtr = reg(4, "enq_ptr_value"), deqPtr = reg(4, "deq_ptr_value"), maybeFull = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value ready = neg(both(equal, maybeFull)), valid = neg(both(equal, neg(maybeFull)));
  Value push = both(ready, field(qa(2), "valid")), pop = both(valid, field(qa(3), "ready"));
  connect(field(qa(2), "ready"), ready); connect(field(qa(3), "valid"), valid);
  auto increment = [&](Value p) -> Value {
    return mux(b.create<EQPrimOp>(loc, p, constant(4, 9)), constant(4, 0),
        b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, p, constant(4, 1)), 3, 0));
  };
  connect(enqPtr, mux(push, increment(enqPtr), enqPtr)); connect(deqPtr, mux(pop, increment(deqPtr), deqPtr));
  connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));
  SmallVector<Type> mt{MemOp::getTypeForPort(10, uint(tokenWidth), MemOp::PortKind::Read),
      MemOp::getTypeForPort(10, uint(tokenWidth), MemOp::PortKind::Write)};
  SmallVector<Attribute> mn{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, mt, 0, 1, 10, RUWAttr::Undefined, mn, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), qa(0)); connect(field(reader, "en"), constant(1, 1));
  connect(field(reader, "addr"), deqPtr); connect(field(qa(3), "bits"), field(reader, "data"));
  connect(field(writer, "clk"), qa(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), constant(1, 1));
  connect(field(writer, "data"), field(qa(2), "bits"));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!consumed.count(i)) { copied.push_back(i); ports.push_back(p); }
  auto externalName = [&](unsigned index, llvm::StringRef original) {
    // Preserve the established single-widget ABI and use explicit indices
    // for every binding in a multiple-widget boundary.
    return bridges.size() == 1 ? original.str() :
        "nic" + std::to_string(index) + "_" + original.drop_front(4).str();
  };
  for (auto [index, widget] : llvm::enumerate(widgets)) {
    if (!loopback) {
    widget.hostIn = ports.size();
    ports.push_back({b.getStringAttr(externalName(index, "nic_from_host_enq")), token(tokenWidth), Direction::In});
    widget.hostOut = ports.size();
    ports.push_back({b.getStringAttr(externalName(index, "nic_to_host_deq")), token(tokenWidth), Direction::Out});
    widget.controls = ports.size();
    for (unsigned j = 8; j < 15; ++j)
      ports.push_back({b.getStringAttr(externalName(index, locals[j])), uint(widths[j]), Direction::In});
    }
    widget.done = ports.size();
    ports.push_back({b.getStringAttr(externalName(index, "nic_done")), bit, Direction::Out});
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto arg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  auto outer = [&](unsigned i) { return arg(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : arg(j),
        p.direction == Direction::In ? arg(j) : sim.getResult(i));
  }
  SmallVector<Attribute> bindings;
  for (auto [index, widget] : llvm::enumerate(widgets)) {
    auto queueInstanceName = [&](llvm::StringRef original) {
      return widgets.size() == 1 ? original.str() :
          "nic" + std::to_string(index) + "_" + original.str();
    };
    auto htnt = b.create<InstanceOp>(loc, queue, queueInstanceName("htnt_queue"));
    auto ntht = b.create<InstanceOp>(loc, queue, queueInstanceName("ntht_queue"));
    for (auto q : {htnt, ntht}) { connect(q.getResult(0), outer(*hostClock)); connect(q.getResult(1), outer(*hostReset)); }
    if (!loopback) {
      b.create<ConnectOp>(loc, htnt.getResult(2), arg(widget.hostIn));
      b.create<ConnectOp>(loc, arg(widget.hostOut), ntht.getResult(3));
    } else {
      // Unchanged SFC HostToNICTokenGenerator(10): wait one cycle, seed
      // exactly ten invalid target tokens subject to queue backpressure,
      // then forward ntht tokens in order. Reset only occupancy/scheduler.
      Value state = b.create<RegResetOp>(loc, uint(2), outer(*hostClock),
          outer(*hostReset), constant(2, 0), queueInstanceName("loopback_state")).getResult();
      Value count = b.create<RegResetOp>(loc, uint(4), outer(*hostClock),
          outer(*hostReset), constant(4, 0), queueInstanceName("loopback_seed_count")).getResult();
      Value initial = b.create<EQPrimOp>(loc, state, constant(2, 0));
      Value seeding = b.create<EQPrimOp>(loc, state, constant(2, 1));
      Value forwarding = b.create<EQPrimOp>(loc, state, constant(2, 2));
      Value in = ntht.getResult(3), out = htnt.getResult(2);
      Value outValid = b.create<OrPrimOp>(loc, seeding, both(forwarding, field(in, "valid")));
      connect(field(out, "valid"), outValid);
      connect(field(in, "ready"), both(forwarding, field(out, "ready")));
      Value inputBits = field(in, "bits");
      Value payloadValid = both(forwarding, b.create<BitsPrimOp>(loc, inputBits, 73, 73));
      connect(field(out, "bits"), b.create<CatPrimOp>(loc, payloadValid,
          b.create<BitsPrimOp>(loc, inputBits, 72, 0)));
      Value seedFire = both(seeding, both(outValid, field(out, "ready")));
      Value lastSeed = b.create<EQPrimOp>(loc, count, constant(4, 9));
      connect(count, mux(seedFire, mux(lastSeed, constant(4, 0),
          b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, count, constant(4, 1)), 3, 0)), count));
      connect(state, mux(both(seedFire, lastSeed), constant(2, 2),
          mux(initial, constant(2, 1), state)));
    }
    Value incoming = htnt.getResult(3), outgoing = ntht.getResult(2);
    Value allValid = constant(1, 1), allReady = constant(1, 1);
    for (unsigned j = 0; j < 4; ++j) allValid = both(allValid, field(sim.getResult(widget.channels[j]), "valid"));
    for (unsigned j = 4; j < 15; ++j) allReady = both(allReady, field(sim.getResult(widget.channels[j]), "ready"));
    connect(field(outgoing, "valid"), allValid); connect(field(incoming, "ready"), allReady);
    Value data = b.create<CatPrimOp>(loc, field(sim.getResult(widget.channels[2]), "bits"),
        field(sim.getResult(widget.channels[0]), "bits"));
    if (loopback) data = b.create<CatPrimOp>(loc, field(sim.getResult(widget.channels[1]), "bits"), data);
    Value packed = b.create<CatPrimOp>(loc, field(sim.getResult(widget.channels[3]), "bits"), data);
    connect(field(outgoing, "bits"), packed);
    for (unsigned j = 0; j < 15; ++j) {
      Value t = sim.getResult(widget.channels[j]), gate = field(j < 4 ? outgoing : incoming, j < 4 ? "ready" : "valid");
      for (unsigned k = j < 4 ? 0 : 4; k < (j < 4 ? 4 : 15); ++k)
        if (j != k) gate = both(gate, field(sim.getResult(widget.channels[k]), j < 4 ? "valid" : "ready"));
      connect(field(t, j < 4 ? "ready" : "valid"), gate);
      if (j >= 4) {
        Value value;
        if (j == 4) value = b.create<BitsPrimOp>(loc, field(incoming, "bits"), 63, 0);
        else if (j == 5) value = loopback ? Value(b.create<BitsPrimOp>(loc, field(incoming, "bits"), 72, 65)) : constant(8, 255);
        else if (j == 6 || j == 7) {
          unsigned offset = j == 6 ? 64 : loopback ? 73 : 65;
          value = b.create<BitsPrimOp>(loc, field(incoming, "bits"), offset, offset);
        } else if (!loopback) value = arg(widget.controls + j - 8);
        else if (j < 12) value = constant(widths[j], j == 9 ? 1 : j == 11 ? 8 : 0);
        // SFC leaves pauser fields unspecified in compile-time loopback. Do
        // not invent a reset-defined value or use them for acceptance cuts.
        else value = b.create<InvalidValueOp>(loc, uint(widths[j]));
        connect(field(t, "bits"), value);
      }
    }
    // SFC genROReg(!tFire) samples channel progress, independent of queue
    // capacity. Expose the combinational source here for the later MMIO bank.
    connect(arg(widget.done), neg(both(allValid, allReady)));
    bindings.push_back(b.getDictionaryAttr({
        b.getNamedAttr("widget", b.getI64IntegerAttr(index)),
        b.getNamedAttr("bridgeTarget", bridges[index].get("target") ?
            bridges[index].get("target") : b.getStringAttr("")),
        b.getNamedAttr("mode", b.getStringAttr(loopback ? "compile_time_loopback" : "normal")),
        b.getNamedAttr("fromHost", b.getStringAttr(loopback ? "" : externalName(index, "nic_from_host_enq"))),
        b.getNamedAttr("toHost", b.getStringAttr(loopback ? "" : externalName(index, "nic_to_host_deq")))}));
  }
  std::string op = "~" + circuit.getName().str(), np = "~" + wrapperName.str(), mp = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue(); if (v == op) return b.getStringAttr(np); if (!v.consume_front(op + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(mp)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) { suffix.replace(0, mp.size(), "|" + wrapperName.str() + ">"); break; }
      }
      return b.getStringAttr(np + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto v : arr) out.push_back(retarget(v)); return b.getArrayAttr(out); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto v : d) out.set(v.getName(), retarget(v.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  wrapper->setAttr("goldengate.nicTokenBindings", retarget(b.getArrayAttr(bindings)));
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}

LogicalResult goldengate::addNICTokenQueues(CircuitOp circuit, std::string &error) {
  return addNICTokenQueuesImpl(circuit, error, false);
}
LogicalResult goldengate::addNICLoopbackTokenQueues(CircuitOp circuit, std::string &error) {
  return addNICTokenQueuesImpl(circuit, error, true);
}
