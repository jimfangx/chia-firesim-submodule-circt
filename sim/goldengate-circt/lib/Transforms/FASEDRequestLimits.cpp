// See LICENSE for license details.
// Oracle: SplitTransactionMMRegIO.maxReqRegisters, Widget.attachIO/genAndAttachReg, MCRIO.bindReg.
// Materialization requires a free bank symbol and preserves top identity/annotations.
// Attachment requires an uninstantiated legacy or selected read-admission root,
// host clock/reset and admission inputs matching the retained bridge profile.
// Consumes/produces annotations: none; preserves and explicitly retargets all.
// IR mutations: two host-clocked reset registers, a decoded MCR fragment for
// words 2/3 (byte offsets 8/12), and a wrapper consuming both admission inputs.
// Analyses required: preceding FASED timing/admission mapping; no cache used.
// Analyses preserved: existing model/channel identities and constructor key.
// Output invariants: full-width MMIO readback, log2Ceil(limit+1) admission bits,
// reset default min(16,maxFlight), writes ignore wstrb and targetFire; remaining top ports keep their types/directions.
#include "goldengate/FASEDRequestLimits.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

namespace {
constexpr llvm::StringLiteral bankName = "GGFASEDRequestLimits";
llvm::StringRef wrapperName(CircuitOp circuit) {
  return circuit.getName() == "GGSelectedPrintFASEDReadAdmissionWrapper"
      ? "GGSelectedPrintFASEDRequestLimitsWrapper" : "GGFASEDRequestLimitsWrapper";
}
constexpr llvm::StringLiteral controlName = "fased_request_limits_mcr";
SmallVector<PortInfo> requestLimitPorts(MLIRContext *ctx, unsigned width) {
  OpBuilder b(ctx);
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, u(1)},
      {b.getStringAttr("valid"), false, u(1)}, {b.getStringAttr("bits"), false, u(32)}});
  auto words = FVectorType::get(token, 2);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, u(4)}});
  auto limits = BundleType::get(ctx, {{b.getStringAttr("write"), false, u(width)},
      {b.getStringAttr("read"), false, u(width)}});
  return {{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), u(1), Direction::In},
      {b.getStringAttr("limits"), limits, Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
}
LogicalResult attachmentTop(CircuitOp circuit, FModuleOp &inner,
    unsigned (&indices)[4], std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDReadAdmissionWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDReadAdmissionWrapper")
    return reject("FASED request limits require the active read admission wrapper");
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == wrapperName(circuit))
      return reject("FASED request limit wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED request limits need a top and retained annotations");
  unsigned limit, width;
  if (failed(goldengate::getFASEDRequestLimitProfile(circuit, limit, width, error))) return failure();
  auto *ctx = circuit.getContext();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  std::optional<unsigned> clock, reset, writeLimit, readLimit;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name == controlName) return reject("FASED request limit MCR port already exists");
    if (port.name == "hostClock" && isa<ClockType>(port.type) && port.direction == Direction::In) clock = i;
    if (port.name == "hostReset" && port.type == bit && port.direction == Direction::In) reset = i;
    if (port.name == "fased_write_max_reqs" && port.type == uint(width) && port.direction == Direction::In) writeLimit = i;
    if (port.name == "fased_read_max_reqs" && port.type == uint(width) && port.direction == Direction::In) readLimit = i;
  }
  if (!clock || !reset || !writeLimit || !readLimit)
    return reject("FASED request limits need host clock/reset and constructor-sized admission limits");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED request limits need an uninstantiated top");

  indices[0] = *clock; indices[1] = *reset;
  indices[2] = *writeLimit; indices[3] = *readLimit;
  return success();
}
} // namespace

// BaseConfig.maxReads/maxWrites cap edge.maxFlight at default BaseParams=16.
// Custom BaseParams are not represented by the supported constructor handoff.
LogicalResult goldengate::getFASEDRequestLimitProfile(CircuitOp circuit, unsigned &limit,
    unsigned &width, std::string &error) {
  FModuleOp engine;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == "GGFASEDTokenEngine") engine = m;
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  // Control allocation materializes this bank before the token engine exists.
  // At that phase use the unique retained BridgeIO constructor. Once mapped,
  // the engine's retained constructor is authoritative, including failures.
  if (!engine) {
    if (auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
      for (auto attr : raw) if (auto annotation = dyn_cast<DictionaryAttr>(attr)) {
        auto cls = annotation.getAs<StringAttr>("class");
        auto widget = annotation.getAs<StringAttr>("widgetClass");
        if (!cls || cls.getValue() != "firesim.lib.bridgeutils.BridgeIOAnnotation" ||
            !widget || widget.getValue() != "midas.models.FASEDMemoryTimingModel") continue;
        if (key) {
          error = "FASED request limits need a unique retained BridgeIO constructor";
          return failure();
        }
        key = annotation.getAs<DictionaryAttr>("widgetConstructorKey");
        if (!key) {
          error = "FASED request limits need a retained BridgeIO constructor key";
          return failure();
        }
      }
  }
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!flight || flight.getValue().getBitWidth() > 64 || flight.getInt() <= 0) {
    error = "FASED request limits need a positive 64-bit maxFlight constructor";
    return failure();
  }
  limit = std::min<uint64_t>(16, flight.getInt());
  width = llvm::Log2_64_Ceil(limit + 1);
  return success();
}

