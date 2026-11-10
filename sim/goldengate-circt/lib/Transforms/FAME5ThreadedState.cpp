// See LICENSE for license details.
// Oracle: midas/passes/fame/MuxingMultithreader.scala, onStmt/onExprRHS.
#include "goldengate/FAME5ThreadedState.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/APSInt.h"
#include <limits>

using namespace mlir;
using namespace circt::firrtl;

static bool recognizeFAME5ClockGate(InstanceOp instance, bool virtualController) {
  auto circuit = instance->getParentOfType<CircuitOp>();
  if (!circuit) return false;
  auto external = dyn_cast_or_null<FExtModuleOp>(
      SymbolTable::lookupSymbolIn(circuit, instance.getModuleName()));
  if (!external || external.getDefname() != "AbstractClockGate" ||
      external.getConvention() != Convention::Internal ||
      !external.getParameters().empty() || !external.getLayers().empty() ||
      external.getNumPorts() != 3 || instance.getNumResults() != 3 ||
      instance->hasAttr("inner_sym") || !instance.getAnnotations().empty()) return false;
  const StringRef names[]{"I", "CE", "O"};
  const Direction directions[]{Direction::In, Direction::In, Direction::Out};
  Type types[]{ClockType::get(instance.getContext()), UIntType::get(instance.getContext(), 1),
               ClockType::get(instance.getContext())};
  for (unsigned p = 0; p < 3; ++p) {
    if (external.getPortName(p) != names[p] || external.getPortType(p) != types[p] ||
        external.getPortDirection(p) != directions[p] ||
        instance.getPortNameStr(p) != names[p] || instance.getResult(p).getType() != types[p] ||
        instance.getPortDirection(p) != directions[p] ||
        !external.getAnnotationsForPort(p).empty() || external.hasPortSymbolAttr(p)) return false;
  }
  if (auto annotations = external->getAttrOfType<ArrayAttr>("annotations"); annotations && !annotations.empty()) return false;
  if (auto portAnnotations = instance->getAttrOfType<ArrayAttr>("portAnnotations"))
    for (auto annotation : portAnnotations)
      if (auto array = dyn_cast<ArrayAttr>(annotation); !array || !array.empty()) return false;
  // The virtual-controller recognizer validates the dynamic shape below. The
  // caller must explicitly choose the Xilinx BUFGCE platform semantics before
  // grouping this controller. In particular, an arbitrary channel valid/data bit is not a clock
  // enable: the enabled flag, finishing condition and host reset mask must all
  // retain their SFC scheduling meaning before and after register banking.
  auto module = instance->getParentOfType<FModuleOp>();
  if (!module) return false;
  auto root = [&](Value value) -> Value {
    llvm::SmallDenseSet<Value, 8> visited;
    while (value && visited.insert(value).second) {
      Operation *def = value.getDefiningOp();
      if (!def) return value;
      if (def->getBlock() != module.getBodyBlock()) return {};
      if (auto node = dyn_cast<NodeOp>(def)) { value = node.getInput(); continue; }
      if (auto cast = dyn_cast<ConstCastOp>(def)) { value = cast.getInput(); continue; }
      if (auto cast = dyn_cast<AsUIntPrimOp>(def); cast && isa<UIntType>(cast.getInput().getType())) { value = cast.getInput(); continue; }
      if (!isa<WireOp>(def)) return value;
      Operation *driver = nullptr;
      for (auto &use : value.getUses())
        if (isa<ConnectOp, StrictConnectOp>(use.getOwner()) && use.getOperandNumber() == 0) {
          if (driver || use.getOwner()->getBlock() != module.getBodyBlock()) return {};
          driver = use.getOwner();
        }
      value = driver ? driver->getOperand(1) : Value();
    }
    return {};
  };
  Value drivers[2];
  for (unsigned p = 0; p < 2; ++p)
    for (auto &use : instance.getResult(p).getUses()) {
      if (!isa<ConnectOp, StrictConnectOp>(use.getOwner()) || use.getOperandNumber() != 0 ||
          use.getOwner()->getBlock() != module.getBodyBlock() || drivers[p]) return false;
      drivers[p] = use.getOwner()->getOperand(1);
    }
  if (!drivers[0] || !drivers[1]) return false;
  auto clock = dyn_cast_or_null<BlockArgument>(root(drivers[0]));
  Value enable = root(drivers[1]);
  auto isBit = [&](Value value, bool one) {
    value = root(value);
    auto constant = value ? value.getDefiningOp<ConstantOp>() : ConstantOp();
    return constant && constant.getValue().getBitWidth() == 1 &&
           (one ? constant.getValue().isOne() : constant.getValue().isZero());
  };
  if (!clock || !isa<ClockType>(clock.getType()) ||
      clock.getOwner() != module.getBodyBlock() ||
      module.getPortName(clock.getArgNumber()) != "hostClock" ||
      module.getPortDirection(clock.getArgNumber()) != Direction::In) return false;
  if (isBit(enable, true)) return !virtualController;
  if (!virtualController) return false;
  Value hostReset;
  for (unsigned p = 0; p < module.getNumPorts(); ++p)
    if (module.getPortName(p) == "hostReset" &&
        module.getPortDirection(p) == Direction::In && module.getPortType(p) == types[1])
      hostReset = module.getArgument(p);
  if (!hostReset) return false;
  auto uniqueDriver = [&](Value value) -> Value {
    Value driver;
    for (auto &use : value.getUses())
      if (isa<ConnectOp, StrictConnectOp>(use.getOwner()) && use.getOperandNumber() == 0) {
        if (driver || use.getOwner()->getBlock() != module.getBodyBlock()) return {};
        driver = use.getOwner()->getOperand(1);
      }
    return root(driver);
  };
  // Preserve the finishing subtree: FAMETransform emits precisely
  // (enabled & finishing) & ~hostReset. Arbitrary raw CE, missing reset masks,
  // inverted/reset-bypass forms and swapped temporal dependencies reject.
  auto ceOuter = enable ? enable.getDefiningOp<AndPrimOp>() : AndPrimOp();
  if (!ceOuter) return false;
  Value innerValue = root(ceOuter.getLhs()), maskValue = root(ceOuter.getRhs());
  auto inner = innerValue ? innerValue.getDefiningOp<AndPrimOp>() : AndPrimOp();
  auto mask = maskValue ? maskValue.getDefiningOp<NotPrimOp>() : NotPrimOp();
  if (!inner || !mask || root(mask.getInput()) != hostReset) return false;
  SmallVector<Value> terms{root(inner.getLhs()), root(inner.getRhs())};
  // Recover the host-owned enabled flag from either its original declaration
  // or the exact async-read register bank generated by threadFAME5Registers.
  // Checking the write mux rather than a cached "valid" marker makes the
  // memory threading API fail closed on malformed/mutated register banks too.
  auto enabledNext = [&](Value flag) -> Value {
    if (auto reg = flag.getDefiningOp<RegResetOp>()) {
      if (root(reg.getClockVal()) != clock || root(reg.getResetSignal()) != hostReset ||
          !isBit(reg.getResetValue(), false)) return {};
      return uniqueDriver(flag);
    }
    auto data = flag.getDefiningOp<SubfieldOp>();
    if (!data || data.getFieldName() != "data") return {};
    auto memory = data.getInput().getDefiningOp<MemOp>();
    if (!memory || !memory->hasAttr("goldengate.fame5.registerState") ||
        memory.getReadLatency() != 0 || memory.getWriteLatency() != 1 ||
        memory.getNumResults() < 2 || memory.getDataType() != types[1] ||
        memory.getPortKind(0) != MemOp::PortKind::Read ||
        memory.getPortKind(1) != MemOp::PortKind::Write || data.getInput() != memory.getResult(0)) return {};
    auto fieldDriver = [&](Value port, StringRef name) -> Value {
      Value driver;
      for (Operation *user : port.getUsers())
        if (auto field = dyn_cast<SubfieldOp>(user); field && field.getFieldName() == name) {
          if (driver) return {};
          driver = uniqueDriver(field.getResult());
        }
      return driver;
    };
    Value read = memory.getResult(0), write = memory.getResult(1);
    Value address = fieldDriver(read, "addr");
    if (!address || address != fieldDriver(write, "addr") ||
        fieldDriver(read, "clk") != clock || fieldDriver(write, "clk") != clock ||
        !isBit(fieldDriver(read, "en"), true) ||
        !isBit(fieldDriver(write, "mask"), true)) return {};
    auto scheduleValid = [&](Value enable) {
      enable = root(enable);
      if (isBit(enable, true)) return true;
      auto bound = enable ? enable.getDefiningOp<LTPrimOp>() : LTPrimOp();
      auto count = bound ? root(bound.getRhs()).getDefiningOp<ConstantOp>() : ConstantOp();
      return bound && root(bound.getLhs()) == root(address) && count &&
             count.getValue().getZExtValue() == memory.getDepth();
    };
    Value writeEnable = root(fieldDriver(write, "en"));
    if (memory.getNumResults() == 2) {
      if (!scheduleValid(writeEnable)) return {};
    } else {
      if (memory.getNumResults() != memory.getDepth() + 2) return {};
      auto gated = writeEnable ? writeEnable.getDefiningOp<AndPrimOp>() : AndPrimOp();
      auto mask = gated ? root(gated.getRhs()).getDefiningOp<NotPrimOp>() : NotPrimOp();
      if (!gated || !scheduleValid(gated.getLhs()) || !mask || root(mask.getInput()) != hostReset) return {};
      for (unsigned p = 2; p < memory.getNumResults(); ++p) {
        Value port = memory.getResult(p);
        auto index = root(fieldDriver(port, "addr")).getDefiningOp<ConstantOp>();
        if (memory.getPortKind(p) != MemOp::PortKind::Write || !index ||
            index.getValue().getZExtValue() != p - 2 ||
            fieldDriver(port, "clk") != clock || root(fieldDriver(port, "en")) != hostReset ||
            !isBit(fieldDriver(port, "mask"), true) || !isBit(fieldDriver(port, "data"), false)) return {};
      }
    }
    Value next = fieldDriver(write, "data");
    auto resetMux = next ? next.getDefiningOp<MuxPrimOp>() : MuxPrimOp();
    if (!resetMux || root(resetMux.getSel()) != hostReset || !isBit(resetMux.getHigh(), false)) return {};
    return root(resetMux.getLow());
  };
  for (Value flag : terms) {
    Value next = flag ? enabledNext(flag) : Value();
    auto update = next ? next.getDefiningOp<MuxPrimOp>() : MuxPrimOp();
    if (!update || !isBit(update.getHigh(), true) || root(update.getLow()) != flag) continue;
    Value finishing = root(update.getSel());
    if (!finishing || finishing == flag) continue;
    if ((root(inner.getLhs()) == flag && root(inner.getRhs()) == finishing) ||
        (root(inner.getRhs()) == flag && root(inner.getLhs()) == finishing)) return true;
  }
  return false;
}

