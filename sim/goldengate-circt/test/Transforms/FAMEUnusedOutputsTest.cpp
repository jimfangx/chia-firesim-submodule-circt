// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
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
void run(MLIRContext &context) {
  for (unsigned rejection = 0; rejection < 8; ++rejection) {
    auto root = parseSourceString<ModuleOp>(R"mlir(
      module { firrtl.circuit "Top" {
        firrtl.module @Top(in %data: !firrtl.uint<8>, out %bypass: !firrtl.uint<8>) {
          firrtl.strictconnect %bypass, %data : !firrtl.uint<8>
        }
        firrtl.module @Parent(in %data: !firrtl.uint<8>, out %bypass: !firrtl.uint<8>) {
          firrtl.strictconnect %bypass, %data : !firrtl.uint<8>
        }
        firrtl.module @Model(in %data: !firrtl.uint<8>,
                            out %retired: !firrtl.uint<8>,
                            out %active: !firrtl.uint<8>,
                            out %aggregate: !firrtl.bundle<x: uint<8>>) {
          firrtl.strictconnect %retired, %data : !firrtl.uint<8>
          firrtl.strictconnect %active, %data : !firrtl.uint<8>
          %x = firrtl.subfield %aggregate[x] : !firrtl.bundle<x: uint<8>>
          firrtl.strictconnect %x, %retired : !firrtl.uint<8>
        }
      } }
    )mlir", &context);
    require(bool(root), "fixture parse failed");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = lookup(circuit, "Model");
    auto top = lookup(circuit, "Top");
    auto parent = lookup(circuit, "Parent");
    for (auto owner : {parent, top}) {
      OpBuilder b(owner.getBodyBlock(), owner.getBodyBlock()->end());
      auto instance = b.create<InstanceOp>(owner.getLoc(), model, "model");
      instance->setAttr("test.preserved", b.getStringAttr("instance metadata"));
      b.create<StrictConnectOp>(owner.getLoc(), instance.getResult(0),
                                owner.getBodyBlock()->getArgument(0));
      auto observed = b.create<WireOp>(owner.getLoc(), UIntType::get(&context, 8),
                                       "observed");
      b.create<StrictConnectOp>(owner.getLoc(), observed.getResult(),
                                instance.getResult(2));
      if (owner != top)
        continue;
      auto nested = b.create<InstanceOp>(owner.getLoc(), parent, "nested");
      b.create<StrictConnectOp>(owner.getLoc(), nested.getResult(0),
                                owner.getBodyBlock()->getArgument(0));
      if (rejection == 1)
        b.create<StrictConnectOp>(owner.getLoc(), observed.getResult(),
                                  instance.getResult(1));
      if (rejection == 2) {
        SmallVector<Attribute> annotations(
            instance.getPortAnnotationsAttr().getValue());
        auto anno = b.getDictionaryAttr(
            {b.getNamedAttr("class", b.getStringAttr("test.Output"))});
        annotations[1] = b.getArrayAttr({anno});
        instance.setPortAnnotationsAttr(b.getArrayAttr(annotations));
      }
    }
    if (rejection == 3)
      model.setPortSymbolsAttr(1, circt::hw::InnerSymAttr::get(
                                     StringAttr::get(&context, "retired_id")));
    if (rejection == 4) {
      OpBuilder b(&context);
      SmallVector<Attribute> annotations(
          model.getPortAnnotationsAttr().getValue());
      annotations.resize(model.getNumPorts(), b.getArrayAttr({}));
      auto anno = b.getDictionaryAttr(
          {b.getNamedAttr("class", b.getStringAttr("test.Output"))});
      annotations[1] = b.getArrayAttr({anno});
      model.setPortAnnotationsAttr(b.getArrayAttr(annotations));
    }
    SmallVector<StringRef> names{"retired", "aggregate"};
    if (rejection == 5)
      names.push_back("retired");
    if (rejection == 6)
      names.push_back("absent");
    if (rejection == 7)
      names.push_back("data");
    std::string before = dump(*root), error;
    auto result = goldengate::internalizeFAMEUnusedOutputs(
        circuit, model, names, error);
    if (rejection) {
      require(failed(result) && !error.empty(),
              "unsafe output internalization accepted");
      require(dump(*root) == before,
              "rejected output internalization mutated IR");
      continue;
    }
    require(succeeded(result) && succeeded(verify(*root)),
            "internalization failed verification");
    require(model.getNumPorts() == 2 && model.getPortName(1) == "active",
            "active output changed or unused output retained");
    unsigned wires = 0;
    for (auto wire : model.getOps<WireOp>()) {
      ++wires;
      if (wire.getName() == "retired") {
        require(std::distance(wire.getResult().use_begin(),
                              wire.getResult().use_end()) == 2,
                "internal output reads/writes lost");
        bool assignment = false;
        for (auto user : wire.getResult().getUsers())
          if (auto connect = dyn_cast<StrictConnectOp>(user))
            assignment |= connect.getDest() == wire.getResult() &&
                          connect.getSrc() == model.getBodyBlock()->getArgument(0);
        require(assignment, "former passthrough model driver lost");
      } else {
        require(wire.getName() == "aggregate" && wire.getResult().hasOneUse(),
                "aggregate output subfield lost");
        require(isa<SubfieldOp>(*wire.getResult().getUsers().begin()),
                "aggregate subfield no longer refers to internal wire");
      }
    }
    require(wires == 2, "same-name internal wires missing");
    unsigned instances = 0;
    circuit.walk([&](InstanceOp instance) {
      if (instance.getModuleName() != "Model")
        return;
      ++instances;
      require(instance.getNumResults() == 2 &&
                  instance.getPortNameStr(1) == "active" &&
                  instance->hasAttr("test.preserved") &&
                  instance.getResult(1).hasOneUse(),
              "instance retained port/SSA/metadata changed");
    });
    require(instances == 2, "not all model instances changed");
    for (auto owner : {top, parent}) {
      require(owner.getNumPorts() == 2, "parent bypass interface changed");
      auto bypass = owner.getBodyBlock()->getArgument(1);
      auto connect = dyn_cast<StrictConnectOp>(*bypass.getUsers().begin());
      require(bypass.hasOneUse() && connect &&
                  connect.getSrc() == owner.getBodyBlock()->getArgument(0),
              "promoted upstream bypass route changed");
    }
  }
  llvm::outs() << "Internalized scalar/aggregate outputs across two parents; preserved "
                  "model assignments, bypass routes, active ports and metadata; "
                  "rejected seven unsafe cases without mutation\n";
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
