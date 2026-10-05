// See LICENSE for license details.
// CPUManagedStreamEngine.attach(count, ReadOnly, substruct=false) through
// FIRRTL operations. The count is live throughout a stalled MMIO response.
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/CPUStreamCountBank.h"
#include "llvm/ADT/StringSet.h"
#include <limits>
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addCPUStreamCountBank(CircuitOp circuit,
                                              std::string &error) {
  if (circuit.getName() != "GGCPUStreamReadWrapper") {
    error = "CPU stream count bank requires the active CPU read wrapper";
    return failure();
  }
  return addCPUStreamCountBank(circuit,
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream_count", 13}}, error);
}

LogicalResult goldengate::addCPUStreamCountBank(
    CircuitOp circuit, ArrayRef<CPUStreamCountPort> counts, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGCPUStreamCountBank";
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamCountWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (counts.empty() || counts.size() > std::numeric_limits<int32_t>::max() / 4)
    return reject("CPU stream count bank needs a nonempty representable word list");
  llvm::StringSet<> streamNames, portNames;
  for (const auto &count : counts) {
    if (count.streamName.empty() || count.portName.empty() ||
        !streamNames.insert(count.streamName).second ||
        !portNames.insert(count.portName).second ||
        count.countBits == 0 || count.countBits > 32)
      return reject("CPU stream count identities and widths must be unique and valid");
  }
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("CPU stream count module already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("CPU stream count bank needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = uint(1);
  std::optional<unsigned> clock, reset;
  SmallVector<unsigned> countPorts;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "cpuStream_mcr") return reject("CPU stream MCR port already exists");
    if (p.name == "hostClock" && isa<ClockType>(p.type) && p.direction == Direction::In) clock = i;
    if (p.name == "hostReset" && p.type == bit && p.direction == Direction::In) reset = i;
  }
  if (!clock || !reset)
    return reject("CPU stream count bank needs host clock/reset");
  for (const auto &count : counts) {
    std::optional<unsigned> index;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name.getValue() == count.portName && p.type == uint(count.countBits) &&
          p.direction == Direction::Out) index = i;
    if (!index) return reject("CPU stream count bank requires each declared queue count output");
    countPorts.push_back(*index);
  }
  bool instantiated = false;
  circuit.walk([&](InstanceOp i) { instantiated |= i.getModuleName() == inner.getName(); });
  if (instantiated) return reject("CPU stream count bank requires an uninstantiated top");

  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(32)}});
  auto words = FVectorType::get(token, counts.size());
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  for (auto [i, count] : llvm::enumerate(counts))
    bankPorts.push_back({b.getStringAttr(counts.size() == 1 ? "count" : "count_" + std::to_string(i)),
                        uint(count.countBits), Direction::In});
  bankPorts.push_back({b.getStringAttr("mcr"), mcr, Direction::Out});
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  SmallVector<Attribute> registers;
  for (auto [i, count] : llvm::enumerate(counts))
    registers.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(count.streamName + "_count")),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)),
        b.getNamedAttr("readable", b.getBoolAttr(true)),
        b.getNamedAttr("writeable", b.getBoolAttr(false))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  for (unsigned i = 0; i < counts.size(); ++i) {
    auto slot = [&](llvm::StringRef group) -> Value {
      return b.create<SubindexOp>(loc, field(arg(counts.size() + 2), group), i);
    };
    Value rd = slot("read"), wr = slot("write");
    connect(field(rd, "bits"), b.create<PadPrimOp>(loc, arg(i + 2), 32));
    connect(field(rd, "valid"), one); connect(field(wr, "ready"), one);
    b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, field(wr, "valid")),
        b.create<NotPrimOp>(loc, arg(1)), "CPU stream count register is read only", ValueRange{}, "");
  }

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!llvm::is_contained(countPorts, i)) {
    copied.push_back(i); ports.push_back(p);
  }
  ports.push_back({b.getStringAttr("cpuStream_mcr"), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "streamCount");
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
        p.direction == Direction::In ? external : sim.getResult(i));
  }
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  for (auto [i, port] : llvm::enumerate(countPorts))
    connect(mmio.getResult(i + 2), sim.getResult(port));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(counts.size() + 2));
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
