// See LICENSE for license details.
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
FModuleOp lookup(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == name)
      return module;
  throw std::runtime_error("missing fixture module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned reject) {
  auto root = parseSourceString<ModuleOp>(R"mlir(
    module { firrtl.circuit "Top" {
      firrtl.module @Top(in %clock: !firrtl.clock, in %data: !firrtl.uint<8>) {}
      firrtl.module @Parent(in %clock: !firrtl.clock, in %data: !firrtl.uint<8>) {}
      firrtl.module @Model(in %data: !firrtl.uint<8>, in %target: !firrtl.clock,
                          out %result: !firrtl.uint<8>) {
        firrtl.strictconnect %result, %data : !firrtl.uint<8>
      }
    } }
  )mlir", &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = lookup(circuit, "Model");
  auto parent = lookup(circuit, "Parent");
  auto top = lookup(circuit, "Top");
  for (auto owner : {parent, top}) {
    OpBuilder b(owner.getBodyBlock(), owner.getBodyBlock()->end());
    auto loc = owner.getLoc();
    auto instance = b.create<InstanceOp>(loc, model, "model");
    instance->setAttr("test.preserved", b.getStringAttr("instance metadata"));
    b.create<StrictConnectOp>(loc, instance.getResult(0),
                             owner.getBodyBlock()->getArgument(1));
    // Exercise both FIRRTL connection forms on the consumed port.
    if (owner == top)
      b.create<ConnectOp>(loc, instance.getResult(1),
                          owner.getBodyBlock()->getArgument(0));
    else
      b.create<StrictConnectOp>(loc, instance.getResult(1),
                                owner.getBodyBlock()->getArgument(0));
    auto output = b.create<WireOp>(loc, UIntType::get(&context, 8), "observed");
    b.create<StrictConnectOp>(loc, output.getResult(), instance.getResult(2));
    auto state = b.create<RegOp>(loc, UIntType::get(&context, 8),
                                owner.getBodyBlock()->getArgument(0), "state");
    b.create<StrictConnectOp>(loc, state.getResult(),
                              owner.getBodyBlock()->getArgument(1));
    if (owner == top) {
      auto nested = b.create<InstanceOp>(loc, parent, "nested");
      b.create<StrictConnectOp>(loc, nested.getResult(0),
                                owner.getBodyBlock()->getArgument(0));
      b.create<StrictConnectOp>(loc, nested.getResult(1),
                                owner.getBodyBlock()->getArgument(1));
      if (reject == 1) {
        auto read = b.create<WireOp>(loc, ClockType::get(&context), "clockRead");
        b.create<StrictConnectOp>(loc, read.getResult(), instance.getResult(1));
      } else if (reject == 2) {
        SmallVector<Attribute> annotations(
            instance.getPortAnnotationsAttr().getValue());
        auto anno = b.getDictionaryAttr(
            {b.getNamedAttr("class", b.getStringAttr("test.Clock"))});
        annotations[1] = b.getArrayAttr({anno});
        instance.setPortAnnotationsAttr(b.getArrayAttr(annotations));
      }
    }
  }
  if (reject == 3) {
    model.setPortSymbolsAttr(1, circt::hw::InnerSymAttr::get(
                                   StringAttr::get(&context, "clock_id")));
  }
  return root;
}
void run(MLIRContext &context) {
  for (unsigned rejection = 0; rejection < 4; ++rejection) {
    auto root = fixture(context, rejection);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = lookup(circuit, "Model");
    std::string before = dump(*root), error;
    auto result = goldengate::removeFAMEVirtualClockPort(
        circuit, model, "target", error);
    if (rejection) {
      require(failed(result) && !error.empty(), "unsafe clock removal accepted");
      require(dump(*root) == before, "rejected clock removal mutated IR");
      continue;
    }
    require(succeeded(result) && succeeded(verify(*root)),
            "clock removal failed verification");
    require(model.getNumPorts() == 2 && model.getPortName(1) == "result",
            "model retained clock or changed unrelated port order");
    unsigned instances = 0, parentStates = 0;
    circuit.walk([&](InstanceOp instance) {
      if (instance.getModuleName() != "Model")
        return;
      ++instances;
      require(instance.getNumResults() == 2 &&
                  instance.getPortNameStr(1) == "result" &&
                  instance->hasAttr("test.preserved"),
              "instance interface/metadata changed");
      require(instance.getResult(1).hasOneUse(), "output SSA uses were lost");
      auto connect = dyn_cast<StrictConnectOp>(
          *instance.getResult(1).getUsers().begin());
      require(connect && connect.getSrc() == instance.getResult(1),
              "output connection shifted");
    });
    circuit.walk([&](RegOp reg) {
      auto parent = reg->getParentOfType<FModuleOp>();
      require(reg.getClockVal() == parent.getBodyBlock()->getArgument(0),
              "shared parent clock was removed");
      ++parentStates;
    });
    require(instances == 2 && parentStates == 2, "not all parents were exercised");
  }
  llvm::outs() << "Removed virtual clock across two parents; preserved shared clocks, "
                  "SSA uses and metadata; rejected three unsafe cases without mutation\n";
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
