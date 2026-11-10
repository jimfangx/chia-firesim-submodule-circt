// See LICENSE for license details.
// SimpleNICBridge.scala / Widget.genROReg / Lib.scala MCRIO.bindReg.
// Normal NIC mode: five unreset write-only UInt<32> configuration registers;
// done is an unreset one-bit sample of !tFire, overridden by writes that cycle.
// Host reset only disables write-only read assertions. It does not reset or
// mask register writes, and MCR write strobes do not mask register data.
// Copied targets transfer recursively; consumed control/status owners remain
// on the retained inner module. Other module bodies and annotations survive.
#include "goldengate/NICMMIOBank.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <set>
#include <map>
using namespace mlir;
using namespace circt::firrtl;

namespace {
LogicalResult addNICMMIOBankImpl(CircuitOp circuit, bool loopback, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGNICMMIOBank";
  constexpr llvm::StringLiteral wrapperName = "GGNICMMIOWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (loopback ? (circuit.getName() != "GGNICLoopbackTokenQueuesWrapper" &&
                  circuit.getName() != "GGNICCPUStreamQueuesWrapper" &&
                  circuit.getName() != "GGNICCPUStreamResponseBufferWrapper") :
                 circuit.getName() != "GGNICCPUStreamResponseBufferWrapper")
    return reject("NIC MMIO requires the active matching token/CPU boundary");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("NIC MMIO module already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("NIC MMIO needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  SmallVector<DictionaryAttr> bridges;
  for (auto attr : raw) {
    auto annotation = dyn_cast<DictionaryAttr>(attr);
    if (annotation && annotation.getAs<StringAttr>("class") == goldengate::AnnotationClasses::BridgeIO &&
        annotation.getAs<StringAttr>("widgetClass") == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (annotation.get("widgetConstructorKey"))
        return reject("NIC MMIO requires constructor-free widgets");
      bridges.push_back(annotation);
    }
  }
  // The annotation-free decoded-bank unit fixture retains the original ABI.
  unsigned nics = bridges.empty() ? 1 : bridges.size();
  if (loopback) {
    // Production attaches MMIO after shared CPU transport/buffers. The token
    // owner then remains nested, rather than being the immediate active top.
    // Resolve exactly one live owner; an unused lookalike is not evidence.
    FModuleOp token;
    std::map<Operation *, unsigned> counts;
    std::set<Operation *> active;
    bool recursive = false;
    std::function<unsigned(FModuleOp)> count = [&](FModuleOp module) {
      if (!active.insert(module).second) { recursive = true; return 0U; }
      auto found = counts.find(module);
      if (found != counts.end()) { active.erase(module); return found->second; }
      unsigned n = module.getName() == "GGNICLoopbackTokenQueuesWrapper";
      if (n) token = module;
      for (auto instance : module.getOps<InstanceOp>())
        for (auto child : circuit.getOps<FModuleOp>())
          if (child.getName() == instance.getModuleName())
            n = std::min(2U, n + count(child));
      active.erase(module); counts[module] = n; return n;
    };
    if (count(inner) != 1 || recursive || !token)
      return reject("NIC loopback MMIO requires a unique live token owner");
    auto bindings = token->getAttrOfType<ArrayAttr>("goldengate.nicTokenBindings");
    if (!bindings || bindings.size() != nics)
      return reject("NIC loopback MMIO requires every token binding");
    for (auto [widget, a] : llvm::enumerate(bindings)) {
      auto row = dyn_cast<DictionaryAttr>(a);
      auto index = row ? row.getAs<IntegerAttr>("widget") : IntegerAttr();
      auto mode = row ? row.getAs<StringAttr>("mode") : StringAttr();
      if (!index || index.getInt() != widget || !mode || mode != "compile_time_loopback")
        return reject("NIC loopback MMIO requires ordered compile-time loopback widgets");
    }
  }
  auto controlName = [&](unsigned widget) {
    return nics == 1 ? std::string("nicBridge_mcr") : "nicBridge_" + std::to_string(widget) + "_mcr";
  };
  auto externalName = [&](unsigned widget, StringRef original) {
    return nics == 1 ? original.str() : "nic" + std::to_string(widget) + original.drop_front(3).str();
  };
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = uint(1);
  const llvm::StringRef signals[]{"nic_macAddr", "nic_rlimit_inc", "nic_rlimit_period",
      "nic_rlimit_size", "nic_pauser_threshold", "nic_pauser_quanta", "nic_pauser_refresh", "nic_done"};
  const unsigned widths[]{48, 8, 8, 8, 16, 16, 16, 1};
  std::set<unsigned> consumed;
  SmallVector<SmallVector<unsigned>> widgets;
  for (unsigned widget = 0; widget < nics; ++widget) {
  SmallVector<unsigned> ports;
  for (unsigned j = loopback ? 7 : 0; j < 8; ++j) {
    std::optional<unsigned> found;
    for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (p.name == externalName(widget, signals[j])) {
      if (found || p.type != uint(widths[j]) ||
          p.direction != (j == 7 ? Direction::Out : Direction::In))
        return reject("NIC MMIO control/status width or direction differs");
      found = i;
    }
    if (!found) return reject("NIC MMIO control/status port is missing");
    if (!consumed.insert(*found).second)
      return reject("NIC MMIO widgets share a control/status port");
    ports.push_back(*found);
  }
  widgets.push_back(std::move(ports));
  }
  std::optional<unsigned> clock, reset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    for (unsigned widget = 0; widget < nics; ++widget)
      if (p.name == controlName(widget)) return reject("NIC decoded MCR port already exists");
    if (p.name == "hostClock" && isa<ClockType>(p.type) && p.direction == Direction::In) clock = i;
    if (p.name == "hostReset" && p.type == bit && p.direction == Direction::In) reset = i;
  }
  if (!clock || !reset) return reject("NIC MMIO needs host clock/reset");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("NIC MMIO requires an uninstantiated top");

  // Validate the entire boundary before introducing FIRRTL operations.
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(32)}});
  unsigned count = loopback ? 1 : 6;
  unsigned doneWord = count - 1, donePort = loopback ? 2 : 9, mcrPort = donePort + 1;
  auto words = FVectorType::get(token, count);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  for (unsigned j = loopback ? 7 : 0; j < 8; ++j)
    bankPorts.push_back({b.getStringAttr(signals[j]), uint(widths[j]), j == 7 ? Direction::In : Direction::Out});
  bankPorts.push_back({b.getStringAttr("mcr"), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  const llvm::StringRef names[]{"macaddr_upper", "macaddr_lower", "rlimit_settings", "pause_threshold", "pause_times", "done"};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < count; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(loopback ? "done" : names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4*i)),
        b.getNamedAttr("readable", b.getBoolAttr(i == doneWord)), b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(mcrPort), group), i);
  };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value zero32 = b.create<ConstantOp>(loc, uint(32), APInt(32, 0));
  SmallVector<Value> values;
  for (unsigned i = 0; i < count; ++i) {
    Value reg = b.create<RegOp>(loc, uint(i == doneWord ? 1 : 32), arg(0), loopback ? "done" : names[i]).getResult();
    values.push_back(reg);
    Value write = slot("write", i), read = slot("read", i);
    Value data = field(write, "bits");
    if (i == doneWord) data = b.create<BitsPrimOp>(loc, data, 0, 0);
    connect(reg, b.create<MuxPrimOp>(loc, field(write, "valid"), data, i == doneWord ? arg(donePort) : reg));
    connect(field(read, "bits"), i == doneWord ? b.create<PadPrimOp>(loc, reg, 32).getResult() : zero32);
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
    if (i < doneWord) b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, field(read, "ready")),
        b.create<NotPrimOp>(loc, arg(1)), "NIC register is write only", ValueRange{}, "");
  }
  auto bits = [&](Value v, unsigned hi, unsigned lo) -> Value { return b.create<BitsPrimOp>(loc, v, hi, lo); };
  if (!loopback) {
  connect(arg(2), b.create<CatPrimOp>(loc, bits(values[0], 15, 0), values[1]));
  connect(arg(3), bits(values[2], 23, 16)); connect(arg(4), bits(values[2], 15, 8)); connect(arg(5), bits(values[2], 7, 0));
  connect(arg(6), bits(values[3], 15, 0)); connect(arg(7), bits(values[4], 15, 0)); connect(arg(8), bits(values[4], 31, 16));
  }

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!llvm::is_contained(consumed, i)) {
    copied.push_back(i); ports.push_back(p);
  }
  unsigned firstMCR = ports.size();
  for (unsigned widget = 0; widget < nics; ++widget)
    ports.push_back({b.getStringAttr(controlName(widget)), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
        p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  SmallVector<Attribute> bindings;
  for (auto [widget, consumedPorts] : llvm::enumerate(widgets)) {
    auto instanceName = nics == 1 ? std::string("nicRegisters") : "nicRegisters_" + std::to_string(widget);
    auto mmio = b.create<InstanceOp>(loc, bank, instanceName);
    connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
    if (!loopback)
      for (unsigned i = 0; i < 7; ++i) connect(sim.getResult(consumedPorts[i]), mmio.getResult(i+2));
    connect(mmio.getResult(donePort), sim.getResult(consumedPorts.back()));
    b.create<ConnectOp>(loc, wrapper.getArgument(firstMCR + widget), mmio.getResult(mcrPort));
    NamedAttrList binding;
    binding.set("mode", b.getStringAttr(loopback ? "compile_time_loopback" : "normal"));
    binding.set("widget", b.getI64IntegerAttr(widget));
    binding.set("mcr", b.getStringAttr(controlName(widget)));
    binding.set("registers", b.getStringAttr(instanceName));
    if (!bridges.empty()) {
      if (auto target = bridges[widget].getAs<StringAttr>("target")) binding.set("bridgeTarget", target);
    }
    bindings.push_back(binding.getDictionary(ctx));
  }

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
  wrapper->setAttr("goldengate.nicMMIOBindings", retarget(b.getArrayAttr(bindings)));
  for (auto module : circuit.getOps<FModuleOp>())
    for (auto key : {"goldengate.nicTokenBindings", "goldengate.nicPacketBindings",
                     "goldengate.nicCPUStreamBindings"})
      if (auto metadata = module->getAttr(key)) module->setAttr(key, retarget(metadata));
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(wrapperName);
  return success();
}

} // namespace

LogicalResult goldengate::addNICMMIOBank(CircuitOp circuit, std::string &error) {
  return addNICMMIOBankImpl(circuit, false, error);
}
LogicalResult goldengate::addNICLoopbackMMIOBank(CircuitOp circuit, std::string &error) {
  return addNICMMIOBankImpl(circuit, true, error);
}
