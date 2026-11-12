// See LICENSE for license details.
#include "goldengate/FAME5Models.h"
#include "goldengate/FAME5ThreadedState.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Support/Timing.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <sstream>
#include <regex>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
std::string text(Operation *op) { std::string result; llvm::raw_string_ostream stream(result); op->print(stream); return result; }
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  return {};
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, StringRef path, unsigned threads) {
  auto buffer = llvm::MemoryBuffer::getFile(path); require(bool(buffer), "missing fixture");
  std::istringstream input(buffer.get()->getBuffer().str()); std::string line, source;
  while (std::getline(input, line)) {
    bool remove = false;
    for (unsigned i = threads; i < 4; ++i)
      remove |= line.find("in" + std::to_string(i)) != std::string::npos || line.find("out" + std::to_string(i)) != std::string::npos || line.find("worker" + std::to_string(i)) != std::string::npos;
    if (!remove) source += line + "\n";
    if (threads > 4 && std::regex_search(line, std::regex("\\b(in|out|worker)3\\b")))
      for (unsigned i = 4; i < threads; ++i) {
        auto clone = line;
        for (auto prefix : {"in", "out", "worker"})
          clone = std::regex_replace(clone, std::regex("\\b" + std::string(prefix) + "3\\b"), std::string(prefix) + std::to_string(i));
        source += clone + "\n";
      }
  }
  llvm::SourceMgr manager; manager.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(source), llvm::SMLoc());
  DefaultTimingManager timer; auto scope = timer.getRootScope();
  auto root = importFIRFile(manager, &context, scope, FIRParserOptions());
  require(bool(root), "fixture import failed");
  Builder b(&context); auto circuit = *root->getOps<CircuitOp>().begin();
  circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::EnableModelMultiThreading)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top/worker0:Worker"))})}));
  return root;
}
uint64_t mask(unsigned bits) { return bits == 64 ? ~uint64_t(0) : (uint64_t(1) << bits) - 1; }
uint64_t evaluate(Value value, const llvm::DenseMap<Value, uint64_t> &inputs) {
  if (auto it = inputs.find(value); it != inputs.end()) return it->second;
  if (auto c = value.getDefiningOp<ConstantOp>()) return c.getValue().getZExtValue();
  if (auto c = value.getDefiningOp<SubfieldOp>()) {
    auto bundle = cast<BundleType>(c.getInput().getType()); unsigned shift = 0;
    for (auto field : bundle.getElements()) {
      unsigned bits = *getBitWidth(field.type);
      if (field.name == c.getFieldName()) return (evaluate(c.getInput(), inputs) >> shift) & mask(bits);
      shift += bits;
    }
  }
  if (auto c = value.getDefiningOp<MuxPrimOp>()) return evaluate(evaluate(c.getSel(), inputs) ? c.getHigh() : c.getLow(), inputs);
  if (auto c = value.getDefiningOp<EQPrimOp>()) return evaluate(c.getLhs(), inputs) == evaluate(c.getRhs(), inputs);
  if (auto c = value.getDefiningOp<AndPrimOp>()) return evaluate(c.getLhs(), inputs) & evaluate(c.getRhs(), inputs);
  if (auto c = value.getDefiningOp<NotPrimOp>()) return (~evaluate(c.getInput(), inputs)) & mask(*getBitWidth(c.getResult().getType()));
  if (auto c = value.getDefiningOp<AddPrimOp>()) return evaluate(c.getLhs(), inputs) + evaluate(c.getRhs(), inputs);
  if (auto c = value.getDefiningOp<BitsPrimOp>()) return (evaluate(c.getInput(), inputs) >> c.getLo()) & mask(c.getHi() - c.getLo() + 1);
  throw std::runtime_error("unsupported scheduler expression");
}
Value argument(FModuleOp module, StringRef name) {
  for (unsigned p = 0; p < module.getNumPorts(); ++p) if (module.getPortName(p) == name) return module.getArgument(p);
  throw std::runtime_error("missing named port");
}
Value drive(FModuleOp top, Value root, StringRef field) {
  for (auto &op : *top.getBodyBlock()) {
    if (!isa<ConnectOp, StrictConnectOp>(op)) continue;
    if (auto dest = op.getOperand(0).getDefiningOp<SubfieldOp>(); dest && dest.getInput() == root && dest.getFieldName() == field)
      return op.getOperand(1);
  }
  throw std::runtime_error("missing channel field driver");
}
void behavior(MLIRContext &context, StringRef path, unsigned threads) {
  auto root = fixture(context, path, threads); auto circuit = *root->getOps<CircuitOp>().begin();
  std::string original = text(named(circuit, "Worker")); auto raw = circuit->getAttr("rawAnnotations");
  std::vector<unsigned> declarationOrder;
  for (auto instance : named(circuit, "Top").getOps<InstanceOp>()) if (instance.getModuleName() == "Worker") {
    unsigned member = 0;
    require(!instance.getName().drop_front(6).getAsInteger(10, member), "bad fixture member name");
    declarationOrder.push_back(member);
  }
  require(declarationOrder.size() == threads, "fixture thread count differs");
  unsigned groups = 999; std::string error;
  auto result = goldengate::multiThreadFAME5Models(circuit, groups, error);
  if (failed(result)) llvm::errs() << "FAME5 rejection: " << error << '\n' << text(circuit) << '\n';
  require(succeeded(result), error.c_str());
  require(groups == 1 && succeeded(verify(*root)), "grouped IR is invalid");
  auto top = named(circuit, "Top"), model = named(circuit, "Worker_threaded");
  require(bool(model) && text(named(circuit, "Worker")) == original && circuit->getAttr("rawAnnotations") == raw, "original definition or selection provenance changed");
  auto shared = *top.getOps<InstanceOp>().begin();
  require(std::distance(top.getOps<InstanceOp>().begin(), top.getOps<InstanceOp>().end()) == 1 && shared.getModuleName() == model.getName(), "repeated hardware was not shared");
  auto members = shared->getAttrOfType<ArrayAttr>("goldengate.fame5.members");
  require(members && members.size() == threads && cast<StringAttr>(members[0]).getValue() == "worker1" && cast<StringAttr>(members[1]).getValue() == "worker0", "declaration order changed");
  auto allocation = circuit->getAttrOfType<ArrayAttr>("goldengate.fame5.groups");
  require(allocation && allocation.size() == 1, "physical threading allocation missing");
  auto group = cast<DictionaryAttr>(allocation[0]);
  require(group.getAs<IntegerAttr>("threads").getInt() == threads &&
          group.getAs<StringAttr>("sharedInstance").getValue() == shared.getName() &&
          group.getAs<ArrayAttr>("members") == members &&
          !group.getAs<BoolAttr>("localSelectorReset").getValue() &&
          group.getAs<StringAttr>("arbiterSelectorReset").getValue() == "hostReset",
          "allocation thread count, identity or startup contract differs");
  auto allocationChannels = group.getAs<ArrayAttr>("channels");
  require(allocationChannels && allocationChannels.size() == 2,
          "allocation lost a model channel");
  for (auto entry : allocationChannels) {
    auto channel = cast<DictionaryAttr>(entry);
    auto peers = channel.getAs<ArrayAttr>("threadPorts");
    auto prefix = channel.getAs<StringAttr>("direction").getValue() == "input" ? "in" : "out";
    require(peers.size() == threads && cast<StringAttr>(peers[0]).getValue() == std::string(prefix)+"1" &&
            cast<StringAttr>(peers[1]).getValue() == std::string(prefix)+"0",
            "physical channel allocation changed thread order");
  }
  auto bank = *model.getOps<MemOp>().begin();
  require(bank.getDepth() == threads && bank.getReadLatency() == 0 && bank.getWriteLatency() == 1, "per-thread register bank geometry changed");
  auto count = *top.getOps<RegResetOp>().begin(); auto local = *model.getOps<RegOp>().begin();
  require(count.getResetSignal() == argument(top, "hostReset") && count.getResetValue().getDefiningOp<ConstantOp>().getValue().isZero(), "top counter reset changed");
  unsigned inPort = 0, outPort = 0;
  for (unsigned p = 0; p < model.getNumPorts(); ++p) { if (model.getPortName(p) == "in") inPort = p; if (model.getPortName(p) == "out") outPort = p; }
  Value in = shared.getResult(inPort), out = shared.getResult(outPort);
  unsigned observations = 0;
  for (unsigned slot = 0; slot < threads; ++slot)
    for (unsigned pattern = 0; pattern < (threads <= 4 ? (1u << (threads * 2)) : 16u); ++pattern) {
      llvm::DenseMap<Value, uint64_t> inputs{{count.getResult(), slot}, {in, pattern & 1}, {out, ((pattern & 2) ? 2 : 0) | (uint64_t(0x12345) << 2)}};
      for (unsigned i = 0; i < threads; ++i) {
        unsigned member = declarationOrder[i];
        inputs[argument(top, "in" + std::to_string(member))] = ((threads <= 4 ? ((pattern >> i) & 1) : unsigned((pattern * 17 + i * 7 + i / 5) % 11 < 6)) << 1) | ((uint64_t(17 + member * 19) & mask(*getBitWidth(cast<BundleType>(in.getType()).getElement("bits")->type))) << 2);
        inputs[argument(top, "out" + std::to_string(member))] = threads <= 4 ? (pattern >> (i + threads)) & 1 : unsigned((pattern * 31 + i * 13 + i / 3) % 7 < 4);
      }
      unsigned member = declarationOrder[slot];
      auto selectedIn = inputs[argument(top, "in" + std::to_string(member))];
      auto gotValid = evaluate(drive(top, in, "valid"), inputs), gotBits = evaluate(drive(top, in, "bits"), inputs);
      if (gotValid != ((selectedIn >> 1) & 1) || gotBits != (selectedIn >> 2))
        llvm::errs() << "threads=" << threads << " slot=" << slot << " member=" << member << " pattern=" << pattern << " got=" << gotValid << "/" << gotBits << " expected=" << ((selectedIn >> 1) & 1) << "/" << (selectedIn >> 2) << "\n";
      require(gotValid == ((selectedIn >> 1) & 1) && gotBits == (selectedIn >> 2), "input channel did not select scheduled thread");
      require(evaluate(drive(top, out, "ready"), inputs) == inputs[argument(top, "out" + std::to_string(member))], "output ready did not select scheduled thread");
      for (unsigned i = 0; i < threads; ++i) {
        unsigned m = declarationOrder[i];
        require(evaluate(drive(top, argument(top, "in" + std::to_string(m)), "ready"), inputs) == ((i == slot) && (pattern & 1)), "ready leaked to another thread");
        require(evaluate(drive(top, argument(top, "out" + std::to_string(m)), "valid"), inputs) == ((i == slot) && (pattern & 2)), "valid leaked to another thread");
        require(evaluate(drive(top, argument(top, "out" + std::to_string(m)), "bits"), inputs) == 0x12345, "demux payload broadcast changed");
        ++observations;
      }
    }
  for (unsigned slot = 0; slot < threads; ++slot) {
    for (auto module : {top, model}) {
      Value index = module == top ? count.getResult() : local.getResult();
      Value next;
      for (auto &op : *module.getBodyBlock()) if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == index) next = op.getOperand(1);
      require(bool(next) && evaluate(next, {{index, slot}}) == (slot + 1) % threads, "counter does not rotate independently each host cycle");
    }
  }
  std::string before = text(circuit); groups = 777;
  require(failed(goldengate::multiThreadFAME5Models(circuit, groups, error)) && text(circuit) == before && groups == 777, "repeated grouping was not atomic");
  llvm::outs() << "Passed " << threads << "-thread grouped state, canonical order, free-running counters and " << observations << (threads <= 4 ? " exhaustive channel observations" : " directed channel observations") << "\n";
}
void synchronousMemories(MLIRContext &context, StringRef path) {
  for (unsigned threads : {2u, 4u}) {
    auto root = fixture(context, path, threads); auto circuit = *root->getOps<CircuitOp>().begin();
    std::string original = text(named(circuit, "Worker")), error; unsigned groups = 777;
    auto result = goldengate::multiThreadFAME5Models(circuit, groups, error);
    if (failed(result)) llvm::errs() << text(circuit) << '\n';
    require(succeeded(result), error.c_str());
    require(groups == 1 && succeeded(verify(*root)), "grouped synchronous IR invalid");
    auto model = named(circuit, "Worker_threaded"), top = named(circuit, "Top");
    require(text(named(circuit, "Worker")) == original, "grouped memory changed original model");
    require(std::distance(top.getOps<InstanceOp>().begin(), top.getOps<InstanceOp>().end()) == 1,
            "grouped synchronous worker hardware not shared");
    unsigned targets = 0, responses = 0, states = 0, gates = 0;
    model.walk([&](InstanceOp op) { gates += goldengate::isFAME5ClockGate(op); });
    model.walk([&](MemOp memory) {
      if (memory->hasAttr("goldengate.fame5.threadedMemory")) {
        ++targets;
        require(memory.getDepth() == 3 * threads && memory.getReadLatency() == 1 &&
                memory.getWriteLatency() == 1 && memory.getRuw() == RUWAttr::Old,
                "grouped target geometry/latency/RUW changed");
      } else if (memory->hasAttr("goldengate.fame5.responseBuffer")) {
        ++responses;
        require(memory.getDepth() == threads && memory.getReadLatency() == (threads < 4 ? 0 : 1),
                "grouped response bank geometry/latency changed");
      } else if (memory->hasAttr("goldengate.fame5.registerState")) ++states;
    });
    require(targets == 1 && responses == 1 && states == 1 && gates == 1,
            "grouped synchronous worker lost target/response/state/gated-clock integration");
    llvm::outs() << "Passed " << threads << "-thread shared synchronous target memory, response bank, register state and always-enabled clock gate\n";
  }
  for (unsigned test = 0; test < 5; ++test) {
    auto root = fixture(context, path, 4); auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = named(circuit, "Worker"); auto gate = *model.getOps<InstanceOp>().begin();
    OpBuilder b(gate); auto bit = UIntType::get(&context, 1);
    if (test == 0) gate.setModuleName("Worker");
    if (test == 1) {
      for (auto &op : *model.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == gate.getResult(0))
          op.setOperand(1, gate.getResult(2));
    }
    if (test == 2) b.create<ConnectOp>(gate.getLoc(), gate.getResult(1), b.create<ConstantOp>(gate.getLoc(), bit, APInt(1, 1)));
    if (test == 4) {
      for (auto &op : *model.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == gate.getResult(1)) {
          auto valid = b.create<SubfieldOp>(gate.getLoc(), argument(model, "in"), "valid");
          op.setOperand(1, valid.getResult());
        }
    }
    if (test == 3) gate->setAttr("annotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown"))})}));
    std::string before = text(circuit), error; unsigned groups = 777;
    require(failed(goldengate::multiThreadFAME5Models(circuit, groups, error)) &&
            before == text(circuit) && groups == 777, "unsafe grouped target clock did not reject atomically");
  }
  llvm::outs() << "Passed five atomic grouped generated-clock rejection cases\n";
}
void dynamicClocks(MLIRContext &context, StringRef path) {
  for (unsigned threads : {2u, 4u}) for (bool broadcast : {false, true}) {
    auto root = fixture(context, path, threads); auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = named(circuit, "Worker"); auto gate = *model.getOps<InstanceOp>().begin();
    require(goldengate::isFAME5VirtualClockController(gate) && !goldengate::isFAME5ClockGate(gate),
            "complete SFC virtual FAME controller was not classified separately");
    std::string original = text(circuit), error; unsigned groups = 777;
    auto result = goldengate::multiThreadFAME5Models(circuit, groups, error);
    require(failed(result) && original == text(circuit) && groups == 777 &&
            error.find("phase correspondence is unsupported") != std::string::npos,
            "unproved dynamic grouping did not reject atomically with phase diagnostic");
    // The production Xilinx primitive is an explicit, separately verified
    // platform choice. Preserve the abstract counterexample/guard above.
    auto xilinxRoot = fixture(context, path, threads);
    auto xilinxCircuit = *xilinxRoot->getOps<CircuitOp>().begin();
    auto raw = xilinxCircuit->getAttr("rawAnnotations");
    groups = 777;
    result = goldengate::multiThreadFAME5Models(xilinxCircuit, groups, error,
        goldengate::FAME5ClockSemantics::XilinxBUFGCE);
    require(succeeded(result) && groups == 1 && succeeded(verify(*xilinxRoot)),
            "explicit Xilinx virtual grouping failed");
    require(xilinxCircuit->getAttr("rawAnnotations") == raw,
            "Xilinx grouping changed selector provenance");
    auto xilinxModel = named(xilinxCircuit, "Worker_threaded");
    unsigned xilinxGates = 0, xilinxStates = 0, xilinxTargets = 0;
    for (auto instance : xilinxModel.getOps<InstanceOp>()) {
      require(instance.getModuleName() == "BUFGCE", "Xilinx grouping retained abstract gate");
      ++xilinxGates;
      for (auto &op : *xilinxModel.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op))
          if (auto field = op.getOperand(0).getDefiningOp<SubfieldOp>();
              field && field.getFieldName() == "clk" &&
              op.getOperand(1) == instance.getResult(2)) ++xilinxTargets;
    }
    for (auto memory : xilinxModel.getOps<MemOp>()) {
      require(memory->hasAttr("goldengate.fame5.registerState") &&
              memory.getDepth() == threads, "Xilinx register-bank geometry changed");
      ++xilinxStates;
      if (memory.getNumResults() > 2) {
        require(memory.getNumResults() == threads + 2, "host-reset bank writers missing");
        auto selector = *xilinxModel.getOps<RegResetOp>().begin();
        for (unsigned resetting = 0; resetting < 2; ++resetting)
          for (unsigned slot = 0; slot < threads; ++slot) {
            llvm::DenseMap<Value, uint64_t> values{{argument(xilinxModel, "hostReset"), resetting},
                                                  {selector.getResult(), slot}};
            SmallVector<unsigned> writes(threads, 0);
            for (unsigned p = 1; p < memory.getNumResults(); ++p)
              if (evaluate(drive(xilinxModel, memory.getResult(p), "en"), values))
                ++writes[evaluate(drive(xilinxModel, memory.getResult(p), "addr"), values)];
            for (unsigned bank = 0; bank < threads; ++bank)
              require(writes[bank] == unsigned(resetting || slot == bank),
                      "host-control reset does not cover every bank exactly once or normal writes collide");
          }
      }
    }
    require(xilinxGates == 1 && xilinxStates == 4 && xilinxTargets == 2,
            "Xilinx grouped target/host state clock ownership changed");
    Builder builder(&context); unsigned indexPort = model.getNumPorts();
    auto type = UIntType::get(&context, threads == 2 ? 1 : 2);
    model.insertPorts({{indexPort, PortInfo(builder.getStringAttr("thread"), type, Direction::In)}});
    result = goldengate::threadFAME5Registers(model, threads, model.getArgument(indexPort), error, broadcast);
    require(succeeded(result), error.c_str());
    auto threaded = model;
    auto threadedGate = *threaded.getOps<InstanceOp>().begin();
    require(goldengate::isFAME5VirtualClockController(threadedGate) &&
            !goldengate::isFAME5ClockGate(threadedGate),
            "banked virtual FAME controller was not classified separately");
    unsigned states = 0, targetStates = 0;
    threaded.walk([&](MemOp memory) {
      if (!memory->hasAttr("goldengate.fame5.registerState")) return;
      ++states; require(memory.getDepth() == threads, "virtual FAME state bank depth changed");
      for (auto &op : *threaded.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op))
          if (auto field = op.getOperand(0).getDefiningOp<SubfieldOp>();
              field && field.getInput() == memory.getResult(1) && field.getFieldName() == "clk" &&
              op.getOperand(1) == threadedGate.getResult(2)) ++targetStates;
    });
    require(states == 4 && targetStates == 1, "virtual FAME host control and target state ownership changed");
    // A generated register-bank marker alone cannot authorize a gate. Mutating
    // its host reset relation or write enable must invalidate recognition.
    unsigned mutated = 0;
    threaded.walk([&](MemOp memory) {
      if (memory.getName() != "clock_enabled") return;
      if (broadcast) {
        bool checked = false;
        for (auto &op : *threaded.getBodyBlock())
          if (isa<ConnectOp, StrictConnectOp>(op))
            if (auto field = op.getOperand(0).getDefiningOp<SubfieldOp>();
                field && field.getInput() == memory.getResult(2) && field.getFieldName() == "data") {
              Value original = op.getOperand(1); OpBuilder b(&op);
              op.setOperand(1, b.create<ConstantOp>(op.getLoc(), UIntType::get(&context, 1), APInt(1, 1)));
              require(!goldengate::isFAME5VirtualClockController(threadedGate),
                      "corrupt broadcast host-control reset was trusted");
              op.setOperand(1, original); checked = true; break;
            }
        require(checked && goldengate::isFAME5VirtualClockController(threadedGate),
                "broadcast host-control negative check did not restore valid controller");
      }
      for (auto &op : *threaded.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op))
          if (auto field = op.getOperand(0).getDefiningOp<SubfieldOp>();
              field && field.getInput() == memory.getResult(1) && field.getFieldName() == "en") {
            OpBuilder b(&op); op.setOperand(1, b.create<ConstantOp>(op.getLoc(), UIntType::get(&context,1), APInt(1,0)));
            ++mutated;
          }
    });
    require(mutated == 1 && !goldengate::isFAME5VirtualClockController(threadedGate), "mutated clock-enabled bank was trusted");
    llvm::outs() << "Passed " << threads << "-thread virtual controller preflight, host/target bank validation and atomic phase rejection\n";
  }
  for (unsigned test = 0; test < 6; ++test) {
    auto root = fixture(context, path, 4); auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = named(circuit, "Worker"); auto gate = *model.getOps<InstanceOp>().begin();
    OpBuilder b(gate); auto bit = UIntType::get(&context,1);
    if (test == 0) {
      auto reg = *model.getOps<RegResetOp>().begin();
      reg->setOperand(2, b.create<ConstantOp>(gate.getLoc(), bit, APInt(1,1)));
    }
    if (test == 1) {
      for (auto &op : *model.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == gate.getResult(1))
          op.setOperand(1, b.create<SubfieldOp>(gate.getLoc(), argument(model,"input_sink"), "valid"));
    }
    if (test == 2) {
      for (auto &op : *model.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == gate.getResult(1)) {
          auto outer = op.getOperand(1).getDefiningOp<AndPrimOp>();
          op.setOperand(1, outer.getLhs());
        }
    }
    if (test == 3) {
      auto reg = *model.getOps<RegResetOp>().begin();
      for (auto &op : *model.getBodyBlock())
        if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == reg.getResult())
          op.setOperand(1, b.create<ConstantOp>(gate.getLoc(), bit, APInt(1,1)));
    }
    if (test == 4) {
      auto reg = *model.getOps<RegResetOp>().begin(); reg->setOperand(0, gate.getResult(2));
    }
    if (test == 5) {
      auto reg = *model.getOps<RegResetOp>().begin();
      b.create<ConnectOp>(gate.getLoc(), reg.getResult(), b.create<ConstantOp>(gate.getLoc(), bit, APInt(1,0)));
    }
    std::string before=text(circuit), error; unsigned groups=777;
    require(!goldengate::isFAME5VirtualClockController(gate) &&
            failed(goldengate::multiThreadFAME5Models(circuit,groups,error)) &&
            before==text(circuit) && groups==777, "malformed virtual FAME clock did not reject atomically");
    require(failed(goldengate::multiThreadFAME5Models(circuit,groups,error,
                goldengate::FAME5ClockSemantics::XilinxBUFGCE)) &&
            before==text(circuit) && groups==777,
            "explicit Xilinx mode trusted malformed virtual controller");
  }
  {
    auto root = fixture(context, path, 2); auto circuit = *root->getOps<CircuitOp>().begin();
    auto gate = *circuit.getOps<FExtModuleOp>().begin();
    auto *collision = gate->clone(); SymbolTable::setSymbolName(collision, "BUFGCE");
    circuit.getBodyBlock()->push_back(collision);
    std::string before=text(circuit), error; unsigned groups=777;
    require(failed(goldengate::multiThreadFAME5Models(circuit,groups,error,
                goldengate::FAME5ClockSemantics::XilinxBUFGCE)) &&
            before==text(circuit) && groups==777 && error.find("unused BUFGCE")!=std::string::npos,
            "Xilinx platform symbol collision was not atomic");
  }
  llvm::outs() << "Passed explicit Xilinx 2/4-thread virtual grouping, six malformed-controller and symbol-collision rejections\n";
}
void dynamicMemories(MLIRContext &context, StringRef path) {
  for (unsigned threads : {2u, 4u}) {
    auto root = fixture(context, path, threads);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto raw = circuit->getAttr("rawAnnotations");
    unsigned groups = 777; std::string error;
    require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error,
                goldengate::FAME5ClockSemantics::XilinxBUFGCE)) &&
            groups == 1 && succeeded(verify(*root)),
            "dynamic asynchronous-memory grouping failed");
    auto sourceMemory = *named(circuit, "Worker").getOps<MemOp>().begin();
    require(sourceMemory.getDepth() == 4 && sourceMemory.getReadLatency() == 0 &&
            raw == circuit->getAttr("rawAnnotations"),
            "dynamic memory grouping changed unthreaded geometry or annotations");
    auto model = named(circuit, "Worker_threaded");
    unsigned targets = 0, controls = 0, gates = 0;
    for (auto mem : model.getOps<MemOp>()) {
      if (mem->hasAttr("goldengate.fame5.threadedMemory")) {
        ++targets;
        require(mem.getDepth() == 4 * threads && mem.getReadLatency() == 0 &&
                mem.getWriteLatency() == 1 && mem.getRuw() == RUWAttr::Old,
                "dynamic memory geometry, latency or RUW changed");
      } else if (mem->hasAttr("goldengate.fame5.registerState")) ++controls;
      else require(false, "unexpected dynamic response memory");
    }
    for (auto gate : model.getOps<InstanceOp>()) {
      require(gate.getModuleName() == "BUFGCE", "dynamic memory gate was not specialized");
      ++gates;
    }
    require(targets == 1 && controls == 3 && gates == 1,
            "dynamic memory group lost target storage or host controls");
    // The synchronous path retains the same virtual controller and banks the
    // response under SFC's separate host-clock edge/tag pipelines.
    auto synchronous = fixture(context, path, threads);
    auto syncCircuit = *synchronous->getOps<CircuitOp>().begin();
    auto target = *named(syncCircuit, "Worker").getOps<MemOp>().begin();
    target.setReadLatencyAttr(IntegerAttr::get(IntegerType::get(&context, 32), 1));
    groups = 777;
    require(succeeded(goldengate::multiThreadFAME5Models(syncCircuit, groups, error,
                goldengate::FAME5ClockSemantics::XilinxBUFGCE)) &&
            groups == 1 && succeeded(verify(*synchronous)),
            "dynamic synchronous memory grouping failed");
    unsigned responses = 0, syncTargets = 0;
    for (auto mem : named(syncCircuit, "Worker_threaded").getOps<MemOp>()) {
      if (mem->hasAttr("goldengate.fame5.threadedMemory")) {
        ++syncTargets;
        require(mem.getDepth() == 4 * threads && mem.getReadLatency() == 1 &&
                mem.getRuw() == RUWAttr::Old, "dynamic synchronous target geometry changed");
      } else if (!mem->hasAttr("goldengate.fame5.registerState")) {
        ++responses;
        require(mem.getDepth() == threads && mem.getReadLatency() == (threads < 4 ? 0 : 1),
                "dynamic synchronous response bank geometry changed");
      }
    }
    require(responses == 1 && syncTargets == 1, "dynamic response bank missing");
    llvm::outs() << "Passed " << threads << "-thread dynamic asynchronous/synchronous target memory and host response banks\n";
  }
}
void hierarchy(MLIRContext &context, StringRef path, bool synchronous = false) {
  for (unsigned threads : {2u, 3u, 4u}) {
    auto root = fixture(context, path, threads);
    auto circuit = *root->getOps<CircuitOp>().begin();
    std::map<std::string, std::string> originals;
    for (StringRef name : {"Worker", "Branch", "Leaf"}) originals[name.str()] = text(named(circuit, name));
    auto raw = circuit->getAttr("rawAnnotations");
    unsigned groups = 777; std::string error;
    require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error)) &&
            groups == 1 && succeeded(verify(*root)), "recursive FAME5 grouping failed");
    for (StringRef name : {"Worker", "Branch", "Leaf"}) {
      require(text(named(circuit, name)) == originals.at(name.str()), "recursive grouping modified source definition");
      auto clone = named(circuit, name.str() + "_threaded");
      require(bool(clone), "recursive threaded definition missing");
      require(argument(clone, "hostClock").getType() == ClockType::get(&context) &&
              argument(clone, "hostReset").getType() == UIntType::get(&context, 1),
              "child host ABI missing");
      require(std::distance(clone.getOps<RegOp>().begin(), clone.getOps<RegOp>().end()) ==
                  (1 + (name == "Leaf" ? (threads == 3 ? 2 : 0) +
                        (synchronous ? (threads < 4 ? 4 : 8) : 0) : 0)),
              "child selector, accessor base or synchronous response helpers missing");
      unsigned states = 0, targets = 0;
      for (auto memory : clone.getOps<MemOp>()) {
        if (memory->hasAttr("goldengate.fame5.registerState")) {
          ++states; require(memory.getDepth() == threads, "child state banking depth changed");
        } else if (memory->hasAttr("goldengate.fame5.threadedMemory")) {
          ++targets; require(name == "Leaf" && memory.getDepth() == 3 * threads &&
                    memory.getReadLatency() == (synchronous ? 1 : 0) && memory.getRuw() == RUWAttr::Old,
                    "child target memory geometry changed");
        }
      }
      require(states == 1 && targets == (name == "Leaf" ? 1u : 0u), "recursive state or memory was lost");
      for (auto instance : clone.getOps<InstanceOp>()) {
        require(instance.getModuleName() == (name == "Worker" ? "Branch_threaded" : "Leaf_threaded"),
                "child did not bind threaded definition");
        for (StringRef host : {"hostClock", "hostReset"}) {
          Value port;
          for (unsigned i = 0; i < instance.getNumResults(); ++i)
            if (instance.getPortNameStr(i) == host) port = instance.getResult(i);
          require(port && port.hasOneUse(), "child host port not wired exactly once");
          auto connection = *port.getUsers().begin();
          require(isa<ConnectOp, StrictConnectOp>(connection) && connection->getOperand(0) == port &&
                  connection->getOperand(1) == argument(clone, host), "child host wiring changed");
        }
      }
    }
    auto worker = named(circuit, "Worker_threaded");
    require(std::distance(worker.getOps<InstanceOp>().begin(), worker.getOps<InstanceOp>().end()) == 2,
            "distinct sibling hardware instances were collapsed");
    require(raw == circuit->getAttr("rawAnnotations"), "hierarchy selection annotation changed");
    llvm::outs() << "Passed " << threads << "-thread recursive definitions, sibling instance isolation, host wiring and register/target-memory banks\n";
  }
  for (unsigned test = 0; test < 5; ++test) {
    auto root = fixture(context, path, 4);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto branch = named(circuit, "Branch");
    auto child = *branch.getOps<InstanceOp>().begin();
    Builder b(&context);
    if (test == 0) child.setModuleName("Worker"); // recursive graph
    if (test == 1) child.setModuleName("UnknownExternal");
    if (test == 2) child->setAttr("annotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown"))})}));
    if (test == 3 || test == 4) {
      SmallVector<Attribute> annotations(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
      annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown")),
          b.getNamedAttr("target", b.getStringAttr(test == 3 ? "~Top|Leaf>state" : "~Top|Worker/left:Branch/leaf:Leaf>state"))}));
      circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    }
    auto before = text(circuit); unsigned groups = 777; std::string error;
    require(failed(goldengate::multiThreadFAME5Models(circuit, groups, error)) && groups == 777 &&
            before == text(circuit), "unsafe recursive hierarchy did not reject atomically");
  }
  {
    auto root = fixture(context, path, 3); auto circuit = *root->getOps<CircuitOp>().begin();
    unsigned groups = 777; std::string error;
    require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error,
                goldengate::FAME5ClockSemantics::XilinxBUFGCE)) &&
            groups == 1 && succeeded(verify(*root)), error.c_str());
    auto leaf = named(circuit, "Leaf_threaded");
    unsigned resetSchedulers = 0;
    for (auto reg : leaf.getOps<RegResetOp>())
      if (reg.getResetSignal() == argument(leaf, "hostReset")) ++resetSchedulers;
    require(resetSchedulers == 3, "production three-thread leaf selector/accessor bases lack aligned host reset");
  }
  llvm::outs() << "Passed five atomic recursive hierarchy rejection cases and three-thread production accessor alignment\n";
}
void consumedProvenance(MLIRContext &context, StringRef path) {
  auto root = fixture(context, path, 2); auto circuit = *root->getOps<CircuitOp>().begin();
  Builder b(&context);
  SmallVector<Attribute> annotations(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::FAMETransform)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Worker"))}));
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts)),
      b.getNamedAttr("localName", b.getStringAttr("in")),
      b.getNamedAttr("ports", b.getArrayAttr({b.getStringAttr("~Top|Worker>in.bits")}))}));
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firrtl.transforms.DedupGroupAnnotation")),
      b.getNamedAttr("target", b.getStringAttr("~Top|Worker")),
      b.getNamedAttr("group", b.getStringAttr("model"))}));
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::MemModel)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Worker>state"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto raw = circuit->getAttr("rawAnnotations"); unsigned groups=777; std::string error;
  require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error)) &&
          groups==1 && raw==circuit->getAttr("rawAnnotations") && succeeded(verify(*root)),
          "consumed FAME source provenance blocked production grouping or changed");
  llvm::outs() << "Passed consumed FAMETransform/channel provenance retention for production grouping\n";
}
void productionGroupsAndSources(MLIRContext &context, StringRef path) {
  for (bool production : {false, true}) {
    auto root = fixture(context, path, 4); auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = named(circuit, "Worker"), top = named(circuit, "Top");
    auto alternate = cast<FModuleOp>(model->clone());
    SymbolTable::setSymbolName(alternate, "Alternate");
    circuit.getBodyBlock()->push_back(alternate);
    unsigned reassigned = 0;
    for (auto instance : top.getOps<InstanceOp>())
      if (reassigned++ < 2) instance->setAttr("moduleName", FlatSymbolRefAttr::get(&context, "Alternate"));
    Builder b(&context);
    circuit->setAttr("rawAnnotations", b.getArrayAttr({
      b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::EnableModelMultiThreading)),
                          b.getNamedAttr("target", b.getStringAttr("~Top|Top/worker3:Worker"))}),
      b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::EnableModelMultiThreading)),
                          b.getNamedAttr("target", b.getStringAttr("~Top|Top/worker1:Alternate"))})}));
    unsigned groups = 777; std::string error;
    require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error,
                production ? goldengate::FAME5ClockSemantics::XilinxBUFGCE
                           : goldengate::FAME5ClockSemantics::Abstract)), error.c_str());
    require(groups == 2 && succeeded(verify(*root)), "two independent groups did not compile");
    auto allocations = circuit->getAttrOfType<ArrayAttr>("goldengate.fame5.groups");
    require(allocations.size() == 2, "multi-group allocation was overwritten");
    for (auto allocation : allocations) {
      auto group = cast<DictionaryAttr>(allocation);
      auto shared = named(circuit, group.getAs<StringAttr>("sharedModule").getValue());
      require(group.getAs<IntegerAttr>("threads").getInt() == 2 &&
              group.getAs<ArrayAttr>("members").size() == 2 &&
              group.getAs<BoolAttr>("localSelectorReset").getValue() == production,
              "group thread/reset allocation differs");
      if (production) {
        auto selector = *shared.getOps<RegResetOp>().begin();
        require(selector.getResetSignal() == argument(shared, "hostReset") &&
                selector.getResetValue().getDefiningOp<ConstantOp>().getValue().isZero(),
                "production local selector does not align with reset arbiter");
      }
      auto bank = *shared.getOps<MemOp>().begin();
      require(bank.getDepth() == 2, "independent group state banking changed");
    }
  }
  {
    auto root = fixture(context, path, 2); auto circuit = *root->getOps<CircuitOp>().begin();
    Builder b(&context); auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> rows(raw.begin(), raw.end());
    for (StringRef name : {"state", "out"})
      rows.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DontTouch)),
        b.getNamedAttr("target", b.getStringAttr("~Top|Worker>" + name))}));
    circuit->setAttr("rawAnnotations", b.getArrayAttr(rows));
    unsigned groups = 777; std::string error;
    require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error)), error.c_str());
    auto updated = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(updated.size() == rows.size() + 2, "local DontTouch directives were not copied to banked clone");
    auto last = cast<DictionaryAttr>(updated[updated.size()-1]);
    require(last.getAs<StringAttr>("target").getValue() == "~Top|Worker_threaded>out",
            "cloned DontTouch target did not retain declaration identity");
  }
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    auto buffer = llvm::MemoryBuffer::getFile(path); require(bool(buffer), "missing fixture");
    auto source = buffer.get()->getBuffer().str();
    auto module = source.find("  module Worker :");
    source.insert(module, "  extmodule configuration :\n    output out : UInt<8>\n    defname = " +
        std::string(mutation == 1 ? "unknown_reader" : "plusarg_reader") +
        "\n    parameter DEFAULT = 9\n    parameter FORMAT = \"feature=%d\"\n    parameter WIDTH = " +
        std::string(mutation == 2 ? "7" : "8") + "\n");
    auto declaration = source.find("    reg state");
    source.insert(declaration, "    inst configuration of configuration\n"
                               "    reg empty : UInt<0>, hostClock\n"
                               "    empty <= UInt<0>(0)\n");
    if (mutation == 3) {
      auto output = source.find("    output out : UInt<8>");
      source.replace(output, std::string("    output out : UInt<8>").size(), "    input out : UInt<8>");
    }
    llvm::SourceMgr manager;
    manager.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(source), llvm::SMLoc());
    DefaultTimingManager timer; auto scope = timer.getRootScope();
    auto root = importFIRFile(manager, &context, scope, FIRParserOptions());
    require(bool(root), "configuration fixture import failed");
    auto circuit = *root->getOps<CircuitOp>().begin(); Builder b(&context);
    circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::EnableModelMultiThreading)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top/worker0:Worker"))})}));
    auto before = text(circuit); unsigned groups = 777; std::string error;
    auto result = goldengate::multiThreadFAME5Models(circuit, groups, error);
    if (mutation) require(failed(result) && before == text(circuit) && groups == 777,
                          "unsafe configuration external did not reject atomically");
    else {
      require(succeeded(result) && succeeded(verify(*root)), error.c_str());
      auto shared = named(circuit, "Worker_threaded");
      auto external = *shared.getOps<InstanceOp>().begin();
      require(external.getModuleName() == "configuration", "configuration source was removed or banked");
      unsigned emptyRegisters = 0;
      for (auto reg : shared.getOps<RegOp>())
        emptyRegisters += cast<IntType>(reg.getResult().getType()).getWidth() == 0;
      require(emptyRegisters == 1, "zero-bit state was rejected or expanded into nonempty storage");
    }
  }
  llvm::outs() << "Passed independent repeated groups, production reset alignment, retained plusarg configuration and three unsafe-source rejections\n";
}
void rejections(MLIRContext &context, StringRef path) {
  for (unsigned test = 0; test < 6; ++test) {
    auto root = fixture(context, path, 4); auto circuit = *root->getOps<CircuitOp>().begin();
    auto top = named(circuit, "Top"), model = named(circuit, "Worker"); Builder b(&context);
    if (test == 0) {
      for (auto &op : *top.getBodyBlock())
        if (auto field = dyn_cast<SubfieldOp>(op); field && field.getInput() == argument(top, "in0"))
          field->setOperand(0, argument(top, "in1"));
    }
    if (test == 1) { OpBuilder add(model.getBodyBlock(), model.getBodyBlock()->begin()); add.create<InstanceOp>(model.getLoc(), model, "nested"); }
    if (test == 2) {
      auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> updated(raw.begin(), raw.end());
      updated.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown")), b.getNamedAttr("target", b.getStringAttr("~Top|Top/worker1:Worker>state"))}));
      circuit->setAttr("rawAnnotations", b.getArrayAttr(updated));
    }
    if (test == 3) {
      auto instance = *top.getOps<InstanceOp>().begin(); instance->setAttr("annotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown"))})}));
    }
    if (test == 4) {
      auto reg = *model.getOps<RegResetOp>().begin(); reg->setAttr("annotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown"))})}));
    }
    if (test == 5) {
      for (auto &op : *top.getBodyBlock()) if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(1) == argument(top, "hostReset")) { op.setOperand(1, argument(top, "in0")); break; }
    }
    std::string before = text(circuit), error; unsigned groups = 777;
    require(failed(goldengate::multiThreadFAME5Models(circuit, groups, error)) && !error.empty() && before == text(circuit) && groups == 777, "unsupported grouping did not reject atomically");
  }
  auto root = fixture(context, path, 1); auto circuit = *root->getOps<CircuitOp>().begin();
  std::string before = text(circuit), error; unsigned groups = 777;
  require(succeeded(goldengate::multiThreadFAME5Models(circuit, groups, error)) && groups == 0 && before == text(circuit), "single instance was needlessly threaded");
  llvm::outs() << "Passed six atomic grouping rejections and single-instance no-op\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: FAME5GroupedModelsTest fixture.fir");
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context, argv[1], 2); behavior(context, argv[1], 3); behavior(context, argv[1], 4); behavior(context, argv[1], 64); rejections(context, argv[1]); productionGroupsAndSources(context, argv[1]); consumedProvenance(context, argv[1]);
    auto synchronous = std::string(argv[1]);
    synchronous.replace(synchronous.rfind("FAME5GroupedModels.fir"), std::string("FAME5GroupedModels.fir").size(), "FAME5GroupedSynchronousMemory.fir");
    synchronousMemories(context, synchronous);
    auto dynamic = std::string(argv[1]);
    dynamic.replace(dynamic.rfind("FAME5GroupedModels.fir"), std::string("FAME5GroupedModels.fir").size(), "FAME5DynamicClock.fir");
    dynamicClocks(context, dynamic);
    auto dynamicMemory = std::string(argv[1]);
    dynamicMemory.replace(dynamicMemory.rfind("FAME5GroupedModels.fir"), std::string("FAME5GroupedModels.fir").size(), "FAME5DynamicMemory.fir");
    dynamicMemories(context, dynamicMemory);
    auto recursive = std::string(argv[1]);
    recursive.replace(recursive.rfind("FAME5GroupedModels.fir"), std::string("FAME5GroupedModels.fir").size(), "FAME5Hierarchy.fir");
    hierarchy(context, recursive);
    auto existingHost = recursive;
    existingHost.replace(existingHost.rfind("FAME5Hierarchy.fir"), std::string("FAME5Hierarchy.fir").size(), "FAME5HierarchyHostPorts.fir");
    hierarchy(context, existingHost);
    auto recursiveSync = recursive;
    recursiveSync.replace(recursiveSync.rfind("FAME5Hierarchy.fir"), std::string("FAME5Hierarchy.fir").size(), "FAME5HierarchySynchronousMemory.fir");
    hierarchy(context, recursiveSync, true); return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