bool goldengate::isFAME5ClockGate(InstanceOp instance) {
  return recognizeFAME5ClockGate(instance, false);
}
bool goldengate::isFAME5VirtualClockController(InstanceOp instance) {
  return recognizeFAME5ClockGate(instance, true);
}

LogicalResult goldengate::threadFAME5Registers(FModuleOp module,
                                               unsigned threadCount,
                                               Value threadIndex,
                                               std::string &error,
                                             bool broadcastHostReset) {
  auto reject = [&](StringRef reason) {
    error = reason.str();
    return failure();
  };
  auto indexType = threadIndex ? dyn_cast<UIntType>(threadIndex.getType())
                               : UIntType();
  unsigned indexWidth = threadCount > 1 ? llvm::Log2_64_Ceil(threadCount) : 0;
  auto indexArg = dyn_cast_or_null<BlockArgument>(threadIndex);
  if (threadCount < 2 || !indexType || indexType.getWidth() != indexWidth ||
      !indexArg || indexArg.getOwner() != module.getBodyBlock() ||
      module.getPortDirection(indexArg.getArgNumber()) != Direction::In)
    return reject("threading requires at least two threads and an input index of ceil(log2(threads)) bits");
  if (module->hasAttr("goldengate.fame5.threadCount"))
    return reject("register state is already threaded");

  struct Rule {
    Operation *reg;
    Value state, clock, reset, init;
    Operation *connect = nullptr;
  };
  SmallVector<Rule> rules;
  bool invalid = false;
  std::string invalidRegister;
  module.walk([&](Operation *op) {
    if (!isa<RegOp, RegResetOp>(op) || invalid)
      return;
    invalidRegister = module.getName().str() + ">" + op->getAttrOfType<StringAttr>("name").str();
    if (op->getBlock() != module.getBodyBlock() || op->getNumResults() != 1) {
      invalid = true;
      return;
    }
    Value state = op->getResult(0), clock, reset, init;
    auto type = dyn_cast<IntType>(state.getType());
    // A zero-width lowered register has no state bits to isolate. Retain the
    // declaration and its zero-width expression uses for normal FIRRTL cleanup;
    // allocating a target memory for it would invent storage for an empty value.
    if (type && type.getWidth() == 0) return;
    auto annos = op->getAttrOfType<ArrayAttr>("annotations");
    if (!type || type.getWidth() <= 0 || op->hasAttr("inner_sym") ||
        (annos && !annos.empty())) {
      invalid = true;
      return;
    }
    if (auto reg = dyn_cast<RegOp>(op))
      clock = reg.getClockVal();
    else {
      auto resetReg = cast<RegResetOp>(op);
      clock = resetReg.getClockVal();
      reset = resetReg.getResetSignal();
      init = resetReg.getResetValue();
      auto resetType = dyn_cast<UIntType>(reset.getType());
      if (!resetType || resetType.getWidth() != 1) {
        invalid = true;
        return;
      }
    }
    Rule rule{op, state, clock, reset, init};
    for (auto &use : state.getUses()) {
      Operation *owner = use.getOwner();
      if (isa<ConnectOp, StrictConnectOp>(owner) && use.getOperandNumber() == 0) {
        if (rule.connect || owner->getBlock() != module.getBodyBlock()) {
          invalid = true;
          return;
        }
        rule.connect = owner;
      } else if (isa<SubfieldOp, SubindexOp, SubaccessOp>(owner)) {
        invalid = true;
        return;
      }
    }
    if (!rule.connect) {
      invalid = true;
      return;
    }
    rules.push_back(rule);
  });
  if (invalid)
    return reject("threading requires scalar, unannotated synchronous registers with one lowered whole-register connect: " + invalidRegister);
  // Archive targets must not silently keep register meaning after a declaration
  // becomes a memory. This includes hierarchical instance paths and selectors.
  // The production grouping pass must explicitly rename/consume such targets.
  llvm::StringSet<> registerNames;
  for (auto &rule : rules)
    registerNames.insert(rule.reg->getAttrOfType<StringAttr>("name").getValue());
  bool referenced = false;
  auto inspect = [&](auto &&self, Attribute attr) -> void {
    if (auto str = dyn_cast<StringAttr>(attr)) {
      auto spelling = str.getValue();
      if (!spelling.starts_with("~") || !spelling.contains('|') || !spelling.contains('>'))
        return;
      auto pathAndRef = spelling.split('|').second.split('>');
      auto leaf = pathAndRef.first;
      if (leaf.contains('/'))
        leaf = leaf.rsplit('/').second.split(':').second;
      auto ref = pathAndRef.second.take_until([](char c) { return c == '.' || c == '['; });
      referenced |= leaf == module.getName() && registerNames.contains(ref);
    } else if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
      for (auto entry : dict)
        self(self, entry.getValue());
    } else if (auto array = dyn_cast<ArrayAttr>(attr)) {
      for (auto entry : array)
        self(self, entry);
    }
  };
  if (auto circuit = module->getParentOfType<CircuitOp>())
    if (auto raw = circuit->getAttr("rawAnnotations"))
      inspect(inspect, raw);
  if (referenced)
    return reject("threading needs explicit renames for retained register annotation targets");

  // Validate the complete module before creating any storage. Keep all old
  // declarations alive until every reference (including cross-register reset
  // values and feedback) can be redirected to the selected read slot.
  auto *ctx = module.getContext();
  auto bit = UIntType::get(ctx, 1);
  Value hostClock, hostReset;
  for (unsigned p = 0; p < module.getNumPorts(); ++p) {
    if (module.getPortName(p) == "hostClock") hostClock = module.getArgument(p);
    if (module.getPortName(p) == "hostReset") hostReset = module.getArgument(p);
  }
  SmallVector<std::pair<Rule, Value>> replacements;
  for (auto &rule : rules) {
    OpBuilder b(rule.reg);
    Location loc = rule.reg->getLoc();
    SmallVector<Type> types{
        MemOp::getTypeForPort(threadCount, cast<FIRRTLBaseType>(rule.state.getType()), MemOp::PortKind::Read),
        MemOp::getTypeForPort(threadCount, cast<FIRRTLBaseType>(rule.state.getType()), MemOp::PortKind::Write)};
    SmallVector<Attribute> names{b.getStringAttr("read"), b.getStringAttr("write")};
    // FAME-created host control flags have synchronous hostReset and constant
    // initial values. Reset their complete bank in production, so resetting the
    // shared selector cannot strand other logical threads' controls unreset.
    // This never clears target memories or target-clocked register state.
    bool broadcast = broadcastHostReset && hostClock && hostReset &&
        rule.clock == hostClock && rule.reset == hostReset &&
        rule.init && rule.init.getDefiningOp<ConstantOp>();
    if (broadcast)
      for (unsigned slot = 0; slot < threadCount; ++slot) {
        types.push_back(types[1]);
        names.push_back(b.getStringAttr("hostReset" + Twine(slot)));
      }
    auto name = rule.reg->getAttrOfType<StringAttr>("name");
    auto mem = b.create<MemOp>(loc, types, 0, 1, threadCount,
                              RUWAttr::Undefined, names, name.getValue());
    mem->setAttr("goldengate.fame5.registerState", b.getUnitAttr());
    auto field = [&](Value port, StringRef name) -> Value {
      return b.create<SubfieldOp>(loc, port, name);
    };
    auto connect = [&](Value dest, Value src) {
      b.create<StrictConnectOp>(loc, dest, src);
    };
    Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
    Value read = mem.getResult(0), write = mem.getResult(1);
    connect(field(read, "clk"), rule.clock);
    connect(field(read, "en"), one);
    connect(field(read, "addr"), threadIndex);
    connect(field(write, "clk"), rule.clock);
    Value valid = one;
    if (!llvm::isPowerOf2_64(threadCount)) {
      Value count = b.create<ConstantOp>(loc, indexType, APInt(indexWidth, threadCount));
      valid = b.create<LTPrimOp>(loc, threadIndex, count);
    }
    if (broadcast)
      valid = b.create<AndPrimOp>(loc, valid, b.create<NotPrimOp>(loc, hostReset));
    connect(field(write, "en"), valid);
    connect(field(write, "addr"), threadIndex);
    connect(field(write, "mask"), one);
    Value data = field(write, "data");
    Value selected = field(read, "data");
    if (broadcast)
      for (unsigned slot = 0; slot < threadCount; ++slot) {
        Value port = mem.getResult(slot + 2);
        connect(field(port, "clk"), hostClock);
        connect(field(port, "en"), hostReset);
        connect(field(port, "addr"), b.create<ConstantOp>(loc, indexType, APInt(indexWidth, slot)));
        connect(field(port, "mask"), one);
        auto initial = rule.init.getDefiningOp<ConstantOp>();
        auto dataType = cast<IntType>(rule.state.getType());
        auto resetBits = isa<SIntType>(rule.init.getType())
            ? initial.getValue().sextOrTrunc(*dataType.getWidth())
            : initial.getValue().zextOrTrunc(*dataType.getWidth());
        connect(field(port, "data"), b.create<ConstantOp>(loc, dataType, resetBits));
      }
    replacements.push_back({rule, selected});
    // Place the write at the old connect, where next-state expressions already
    // dominate. The reset mux matches the SFC appended conditional reset store.
    b.setInsertionPoint(rule.connect);
    Value next = rule.connect->getOperand(1);
    if (rule.reset)
      next = b.create<MuxPrimOp>(loc, rule.reset, rule.init, next);
    b.create<ConnectOp>(loc, data, next);
  }
  for (auto &[rule, selected] : replacements) {
    rule.connect->erase();
    rule.state.replaceAllUsesWith(selected);
  }
  for (auto &[rule, selected] : replacements)
    rule.reg->erase();
  module->setAttr("goldengate.fame5.threadCount",
                  IntegerAttr::get(IntegerType::get(ctx, 32), threadCount));
  return success();
}

