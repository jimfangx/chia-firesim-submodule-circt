// See LICENSE for license details.
// Port junctions.nasti.scala NastiErrorSlave as FIRRTL operations. The depth-one
// queues have neither flow nor pipe: no empty bypass or full replacement on a
// concurrent pop. Reset flushes occupancy/control but does not suppress payload
// writes accepted at the reset edge. Read startup takes one additional cycle;
// W before AW is blocked and B remains blocked until an accepted W.last.
// Invalid address printf conditions and constant DECERR sidebands are retained.
// This is the control router's error endpoint, not the completed interconnect.
#include "goldengate/ControlErrorSlave.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addControlErrorSlave(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlErrorWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlErrorSlave";
  auto reject = [&](llvm::StringRef message) { error = message.str(); return failure(); };
  if (!addressBits || addressBits > 64 || !idBits || idBits > 64)
    return reject("control error slave requires address and ID widths in 1..64");
  if (circuit.getName() != "GGLoadMemControlWrapper")
    return reject("control error slave requires the LoadMem control wrapper");
  FModuleOp inner;
  for (auto module : circuit.getOps<FModuleLike>()) {
    if (module.getModuleName() == wrapperName || module.getModuleName() == helperName)
      return reject("control error slave helper or wrapper already exists");
    if (module.getModuleName() == circuit.getName())
      inner = dyn_cast<FModuleOp>(module.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("control error slave needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(context, width, false); };
  std::optional<unsigned> clock, reset;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    auto name = port.name.getValue();
    if (name.starts_with("ctrl_error_"))
      return reject("control error slave boundary already exists");
    if (name == "hostClock" && port.direction == Direction::In && port.type == ClockType::get(context)) clock = i;
    if (name == "hostReset" && port.direction == Direction::In && port.type == uint(1)) reset = i;
  }
  if (!clock || !reset) return reject("control error slave needs exact host clock/reset");
  bool used = false;
  circuit.walk([&](InstanceOp instance) { used |= instance.getModuleName() == inner.getName(); });
  if (used) return reject("control error slave requires an uninstantiated top");

  SmallVector<PortInfo> helperPorts{
      {b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), uint(1), Direction::In}};
  auto append = [&](llvm::StringRef name, unsigned width, Direction direction) {
    helperPorts.push_back({b.getStringAttr(name), uint(width), direction});
  };
  append("aw_ready", 1, Direction::Out); append("aw_valid", 1, Direction::In);
  append("aw_bits_addr", addressBits, Direction::In); append("aw_bits_id", idBits, Direction::In);
  append("w_ready", 1, Direction::Out); append("w_valid", 1, Direction::In);
  append("w_bits_last", 1, Direction::In); append("b_ready", 1, Direction::In);
  append("b_valid", 1, Direction::Out); append("b_bits_id", idBits, Direction::Out);
  append("ar_ready", 1, Direction::Out); append("ar_valid", 1, Direction::In);
  append("ar_bits_addr", addressBits, Direction::In); append("ar_bits_len", 8, Direction::In);
  append("ar_bits_id", idBits, Direction::In); append("r_ready", 1, Direction::In);
  append("r_valid", 1, Direction::Out); append("r_bits_last", 1, Direction::Out);
  append("r_bits_id", idBits, Direction::Out); append("r_bits_data", 32, Direction::Out);
  append("r_bits_resp", 2, Direction::Out); append("r_bits_user", 1, Direction::Out);
  append("b_bits_resp", 2, Direction::Out); append("b_bits_user", 1, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(context, Convention::Internal), helperPorts);
  helper->setAttr("goldengate.queueDepth", b.getI32IntegerAttr(1));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned width, uint64_t value) -> Value {
    return b.create<ConstantOp>(loc, uint(width), APInt(width, value));
  };
  auto connect = [&](Value dest, Value source) { b.create<StrictConnectOp>(loc, dest, source); };
  auto and2 = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto not1 = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value select, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, select, yes, no); };
  auto reg = [&](unsigned width, llvm::StringRef name, bool reset) -> Value {
    if (reset) return b.create<RegResetOp>(loc, uint(width), arg(0), arg(1), constant(width, 0), name).getResult();
    return b.create<RegOp>(loc, uint(width), arg(0), name).getResult();
  };
  Value zero = constant(1, 0), one = constant(1, 1);
  Value readFull = reg(1, "r_full", true), writeFull = reg(1, "b_full", true);
  Value responding = reg(1, "responding", true), draining = reg(1, "draining", true);
  Value beatsLeft = reg(8, "beats_left", true);
  Value readLen = reg(8, "r_len", false), readID = reg(idBits, "r_id", false);
  Value writeID = reg(idBits, "b_id", false);
  Value arReady = not1(readFull), awReady = and2(not1(writeFull), not1(draining));
  Value readValid = and2(readFull, responding), writeValid = and2(writeFull, not1(draining));
  Value last = b.create<EQPrimOp>(loc, beatsLeft, constant(8, 0));
  Value arFire = and2(arg(13), arReady), awFire = and2(arg(3), awReady);
  Value rFire = and2(arg(17), readValid), bFire = and2(arg(9), writeValid);
  Value readPop = and2(rFire, last), writeLast = and2(and2(arg(7), draining), arg(8));
  connect(arg(2), awReady); connect(arg(6), draining);
  connect(arg(10), writeValid); connect(arg(11), writeID);
  connect(arg(12), arReady); connect(arg(18), readValid);
  connect(arg(19), last); connect(arg(20), readID);
  connect(arg(21), constant(32, 0)); connect(arg(22), constant(2, 3)); connect(arg(23), zero);
  connect(arg(24), constant(2, 3)); connect(arg(25), zero);
  // Occupancy updates exactly as Queue(..., 1, pipe=false, flow=false).
  connect(readFull, mux(b.create<XorPrimOp>(loc, arFire, readPop), arFire, readFull));
  connect(writeFull, mux(b.create<XorPrimOp>(loc, awFire, bFire), awFire, writeFull));
  connect(readLen, mux(arFire, arg(15), readLen)); connect(readID, mux(arFire, arg(16), readID));
  connect(writeID, mux(awFire, arg(5), writeID));
  Value startRead = and2(not1(responding), readFull);
  connect(responding, mux(readPop, zero, mux(startRead, one, responding)));
  Value decrement = b.create<BitsPrimOp>(loc,
      b.create<SubPrimOp>(loc, beatsLeft, constant(8, 1)), 7, 0);
  connect(beatsLeft, mux(and2(rFire, not1(last)), decrement, mux(startRead, readLen, beatsLeft)));
  connect(draining, mux(writeLast, zero, mux(awFire, one, draining)));
  b.create<PrintFOp>(loc, arg(0), and2(arFire, not1(arg(1))),
      "Invalid read address %x\n", ValueRange{arg(14)}, "");
  b.create<PrintFOp>(loc, arg(0), and2(awFire, not1(arg(1))),
      "Invalid write address %x\n", ValueRange{arg(4)}, "");

  SmallVector<PortInfo> ports(inner.getPorts()); unsigned first = ports.size();
  for (auto port : ArrayRef<PortInfo>(helperPorts).drop_front(2)) {
    port.name = b.getStringAttr("ctrl_error_" + port.name.getValue().str()); ports.push_back(port);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto endpoint = b.create<InstanceOp>(loc, helper, "controlError");
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
        port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(endpoint.getResult(0), wrapper.getBodyBlock()->getArgument(*clock));
  connect(endpoint.getResult(1), wrapper.getBodyBlock()->getArgument(*reset));
  for (unsigned i = 2; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(first + i - 2);
    if (helperPorts[i].direction == Direction::In) connect(endpoint.getResult(i), external);
    else connect(external, endpoint.getResult(i));
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
