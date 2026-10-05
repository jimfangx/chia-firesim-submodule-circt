// See LICENSE for license details.
// Port Master.scala and Widget.gen{RO,WO}Reg through typed FIRRTL operations.
// Materialization requires a free bank symbol; it leaves the circuit identity,
// top ports and annotations unchanged so allocation can inspect the actual IR.
// Attachment requires that standalone bank, an uninstantiated post-control-
// tracker top, host clock/reset, and retained annotations. The decoded MCR bank
// is the adapter boundary. All validation precedes mutation in either phase.
// Annotations consumed/produced: none; retained top-port targets transfer.
// Analyses required/preserved: none.
// IR mutations: materialize the bank; later attach it once through a wrapper,
// copy ports and transfer targets. The combined API retains atomic preflight.
// Output invariants: five registers, synchronous reset for four; all three
// slots are ReadWrite, as Widget.attach defaults, regardless of genRO/WO name.
#include "goldengate/SimulationMaster.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

namespace {
constexpr llvm::StringLiteral bankName = "GGSimulationMasterBank";
constexpr llvm::StringLiteral wrapperName = "GGSimulationMasterWrapper";
BundleType masterMCRType(MLIRContext *ctx) {
  OpBuilder b(ctx);
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(32)}});
  auto words = FVectorType::get(token, 3);
  return BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
}
LogicalResult attachmentTop(CircuitOp circuit, FModuleOp &inner,
    std::optional<unsigned> &clock, std::optional<unsigned> &reset, std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGControlWriteTrackerWrapper")
    return reject("SimulationMaster bank requires the control write tracker wrapper");
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == wrapperName)
      return reject("SimulationMaster module already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("SimulationMaster bank needs a top and retained annotations");
  auto bit = UIntType::get(circuit.getContext(), 1, false);
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "simulationMaster_mcr") return reject("SimulationMaster MCR port already exists");
    if (p.name == "hostClock" && isa<ClockType>(p.type) && p.direction == Direction::In) clock = i;
    if (p.name == "hostReset" && p.type == bit && p.direction == Direction::In) reset = i;
  }
  if (!clock || !reset)
    return reject("SimulationMaster bank needs host clock and synchronous Boolean reset");
  bool instantiated = false;
  circuit.walk([&](InstanceOp i) { instantiated |= i.getModuleName() == inner.getName(); });
  if (instantiated) return reject("SimulationMaster bank requires an uninstantiated top");

  return success();
}
} // namespace

LogicalResult goldengate::materializeSimulationMasterBank(CircuitOp circuit,
    FModuleOp &result, std::string &error) {
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getName() == bankName) {
      error = "SimulationMaster bank module already exists";
      return failure();
    }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = uint(1);
  auto mcr = masterMCRType(ctx);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  SmallVector<Attribute> entries;
  for (auto [i, name] : llvm::enumerate(ArrayRef<llvm::StringRef>{
      "INIT_DONE", "PRESENCE_READ", "PRESENCE_WRITE"}))
    entries.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(name)),
        b.getNamedAttr("offset", b.getI32IntegerAttr(i * 4)),
        b.getNamedAttr("readable", b.getBoolAttr(true)),
        b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(entries));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(2), group), i);
  };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, uint(w), APInt(w, n));
  };
  auto mux = [&](Value s, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, s, y, n); };
  auto resetReg = [&](unsigned w, uint64_t n, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), constant(w, n), name).getResult();
  };
  Value delay = resetReg(7, 64, "initDelay"), done = resetReg(32, 0, "INIT_DONE");
  Value fingerprint = resetReg(32, 0x46697265, "rFingerprint");
  Value read = b.create<RegOp>(loc, uint(32), arg(0), "PRESENCE_READ").getResult();
  Value write = resetReg(32, 0x46697265, "PRESENCE_WRITE");
  Value expired = b.create<EQPrimOp>(loc, delay, constant(7, 0));
  Value decrement = b.create<BitsPrimOp>(loc, b.create<SubPrimOp>(loc, delay, constant(7, 1)), 6, 0);
  connect(delay, mux(expired, delay, decrement));
  connect(fingerprint, mux(b.create<EQPrimOp>(loc, write, fingerprint), fingerprint, write));
  // MCR bindReg is elaborated after default sampling, so every valid write
  // overrides that sample. Reset still wins on RegInit, but not PRESENCE_READ.
  SmallVector<Value> regs{done, read, write}, defaults{b.create<PadPrimOp>(loc, expired, 32), fingerprint, write};
  Value one = constant(1, 1);
  for (unsigned i = 0; i < 3; ++i) {
    Value rd = slot("read", i), wr = slot("write", i);
    connect(field(rd, "bits"), regs[i]); connect(field(rd, "valid"), one);
    connect(field(wr, "ready"), one);
    connect(regs[i], mux(field(wr, "valid"), field(wr, "bits"), defaults[i]));
  }
  // wstrb is intentionally ignored: these Scala bindReg paths write full words.

  result = bank;
  return success();
}

LogicalResult goldengate::attachSimulationMasterBank(CircuitOp circuit,
    FModuleOp bank, std::string &error) {
  FModuleOp inner;
  std::optional<unsigned> clock, reset;
  if (failed(attachmentTop(circuit, inner, clock, reset, error))) return failure();
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (!bank || bank->getParentOp() != circuit.getOperation() || bank.getName() != bankName)
    return reject("SimulationMaster attachment requires its materialized bank in this circuit");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto mcr = masterMCRType(ctx);
  auto bankPorts = bank.getPorts();
  if (bankPorts.size() != 3 || bankPorts[0].name != "clock" ||
      bankPorts[0].type != ClockType::get(ctx) || bankPorts[0].direction != Direction::In ||
      bankPorts[1].name != "reset" || bankPorts[1].type != UIntType::get(ctx, 1, false) ||
      bankPorts[1].direction != Direction::In || bankPorts[2].name != "mcr" ||
      bankPorts[2].type != mcr || bankPorts[2].direction != Direction::Out)
    return reject("SimulationMaster attachment requires exact clock/reset and three-word MCR ports");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == bank.getName(); });
  if (used) return reject("SimulationMaster attachment requires an uninstantiated bank");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    copied.push_back(i); ports.push_back(p);
  }
  ports.push_back({b.getStringAttr("simulationMaster_mcr"), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "simulationMaster");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
        p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(2));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto local = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (local == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> vs; for (auto v : arr) vs.push_back(retarget(v)); return b.getArrayAttr(vs); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList vs; for (auto v : d) vs.set(v.getName(), retarget(v.getValue())); return vs.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(wrapperName);
  return success();
}

LogicalResult goldengate::addSimulationMasterBank(CircuitOp circuit,
                                                std::string &error) {
  FModuleOp inner, bank;
  std::optional<unsigned> clock, reset;
  // Preserve the combined API's atomic failure contract: validate the top
  // before materialization, whose only additional precondition is a free name.
  if (failed(attachmentTop(circuit, inner, clock, reset, error)) ||
      failed(materializeSimulationMasterBank(circuit, bank, error))) return failure();
  return attachSimulationMasterBank(circuit, bank, error);
}
