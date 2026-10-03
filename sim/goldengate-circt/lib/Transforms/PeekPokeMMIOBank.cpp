// See LICENSE for license details.
// Rocket PeekPoke's Widget register bindings and STEP Queue through FIRRTL ops.
// The seven decoded MCR words replace the provisional cycle-engine boundary.
// Nasti transport and driver collateral remain subsequent stages.
#include "goldengate/PeekPokeCycleEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addPeekPokeMMIOBank(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGPeekPokeStepQueue";
  constexpr llvm::StringLiteral bankName = "GGPeekPokeMMIOBank";
  constexpr llvm::StringLiteral wrapperName = "GGPeekPokeMMIOWrapper";
  constexpr llvm::StringLiteral controlName = "peekPokeBridge_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGPeekPokeCycleWrapper")
    return reject("PeekPoke MMIO requires the active PeekPoke cycle wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == queueName || m.getName() == bankName || m.getName() == wrapperName)
      return reject("PeekPoke MMIO module or wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("PeekPoke MMIO needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  auto token = [&](FIRRTLBaseType payload) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  auto wordToken = token(uint(32));
  auto cycleControl = BundleType::get(ctx, {
      {b.getStringAttr("step"), true, wordToken}, {b.getStringAttr("poke"), true, bit},
      {b.getStringAttr("resetValue"), true, bit},
      {b.getStringAttr("tCycle"), false, uint(64)}, {b.getStringAttr("done"), false, bit},
      {b.getStringAttr("precisePeekable"), false, bit}});
  std::optional<unsigned> clock, reset, cyclePort;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == controlName) return reject("PeekPoke decoded MCR port already exists");
    if (p.name == "hostClock" && isa<ClockType>(p.type) && p.direction == Direction::In) clock = i;
    if (p.name == "hostReset" && p.type == bit && p.direction == Direction::In) reset = i;
    if (p.name == "peekPokeBridge_cycle" && p.type == cycleControl && p.direction == Direction::Out) cyclePort = i;
  }
  if (!clock || !reset || !cyclePort)
    return reject("PeekPoke MMIO needs hostClock/hostReset and the exact cycle-engine boundary");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("PeekPoke MMIO needs an uninstantiated top");

  // Validate the complete boundary before introducing hardware.
  auto words = FVectorType::get(wordToken, 7);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  // Queue(UInt(32), 2), flow=false and pipe=false: no empty bypass and
  // no full enqueue on a simultaneous dequeue. Host reset flushes pointers,
  // but accepted pre-edge writes still update its unreset memory.
  SmallVector<PortInfo> queuePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq"), wordToken, Direction::In}, {b.getStringAttr("deq"), wordToken, Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(queueName),
      ConventionAttr::get(ctx, Convention::Internal), queuePorts);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto qa = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  auto qreg = [&](llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, bit, qa(0), qa(1), zero, name).getResult();
  };
  Value enqPtr = qreg("enq_ptr_value"), deqPtr = qreg("deq_ptr_value"), maybeFull = qreg("maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value full = b.create<AndPrimOp>(loc, equal, maybeFull);
  Value empty = b.create<AndPrimOp>(loc, equal, b.create<NotPrimOp>(loc, maybeFull));
  Value ready = b.create<NotPrimOp>(loc, full), valid = b.create<NotPrimOp>(loc, empty);
  Value push = b.create<AndPrimOp>(loc, ready, field(qa(2), "valid"));
  Value pop = b.create<AndPrimOp>(loc, valid, field(qa(3), "ready"));
  connect(field(qa(2), "ready"), ready); connect(field(qa(3), "valid"), valid);
  connect(enqPtr, b.create<MuxPrimOp>(loc, push, b.create<NotPrimOp>(loc, enqPtr), enqPtr));
  connect(deqPtr, b.create<MuxPrimOp>(loc, pop, b.create<NotPrimOp>(loc, deqPtr), deqPtr));
  connect(maybeFull, b.create<MuxPrimOp>(loc, b.create<XorPrimOp>(loc, push, pop), push, maybeFull));
  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(2, uint(32), MemOp::PortKind::Read),
      MemOp::getTypeForPort(2, uint(32), MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 2, RUWAttr::Undefined, memoryNames, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), qa(0)); connect(field(reader, "en"), one);
  connect(field(reader, "addr"), deqPtr); connect(field(qa(3), "bits"), field(reader, "data"));
  connect(field(writer, "clk"), qa(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), one);
  connect(field(writer, "data"), field(qa(2), "bits"));

  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("control"), cycleControl, Direction::In},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  const llvm::StringRef names[]{"tCycle_0", "tCycle_1", "tCycle_latch", "STEP", "DONE", "reset_0", "PRECISE_PEEKABLE"};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < 7; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)),
        b.getNamedAttr("readable", b.getBoolAttr(i != 2 && i != 3)),
        b.getNamedAttr("writeable", b.getBoolAttr(i >= 2))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(3), group), i);
  };
  // Values are local to each module; do not reuse queue constants here.
  zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value zeroWord = b.create<ConstantOp>(loc, uint(32), APInt(32, 0));
  Value snapshot = b.create<RegOp>(loc, uint(64), arg(0), "tCycle_tCycle_mmreg").getResult();
  Value resetValue = b.create<RegOp>(loc, bit, arg(0), "target_reset_i").getResult();
  Value done = b.create<RegResetOp>(loc, uint(32), arg(0), arg(1), zeroWord, "DONE").getResult();
  Value precise = b.create<RegResetOp>(loc, uint(32), arg(0), arg(1), zeroWord, "PRECISE_PEEKABLE").getResult();
  Value latchWrite = slot("write", 2), pokeWrite = slot("write", 5);
  Value latchBit = b.create<BitsPrimOp>(loc, field(latchWrite, "bits"), 0, 0);
  Value latch = b.create<AndPrimOp>(loc, field(latchWrite, "valid"), latchBit);
  connect(snapshot, b.create<MuxPrimOp>(loc, latch, field(arg(2), "tCycle"), snapshot));
  connect(resetValue, b.create<MuxPrimOp>(loc, field(pokeWrite, "valid"),
      b.create<BitsPrimOp>(loc, field(pokeWrite, "bits"), 0, 0), resetValue));
  for (auto pair : {std::make_pair(4U, done), std::make_pair(6U, precise)}) {
    Value write = slot("write", pair.first);
    Value sample = b.create<PadPrimOp>(loc, field(arg(2), pair.first == 4 ? "done" : "precisePeekable"), 32);
    connect(pair.second, b.create<MuxPrimOp>(loc, field(write, "valid"), field(write, "bits"), sample));
  }
  connect(field(arg(2), "poke"), field(pokeWrite, "valid"));
  connect(field(arg(2), "resetValue"), resetValue);
  auto step = b.create<InstanceOp>(loc, queue, "step_q");
  connect(step.getResult(0), arg(0)); connect(step.getResult(1), arg(1));
  b.create<ConnectOp>(loc, step.getResult(2), slot("write", 3));
  b.create<ConnectOp>(loc, field(arg(2), "step"), step.getResult(3));
  Value reads[]{b.create<BitsPrimOp>(loc, snapshot, 31, 0),
      b.create<BitsPrimOp>(loc, snapshot, 63, 32), zeroWord, zeroWord, done,
      b.create<PadPrimOp>(loc, resetValue, 32), precise};
  Value enabled = b.create<NotPrimOp>(loc, arg(1));
  for (unsigned i = 0; i < 7; ++i) {
    connect(field(slot("read", i), "bits"), reads[i]);
    // The DecoupledSink STEP read side is DontCare in Scala and lowers to
    // zero/invalid in this oracle. The WriteOnly latch read still has valid=1.
    connect(field(slot("read", i), "valid"), i == 3 ? zero : one);
    if (i != 3) connect(field(slot("write", i), "ready"), one);
    if (i < 4) {
      Value forbidden = field(slot(i < 2 ? "write" : "read", i), i < 2 ? "valid" : "ready");
      b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, forbidden), enabled,
          i < 2 ? "tCycle register is read only" : i == 2 ? "tCycle_latch is write only" : "STEP is a write-only decoupled sink",
          ValueRange{}, "");
    }
  }

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != *cyclePort) {
    copied.push_back(i); ports.push_back(p);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "peekPokeRegisters");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  b.create<ConnectOp>(loc, mmio.getResult(2), sim.getResult(*cyclePort));
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
