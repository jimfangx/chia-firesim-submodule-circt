// See LICENSE for license details.
// FPGATop CPU AXI4Buffer(): independent depth-two AW and W queues, flow=false,
// pipe=false. Retains the common AW address/length fields from golden Queue_51;
// W data/last have no users in Rocket's empty incoming CPU stream configuration.
// Requires the uninstantiated write wrapper, exact U250 AW/W scalars and raw
// annotations. Rejects boundary/module collisions before mutation. AW/W targets
// stay on the inner engine-facing boundary; other copied ports move outward.
// Produces native FIRRTL memories/registers and a connected outer boundary.
// B, AR, R buffering and the FPGA shell remain subsequent compiler stages.
#include "goldengate/TracerVTokenEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addCPUStreamWriteBuffer(CircuitOp circuit,
                                                std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamWriteBufferWrapper";
  constexpr llvm::StringLiteral awName = "GGCPUStreamAWQueue2";
  constexpr llvm::StringLiteral wName = "GGCPUStreamWQueue2";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGCPUStreamWriteWrapper")
    return reject("CPU AW/W buffers require the active CPU write wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == awName || m.getModuleName() == wName)
      return reject("CPU AW/W buffer module already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("CPU AW/W buffers need a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  const llvm::StringRef names[]{"hostClock", "hostReset", "cpu_stream_aw_ready", "cpu_stream_aw_valid",
      "cpu_stream_aw_bits_id", "cpu_stream_aw_bits_size", "cpu_stream_w_ready", "cpu_stream_w_valid", "cpu_stream_w_bits_strb"};
  const unsigned widths[]{0, 1, 1, 1, 16, 3, 1, 1, 64};
  const Direction dirs[]{Direction::In, Direction::In, Direction::Out, Direction::In,
      Direction::In, Direction::In, Direction::Out, Direction::In, Direction::In};
  unsigned indices[9];
  for (unsigned j = 0; j < 9; ++j) {
    std::optional<unsigned> found;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.direction == dirs[j] &&
          p.type == (j == 0 ? Type(ClockType::get(context)) : Type(uint(widths[j])))) found = i;
    if (!found) return reject("CPU AW/W buffers need the exact U250 clock/reset and write interface");
    indices[j] = *found;
  }
  for (auto p : inner.getPorts()) {
    auto n = p.name.getValue();
    if ((n.starts_with("cpu_stream_aw_") || n.starts_with("cpu_stream_w_")) &&
        !llvm::is_contained(ArrayRef<llvm::StringRef>(names), n))
      return reject("unsupported or already buffered CPU AW/W field");
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("CPU AW/W buffers need an uninstantiated top");

  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto makeQueue = [&](llvm::StringRef name, BundleType payload) {
    auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
    SmallVector<PortInfo> ports{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
        {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("enq"), token, Direction::In},
        {b.getStringAttr("deq"), token, Direction::Out}};
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto queue = b.create<FModuleOp>(loc, b.getStringAttr(name), ConventionAttr::get(context, Convention::Internal), ports);
    queue->setAttr("goldengate.queueDepth", b.getI32IntegerAttr(2));
    queue->setAttr("goldengate.queueFlow", b.getBoolAttr(false)); queue->setAttr("goldengate.queuePipe", b.getBoolAttr(false));
    b.setInsertionPointToStart(queue.getBodyBlock());
    auto arg = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
    auto reg = [&](llvm::StringRef n) -> Value { return b.create<RegResetOp>(loc, bit, arg(0), arg(1), constant(1, 0), n).getResult(); };
    auto both = [&](Value x, Value y) -> Value { return b.create<AndPrimOp>(loc, x, y); };
    auto invert = [&](Value x) -> Value { return b.create<NotPrimOp>(loc, x); };
    auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
    Value enq = reg("enq_ptr_value"), deq = reg("deq_ptr_value"), maybeFull = reg("maybe_full");
    Value equal = b.create<EQPrimOp>(loc, enq, deq);
    Value ready = invert(both(equal, maybeFull)), valid = invert(both(equal, invert(maybeFull)));
    Value push = both(ready, field(arg(2), "valid")), pop = both(valid, field(arg(3), "ready"));
    connect(field(arg(2), "ready"), ready); connect(field(arg(3), "valid"), valid);
    connect(enq, mux(push, invert(enq), enq)); connect(deq, mux(pop, invert(deq), deq));
    connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));
    SmallVector<Type> types{MemOp::getTypeForPort(2, payload, MemOp::PortKind::Read),
        MemOp::getTypeForPort(2, payload, MemOp::PortKind::Write)};
    SmallVector<Attribute> portNames{b.getStringAttr("read"), b.getStringAttr("write")};
    auto ram = b.create<MemOp>(loc, types, 0, 1, 2, RUWAttr::Undefined, portNames, "ram");
    Value rd = ram.getResult(0), wr = ram.getResult(1);
    connect(field(rd, "clk"), arg(0)); connect(field(rd, "en"), constant(1, 1)); connect(field(rd, "addr"), deq);
    connect(field(wr, "clk"), arg(0)); connect(field(wr, "en"), push); connect(field(wr, "addr"), enq);
    for (auto elem : payload.getElements()) {
      connect(field(field(arg(3), "bits"), elem.name.getValue()), field(field(rd, "data"), elem.name.getValue()));
      connect(field(field(wr, "data"), elem.name.getValue()), field(field(arg(2), "bits"), elem.name.getValue()));
      connect(field(field(wr, "mask"), elem.name.getValue()), constant(1, 1));
    }
    // Queue reset flushes control state only. A pre-edge accepted push writes
    // RAM even during reset; no empty bypass or full-with-pop acceptance.
    return queue;
  };
  auto awPayload = BundleType::get(context, {{b.getStringAttr("id"), false, uint(16)},
      {b.getStringAttr("addr"), false, uint(64)}, {b.getStringAttr("len"), false, uint(8)}, {b.getStringAttr("size"), false, uint(3)}});
  auto wPayload = BundleType::get(context, {{b.getStringAttr("strb"), false, uint(64)}});
  auto awQueue = makeQueue(awName, awPayload), wQueue = makeQueue(wName, wPayload);
  SmallVector<PortInfo> ports(inner.getPorts());
  unsigned addrPort = ports.size(); ports.push_back({b.getStringAttr("cpu_stream_aw_bits_addr"), uint(64), Direction::In});
  unsigned lenPort = ports.size(); ports.push_back({b.getStringAttr("cpu_stream_aw_bits_len"), uint(8), Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), aw = b.create<InstanceOp>(loc, awQueue, "awQueue"), w = b.create<InstanceOp>(loc, wQueue, "wQueue");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!llvm::is_contained(ArrayRef<unsigned>(indices).drop_front(2), i)) {
    copied.push_back(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : outer(i),
        p.direction == Direction::In ? outer(i) : sim.getResult(i));
  }
  for (auto q : {aw, w}) { connect(q.getResult(0), outer(indices[0])); connect(q.getResult(1), outer(indices[1])); }
  auto wireChannel = [&](InstanceOp q, unsigned ready, unsigned valid) {
    connect(outer(indices[ready]), field(q.getResult(2), "ready"));
    connect(field(q.getResult(2), "valid"), outer(indices[valid]));
    connect(field(q.getResult(3), "ready"), sim.getResult(indices[ready]));
    connect(sim.getResult(indices[valid]), field(q.getResult(3), "valid"));
  };
  wireChannel(aw, 2, 3); wireChannel(w, 6, 7);
  auto wirePayload = [&](InstanceOp q, unsigned idx, llvm::StringRef name) {
    connect(field(field(q.getResult(2), "bits"), name), outer(indices[idx]));
    connect(sim.getResult(indices[idx]), field(field(q.getResult(3), "bits"), name));
  };
  wirePayload(aw, 4, "id"); wirePayload(aw, 5, "size"); wirePayload(w, 8, "strb");
  connect(field(field(aw.getResult(2), "bits"), "addr"), outer(addrPort));
  connect(field(field(aw.getResult(2), "bits"), "len"), outer(lenPort));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
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
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
