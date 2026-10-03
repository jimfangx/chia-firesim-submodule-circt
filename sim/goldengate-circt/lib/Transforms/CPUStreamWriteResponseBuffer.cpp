// See LICENSE for license details.
// FPGATop AXI4Buffer's B queue is depth two, flow=false, pipe=false.
// Preserve id16/resp2 RAM reads as in U250 Queue_53, including invalid payloads.
// Rocket's empty CPU sink list drives enqueue valid zero and response Okay;
// do not replace the queue's uninitialized payload with that live engine value.
// Required input invariants: exact uninstantiated R wrapper, U250 B interface,
// host Clock/UInt<1> reset, retained rawAnnotations, and fresh helper names.
// Annotations consumed/produced: none removed or added. Retarget retained
// circuit identities and copied top ports; B references remain engine-facing.
// IR mutations: add the queue memory/control operations and outer wrapper;
// rename circuit. Inner modules and their operations remain intact.
// Analyses required: none. Preserved: inner module semantics and channel
// endpoints. Hierarchy/instance analyses must be rebuilt after wrapping.
// Output invariants: verified FIRRTL with independent depth-two B storage and
// the same external ports. Incoming CPU streams and simulator emission remain.
#include "goldengate/TracerVTokenEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addCPUStreamWriteResponseBuffer(CircuitOp circuit,
                                                        std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamWriteResponseBufferWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGCPUStreamResponseBufferWrapper")
    return reject("CPU B buffer requires the active CPU R buffer wrapper");
  constexpr llvm::StringLiteral queueName = "GGCPUStreamBQueue2";
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == queueName)
      return reject("CPU B buffer wrapper or queue already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw)
    return reject("CPU B buffer needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  const llvm::StringRef names[]{"hostClock", "hostReset", "cpu_stream_b_ready", "cpu_stream_b_valid",
      "cpu_stream_b_bits_id", "cpu_stream_b_bits_resp"};
  const unsigned widths[]{0, 1, 1, 1, 16, 2};
  unsigned indices[6];
  for (unsigned j = 0; j < 6; ++j) {
    std::optional<unsigned> found;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.direction == (j < 3 ? Direction::In : Direction::Out) &&
          p.type == (j == 0 ? Type(ClockType::get(context)) : Type(uint(widths[j])))) found = i;
    if (!found) return reject("CPU B buffer needs the exact U250 clock/reset and B interface");
    indices[j] = *found;
  }
  for (auto p : inner.getPorts())
    if (p.name.getValue().starts_with("cpu_stream_b_") &&
        !llvm::is_contained(ArrayRef<llvm::StringRef>(names), p.name.getValue()))
      return reject("unsupported CPU B field");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("CPU B buffer needs an uninstantiated top");

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
  auto payload = BundleType::get(context, {{b.getStringAttr("id"), false, uint(16)},
      {b.getStringAttr("resp"), false, uint(2)}});
  auto queue = makeQueue(queueName, payload);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), inner.getPorts());
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), response = b.create<InstanceOp>(loc, queue, "bQueue");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices).drop_front(2), i)) {
      copied.push_back(i);
      b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : outer(i),
          p.direction == Direction::In ? outer(i) : sim.getResult(i));
    }
  connect(response.getResult(0), outer(indices[0])); connect(response.getResult(1), outer(indices[1]));
  connect(sim.getResult(indices[2]), field(response.getResult(2), "ready"));
  connect(field(response.getResult(2), "valid"), sim.getResult(indices[3]));
  connect(field(response.getResult(3), "ready"), outer(indices[2]));
  connect(outer(indices[3]), field(response.getResult(3), "valid"));
  const llvm::StringRef fields[]{"id", "resp"};
  for (unsigned i = 0; i < 2; ++i) {
    connect(field(field(response.getResult(2), "bits"), fields[i]), sim.getResult(indices[i + 4]));
    connect(outer(indices[i + 4]), field(field(response.getResult(3), "bits"), fields[i]));
  }
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
