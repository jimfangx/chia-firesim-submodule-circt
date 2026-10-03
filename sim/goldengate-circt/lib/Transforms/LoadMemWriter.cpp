// See LICENSE for license details.
// Required input invariants: uninstantiated CPU B wrapper, host clock/reset,
// retained rawAnnotations, fresh LoadMem names. Only U250 addr34/data64 with
// maxBurst32 is supported, as in the recorded LoadMemWriter oracle.
// Annotations consumed/produced: none added/removed; copied top port and circuit
// identities move to the new wrapper. Inner channel endpoints remain intact.
// IR mutations: add native request/AW/W/B state machine, wrapper and scalar
// LoadMem request/data/memory boundaries. Rebuild hierarchy analyses afterward.
// Output: same inner semantics and an independent host-clock writer. Only FSM
// and beat counter reset; payload registers still capture handshakes on reset.
// No zero-length special case: AW len wraps to 255 and 5-bit beats to 31.
// LoadMem MMIO, request/data queues, constant Nasti sidebands, read transport
// and host memory interconnect are subsequent stages; no simulator RTL yet.
#include "goldengate/LoadMemWriter.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addLoadMemWriter(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGLoadMemWriter";
  constexpr llvm::StringLiteral wrapperName = "GGLoadMemWriterWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGCPUStreamWriteResponseBufferWrapper")
    return reject("LoadMem writer requires the CPU B buffer wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("LoadMem writer helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("LoadMem writer needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context);
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  std::optional<unsigned> clock, reset;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    auto n = port.name.getValue();
    if (n.starts_with("loadmem_")) return reject("LoadMem boundary already exists");
    if (n == "hostClock" && port.type == ClockType::get(context) && port.direction == Direction::In) clock = i;
    if (n == "hostReset" && port.type == bit && port.direction == Direction::In) reset = i;
  }
  if (!clock || !reset) return reject("LoadMem writer needs host clock/reset");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("LoadMem writer needs an uninstantiated top");

  Location loc = circuit.getLoc();
  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  auto append = [&](llvm::StringRef n, unsigned w, Direction d) {
    helperPorts.push_back({b.getStringAttr(n), uint(w), d});
  };
  append("req_ready", 1, Direction::Out); append("req_valid", 1, Direction::In);
  append("req_bits_zero", 1, Direction::In); append("req_bits_addr", 34, Direction::In);
  append("req_bits_len", 34, Direction::In); append("data_ready", 1, Direction::Out);
  append("data_valid", 1, Direction::In); append("data_bits", 64, Direction::In);
  append("mem_aw_ready", 1, Direction::In); append("mem_aw_valid", 1, Direction::Out);
  append("mem_aw_bits_addr", 34, Direction::Out); append("mem_aw_bits_len", 8, Direction::Out);
  append("mem_w_ready", 1, Direction::In); append("mem_w_valid", 1, Direction::Out);
  append("mem_w_bits_data", 64, Direction::Out); append("mem_w_bits_last", 1, Direction::Out);
  append("mem_b_ready", 1, Direction::Out); append("mem_b_valid", 1, Direction::In);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(context, Convention::Internal), helperPorts);
  helper->setAttr("goldengate.maxBurst", b.getI32IntegerAttr(32));
  helper->setAttr("goldengate.addressBits", b.getI32IntegerAttr(34));
  helper->setAttr("goldengate.dataBits", b.getI32IntegerAttr(64));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value x, Value y) -> Value { return b.create<AndPrimOp>(loc, x, y); };
  auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
  auto truncate = [&](Value v, unsigned w) -> Value { return b.create<BitsPrimOp>(loc, v, w - 1, 0); };
  auto payloadReg = [&](llvm::StringRef n, unsigned w) -> Value { return b.create<RegOp>(loc, uint(w), arg(0), n).getResult(); };
  auto resetReg = [&](llvm::StringRef n, unsigned w) -> Value { return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), constant(w, 0), n).getResult(); };
  Value zero = payloadReg("wZero", 1), address = payloadReg("wAddr", 34), length = payloadReg("wLen", 34);
  Value beats = resetReg("wBeatsLeft", 5), state = resetReg("state", 2);
  auto isState = [&](unsigned n) -> Value { return b.create<EQPrimOp>(loc, state, constant(2, n)); };
  Value idle = isState(0), addr = isState(1), data = isState(2), resp = isState(3);
  Value burst = mux(b.create<GTPrimOp>(loc, length, constant(34, 32)), constant(34, 32), length);
  Value burstMinusOne = truncate(b.create<SubPrimOp>(loc, burst, constant(34, 1)), 34);
  Value last = b.create<EQPrimOp>(loc, beats, constant(5, 0));
  Value writeValid = both(data, b.create<OrPrimOp>(loc, zero, arg(8)));
  Value reqFire = both(idle, arg(3)), awFire = both(addr, arg(10));
  Value wFire = both(writeValid, arg(14)), bFire = both(resp, arg(19));
  connect(arg(2), idle); connect(arg(7), both(data, both(b.create<NotPrimOp>(loc, zero), arg(14))));
  connect(arg(11), addr); connect(arg(12), address); connect(arg(13), truncate(burstMinusOne, 8));
  connect(arg(15), writeValid); connect(arg(16), mux(zero, constant(64, 0), arg(9)));
  connect(arg(17), last); connect(arg(18), resp);
  // Width truncation implements both 34-bit address wrap and the 5-bit
  // burst-counter underflow after the final beat. No payload reset mux.
  Value nextAddr = truncate(b.create<AddPrimOp>(loc, address, b.create<ShlPrimOp>(loc, burst, 3)), 34);
  connect(zero, mux(reqFire, arg(4), zero));
  connect(address, mux(awFire, nextAddr, mux(reqFire, arg(5), address)));
  connect(length, mux(awFire, truncate(b.create<SubPrimOp>(loc, length, burst), 34), mux(reqFire, arg(6), length)));
  connect(beats, mux(wFire, truncate(b.create<SubPrimOp>(loc, beats, constant(5, 1)), 5), mux(awFire, truncate(burstMinusOne, 5), beats)));
  Value nextState = mux(reqFire, constant(2, 1), state);
  nextState = mux(awFire, constant(2, 2), nextState);
  nextState = mux(both(wFire, last), constant(2, 3), nextState);
  nextState = mux(bFire, mux(b.create<EQPrimOp>(loc, length, constant(34, 0)), constant(2, 0), constant(2, 1)), nextState);
  connect(state, nextState);

  SmallVector<PortInfo> ports(inner.getPorts()); SmallVector<unsigned> copied;
  unsigned firstWriter = ports.size();
  for (auto p : ArrayRef<PortInfo>(helperPorts).drop_front(2)) {
    p.name = b.getStringAttr("loadmem_" + p.name.getValue().str()); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto writer = b.create<InstanceOp>(loc, helper, "loadmemWriter");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    copied.push_back(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : outer(i),
        p.direction == Direction::In ? outer(i) : sim.getResult(i));
  }
  connect(writer.getResult(0), outer(*clock)); connect(writer.getResult(1), outer(*reset));
  for (unsigned i = 2; i < helperPorts.size(); ++i) {
    Value external = outer(firstWriter + i - 2);
    if (helperPorts[i].direction == Direction::In) connect(writer.getResult(i), external);
    else connect(external, writer.getResult(i));
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
