// See LICENSE for license details.
// Oracle: LatencyPipeMMRegIO, Widget.attachIO/genAndAttachReg, MCRIO.bindReg.
// Materialization requires a free bank symbol and preserves top identity/annotations.
// Attachment requires an uninstantiated GGFASEDRequestLimitsWrapper, host clock/reset,
// 32-bit latency inputs and the retained recorded ten-flight bridge profile.
// Consumes/produces annotations: none; preserves and explicitly retargets all.
// IR mutations: two host-clocked reset registers, a decoded MCR fragment for
// words 0/1 (byte offsets 0/4), and a wrapper consuming both latency inputs.
// Analyses required: preceding FASED timing/admission mapping; no cache used.
// Analyses preserved: existing model/channel identities and constructor key.
// Output invariants: full-width latency values, reset default 30, writes ignore
// wstrb and targetFire; remaining top ports keep their types/directions.
#include "goldengate/FASEDLatencyRegisters.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

namespace {
constexpr llvm::StringLiteral bankName = "GGFASEDLatencyRegisters";
constexpr llvm::StringLiteral wrapperName = "GGFASEDLatencyRegistersWrapper";
constexpr llvm::StringLiteral controlName = "fased_latency_mcr";
SmallVector<PortInfo> latencyPorts(MLIRContext *ctx) {
  OpBuilder b(ctx);
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, u(1)},
      {b.getStringAttr("valid"), false, u(1)}, {b.getStringAttr("bits"), false, u(32)}});
  auto words = FVectorType::get(token, 2);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, u(4)}});
  auto latencies = BundleType::get(ctx, {{b.getStringAttr("write"), false, u(32)},
      {b.getStringAttr("read"), false, u(32)}});
  return {{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), u(1), Direction::In},
      {b.getStringAttr("latencies"), latencies, Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
}
LogicalResult attachmentTop(CircuitOp circuit, FModuleOp &inner,
    unsigned (&indices)[4], std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDRequestLimitsWrapper")
    return reject("FASED latency registers require the active request limit wrapper");
  FModuleOp engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == wrapperName)
      return reject("FASED latency register wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED latency registers need a top and retained annotations");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!flight || flight.getInt() != 10)
    return reject("FASED latency registers currently require the recorded ten-flight constructor");
  auto *ctx = circuit.getContext();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  std::optional<unsigned> clock, reset, writeLatency, readLatency;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name == controlName) return reject("FASED latency register MCR port already exists");
    if (port.name == "hostClock" && isa<ClockType>(port.type) && port.direction == Direction::In) clock = i;
    if (port.name == "hostReset" && port.type == bit && port.direction == Direction::In) reset = i;
    if (port.name == "fased_write_latency" && port.type == uint(32) && port.direction == Direction::In) writeLatency = i;
    if (port.name == "fased_read_latency" && port.type == uint(32) && port.direction == Direction::In) readLatency = i;
  }
  if (!clock || !reset || !writeLatency || !readLatency)
    return reject("FASED latency registers need host clock/reset and 32-bit runtime latencies");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED latency registers need an uninstantiated top");

  indices[0] = *clock; indices[1] = *reset;
  indices[2] = *writeLatency; indices[3] = *readLatency;
  return success();
}
} // namespace

LogicalResult goldengate::materializeFASEDLatencyRegisters(CircuitOp circuit,
    FModuleOp &result, std::string &error) {
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getName() == bankName) {
      error = "FASED latency register module already exists";
      return failure();
    }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1); auto bankPorts = latencyPorts(ctx);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  const llvm::StringRef names[]{"writeLatency", "readLatency"};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < 2; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)),
        b.getNamedAttr("readable", b.getBoolAttr(true)), b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(3), group), i);
  };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value initial = b.create<ConstantOp>(loc, uint(32), APInt(32, 30));
  for (unsigned i = 0; i < 2; ++i) {
    Value reg = b.create<RegResetOp>(loc, uint(32), arg(0), arg(1), initial, names[i]).getResult();
    Value write = slot("write", i), read = slot("read", i);
    // bindReg ignores wstrb, has no target-cycle enable, and reads the whole word.
    connect(reg, b.create<MuxPrimOp>(loc, field(write, "valid"), field(write, "bits"), reg));
    connect(field(read, "bits"), reg);
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
    connect(field(arg(2), i == 0 ? "write" : "read"), reg);
  }

  result = bank;
  return success();
}

LogicalResult goldengate::attachFASEDLatencyRegisters(CircuitOp circuit,
    FModuleOp bank, std::string &error) {
  FModuleOp inner; unsigned indices[4];
  if (failed(attachmentTop(circuit, inner, indices, error))) return failure();
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (!bank || bank->getParentOp() != circuit.getOperation() || bank.getName() != bankName)
    return reject("FASED latency attachment requires its materialized bank in this circuit");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto expected = latencyPorts(ctx); auto actual = bank.getPorts();
  if (actual.size() != expected.size()) return reject("FASED latency bank needs exactly four ports");
  for (auto [i, port] : llvm::enumerate(actual))
    if (port.name != expected[i].name || port.type != expected[i].type ||
        port.direction != expected[i].direction)
      return reject("FASED latency bank ports differ from the clock/reset/full-width latency/two-word MCR boundary");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == bank.getName(); });
  if (used) return reject("FASED latency attachment requires an uninstantiated bank");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto mcr = expected[3].type;
  std::optional<unsigned> clock = indices[0], reset = indices[1],
      writeLatency = indices[2], readLatency = indices[3];
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) if (i != *writeLatency && i != *readLatency) {
    copied.push_back(i); ports.push_back(port);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "latencyRegisters");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto port = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
                             port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  connect(sim.getResult(*writeLatency), field(mmio.getResult(2), "write"));
  connect(sim.getResult(*readLatency), field(mmio.getResult(2), "read"));
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

LogicalResult goldengate::addFASEDLatencyRegisters(CircuitOp circuit, std::string &error) {
  FModuleOp inner, bank; unsigned indices[4];
  if (failed(attachmentTop(circuit, inner, indices, error)) ||
      failed(materializeFASEDLatencyRegisters(circuit, bank, error))) return failure();
  return attachFASEDLatencyRegisters(circuit, bank, error);
}
