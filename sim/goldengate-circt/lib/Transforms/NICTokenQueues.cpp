// See LICENSE for license details.
// Oracle: SimpleNICBridgeModule's normal (non-loopback) HostPort and two
// Queue(..., 10), flow=false, pipe=false. Requires one constructor-free NIC,
// fifteen latency-one single-clock pipe endpoints on an uninstantiated top.
// Consumes channel-facing ports; preserves all annotation classes and bodies.
// Targets on consumed ports remain on the retained inner module; copied port
// and host identities transfer to the wrapper. Circuit identities transfer.
// Adds ordinary FIRRTL memories, pointer/reset state and channel gates.
// Hierarchy/port analyses must be rebuilt. Reset flushes queue occupancy only
// on host reset. Incoming keep is constant FF; outgoing keep is synchronized
// with the other source channels although the CPU packet adapter discards it.
// Stream packet adapters, MMIO state and loopback mode are subsequent work.
#include "goldengate/NICTokenQueues.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addNICTokenQueues(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGNICTokenQueue10";
  constexpr llvm::StringLiteral wrapperName = "GGNICTokenQueuesWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC queues require retained annotations");
  DictionaryAttr bridge;
  for (auto attr : raw) {
    Annotation a(attr); auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (bridge) return reject("multiple NIC widgets require distinct stream/control bindings");
      bridge = dyn_cast<DictionaryAttr>(attr);
    }
  }
  if (!bridge) return success();
  if (bridge.get("widgetConstructorKey"))
    return reject("SimpleNIC requires its constructor-free BridgeIO annotation");
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
  auto mapping = bridge.getAs<DictionaryAttr>("channelMapping");
  if (!mapping || mapping.size() != 15) return reject("NIC requires fifteen pipe channels");
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
  unsigned channels[15]; Attribute clock; std::set<unsigned> consumed;
  std::set<std::string> names;
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
    channels[j] = *t->port;
  }
  // Channel clock analysis already resolved this identity before FAME. At
  // this boundary FAME has removed the physical target-clock port; retain the
  // common historical identity rather than interpreting it as a host clock.
  if (!isa<StringAttr>(clock)) return reject("NIC needs a retained target clock identity");
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
    if (p.name.getValue().starts_with("nic_")) return reject("NIC host boundary already exists");

  // All boundary rejections precede mutation. Bits[63:0] data, [64] last,
  // [65] valid are the nonconstant packet fields in both queues.
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> qp{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("enq"), token(66), Direction::In},
      {b.getStringAttr("deq"), token(66), Direction::Out}};
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
  SmallVector<Type> mt{MemOp::getTypeForPort(10, uint(66), MemOp::PortKind::Read),
      MemOp::getTypeForPort(10, uint(66), MemOp::PortKind::Write)};
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
  unsigned hostIn = ports.size(); ports.push_back({b.getStringAttr("nic_from_host_enq"), token(66), Direction::In});
  unsigned hostOut = ports.size(); ports.push_back({b.getStringAttr("nic_to_host_deq"), token(66), Direction::Out});
  unsigned controls = ports.size();
  for (unsigned j = 8; j < 15; ++j) ports.push_back({b.getStringAttr(locals[j]), uint(widths[j]), Direction::In});
  unsigned done = ports.size(); ports.push_back({b.getStringAttr("nic_done"), bit, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), htnt = b.create<InstanceOp>(loc, queue, "htnt_queue"), ntht = b.create<InstanceOp>(loc, queue, "ntht_queue");
  auto arg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  auto outer = [&](unsigned i) { return arg(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : arg(j),
        p.direction == Direction::In ? arg(j) : sim.getResult(i));
  }
  for (auto q : {htnt, ntht}) { connect(q.getResult(0), outer(*hostClock)); connect(q.getResult(1), outer(*hostReset)); }
  b.create<ConnectOp>(loc, htnt.getResult(2), arg(hostIn)); b.create<ConnectOp>(loc, arg(hostOut), ntht.getResult(3));
  Value incoming = htnt.getResult(3), outgoing = ntht.getResult(2);
  Value allValid = constant(1, 1), allReady = constant(1, 1);
  for (unsigned j = 0; j < 4; ++j) allValid = both(allValid, field(sim.getResult(channels[j]), "valid"));
  for (unsigned j = 4; j < 15; ++j) allReady = both(allReady, field(sim.getResult(channels[j]), "ready"));
  connect(field(outgoing, "valid"), allValid); connect(field(incoming, "ready"), allReady);
  Value packed = b.create<CatPrimOp>(loc, field(sim.getResult(channels[3]), "bits"),
      b.create<CatPrimOp>(loc, field(sim.getResult(channels[2]), "bits"), field(sim.getResult(channels[0]), "bits")));
  connect(field(outgoing, "bits"), packed);
  for (unsigned j = 0; j < 15; ++j) {
    Value t = sim.getResult(channels[j]), gate = field(j < 4 ? outgoing : incoming, j < 4 ? "ready" : "valid");
    for (unsigned k = j < 4 ? 0 : 4; k < (j < 4 ? 4 : 15); ++k)
      if (j != k) gate = both(gate, field(sim.getResult(channels[k]), j < 4 ? "valid" : "ready"));
    connect(field(t, j < 4 ? "ready" : "valid"), gate);
    if (j >= 4) {
      Value value;
      if (j == 4) value = b.create<BitsPrimOp>(loc, field(incoming, "bits"), 63, 0);
      else if (j == 5) value = constant(8, 255);
      else if (j == 6 || j == 7) value = b.create<BitsPrimOp>(loc, field(incoming, "bits"), j == 6 ? 64 : 65, j == 6 ? 64 : 65);
      else value = arg(controls + j - 8);
      connect(field(t, "bits"), value);
    }
  }
  // SFC genROReg(!tFire) samples channel progress, independent of queue
  // capacity. Expose the combinational source here for the later MMIO bank.
  connect(arg(done), neg(both(allValid, allReady)));
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
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
