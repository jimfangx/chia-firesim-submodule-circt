// See LICENSE for license details.
// TSIBridge.scala / Widget.gen{RO,WO}Reg and Pulsify(1), through FIRRTL ops.
// All nine words are attached ReadWrite. Status samples each host cycle, with
// writes overriding samples. Pulses clear next cycle unless written again.
// Only pulse registers have host reset; target reset belongs to the queues.
#include "goldengate/TSIMMIOBank.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addTSIMMIOBank(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGTSIMMIOBank";
  constexpr llvm::StringLiteral wrapperName = "GGTSIMMIOWrapper";
  constexpr llvm::StringLiteral controlName = "tsiBridge_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGTSIWordQueuesWrapper")
    return reject("TSI MMIO requires the active TSI word queue wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("TSI MMIO module or wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("TSI MMIO needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  auto token = [&](FIRRTLBaseType payload) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  auto wordToken = token(uint(32));
  auto control = BundleType::get(ctx, {{b.getStringAttr("step_size"), true, uint(32)},
      {b.getStringAttr("start"), true, bit}, {b.getStringAttr("done"), false, bit}});
  std::optional<unsigned> clock, reset, input, output, scheduler;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == controlName) return reject("TSI decoded MCR port already exists");
    if (p.name == "hostClock" && isa<ClockType>(p.type) && p.direction == Direction::In) clock = i;
    if (p.name == "hostReset" && p.type == bit && p.direction == Direction::In) reset = i;
    if (p.name == "tsi_in_enq" && p.type == wordToken && p.direction == Direction::In) input = i;
    if (p.name == "tsi_out_deq" && p.type == wordToken && p.direction == Direction::Out) output = i;
    if (p.name == "tsi_control" && p.type == control && p.direction == Direction::Out) scheduler = i;
  }
  if (!clock || !reset || !input || !output || !scheduler)
    return reject("TSI MMIO needs hostClock/hostReset and Decoupled word queues and scheduler control");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("TSI MMIO needs an uninstantiated top");

  // Validate the complete boundary before introducing hardware.
  auto words = FVectorType::get(token(uint(32)), 9);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("inBuf"), wordToken, Direction::Out},
      {b.getStringAttr("outBuf"), wordToken, Direction::In},
      {b.getStringAttr("control"), control, Direction::In},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  const llvm::StringRef names[]{"in_bits", "in_valid", "in_ready", "out_bits", "out_valid", "out_ready",
      "step_size", "done", "start"};
  const unsigned widths[]{32, 1, 1, 32, 1, 1, 32, 1, 1};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < 9; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)),
        b.getNamedAttr("readable", b.getBoolAttr(true)), b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(5), group), i);
  };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  SmallVector<Value> values;
  for (unsigned i = 0; i < 9; ++i) {
    Value reg = (i == 1 || i == 5 || i == 8)
        ? b.create<RegResetOp>(loc, bit, arg(0), arg(1), zero, names[i]).getResult()
        : b.create<RegOp>(loc, uint(widths[i]), arg(0), names[i]).getResult();
    values.push_back(reg);
    Value sample = i == 2 ? field(arg(2), "ready") : i == 3 ? field(arg(3), "bits")
        : i == 4 ? field(arg(3), "valid") : i == 7 ? field(arg(4), "done")
        : (i == 0 || i == 6) ? reg : zero;
    Value write = slot("write", i), read = slot("read", i);
    Value data = b.create<BitsPrimOp>(loc, field(write, "bits"), widths[i] - 1, 0);
    connect(reg, b.create<MuxPrimOp>(loc, field(write, "valid"), data, sample));
    connect(field(read, "bits"), b.create<PadPrimOp>(loc, reg, 32));
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
  }
  connect(field(arg(2), "bits"), values[0]); connect(field(arg(2), "valid"), values[1]);
  connect(field(arg(3), "ready"), values[5]);
  connect(field(arg(4), "step_size"), values[6]); connect(field(arg(4), "start"), values[8]);

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != *input && i != *output && i != *scheduler) {
    copied.push_back(i); ports.push_back(p);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "tsiRegisters");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  b.create<ConnectOp>(loc, sim.getResult(*input), mmio.getResult(2));
  b.create<ConnectOp>(loc, mmio.getResult(3), sim.getResult(*output));
  b.create<ConnectOp>(loc, mmio.getResult(4), sim.getResult(*scheduler));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(5));

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
