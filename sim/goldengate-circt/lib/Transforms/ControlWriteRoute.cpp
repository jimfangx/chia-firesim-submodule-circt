// See LICENSE for license details.
// Port NastiRouter's write route queue and request coordination through FIRRTL
// memory, registers and ready/valid expressions. The decoded AW route is copied
// into a circular queue with one entry per decoded slave. Accepted W.last
// advances the route. Tracker and selected-slave readiness remain boundaries.
#include "goldengate/ControlWriteRoute.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addControlWriteRoute(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlWriteRouteWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlWriteRoute";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGControlDecodeWrapper")
    return reject("control write route requires the active control decoder wrapper");
  FModuleOp inner, decoder;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("control write route helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGControlAddressDecode") decoder = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto catalog = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr();
  if (!inner || !raw || !catalog || catalog.empty() || catalog.size() > 63)
    return reject("control write route needs a top, retained annotations and one to 63 decoded regions");
  const unsigned slaveCount = catalog.size();
  const unsigned pointerWidth = std::max(1u, llvm::Log2_64_Ceil(slaveCount));
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  const llvm::StringRef names[]{"hostClock", "hostReset", "ctrl_decode_aw_route"};
  const Type types[]{ClockType::get(context), uint(1), uint(slaveCount)};
  unsigned indices[3];
  for (unsigned j = 0; j < 3; ++j) {
    std::optional<unsigned> found;
    for (auto [i, port] : llvm::enumerate(inner.getPorts()))
      if (port.name.getValue() == names[j] && port.type == types[j] &&
          port.direction == (j == 2 ? Direction::Out : Direction::In)) found = i;
    if (!found) return reject("control write route needs exact host clock/reset and decoded AW route");
    indices[j] = *found;
  }
  for (auto port : inner.getPorts())
    if (port.name.getValue().starts_with("ctrl_write_route_")) return reject("control write route boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("control write route requires an uninstantiated top");

  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), uint(1), Direction::In},
      {b.getStringAttr("aw_route"), uint(slaveCount), Direction::In}};
  for (auto n : {"aw_valid", "aw_tracker_ready", "aw_slave_ready", "w_valid", "w_last", "w_slave_ready"})
    helperPorts.push_back({b.getStringAttr(n), uint(1), Direction::In});
  for (auto n : {"aw_ready", "aw_slave_valid", "aw_track_valid", "w_ready", "w_slave_valid"})
    helperPorts.push_back({b.getStringAttr(n), uint(1), Direction::Out});
  helperPorts.push_back({b.getStringAttr("w_route"), uint(slaveCount), Direction::Out});
  helperPorts.push_back({b.getStringAttr("w_error"), uint(1), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(context, Convention::Internal), helperPorts);
  helper->setAttr("goldengate.queueDepth", b.getI32IntegerAttr(slaveCount));
  helper->setAttr("goldengate.queueFlow", b.getBoolAttr(false));
  helper->setAttr("goldengate.queuePipe", b.getBoolAttr(false));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto invert = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value s, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, s, y, n); };
  auto reg = [&](unsigned w, llvm::StringRef n) -> Value {
    return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), constant(w, 0), n).getResult();
  };
  // Chisel Counter(1) has no pointer state; its RAM addresses are constant zero.
  Value enq = slaveCount == 1 ? constant(1, 0) : reg(pointerWidth, "enq_ptr_value");
  Value deq = slaveCount == 1 ? enq : reg(pointerWidth, "deq_ptr_value");
  Value full = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enq, deq);
  Value ready = invert(both(equal, full)), valid = invert(both(equal, invert(full)));
  Value enqueueValid = both(both(arg(3), arg(4)), arg(5));
  Value dequeueReady = both(both(arg(6), arg(8)), arg(7));
  Value push = both(ready, enqueueValid), pop = both(valid, dequeueReady);
  connect(arg(9), both(both(ready, arg(4)), arg(5)));
  connect(arg(10), both(both(arg(3), ready), arg(4)));
  connect(arg(11), both(both(arg(3), ready), arg(5)));
  connect(arg(12), both(valid, arg(8))); connect(arg(13), both(arg(6), valid));
  auto advance = [&](Value ptr) -> Value {
    Value increment = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, ptr, constant(pointerWidth, 1)), pointerWidth - 1, 0);
    return mux(b.create<EQPrimOp>(loc, ptr, constant(pointerWidth, slaveCount - 1)), constant(pointerWidth, 0), increment);
  };
  if (slaveCount > 1) {
    connect(enq, mux(push, advance(enq), enq));
    connect(deq, mux(pop, advance(deq), deq));
  }
  connect(full, mux(b.create<XorPrimOp>(loc, push, pop), push, full));
  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(slaveCount, uint(slaveCount), MemOp::PortKind::Read),
      MemOp::getTypeForPort(slaveCount, uint(slaveCount), MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryPorts{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, slaveCount, RUWAttr::Undefined, memoryPorts, "ram");
  Value rd = ram.getResult(0), wr = ram.getResult(1);
  connect(field(rd, "clk"), arg(0)); connect(field(rd, "en"), constant(1, 1)); connect(field(rd, "addr"), deq);
  connect(field(wr, "clk"), arg(0)); connect(field(wr, "en"), push); connect(field(wr, "addr"), enq);
  connect(field(wr, "data"), arg(2)); connect(field(wr, "mask"), constant(1, 1));
  connect(arg(14), field(rd, "data")); connect(arg(15), b.create<EQPrimOp>(loc, field(rd, "data"), constant(slaveCount, 0)));

  SmallVector<PortInfo> ports(inner.getPorts()); unsigned first = ports.size();
  for (auto port : ArrayRef<PortInfo>(helperPorts).drop_front(3)) {
    port.name = b.getStringAttr("ctrl_write_route_" + port.name.getValue().str()); ports.push_back(port);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), router = b.create<InstanceOp>(loc, helper, "controlWriteRoute");
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
        port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(router.getResult(0), wrapper.getBodyBlock()->getArgument(indices[0]));
  connect(router.getResult(1), wrapper.getBodyBlock()->getArgument(indices[1]));
  connect(router.getResult(2), sim.getResult(indices[2]));
  for (unsigned i = 3; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(first + i - 3);
    if (helperPorts[i].direction == Direction::In) connect(router.getResult(i), external);
    else connect(external, router.getResult(i));
  }
  // Retain classes/constructors and internal model identities; transfer copied
  // top ports while changing the circuit prefix for all retained targets.
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto port : inner.getPorts()) if (name == port.name.getValue()) {
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
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}
