// See LICENSE for license details.
// Oracle: IngressUnit.scala AR Queue/read_req_done and SFC Queue_9.
// Requires: uninstantiated GGFASEDIngressWQueueWrapper; retained annotations;
// exact AR enqueue, host clock and qualified ingress reset boundaries.
// Consumes: AR enqueue. Produces: raw AR dequeue before host issue ordering,
// and accepted-read pulse for the future transaction ordering queue.
// Preserves: copied ports and annotation classes; consumed targets stay on sim.
// Mutates hierarchy/ports through FIRRTL registers, async RAM and connections.
// Four entries, 69-bit address payload, no flow/pipe bypass. Reset clears
// pointer/full state only; a pre-edge accepted RAM write still occurs.
#include "goldengate/FASEDIngressARQueue.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDIngressARQueue(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGFASEDIngressARQueue4";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDIngressARQueueWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGFASEDIngressWQueueWrapper")
    return reject("FASED AR ingress queue requires the active FASED W ingress wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (!m) continue;
    if (m.getModuleName() == queueName || m.getModuleName() == wrapperName)
      return reject("FASED AR ingress queue or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED AR ingress queue needs a top module and retained annotations");
  auto *context = circuit.getContext();
  OpBuilder b(context);
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  SmallVector<BundleType::BundleElement> elements;
  for (auto [name, width] : std::initializer_list<std::pair<llvm::StringRef, unsigned>>{
      {"user",1}, {"id",4}, {"region",4}, {"qos",4}, {"prot",3}, {"cache",4},
      {"lock",1}, {"burst",2}, {"size",3}, {"len",8}, {"addr",35}})
    elements.push_back({b.getStringAttr(name), false, uint(width)});
  auto payload = BundleType::get(context, elements);
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(context), Direction::In);
  auto output = port("fased_ingress_ar_enq", token, Direction::Out);
  auto reset = port("fased_ingress_reset", bit, Direction::Out);
  if (!clock || !output || !reset)
    return reject("FASED AR ingress queue needs hostClock, Decoupled read address and queue reset");
  for (auto p : inner.getPorts())
    if (p.name == "fased_ingress_ar_deq" || p.name == "fased_ingress_ar_enq_fire")
      return reject("FASED AR ingress queue boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED AR ingress queue needs an uninstantiated top");

  // Create hardware only after all boundary invariants have been checked.
  Location loc = circuit.getLoc();
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> queuePorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq"), token, Direction::In}, {b.getStringAttr("deq"), token, Direction::Out},
      {b.getStringAttr("enq_fire"), bit, Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(queueName), ConventionAttr::get(context, Convention::Internal), queuePorts);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto arg = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto constant = [&](unsigned width, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, UIntType::get(context, width, false), APInt(width, n));
  };
  auto reg = [&](unsigned width, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, UIntType::get(context, width, false), arg(0), arg(1), constant(width, 0), name).getResult();
  };
  auto mux = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  Value enqPtr = reg(2, "enq_ptr_value"), deqPtr = reg(2, "deq_ptr_value");
  Value maybeFull = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value full = both(equal, maybeFull), empty = both(equal, b.create<NotPrimOp>(loc, maybeFull));
  Value ready = b.create<NotPrimOp>(loc, full), valid = b.create<NotPrimOp>(loc, empty);
  Value push = both(ready, field(arg(2), "valid")), pop = both(valid, field(arg(3), "ready"));
  connect(field(arg(2), "ready"), ready); connect(field(arg(3), "valid"), valid);
  auto increment = [&](Value pointer) -> Value {
    return b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, pointer, constant(2, 1)), 1, 0);
  };
  connect(enqPtr, mux(push, increment(enqPtr), enqPtr));
  connect(deqPtr, mux(pop, increment(deqPtr), deqPtr));
  connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));

  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(4, UIntType::get(context, 69, false), MemOp::PortKind::Read),
      MemOp::getTypeForPort(4, UIntType::get(context, 69, false), MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 4,
      RUWAttr::Undefined, memoryNames, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), arg(0)); connect(field(reader, "en"), constant(1, 1));
  connect(field(reader, "addr"), deqPtr);
  Value readData = field(reader, "data"), deqBits = field(arg(3), "bits");
  unsigned low = 0;
  for (auto e : llvm::reverse(payload.getElements())) {
    unsigned width = *cast<UIntType>(e.type).getWidth();
    connect(field(deqBits, e.name), b.create<BitsPrimOp>(loc, readData, low + width - 1, low));
    low += width;
  }
  connect(field(writer, "clk"), arg(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), constant(1, 1));
  Value enqBits = field(arg(2), "bits");
  Value packed;
  for (auto e : payload.getElements()) {
    Value leaf = field(enqBits, e.name);
    packed = packed ? b.create<CatPrimOp>(loc, packed, leaf).getResult() : leaf;
  }
  connect(field(writer, "data"), packed);
  connect(arg(4), push);
  // Reset flushes pointers and maybe_full, not RAM. A pre-edge accepted push
  // still writes during reset, just as the SFC Queue_9 does.

  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (i != *output) {
      copied.push_back(i); ports.push_back(p);
    }
  unsigned hostOutput = ports.size();
  ports.push_back({b.getStringAttr("fased_ingress_ar_deq"), token, Direction::Out});
  ports.push_back({b.getStringAttr("fased_ingress_ar_enq_fire"), bit, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto arQueue = b.create<InstanceOp>(loc, queue, "arQueue");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(arQueue.getResult(0), outer(*clock));
  connect(arQueue.getResult(1), sim.getResult(*reset));
  b.create<ConnectOp>(loc, arQueue.getResult(2), sim.getResult(*output));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(hostOutput), arQueue.getResult(3));

  connect(wrapper.getBodyBlock()->getArgument(hostOutput + 1), arQueue.getResult(4));

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
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