LogicalResult goldengate::materializeFASEDRequestLimits(CircuitOp circuit,
    FModuleOp &result, std::string &error) {
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getName() == bankName) {
      error = "FASED request limit module already exists";
      return failure();
    }
  unsigned limit, width;
  if (failed(goldengate::getFASEDRequestLimitProfile(circuit, limit, width, error))) return failure();
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1); auto bankPorts = requestLimitPorts(ctx, width);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  const llvm::StringRef names[]{"writeMaxReqs", "readMaxReqs"};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < 2; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(8 + 4 * i)),
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
  Value initial = b.create<ConstantOp>(loc, uint(32), APInt(32, limit));
  for (unsigned i = 0; i < 2; ++i) {
    Value reg = b.create<RegResetOp>(loc, uint(32), arg(0), arg(1), initial, names[i]).getResult();
    Value write = slot("write", i), read = slot("read", i);
    // bindReg ignores wstrb, has no target-cycle enable, and reads the whole word.
    connect(reg, b.create<MuxPrimOp>(loc, field(write, "valid"), field(write, "bits"), reg));
    connect(field(read, "bits"), reg);
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
    connect(field(arg(2), i == 0 ? "write" : "read"), b.create<BitsPrimOp>(loc, reg, width - 1, 0));
  }

  result = bank;
  return success();
}

LogicalResult goldengate::attachFASEDRequestLimits(CircuitOp circuit,
    FModuleOp bank, std::string &error) {
  FModuleOp inner; unsigned indices[4];
  if (failed(attachmentTop(circuit, inner, indices, error))) return failure();
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (!bank || bank->getParentOp() != circuit.getOperation() || bank.getName() != bankName)
    return reject("FASED request-limit attachment requires its materialized bank in this circuit");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  unsigned limit, width;
  if (failed(goldengate::getFASEDRequestLimitProfile(circuit, limit, width, error))) return failure();
  auto expected = requestLimitPorts(ctx, width); auto actual = bank.getPorts();
  if (actual.size() != expected.size()) return reject("FASED request-limit bank needs exactly four ports");
  for (auto [i, port] : llvm::enumerate(actual))
    if (port.name != expected[i].name || port.type != expected[i].type ||
        port.direction != expected[i].direction)
      return reject("FASED request-limit bank ports differ from the clock/reset/constructor-sized admission/two-word MCR boundary");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == bank.getName(); });
  if (used) return reject("FASED request-limit attachment requires an uninstantiated bank");
  auto name = wrapperName(circuit);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto mcr = expected[3].type;
  std::optional<unsigned> clock = indices[0], reset = indices[1],
      writeLimit = indices[2], readLimit = indices[3];
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) if (i != *writeLimit && i != *readLimit) {
    copied.push_back(i); ports.push_back(port);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(name), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "requestLimits");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto port = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
                             port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  connect(sim.getResult(*writeLimit), field(mmio.getResult(2), "write"));
  connect(sim.getResult(*readLimit), field(mmio.getResult(2), "read"));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(3));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + name.str();
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
          suffix.replace(0, modulePrefix.size(), "|" + name.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> vs; for (auto v : arr) vs.push_back(retarget(v)); return b.getArrayAttr(vs); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList vs; for (auto v : d) vs.set(v.getName(), retarget(v.getValue())); return vs.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(name);
  return success();
}

LogicalResult goldengate::addFASEDRequestLimits(CircuitOp circuit, std::string &error) {
  FModuleOp inner, bank; unsigned indices[4];
  if (failed(attachmentTop(circuit, inner, indices, error)) ||
      failed(materializeFASEDRequestLimits(circuit, bank, error))) return failure();
  return attachFASEDRequestLimits(circuit, bank, error);
}
