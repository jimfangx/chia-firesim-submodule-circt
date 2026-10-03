// See LICENSE for license details.
// Oracle: FuncModelProgrammableRegs, Widget.genAndAttachReg, MCRIO.bindReg.
// Requires: uninstantiated latency-register wrapper, exact host clock/reset,
// one-bit ingress relaxed input and retained ten-flight bridge constructor.
// Consumes/produces annotations: none; preserves and explicitly retargets all.
// IR mutations: a host-clocked 32-bit reset register, decoded MCR word 18
// (byte offset 72), and a wrapper consuming the ingress relaxation input.
// Analyses required: preceding FASED ingress/latency mapping; no cache used.
// Analyses preserved: existing channel/model identities and constructor key.
// Output invariants: full-width readback, reset default zero, only bit zero
// drives ingress; writes ignore wstrb and targetFire; copied ports unchanged.
#include "goldengate/FASEDFunctionalModelRegister.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDFunctionalModelRegister(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGFASEDFunctionalModelRegister";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDFunctionalModelRegisterWrapper";
  constexpr llvm::StringLiteral controlName = "fased_functional_model_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDLatencyRegistersWrapper")
    return reject("FASED functional model register requires the active latency register wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("FASED functional model register module or wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED functional model register needs a top and retained annotations");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!flight || flight.getInt() != 10)
    return reject("FASED functional model register currently requires the recorded ten-flight constructor");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  auto token = [&](FIRRTLBaseType payload) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  std::optional<unsigned> clock, reset, relaxed;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name == controlName) return reject("FASED functional model MCR port already exists");
    if (port.name == "hostClock" && isa<ClockType>(port.type) && port.direction == Direction::In) clock = i;
    if (port.name == "hostReset" && port.type == bit && port.direction == Direction::In) reset = i;
    if (port.name == "fased_ingress_relaxed" && port.type == bit && port.direction == Direction::In) relaxed = i;
  }
  if (!clock || !reset || !relaxed)
    return reject("FASED functional model register needs host clock/reset and one-bit ingress relaxation");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED functional model register needs an uninstantiated top");

  // Preflight is complete. Local lane zero maps to global word 18 (72 bytes).
  auto words = FVectorType::get(token(uint(32)), 1);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("relaxed"), bit, Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr("relaxFunctionalModel")),
      b.getNamedAttr("offset", b.getI32IntegerAttr(72)),
      b.getNamedAttr("readable", b.getBoolAttr(true)),
      b.getNamedAttr("writeable", b.getBoolAttr(true))})}));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(3), group), i);
  };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value initial = b.create<ConstantOp>(loc, uint(32), APInt(32, 0));
  // The SFC widget holds the full control word; Bool ingress consumes only bit 0.
  Value reg = b.create<RegResetOp>(loc, uint(32), arg(0), arg(1), initial,
      "relaxFunctionalModel").getResult();
  Value write = slot("write", 0), read = slot("read", 0);
  connect(reg, b.create<MuxPrimOp>(loc, field(write, "valid"), field(write, "bits"), reg));
  connect(field(read, "bits"), reg);
  connect(field(read, "valid"), one); connect(field(write, "ready"), one);
  connect(arg(2), b.create<BitsPrimOp>(loc, reg, 0, 0));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) if (i != *relaxed) {
    copied.push_back(i); ports.push_back(port);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "functionalModelRegister");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto port = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
                             port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  connect(sim.getResult(*relaxed), mmio.getResult(2));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(3));

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
