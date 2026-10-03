// See LICENSE for license details.
// Requires: active uninstantiated GGFASEDHostOutstandingWrapper and retained
// annotations, exact Rocket HostDecoupled request and ingress reset boundaries.
// Consumes: fased_ingress; copies its qualified reset and every other port.
// Produces: a 10-entry AW FIFO with 69-bit payload; W/AR enqueue boundaries and
// raw AW dequeue before host ordering/credit policy. Host-clocked async RAM.
// Enqueue predicates exclude their own queue ready. Reset flushes pointer state
// but does not clear memory or suppress a pre-edge accepted memory write.
// Transfers: circuit/copied-port identities; consumed targets remain on sim.
// Mutates: hierarchy/ports; preserves annotation classes/constructor/clocks.
// Oracle: IngressUnit.scala 66-69, 81-86, 126-137 and SFC Queue_7.
#include "goldengate/FASEDIngressAWQueue.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDIngressAWQueue(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGFASEDIngressAWQueue10";
  constexpr llvm::StringLiteral gateName = "GGFASEDIngressAW";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDIngressAWWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGFASEDHostOutstandingWrapper")
    return reject("FASED AW ingress requires the active host outstanding wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == queueName || m.getModuleName() == gateName || m.getModuleName() == wrapperName)
      return reject("FASED AW ingress helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED AW ingress needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> elements;
    for (auto [name, width] : fields) elements.push_back({b.getStringAttr(name), false, uint(width)});
    return BundleType::get(ctx, elements);
  };
  auto address = payload({{"user",1}, {"id",4}, {"region",4}, {"qos",4}, {"prot",3}, {"cache",4},
      {"lock",1}, {"burst",2}, {"size",3}, {"len",8}, {"addr",35}});
  auto data = payload({{"user",1}, {"strb",8}, {"id",4}, {"last",1}, {"data",64}});
  auto validToken = [&](FIRRTLBaseType bits) { return BundleType::get(ctx, {{b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bits}}); };
  auto decoupled = [&](FIRRTLBaseType bits) { return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bits}}); };
  auto addressToken = decoupled(address), dataToken = decoupled(data);
  auto requests = BundleType::get(ctx, {{b.getStringAttr("aw"), false, validToken(address)},
      {b.getStringAttr("w"), false, validToken(data)}, {b.getStringAttr("ar"), false, validToken(address)}});
  auto ingress = BundleType::get(ctx, {{b.getStringAttr("hReady"), true, bit},
      {b.getStringAttr("hValid"), false, bit}, {b.getStringAttr("hBits"), false, requests}});
  auto port = [&](llvm::StringRef n, Type t, Direction d) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (p.name == n && p.type == t && p.direction == d) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(ctx), Direction::In);
  auto input = port("fased_ingress", ingress, Direction::Out);
  auto reset = port("fased_ingress_reset", bit, Direction::Out);
  if (!clock || !input || !reset) return reject("FASED AW ingress needs exact host clock, request and qualified reset boundaries");
  const llvm::StringRef addedNames[]{"fased_ingress_aw_deq", "fased_ingress_w_enq", "fased_ingress_ar_enq", "fased_ingress_aw_enq_fire"};
  for (auto p : inner.getPorts()) if (llvm::is_contained(addedNames, p.name.getValue())) return reject("FASED AW ingress boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED AW ingress needs an uninstantiated top");
  Location loc = circuit.getLoc(); b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> queuePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq"), addressToken, Direction::In}, {b.getStringAttr("deq"), addressToken, Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(queueName), ConventionAttr::get(ctx, Convention::Internal), queuePorts);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto arg = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto constant = [&](unsigned width, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, UIntType::get(ctx, width, false), APInt(width, n));
  };
  auto reg = [&](unsigned width, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, UIntType::get(ctx, width, false), arg(0), arg(1), constant(width, 0), name).getResult();
  };
  auto mux = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  Value enqPtr = reg(4, "enq_ptr_value"), deqPtr = reg(4, "deq_ptr_value");
  Value maybeFull = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value full = both(equal, maybeFull), empty = both(equal, b.create<NotPrimOp>(loc, maybeFull));
  Value ready = b.create<NotPrimOp>(loc, full), valid = b.create<NotPrimOp>(loc, empty);
  Value push = both(ready, field(arg(2), "valid")), pop = both(valid, field(arg(3), "ready"));
  connect(field(arg(2), "ready"), ready); connect(field(arg(3), "valid"), valid);
  auto increment = [&](Value pointer) -> Value {
    Value next = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, pointer, constant(4, 1)), 3, 0);
    return mux(b.create<EQPrimOp>(loc, pointer, constant(4, 9)), constant(4, 0), next);
  };
  connect(enqPtr, mux(push, increment(enqPtr), enqPtr));
  connect(deqPtr, mux(pop, increment(deqPtr), deqPtr));
  connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));

  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(10, UIntType::get(ctx, 69, false), MemOp::PortKind::Read),
      MemOp::getTypeForPort(10, UIntType::get(ctx, 69, false), MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 10,
      RUWAttr::Undefined, memoryNames, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), arg(0)); connect(field(reader, "en"), constant(1, 1));
  connect(field(reader, "addr"), deqPtr);
  Value readData = field(reader, "data"), deqBits = field(arg(3), "bits");
  unsigned low = 0;
  for (auto e : llvm::reverse(address.getElements())) {
    unsigned width = *cast<UIntType>(e.type).getWidth();
    connect(field(deqBits, e.name), b.create<BitsPrimOp>(loc, readData, low + width - 1, low));
    low += width;
  }
  connect(field(writer, "clk"), arg(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), constant(1, 1));
  Value enqBits = field(arg(2), "bits");
  Value packed;
  for (auto e : address.getElements()) {
    Value leaf = field(enqBits, e.name);
    packed = packed ? b.create<CatPrimOp>(loc, packed, leaf).getResult() : leaf;
  }
  connect(field(writer, "data"), packed);
  // Reset flushes pointers and maybe_full, not RAM. A pre-edge accepted push
  // still writes during reset, just as the SFC Queue_7 does.


  SmallVector<PortInfo> gatePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("ingress"), ingress, Direction::In},
      {b.getStringAttr("aw_deq"), addressToken, Direction::Out}, {b.getStringAttr("w_enq"), dataToken, Direction::Out},
      {b.getStringAttr("ar_enq"), addressToken, Direction::Out}, {b.getStringAttr("aw_enq_fire"), bit, Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto gates = b.create<FModuleOp>(loc, b.getStringAttr(gateName), ConventionAttr::get(ctx, Convention::Internal), gatePorts);
  b.setInsertionPointToStart(gates.getBodyBlock());
  auto gateArg = [&](unsigned i) { return gates.getBodyBlock()->getArgument(i); };
  auto awQueue = b.create<InstanceOp>(loc, queue, "awQueue");
  connect(awQueue.getResult(0), gateArg(0)); connect(awQueue.getResult(1), gateArg(1));
  Value awReady = field(awQueue.getResult(2), "ready");
  Value wReady = field(gateArg(4), "ready"), arReady = field(gateArg(5), "ready");
  Value hValid = field(gateArg(2), "hValid"), bits = field(gateArg(2), "hBits");
  connect(field(gateArg(2), "hReady"), both(both(awReady, wReady), arReady));
  Value aw = field(bits, "aw"), w = field(bits, "w"), ar = field(bits, "ar");
  Value awValid = both(both(both(hValid, wReady), arReady), field(aw, "valid"));
  connect(field(awQueue.getResult(2), "valid"), awValid);
  b.create<ConnectOp>(loc, field(awQueue.getResult(2), "bits"), field(aw, "bits"));
  connect(field(gateArg(4), "valid"), both(both(both(hValid, awReady), arReady), field(w, "valid")));
  connect(field(gateArg(5), "valid"), both(both(both(hValid, awReady), wReady), field(ar, "valid")));
  b.create<ConnectOp>(loc, field(gateArg(4), "bits"), field(w, "bits"));
  b.create<ConnectOp>(loc, field(gateArg(5), "bits"), field(ar, "bits"));
  b.create<ConnectOp>(loc, gateArg(3), awQueue.getResult(3));
  connect(gateArg(6), both(awReady, awValid));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != *input) { copied.push_back(i); ports.push_back(p); }
  unsigned added = ports.size();
  for (unsigned i = 0; i < 4; ++i) ports.push_back({b.getStringAttr(addedNames[i]), gatePorts[3+i].type, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto ingressState = b.create<InstanceOp>(loc, gates, "ingressAW");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(ingressState.getResult(0), outer(*clock)); connect(ingressState.getResult(1), sim.getResult(*reset));
  b.create<ConnectOp>(loc, ingressState.getResult(2), sim.getResult(*input));
  for (unsigned i = 0; i < 4; ++i) b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(added+i), ingressState.getResult(3+i));
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
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
