// See LICENSE for license details.
// Oracle: FAMETransform.targetClockMetadata and DefineAbstractClockGate.
#include "goldengate/FAMEInputChannel.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  return parseSourceString<ModuleOp>(R"mlir(
    module { firrtl.circuit "First" {
      firrtl.module @First(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock,
          in %other: !firrtl.clock) {
        %done = firrtl.wire : !firrtl.uint<1>
        %state = firrtl.reg %target : !firrtl.clock, !firrtl.uint<8>
        %otherState = firrtl.reg %other : !firrtl.clock, !firrtl.uint<8>
      }
      firrtl.module @Second(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock) {
        %done = firrtl.wire : !firrtl.uint<1>
        %state = firrtl.reg %target : !firrtl.clock, !firrtl.uint<8>
      }
    } }
  )mlir", &context);
}
Value driver(FModuleOp model, Value dest) {
  for (auto c : model.getOps<StrictConnectOp>())
    if (c.getDest() == dest)
      return c.getSrc();
  throw std::runtime_error("missing clock gate driver");
}
void run(MLIRContext &context) {
  auto root = fixture(context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  SmallVector<FModuleOp> models(circuit.getOps<FModuleOp>());
  for (auto model : models) {
    auto finishing = *model.getOps<WireOp>().begin();
    finishing.setName("targetCycleFinishing");
    std::string error;
    require(succeeded(goldengate::addFAMEClockEnable(model, "target", {}, error)),
            error.c_str());
  }
  std::string error;
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, models[0], "target", error)), error.c_str());
  auto gate = *circuit.getOps<FExtModuleOp>().begin();
  // Reuse must preserve the definition, including externally supplied metadata.
  OpBuilder b(&context);
  gate->setAttr("test.preserved", b.getStringAttr("shared definition"));
  gate.setPortSymbolsAttr(0, circt::hw::InnerSymAttr::get(b.getStringAttr("input_id")));
  auto anno = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Gate"))});
  gate.setAnnotationsAttr(b.getArrayAttr({anno}));
  std::string definition = dump(gate);
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, models[1], "target", error)), error.c_str());
  require(succeeded(goldengate::addFAMEClockEnable(
              models[0], "other", {}, error)), error.c_str());
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, models[0], "other", error)), error.c_str());
  require(dump(gate) == definition &&
              std::distance(circuit.getOps<FExtModuleOp>().begin(),
                            circuit.getOps<FExtModuleOp>().end()) == 1,
          "shared gate definition was changed or duplicated");
  unsigned count = 0;
  for (auto model : models) {
    for (auto instance : model.getOps<InstanceOp>()) {
      ++count;
      require(driver(model, instance.getResult(0)) == model.getArgument(0),
              "gate uses another model's host clock");
      auto ce = driver(model, instance.getResult(1)).getDefiningOp<AndPrimOp>();
      auto active = ce.getLhs().getDefiningOp<AndPrimOp>();
      auto reset = ce.getRhs().getDefiningOp<NotPrimOp>();
      auto enabled = active.getLhs().getDefiningOp<RegResetOp>();
      auto finishing = active.getRhs().getDefiningOp<WireOp>();
      require(enabled && finishing && reset &&
                  enabled->getParentOp() == model && finishing->getParentOp() == model &&
                  reset.getInput() == model.getArgument(1) &&
                  enabled.getName() ==
                      (instance.getName().drop_back(7) + "_enabled").str(),
              "gate shares enable, completion or reset controls");
      for (auto state : model.getOps<RegOp>()) {
        auto name = state.getName();
        if ((name == "state" && instance.getName() == "target_buffer") ||
            (name == "otherState" && instance.getName() == "other_buffer"))
          require(state.getClockVal() == instance.getResult(2),
                  "target state did not receive its domain's gate output");
      }
    }
  }
  require(count == 3 && succeeded(verify(*root)), "shared gate circuit failed verification");

  // Each invalid symbol/schema must fail without changing the circuit.
  for (unsigned rejection = 0; rejection < 8; ++rejection) {
    auto badRoot = fixture(context);
    auto badCircuit = *badRoot->getOps<CircuitOp>().begin();
    auto model = *badCircuit.getOps<FModuleOp>().begin();
    (*model.getOps<WireOp>().begin()).setName("targetCycleFinishing");
    require(succeeded(goldengate::addFAMEClockEnable(model, "target", {}, error)),
            error.c_str());
    b.setInsertionPointToEnd(badCircuit.getBodyBlock());
    SmallVector<PortInfo> ports{
      {b.getStringAttr("I"), ClockType::get(&context), Direction::In},
      {b.getStringAttr("CE"), UIntType::get(&context, 1), Direction::In},
      {b.getStringAttr("O"), ClockType::get(&context), Direction::Out}};
    if (rejection == 1) ports[1].type = UIntType::get(&context, 2);
    if (rejection == 2) ports[0].direction = Direction::Out;
    if (rejection == 3) ports[0].name = b.getStringAttr("wrong");
    if (rejection == 4) ports.pop_back();
    if (rejection == 0)
      b.create<FModuleOp>(model.getLoc(), b.getStringAttr("AbstractClockGate"),
          ConventionAttr::get(&context, Convention::Internal), ports);
    else {
      auto invalid = b.create<FExtModuleOp>(model.getLoc(), b.getStringAttr("AbstractClockGate"),
          ConventionAttr::get(&context, rejection == 6 ? Convention::Scalarized : Convention::Internal),
          ports, rejection == 5 ? "DifferentGate" : "AbstractClockGate");
      if (rejection == 7)
        invalid.setParametersAttr(b.getArrayAttr({ParamDeclAttr::get(
            &context, b.getStringAttr("mode"), b.getI32Type(), b.getI32IntegerAttr(1))}));
    }
    auto before = dump(*badRoot);
    require(failed(goldengate::addFAMEClockGate(badCircuit, model, "target", error)) &&
                !error.empty(), "incompatible gate definition was accepted");
    require(dump(*badRoot) == before, "rejected gate definition mutated IR");
  }
  llvm::outs() << "Shared one gate definition across two models and three domains; "
                  "preserved metadata and independent controls; rejected eight incompatible definitions\n";
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
