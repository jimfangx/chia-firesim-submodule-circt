// See LICENSE for license details.
// Port LoadMemWidget's Queue_27 and priority Arbiter from the recorded U250
// oracle. Required: uninstantiated LoadMem writer top, rawAnnotations, exact
// host clock/reset and addr34/len34 request boundary, fresh helper names.
// Preserve RAM zero bits and reset-time writes; reset flushes only queue control.
// The recorded single-region U250 zero-fill profile is base=0, 16 GiB (2^31
// eight-byte beats). General host DRAM configuration is not yet supported.
// Consume the writer request ports; expose queued writes and zero-fill valid.
// ZERO_FINISHED observes writer req.ready, as in Scala LoadMem.scala.
// Retarget circuit and copied ports; consumed targets remain on the inner top.
// IR mutation: add queue/arbiter and wrapper; rebuild hierarchy analyses.
// MMIO, write data FIFO, read path and host interconnect remain unported.
#include "goldengate/LoadMemWriter.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addLoadMemRequests(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGLoadMemRequestWrapper";
  constexpr llvm::StringLiteral queueName = "GGLoadMemRequestQueue2";
  constexpr llvm::StringLiteral arbiterName = "GGLoadMemRequestArbiter";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGLoadMemWriterWrapper")
    return reject("LoadMem requests require the active LoadMem writer wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == queueName || m.getModuleName() == arbiterName)
      return reject("LoadMem request helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("LoadMem requests need a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  const llvm::StringRef names[]{"hostClock", "hostReset", "loadmem_req_ready", "loadmem_req_valid",
      "loadmem_req_bits_zero", "loadmem_req_bits_addr", "loadmem_req_bits_len"};
  const unsigned widths[]{0, 1, 1, 1, 1, 34, 34};
  unsigned indices[7];
  for (unsigned j = 0; j < 7; ++j) {
    std::optional<unsigned> found;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.direction == (j == 2 ? Direction::Out : Direction::In) &&
          p.type == (j == 0 ? Type(ClockType::get(context)) : Type(uint(widths[j])))) found = i;
    if (!found) return reject("LoadMem requests need exact U250 clock/reset and request geometry");
    indices[j] = *found;
  }
  for (auto p : inner.getPorts()) {
    auto n = p.name.getValue();
    if (n.starts_with("loadmem_req_") && !llvm::is_contained(ArrayRef<llvm::StringRef>(names), n))
      return reject("unsupported LoadMem request field");
    if (n.starts_with("loadmem_write_") || n.starts_with("loadmem_zero_"))
      return reject("LoadMem request boundary already exists");
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("LoadMem requests need an uninstantiated top");
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

  auto payload = BundleType::get(context, {{b.getStringAttr("zero"), false, bit},
      {b.getStringAttr("addr"), false, uint(34)}, {b.getStringAttr("len"), false, uint(34)}});
  auto queue = makeQueue(queueName, payload);
  const llvm::StringRef anames[]{"in_0_ready", "in_0_valid", "in_0_bits_zero", "in_0_bits_addr", "in_0_bits_len",
      "in_1_ready", "in_1_valid", "out_ready", "out_valid", "out_bits_zero", "out_bits_addr", "out_bits_len"};
  const unsigned awidths[]{1, 1, 1, 34, 34, 1, 1, 1, 1, 1, 34, 34};
  SmallVector<PortInfo> aport;
  for (unsigned i = 0; i < 12; ++i) aport.push_back({b.getStringAttr(anames[i]), uint(awidths[i]),
      i == 0 || i == 5 || i >= 8 ? Direction::Out : Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto arbiter = b.create<FModuleOp>(loc, b.getStringAttr(arbiterName),
      ConventionAttr::get(context, Convention::Internal), aport);
  arbiter->setAttr("goldengate.zeroFillBase", b.getI64IntegerAttr(0));
  arbiter->setAttr("goldengate.zeroFillBeats", b.getI64IntegerAttr(uint64_t(1) << 31));
  b.setInsertionPointToStart(arbiter.getBodyBlock());
  auto a = [&](unsigned i) { return arbiter.getBodyBlock()->getArgument(i); };
  connect(a(0), a(7));
  connect(a(5), b.create<AndPrimOp>(loc, b.create<NotPrimOp>(loc, a(1)), a(7)));
  connect(a(8), b.create<OrPrimOp>(loc, a(1), a(6)));
  connect(a(9), b.create<MuxPrimOp>(loc, a(1), a(2), constant(1, 1)));
  connect(a(10), b.create<MuxPrimOp>(loc, a(1), a(3), constant(34, 0)));
  connect(a(11), b.create<MuxPrimOp>(loc, a(1), a(4), constant(34, uint64_t(1) << 31)));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices).drop_front(2), i)) { copied.push_back(i); ports.push_back(p); }
  unsigned writeReady = ports.size();
  const llvm::StringRef fresh[]{"loadmem_write_ready", "loadmem_write_valid", "loadmem_write_bits_addr", "loadmem_write_bits_len",
      "loadmem_zero_ready", "loadmem_zero_valid", "loadmem_zero_finished"};
  const unsigned fwidths[]{1, 1, 34, 34, 1, 1, 1};
  for (unsigned i = 0; i < 7; ++i) ports.push_back({b.getStringAttr(fresh[i]), uint(fwidths[i]),
      i == 0 || i == 4 || i == 6 ? Direction::Out : Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto requests = b.create<InstanceOp>(loc, queue, "writeRequests");
  auto arb = b.create<InstanceOp>(loc, arbiter, "requestArbiter");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  for (auto [o, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i];
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : outer(o),
        p.direction == Direction::In ? outer(o) : sim.getResult(i));
    if (i == indices[0] || i == indices[1]) connect(requests.getResult(i == indices[0] ? 0 : 1), outer(o));
  }
  connect(outer(writeReady), field(requests.getResult(2), "ready"));
  connect(field(requests.getResult(2), "valid"), outer(writeReady + 1));
  auto enqBits = field(requests.getResult(2), "bits");
  connect(field(enqBits, "zero"), constant(1, 0));
  connect(field(enqBits, "addr"), outer(writeReady + 2)); connect(field(enqBits, "len"), outer(writeReady + 3));
  connect(field(requests.getResult(3), "ready"), arb.getResult(0));
  connect(arb.getResult(1), field(requests.getResult(3), "valid"));
  const llvm::StringRef fields[]{"zero", "addr", "len"};
  for (unsigned i = 0; i < 3; ++i) connect(arb.getResult(i + 2), field(field(requests.getResult(3), "bits"), fields[i]));
  connect(outer(writeReady + 4), arb.getResult(5)); connect(arb.getResult(6), outer(writeReady + 5));
  connect(arb.getResult(7), sim.getResult(indices[2])); connect(sim.getResult(indices[3]), arb.getResult(8));
  for (unsigned i = 0; i < 3; ++i) connect(sim.getResult(indices[i + 4]), arb.getResult(i + 9));
  connect(outer(writeReady + 6), sim.getResult(indices[2]));
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