LogicalResult goldengate::threadFAME5Memories(FModuleOp module,
                                             unsigned threadCount,
                                             Value threadIndex,
                                             std::string &error,
                                             FAME5MemoryClockSemantics semantics) {
  auto reject = [&](StringRef reason) { error = reason.str(); return failure(); };
  auto indexType = threadIndex ? dyn_cast<UIntType>(threadIndex.getType()) : UIntType();
  auto indexArg = dyn_cast_or_null<BlockArgument>(threadIndex);
  unsigned indexWidth = threadCount > 1 ? llvm::Log2_64_Ceil(threadCount) : 0;
  if (threadCount < 2 || !indexType ||
      indexType.getWidth() != indexWidth || !indexArg ||
      indexArg.getOwner() != module.getBodyBlock() ||
      module.getPortDirection(indexArg.getArgNumber()) != Direction::In)
    return reject("target-memory threading requires at least two threads and matching input index");
  if (module->hasAttr("goldengate.fame5.memoryThreadCount"))
    return reject("target memories are already threaded");
  struct Memory { MemOp op; Value targetClock; };
  SmallVector<Memory> memories;
  llvm::StringSet<> names;
  bool invalid = false;
  Value hostClock, hostReset;
  for (unsigned p = 0; p < module.getNumPorts(); ++p) {
    if (module.getPortName(p) == "hostClock" &&
        module.getPortDirection(p) == Direction::In &&
        isa<ClockType>(module.getPortType(p)))
      hostClock = module.getBodyBlock()->getArgument(p);
    if (module.getPortName(p) == "hostReset" &&
        module.getPortDirection(p) == Direction::In &&
        module.getPortType(p) == UIntType::get(module.getContext(), 1))
      hostReset = module.getBodyBlock()->getArgument(p);
  }
  if (!llvm::isPowerOf2_64(threadCount) &&
      semantics == FAME5MemoryClockSemantics::XilinxBUFGCE && !hostReset)
    return reject("production non-power-of-two threading needs hostReset to align accessor bases");
  // Preserve SFC's AbstractClockGate infrastructure while identifying the one
  // target clock shared by all accessors. Its enable contains banked model state
  // and token-fire logic, so do not reinterpret it as an ordinary clock alias.
  auto uniqueDriver = [&](Value wire) -> Value {
    Operation *driver = nullptr;
    for (auto &use : wire.getUses())
      if (isa<ConnectOp, StrictConnectOp>(use.getOwner()) && use.getOperandNumber() == 0) {
        if (driver || use.getOwner()->getBlock() != module.getBodyBlock()) return {};
        driver = use.getOwner();
      }
    return driver ? driver->getOperand(1) : Value();
  };
  auto aliasRoot = [&](Value value) -> Value {
    llvm::SmallDenseSet<Value, 8> visited;
    while (value && visited.insert(value).second) {
      Operation *op = value.getDefiningOp();
      if (!op) return value;
      if (op->getBlock() != module.getBodyBlock()) return {};
      if (auto node = dyn_cast<NodeOp>(op)) { value = node.getInput(); continue; }
      if (isa<WireOp>(op)) { value = uniqueDriver(value); continue; }
      return value;
    }
    return {};
  };
  auto inputClock = [&](Value value) -> Value {
    value = aliasRoot(value);
    if (!value) return {};
    if (auto argument = dyn_cast<BlockArgument>(value))
      return argument.getOwner() == module.getBodyBlock() &&
                     module.getPortDirection(argument.getArgNumber()) == Direction::In &&
                     isa<ClockType>(value.getType()) ? value : Value();
    auto gate = value.getDefiningOp<InstanceOp>();
    bool supportedGate = gate && (isFAME5ClockGate(gate) ||
        (semantics == FAME5MemoryClockSemantics::XilinxBUFGCE &&
         isFAME5VirtualClockController(gate)));
    if (!supportedGate || value != gate.getResult(2) || !hostClock)
      return {};
    Value clock = uniqueDriver(gate.getResult(0));
    Value enable = uniqueDriver(gate.getResult(1));
    return clock && enable && aliasRoot(clock) == hostClock ? value : Value();
  };
  module.walk([&](MemOp mem) {
    if (mem->hasAttr("goldengate.fame5.registerState"))
      return;
    if (mem->getBlock() != module.getBodyBlock() || mem.getReadLatency() > 1 ||
        mem.getWriteLatency() != 1 || !mem.getDepth() ||
        mem.getDepth() > uint64_t(std::numeric_limits<int64_t>::max()) / threadCount ||
        mem->hasAttr("inner_sym") || mem->hasAttr("init") ||
        mem->hasAttr("goldengate.fame5.threadedMemory") ||
        !mem.getAnnotations().empty()) {
      invalid = true;
      return;
    }
    for (Attribute portAnnotations : mem.getPortAnnotations())
      if (auto array = dyn_cast<ArrayAttr>(portAnnotations); !array || !array.empty())
        invalid = true;
    // SFC operates on lowered target memories. Response buffers have scalar
    // masks; do not invent an aggregate-mask or internally generated clock
    // interpretation before those forms have been lowered by the caller.
    if (mem.getReadLatency() == 1) {
      auto dataType = dyn_cast<IntType>(mem.getDataType());
      if (!hostClock || !dataType || dataType.getWidth() <= 0 ||
          !mem.getNumResults())
        invalid = true;
    }
    Memory rule{mem, {}};
    for (auto [port, result] : llvm::enumerate(mem.getResults())) {
      if (mem.getPortKind(port) == MemOp::PortKind::Debug) {
        invalid = true;
        continue;
      }
      unsigned addresses = 0;
      unsigned clocks = 0;
      for (Operation *user : result.getUsers()) {
        auto field = dyn_cast<SubfieldOp>(user);
        if (!field || field->getBlock() != module.getBodyBlock()) {
          invalid = true;
          continue;
        }
        bool clockField = field.getFieldName() == "clk" && mem.getReadLatency() == 1;
        if (clockField) ++clocks;
        if (field.getFieldName() != "addr" && !clockField)
          continue;
        if (!clockField) ++addresses;
        if (!field.getResult().hasOneUse()) { invalid = true; continue; }
        Operation *driver = *field.getResult().getUsers().begin();
        if (!isa<ConnectOp, StrictConnectOp>(driver) ||
            driver->getOperand(0) != field.getResult() ||
            driver->getBlock() != module.getBodyBlock()) {
          invalid = true;
          continue;
        }
        Value rhs = driver->getOperand(1);
        if (clockField) {
          rhs = inputClock(rhs);
          if (!rhs ||
              (rule.targetClock && rule.targetClock != rhs))
            invalid = true;
          rule.targetClock = rhs;
        } else {
          auto rhsType = dyn_cast<UIntType>(rhs.getType());
          if (!rhsType || rhsType.getWidth() <= 0) invalid = true;
        }
      }
      if (addresses != 1 || (mem.getReadLatency() == 1 && clocks != 1))
        invalid = true;
    }
    names.insert(mem.getName());
    memories.push_back(std::move(rule));
  });
  if (!llvm::isPowerOf2_64(threadCount) && !hostClock) invalid = true;
  if (invalid)
    return reject("target-memory threading requires unannotated flat memories with read latency zero/one, one-cycle writes and one lowered address connect per port; non-power-of-two threads need hostClock; synchronous reads additionally need scalar integer data, hostClock and one shared input target clock or canonical AbstractClockGate driven by hostClock; dynamic virtual controllers require explicit Xilinx semantics");
  bool referenced = false;
  auto inspect = [&](auto &&self, Attribute attr) -> void {
    if (auto str = dyn_cast<StringAttr>(attr)) {
      auto spelling = str.getValue();
      if (!spelling.starts_with("~") || !spelling.contains('|') || !spelling.contains('>')) return;
      auto pathAndRef = spelling.split('|').second.split('>');
      auto leaf = pathAndRef.first;
      if (leaf.contains('/')) leaf = leaf.rsplit('/').second.split(':').second;
      auto ref = pathAndRef.second.take_until([](char c) { return c == '.' || c == '['; });
      referenced |= leaf == module.getName() && names.contains(ref);
    } else if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
      for (auto entry : dict) self(self, entry.getValue());
    } else if (auto array = dyn_cast<ArrayAttr>(attr)) {
      for (auto entry : array) self(self, entry);
    }
  };
  if (auto circuit = module->getParentOfType<CircuitOp>())
    if (auto raw = circuit->getAttr("rawAnnotations")) inspect(inspect, raw);
  if (referenced)
    return reject("target-memory threading needs explicit renames for retained memory annotation targets");

  llvm::StringSet<> usedNames;
  for (unsigned p = 0; p < module.getNumPorts(); ++p)
    usedNames.insert(module.getPortName(p));
  module.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"))
      usedNames.insert(name.getValue());
  });
  auto freshName = [&](const Twine &base) {
    std::string name = base.str(), stem = name;
    unsigned suffix = 0;
    while (!usedNames.insert(name).second)
      name = stem + "_" + std::to_string(++suffix);
    return name;
  };
  for (auto &rule : memories) {
    MemOp old = rule.op;
    OpBuilder b(old);
    uint64_t depth = old.getDepth() * threadCount;
    unsigned newWidth = llvm::Log2_64_Ceil(depth);
    SmallVector<Type> types;
    for (Type type : old.getResultTypes()) {
      SmallVector<BundleType::BundleElement> fields;
      for (auto field : cast<BundleType>(type).getElements()) {
        if (field.name.getValue() == "addr") field.type = UIntType::get(module.getContext(), newWidth);
        fields.push_back(field);
      }
      types.push_back(BundleType::get(module.getContext(), fields));
    }
    auto mem = b.create<MemOp>(old.getLoc(), types, old.getReadLatency(), 1, depth, old.getRuw(),
                               old.getPortNames(), old.getName());
    mem->setAttrs(old->getAttrs());
    mem.setDepthAttr(b.getI64IntegerAttr(depth));
    mem->setAttr("goldengate.fame5.threadedMemory", b.getI32IntegerAttr(threadCount));
    SmallVector<Value> bufferedOutputs(mem.getNumResults());
    if (old.getReadLatency() == 1) {
      // Reproduce ImplementThreadedMems exactly. The target edge toggle crosses
      // into host-clock scheduling through a tracker. Buffer writes occur only
      // after a target edge, with thread tags delayed alongside the target data.
      // These helper registers intentionally have no reset, just as in SFC.
      Location loc = old.getLoc();
      auto bit = UIntType::get(module.getContext(), 1);
      Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
      auto field = [&](Value port, StringRef name) -> Value {
        return b.create<SubfieldOp>(loc, port, name);
      };
      auto connect = [&](Value dest, Value src) {
        b.create<StrictConnectOp>(loc, dest, src);
      };
      auto regNext = [&](Value value, StringRef name) -> Value {
        Value reg = b.create<RegOp>(loc, value.getType(), hostClock,
                                   freshName(old.getName() + "_" + name)).getResult();
        connect(reg, value);
        return reg;
      };
      auto pipeline = [&](Value value, unsigned count, StringRef name) -> Value {
        for (unsigned i = 0; i < count; ++i)
          value = regNext(value, name);
        return value;
      };
      Value edgeCount = b.create<RegOp>(loc, bit, rule.targetClock,
                                      freshName(old.getName() + "_edgeCount")).getResult();
      connect(edgeCount, b.create<NotPrimOp>(loc, edgeCount));
      Value tracker = regNext(edgeCount, "edgeCountTracker");
      Value edgeStatus = b.create<XorPrimOp>(loc, tracker, edgeCount);
      unsigned pipeDepth = threadCount < 4 ? 0 : 1;
      unsigned readLatency = threadCount < 4 ? 0 : 1;
      Value lastThread = regNext(threadIndex, "threadLast");
      Value writeThread = pipeline(lastThread, pipeDepth + readLatency + 1, "threadPipe");
      Value writeEnable = pipeline(edgeStatus, pipeDepth, "edgePipe");
      for (auto [portIndex, port] : llvm::enumerate(mem.getResults())) {
        auto kind = mem.getPortKind(portIndex);
        if (kind == MemOp::PortKind::Write) continue;
        Value data = field(port, kind == MemOp::PortKind::Read ? "data" : "rdata");
        Value pipedData = pipeline(data, pipeDepth, "readDataPipe");
        SmallVector<Type> bufferTypes{
            MemOp::getTypeForPort(threadCount, mem.getDataType(), MemOp::PortKind::Read),
            MemOp::getTypeForPort(threadCount, mem.getDataType(), MemOp::PortKind::Write)};
        SmallVector<Attribute> bufferNames{b.getStringAttr("r"), b.getStringAttr("w")};
        auto buffer = b.create<MemOp>(loc, bufferTypes, readLatency, 1, threadCount,
            RUWAttr::Undefined, bufferNames,
            freshName(old.getName() + "_response"));
        buffer->setAttr("goldengate.fame5.responseBuffer", b.getUnitAttr());
        Value read = buffer.getResult(0), write = buffer.getResult(1);
        connect(field(read, "clk"), hostClock);
        connect(field(read, "en"), one);
        connect(field(read, "addr"), lastThread);
        bufferedOutputs[portIndex] = field(read, "data");
        connect(field(write, "clk"), hostClock);
        connect(field(write, "en"), writeEnable);
        connect(field(write, "addr"), writeThread);
        connect(field(write, "mask"), one);
        connect(field(write, "data"), pipedData);
      }
    }
    // Rebuild only first-level port fields. Data/mask subtrees and all their
    // connections keep exactly the original types and scheduling expressions.
    for (auto [portIndex, ports] : llvm::enumerate(llvm::zip(old.getResults(), mem.getResults()))) {
      auto [oldPort, newPort] = ports;
      SmallVector<SubfieldOp> fields;
      for (Operation *user : oldPort.getUsers()) fields.push_back(cast<SubfieldOp>(user));
      for (SubfieldOp field : fields) {
        b.setInsertionPoint(field);
        bool readData = field.getFieldName() == "data" && mem.getPortKind(portIndex) == MemOp::PortKind::Read;
        readData |= field.getFieldName() == "rdata" && mem.getPortKind(portIndex) == MemOp::PortKind::ReadWrite;
        Value replacement = bufferedOutputs[portIndex] && readData
            ? bufferedOutputs[portIndex]
            : b.create<SubfieldOp>(field.getLoc(), newPort, field.getFieldName()).getResult();
        if (field.getFieldName() == "addr") {
          Operation *driver = *field.getResult().getUsers().begin();
          b.setInsertionPoint(driver);
          Value address = driver->getOperand(1);
          unsigned width = cast<UIntType>(field.getType()).getWidth().value();
          unsigned rhsWidth = cast<UIntType>(address.getType()).getWidth().value();
          if (rhsWidth < width) address = b.create<PadPrimOp>(driver->getLoc(), address, width);
          if (width && rhsWidth > width) address = b.create<BitsPrimOp>(driver->getLoc(), address, width - 1, 0);
          Value translated;
          if (llvm::isPowerOf2_64(threadCount)) {
            translated = threadIndex;
            if (width) translated = b.create<CatPrimOp>(driver->getLoc(), address, threadIndex);
          } else {
            // ImplementThreadedMems gives each accessor its own free-running
            // base counter for non-power-of-two schedules. Preserve that
            // unreset helper and its host-clock phase rather than replacing it
            // with threadIndex*depth (which would invent a startup relation).
            unsigned baseWidth = llvm::Log2_64_Ceil((threadCount - 1) * old.getDepth() + 1);
            auto baseType = UIntType::get(module.getContext(), baseWidth);
            auto baseName = freshName(old.getName() + "_base_addr_counter");
            Value resetBase = b.create<ConstantOp>(driver->getLoc(), baseType, APInt(baseWidth, 0));
            Value base = semantics == FAME5MemoryClockSemantics::XilinxBUFGCE
                ? b.create<RegResetOp>(driver->getLoc(), baseType, hostClock,
                                      hostReset, resetBase, baseName).getResult()
                : b.create<RegOp>(driver->getLoc(), baseType, hostClock, baseName).getResult();
            Value maximum = b.create<ConstantOp>(driver->getLoc(), baseType,
                APInt(baseWidth, (threadCount - 1) * old.getDepth()));
            Value step = b.create<ConstantOp>(driver->getLoc(), baseType,
                APInt(baseWidth, old.getDepth()));
            Value zero = b.create<ConstantOp>(driver->getLoc(), baseType, APInt(baseWidth, 0));
            Value increment = b.create<AddPrimOp>(driver->getLoc(), base, step);
            increment = b.create<BitsPrimOp>(driver->getLoc(), increment, baseWidth - 1, 0);
            Value next = b.create<MuxPrimOp>(driver->getLoc(),
                b.create<GEQPrimOp>(driver->getLoc(), base, maximum), zero, increment);
            b.create<ConnectOp>(driver->getLoc(), base, next);
            translated = b.create<AddPrimOp>(driver->getLoc(), base, address);
          }
          // Match FIRRTL's destination-width truncation, including the dummy
          // one-bit original address on a depth-one power-of-two memory.
          if (cast<UIntType>(translated.getType()).getWidth() > newWidth)
            translated = b.create<BitsPrimOp>(driver->getLoc(), translated, newWidth - 1, 0);
          driver->setOperand(1, translated);
        }
        field.getResult().replaceAllUsesWith(replacement);
        field.erase();
      }
    }
    old.erase();
  }
  module->setAttr("goldengate.fame5.memoryThreadCount", IntegerAttr::get(IntegerType::get(module.getContext(), 32), threadCount));
  return success();
}
