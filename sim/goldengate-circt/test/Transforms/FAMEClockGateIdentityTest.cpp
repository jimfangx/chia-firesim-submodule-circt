// See LICENSE for license details.
// Oracle: FAMETransform clockMetadata and its bufferOutputRT XDC annotation.
#include "goldengate/FAMEClockGate.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/XDCEmission.h"
#include "goldengate/XilinxHostSpecialization.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, StringRef why) {
  if (!value) throw std::runtime_error(why.str());
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
Value driver(FModuleOp model, Value dest) {
  for (auto connect : model.getOps<StrictConnectOp>())
    if (connect.getDest() == dest) return connect.getSrc();
  throw std::runtime_error("missing gate driver");
}
unsigned eval(Value value, const llvm::DenseMap<Value, unsigned> &values) {
  if (auto it = values.find(value); it != values.end()) return it->second;
  if (auto op = value.getDefiningOp<AndPrimOp>())
    return eval(op.getLhs(), values) & eval(op.getRhs(), values);
  if (auto op = value.getDefiningOp<NotPrimOp>())
    return !eval(op.getInput(), values);
  throw std::runtime_error("unsupported CE expression");
}
void paths(CircuitOp circuit, StringRef prefix) {
  OpBuilder b(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::XDCPaths)),
      b.getNamedAttr("postLinkPath", b.getStringAttr(prefix))})}));
}
std::string xdc(CircuitOp circuit) {
  std::string error;
  require(succeeded(goldengate::prepareXDCOutput(circuit, error)), error);
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto dict = cast<DictionaryAttr>(attr);
    if (auto suffix = dict.getAs<StringAttr>("suffix");
        suffix && suffix.getValue() == ".implementation.xdc")
      return dict.getAs<StringAttr>("fileBody").getValue().str();
  }
  throw std::runtime_error("missing implementation XDC");
}
std::set<std::string> clockLines(StringRef text) {
  SmallVector<StringRef> lines;
  text.split(lines, '\n');
  std::set<std::string> result;
  for (auto line : lines)
    if (line.starts_with("create_generated_clock ") ||
        line.starts_with("set_multicycle_path ")) result.insert(line.str());
  return result;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  return parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Model" {
    firrtl.module @Model(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock,
        in %other: !firrtl.clock, in %third: !firrtl.clock,
        in %target_buffer: !firrtl.uint<1>) {
      %done = firrtl.wire : !firrtl.uint<1>
      %target_buffer_0 = firrtl.wire : !firrtl.uint<1>
      %third_buffer = firrtl.node %target_buffer : !firrtl.uint<1>
      %state = firrtl.reg %target : !firrtl.clock, !firrtl.uint<8>
      %otherState = firrtl.reg %other : !firrtl.clock, !firrtl.uint<8>
      %thirdState = firrtl.reg %third : !firrtl.clock, !firrtl.uint<8>
    }
  } })mlir", &ctx);
}
void collisions(MLIRContext &ctx) {
  auto root = fixture(ctx);
  require(bool(root), "collision fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  (*model.getOps<WireOp>().begin()).setName("targetCycleFinishing");
  std::string error;
  require(succeeded(goldengate::addFAMEClockEnable(model, "target", {}, error)), error);
  require(succeeded(goldengate::addFAMEClockGate(circuit, model, "target", error)), error);
  auto definition = *circuit.getOps<FExtModuleOp>().begin();
  OpBuilder b(&ctx);
  b.setInsertionPointToEnd(model.getBodyBlock());
  // An existing target instance has exactly the abstract gate schema.
  auto decoy = b.create<InstanceOp>(model.getLoc(), definition, "other_buffer");
  b.create<StrictConnectOp>(model.getLoc(), decoy.getResult(0), model.getArgument(0));
  b.create<StrictConnectOp>(model.getLoc(), decoy.getResult(1), model.getArgument(5));
  for (auto clock : {"other", "third"}) {
    require(succeeded(goldengate::addFAMEClockEnable(model, clock, {}, error)), error);
    require(succeeded(goldengate::addFAMEClockGate(circuit, model, clock, error)), error);
  }
  goldengate::FAMEClockGateIndex gates;
  require(succeeded(gates.collect(model, error)), error);
  const char *clocks[] = {"target", "other", "third"};
  const char *names[] = {"target_buffer_1", "other_buffer_0", "third_buffer_0"};
  for (unsigned i = 0; i < 3; ++i) {
    auto gate = gates.lookup(clocks[i], false);
    require(gate && gate.getName() == names[i], "gate namespace differs from SFC");
    llvm::outs() << "GATE " << clocks[i] << ' ' << gate.getName() << '\n';
    auto before = dump(*root);
    require(failed(goldengate::addFAMEClockGate(circuit, model, clocks[i], error)) &&
                dump(*root) == before, "duplicate gate creation mutated IR");
    gate.setName((Twine("renamed_") + clocks[i]).str());
  }
  require(!decoy->hasAttr(goldengate::fameClockGateAttr) &&
              driver(model, decoy.getResult(1)) == model.getArgument(5),
          "colliding target instance was modified");
  root = parseSourceString<ModuleOp>(dump(*root), &ctx);
  require(bool(root), "gate identity serialization failed");
  circuit = *root->getOps<CircuitOp>().begin();
  model = *circuit.getOps<FModuleOp>().begin();
  require(succeeded(gates.collect(model, error)), error);
  goldengate::FAMEClockEnableIndex enables;
  require(succeeded(enables.collect(model, error)), error);
  Value done;
  for (auto wire : model.getOps<WireOp>())
    if (wire.getName() == "targetCycleFinishing") done = wire.getResult();
  unsigned i = 0;
  for (auto reg : model.getOps<RegOp>()) {
    auto gate = gates.lookup(clocks[i++]);
    require(reg.getClockVal() == gate.getResult(2), "target state uses wrong gate");
  }
  paths(circuit, "shell/target");
  for (auto clock : clocks) {
    auto gate = gates.lookup(clock);
    require(gate.getName() == (Twine("renamed_") + clock).str(), "lost renamed gate");
    require(driver(model, gate.getResult(0)) == model.getArgument(0), "incorrect gate I");
    for (unsigned mask = 0; mask < 8; ++mask) {
      unsigned enabled = mask & 1, finishing = (mask >> 1) & 1, reset = (mask >> 2) & 1;
      llvm::DenseMap<Value, unsigned> values{{enables.lookup(clock), enabled},
          {done, finishing}, {model.getArgument(1), reset}};
      require(eval(driver(model, gate.getResult(1)), values) ==
                  (enabled & finishing & !reset), "CE differs from SFC");
    }
    goldengate::RationalClockInfo info{clock, 1, 1, 3};
    require(succeeded(goldengate::addFAMEClockConstraint(circuit, model, clock, info, error)), error);
  }
  auto before = dump(*root);
  require(failed(goldengate::addFAMEClockConstraint(circuit, model, "other",
              {"other", 1, 1, 3}, error)) && dump(*root) == before,
          "duplicate XDC attachment mutated IR");
  // FPGA specialization changes the referenced module, keeping gate identity
  // and attached constraints. Emission follows the operation's actual name.
  require(succeeded(goldengate::specializeXilinxClockGates(circuit, error)), error);
  auto output = xdc(circuit);
  for (auto clock : clocks)
    require(output.find((Twine("shell/target/renamed_") + clock + "/O").str()) !=
                std::string::npos, "XDC detached from renamed gate output");
  require(output.find("/other_buffer/O") == std::string::npos &&
              output.find("set_multicycle_path 3 -setup") != std::string::npos &&
              output.find("set_multicycle_path 2 -hold") != std::string::npos &&
              succeeded(verify(*root)), "XDC used decoy or incorrect MFMR");
  llvm::outs() << "Matched 24 gate CE cases; renamed/serialized/specialized XDC follows actual outputs\n";
}
void rejectedIdentities(MLIRContext &ctx) {
  std::string error;
  for (unsigned mode = 0; mode < 8; ++mode) {
    auto root = fixture(ctx);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = *circuit.getOps<FModuleOp>().begin();
    auto finishing = *model.getOps<WireOp>().begin();
    finishing.setName("targetCycleFinishing");
    for (auto clock : {"target", "other"}) {
      require(succeeded(goldengate::addFAMEClockEnable(model, clock, {}, error)), error);
      require(succeeded(goldengate::addFAMEClockGate(circuit, model, clock, error)), error);
    }
    goldengate::FAMEClockGateIndex gates;
    require(succeeded(gates.collect(model, error)), error);
    auto target = gates.lookup("target"), other = gates.lookup("other");
    OpBuilder b(&ctx);
    if (mode == 0) target->setAttr(goldengate::fameClockGateAttr, b.getI32IntegerAttr(0));
    if (mode == 1) target->setAttr(goldengate::fameClockGateAttr, b.getStringAttr(""));
    if (mode == 2) finishing->setAttr(goldengate::fameClockGateAttr, b.getStringAttr("wrong"));
    if (mode == 3) other->setAttr(goldengate::fameClockGateAttr, b.getStringAttr("target"));
    if (mode == 4) target.setModuleName("WrongGate");
    if (mode == 6) target.getResult(1).setType(UIntType::get(&ctx, 2));
    if (mode == 7) other.setName(target.getName());
    if (mode == 5) {
      // Native identities disable legacy fallback even if the old name is an
      // otherwise perfectly valid gate instance.
      target->removeAttr(goldengate::fameClockGateAttr);
      target.setName("target_buffer");
      require(succeeded(gates.collect(model, error)) && !gates.lookup("target"),
              "native gate lookup fell back to target instance name");
    }
    paths(circuit, "shell/target");
    auto before = dump(*root);
    require(failed(goldengate::addFAMEClockConstraint(circuit, model, "target",
                {"target", 1, 1, 1}, error)) && dump(*root) == before,
            "invalid identity mutated constraint attachment");
    if (mode != 5)
      require(failed(goldengate::addFAMEClockGate(circuit, model, "third", error)) &&
                  dump(*root) == before, "invalid identity mutated gate creation");
  }
  // Both invalid token source and missing clock reads fail before creating an
  // AbstractClockGate definition. These were previously late mutation errors.
  for (unsigned mode = 0; mode < 2; ++mode) {
    auto root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Token" {
      firrtl.module @Token(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>, in %token: !firrtl.bundle<bits: clock>) {
        %done = firrtl.wire : !firrtl.uint<1>
        %invalid = firrtl.wire : !firrtl.clock
      }
    } })mlir", &ctx);
    require(bool(root), "token fixture parse");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = *circuit.getOps<FModuleOp>().begin();
    (*model.getOps<WireOp>().begin()).setName("targetCycleFinishing");
    require(succeeded(goldengate::addFAMEClockEnable(model, "target", {}, error)), error);
    OpBuilder b(&ctx); b.setInsertionPointToEnd(model.getBodyBlock());
    Value bits = b.create<SubfieldOp>(model.getLoc(), model.getArgument(2), "bits");
    if (mode == 0)
      for (auto wire : model.getOps<WireOp>())
        if (wire.getName() == "invalid") bits = wire.getResult();
    auto before = dump(*root);
    require(failed(goldengate::addFAMEClockGate(circuit, model, "target", error, bits)) &&
                dump(*root) == before, "invalid token mutated gate construction");
  }
  llvm::outs() << "Rejected eight invalid/missing identities and two invalid tokens without mutation\n";
}
void golden(MLIRContext &ctx, StringRef goldenXDC, StringRef goldenRTL) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "FireSim" {
    firrtl.module @FireSim(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, in %clockBridge_clocks_0: !firrtl.clock) {
      %done = firrtl.wire : !firrtl.uint<1>
      %state = firrtl.reg %clockBridge_clocks_0 : !firrtl.clock, !firrtl.uint<8>
    }
  } })mlir", &ctx);
  require(bool(root), "Rocket clock fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  (*model.getOps<WireOp>().begin()).setName("targetCycleFinishing");
  std::string error;
  require(succeeded(goldengate::addFAMEClockEnable(model, "clockBridge_clocks_0", {}, error)), error);
  require(succeeded(goldengate::addFAMEClockGate(circuit, model, "clockBridge_clocks_0", error)), error);
  paths(circuit, "firesim_top/top/sim/target/FireSim_");
  require(succeeded(goldengate::addFAMEClockConstraint(circuit, model, "clockBridge_clocks_0",
              {"uart_clock,clock_1000.0MHz,harnessbinder_clock,reference", 1, 1, 1}, error)), error);
  auto referenceXDC = llvm::MemoryBuffer::getFile(goldenXDC);
  auto referenceRTL = llvm::MemoryBuffer::getFile(goldenRTL);
  require(bool(referenceXDC) && bool(referenceRTL), "cannot read immutable golden artifacts");
  auto rtl = (*referenceRTL)->getBuffer();
  require(rtl.contains("BUFGCE clockBridge_clocks_0_buffer (") &&
              rtl.contains("assign clockBridge_clocks_0_buffer_I = hostClock;") &&
              rtl.contains("assign clockBridge_clocks_0_buffer_CE = clockBridge_clocks_0_enabled & targetCycleFinishing & ~hostReset;"),
          "recorded SFC gate RTL contract differs");
  require(clockLines(xdc(circuit)) == clockLines((*referenceXDC)->getBuffer()),
          "native generated-clock/MFMR/pin collateral differs from immutable SFC");
  llvm::outs() << "Matched immutable Rocket gate RTL contract and all three generated-clock XDC commands\n";
}
void rocketBoundary(MLIRContext &ctx, StringRef candidate, StringRef goldenXDC) {
  auto root = parseSourceFile<ModuleOp>(candidate, &ctx);
  require(bool(root), "Rocket candidate parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp model;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == "FireSim") model = module;
  require(bool(model), "Rocket model missing");
  std::string error;
  goldengate::FAMEClockGateIndex gates;
  require(succeeded(gates.collect(model, error)), error);
  auto oldGate = gates.lookup("clockBridge_clocks_0");
  require(bool(oldGate), "Rocket abstract gate missing");
  auto enabled = goldengate::lookupFAMEClockEnable(model, "clockBridge_clocks_0", error);
  auto update = driver(model, enabled).getDefiningOp<MuxPrimOp>();
  auto cast = update ? update.getHigh().getDefiningOp<AsUIntPrimOp>() : AsUIntPrimOp();
  auto rawBits = cast ? cast.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
  require(bool(rawBits), "Rocket incoming clock token missing");
  OpBuilder b(oldGate);
  Value clockRead = b.create<SubfieldOp>(oldGate.getLoc(), rawBits.getInput(), "bits");
  unsigned clockUses = std::distance(oldGate.getResult(2).use_begin(),
                                    oldGate.getResult(2).use_end());
  require(clockUses > 0, "Rocket gate output has no target uses");
  oldGate.getResult(2).replaceAllUsesWith(clockRead);
  for (unsigned i = 0; i < 2; ++i)
    for (auto &use : llvm::make_early_inc_range(oldGate.getResult(i).getUses())) {
      auto connect = dyn_cast<StrictConnectOp>(use.getOwner());
      require(connect && connect.getDest() == oldGate.getResult(i), "unexpected Rocket gate input use");
      connect.erase();
    }
  oldGate.erase();
  require(succeeded(goldengate::addFAMEClockGate(circuit, model,
              "clockBridge_clocks_0", error, rawBits.getResult())), error);
  require(succeeded(gates.collect(model, error)), error);
  auto gate = gates.lookup("clockBridge_clocks_0", false);
  require(gate && gate.getName() == "clockBridge_clocks_0_buffer" &&
              clockRead.use_empty() && !rawBits.getResult().use_empty() &&
              std::distance(gate.getResult(2).use_begin(), gate.getResult(2).use_end()) == clockUses,
          "Rocket gate name/target uses/raw token changed");
  auto reg = enabled.getDefiningOp<RegResetOp>();
  require(driver(model, gate.getResult(0)) == reg.getClockVal(), "Rocket gate I differs from SFC");
  for (unsigned mask = 0; mask < 8; ++mask) {
    unsigned en = mask & 1, done = (mask >> 1) & 1, reset = (mask >> 2) & 1;
    llvm::DenseMap<Value, unsigned> values{{enabled, en}, {update.getSel(), done},
                                         {reg.getResetSignal(), reset}};
    require(eval(driver(model, gate.getResult(1)), values) == (en & done & !reset),
            "regenerated Rocket gate CE differs from immutable SFC");
  }
  require(succeeded(verify(*root)), "regenerated Rocket gate failed verification");
  // This boundary precedes the final wrapper insertion and module uniquing.
  // Supply the recorded final wrapper prefix and rename its actual model
  // instance as the later simulator mapping does, then resolve its O pin.
  for (auto module : circuit.getOps<FModuleOp>())
    for (auto instance : module.getOps<InstanceOp>())
      if (instance.getModuleName() == "FireSim") instance.setName("FireSim_");
  paths(circuit, "firesim_top/top/sim/target");
  require(succeeded(goldengate::addFAMEClockConstraint(circuit, model, "clockBridge_clocks_0",
              {"uart_clock,clock_1000.0MHz,harnessbinder_clock,reference", 1, 1, 1}, error)), error);
  auto reference = llvm::MemoryBuffer::getFile(goldenXDC);
  require(bool(reference), "cannot read immutable Rocket XDC");
  auto output = xdc(circuit);
  require(clockLines(output) == clockLines((*reference)->getBuffer()),
          "actual Rocket candidate XDC differs from immutable SFC");
  llvm::outs() << "Regenerated actual Rocket gate: preserved " << clockUses
               << " target clock uses and raw enable token; eight CE cases passed\n"
               << output;
}
void cliBoundary(MLIRContext &ctx, StringRef candidate) {
  auto root = parseSourceFile<ModuleOp>(candidate, &ctx);
  require(bool(root), "CLI collision candidate parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  std::string error;
  goldengate::FAMEClockGateIndex gates;
  require(succeeded(gates.collect(model, error)), error);
  auto gate = gates.lookup("target", false);
  require(gate && gate.getName() == "target_buffer_1", "CLI did not unique gate name");
  auto enabled = goldengate::lookupFAMEClockEnable(model, "target", error);
  auto reg = enabled.getDefiningOp<RegResetOp>();
  auto ce = driver(model, gate.getResult(1));
  auto active = ce.getDefiningOp<AndPrimOp>().getLhs().getDefiningOp<AndPrimOp>();
  require(active.getLhs() == enabled, "CLI gate uses wrong enable identity");
  for (unsigned mask = 0; mask < 8; ++mask) {
    unsigned en = mask & 1, done = (mask >> 1) & 1, reset = (mask >> 2) & 1;
    llvm::DenseMap<Value, unsigned> values{{enabled, en}, {active.getRhs(), done},
                                         {reg.getResetSignal(), reset}};
    require(eval(ce, values) == (en & done & !reset), "CLI gate CE differs from SFC");
  }
  for (auto state : model.getOps<RegOp>())
    if (state.getName() == "state")
      require(state.getClockVal() == gate.getResult(2), "CLI target state uses wrong gate");
  bool wire = false, node = false;
  for (auto declaration : model.getOps<WireOp>())
    if (declaration.getName() == "target_buffer") wire = true;
  for (auto declaration : model.getOps<NodeOp>())
    if (declaration.getName() == "target_buffer_0") node = true;
  require(wire && node && succeeded(verify(*root)), "CLI dropped colliding target declarations");
  llvm::outs() << "CLI unique clock gate preserves target declarations and eight CE cases\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    collisions(ctx);
    rejectedIdentities(ctx);
    if (argc == 3 && StringRef(argv[1]) == "--cli") cliBoundary(ctx, argv[2]);
    else {
      if (argc >= 3) golden(ctx, argv[1], argv[2]);
      if (argc == 4) rocketBoundary(ctx, argv[3], argv[1]);
    }
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
