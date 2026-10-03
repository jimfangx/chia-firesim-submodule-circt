// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/InnerSymbolTable.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <stdexcept>
#include <tuple>

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
  for (unsigned rejection = 0; rejection < 11; ++rejection) {
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
    if (rejection == 3 || rejection == 8 || rejection == 10) {
      // Invalid ground/aggregate field IDs must fail before the first wire is
      // created, even if only the second output has an invalid identity.
      OpBuilder b(&context);
      model.setPortSymbolsAttr(rejection == 3 ? 1 : 3,
          circt::hw::InnerSymAttr::get(&context, {
              circt::hw::InnerSymPropertiesAttr::get(&context,
                  b.getStringAttr("invalid_id"), rejection == 3 ? 1 : (rejection == 8 ? 2 : 0),
                  b.getStringAttr("private"))}));
    }
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
    if (rejection == 9) {
      OpBuilder b(model.getBodyBlock(), model.getBodyBlock()->end());
      b.create<WireOp>(model.getLoc(), model.getPortType(3), "aggregate");
    }
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
                  "rejected ten unsafe cases without mutation\n";
}

void runIdentities(MLIRContext &context) {
  using namespace circt::hw;
  auto root = parseSourceString<ModuleOp>(R"mlir(
module { firrtl.circuit "Top" {
  firrtl.module @Top() {}
  firrtl.module @Parent() {}
  firrtl.module @Model(in %data: !firrtl.uint<8>,
      out %retired: !firrtl.uint<8>, out %active: !firrtl.uint<8>,
      out %aggregate: !firrtl.bundle<x: uint<8>, nested: bundle<y: uint<8>>>) {
    firrtl.strictconnect %retired, %data : !firrtl.uint<8>
    firrtl.strictconnect %active, %retired : !firrtl.uint<8>
    %x = firrtl.subfield %aggregate[x] : !firrtl.bundle<x: uint<8>, nested: bundle<y: uint<8>>>
    %nested = firrtl.subfield %aggregate[nested] : !firrtl.bundle<x: uint<8>, nested: bundle<y: uint<8>>>
    %y = firrtl.subfield %nested[y] : !firrtl.bundle<y: uint<8>>
    firrtl.strictconnect %x, %retired : !firrtl.uint<8>
    firrtl.strictconnect %y, %data : !firrtl.uint<8>
  }
} })mlir", &context);
  require(bool(root), "identity fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = lookup(circuit, "Model");
  OpBuilder b(&context);
  auto property = [&](StringRef name, unsigned field, StringRef visibility) {
    return InnerSymPropertiesAttr::get(&context, b.getStringAttr(name), field,
                                       b.getStringAttr(visibility));
  };
  auto scalar = InnerSymAttr::get(&context, {property("retired_id", 0, "public")});
  auto aggregate = InnerSymAttr::get(&context, {
      property("x_id", 1, "private"), property("y_id", 3, "public")});
  model.setPortSymbolsAttr(1, scalar);
  model.setPortSymbolsAttr(3, aggregate);
  model.setPortSymbolsAttr(2, InnerSymAttr::get(b.getStringAttr("active_id")));
  auto reference = [&](StringRef module, StringRef name) {
    return InnerRefAttr::get(b.getStringAttr(module), b.getStringAttr(name));
  };
  SmallVector<Attribute> refs{reference("Model", "retired_id"),
      reference("Model", "x_id"), reference("Model", "y_id"),
      reference("Model", "active_id")};
  for (StringRef ownerName : {"Top", "Parent"}) {
    auto owner = lookup(circuit, ownerName);
    b.setInsertionPointToEnd(owner.getBodyBlock());
    auto instance = b.create<InstanceOp>(owner.getLoc(), model, "model");
    instance.setInnerSymAttr(InnerSymAttr::get(b.getStringAttr("instance_id")));
    instance->setAttr("test.metadata", b.getStringAttr(ownerName));
    refs.push_back(reference(ownerName, "instance_id"));
  }
  auto stableRefs = b.getArrayAttr(refs);
  circuit->setAttr("test.stable_refs", stableRefs);
  auto resolve = [&](StringRef module, StringRef name) {
    SymbolTable modules(circuit);
    InnerSymbolTableCollection tables;
    InnerRefNamespace names{modules, tables};
    return names.lookup(reference(module, name));
  };
  require(succeeded(verify(*root)), "initial identity fixture is invalid");
  for (auto [name, field] : {std::pair<StringRef, unsigned>{"retired_id", 0},
                            {"x_id", 1}, {"y_id", 3}}) {
    auto target = resolve("Model", name);
    require(target.isPort() && target.getField() == field,
            "initial output identity does not resolve");
  }
  std::string error;
  // A valid field ID may still target a bundle that LowerTypes cannot retain.
  // Reject the second output atomically, then restore the supported leaf IDs.
  auto before = dump(*root);
  model.setPortSymbolsAttr(3, InnerSymAttr::get(&context, {
      property("nested_id", 2, "public")}));
  auto unsupported = dump(*root);
  require(failed(goldengate::internalizeFAMEUnusedOutputs(
              circuit, model, {"retired", "aggregate"}, error)) &&
              dump(*root) == unsupported && error.find("LowerTypes") != std::string::npos,
          "sub-bundle identity was not rejected atomically");
  model.setPortSymbolsAttr(3, aggregate);
  require(dump(*root) == before, "leaf identity fixture was not restored");
  require(succeeded(goldengate::internalizeFAMEUnusedOutputs(
              circuit, model, {"retired", "aggregate"}, error)) &&
              succeeded(verify(*root)), "symbolized output internalization failed");
  for (auto wire : model.getOps<WireOp>())
    require(wire.getInnerSymAttr() == (wire.getName() == "retired" ? scalar : aggregate),
            "wire symbol names, visibility or aggregate fields changed");
  auto check = [&](bool lowered) {
    for (auto [name, wireName, field] : {
        std::tuple<StringRef, StringRef, unsigned>{"retired_id", "retired", 0},
        {"x_id", lowered ? "aggregate_x" : "aggregate", lowered ? 0u : 1u},
        {"y_id", lowered ? "aggregate_nested_y" : "aggregate", lowered ? 0u : 3u}}) {
      auto target = resolve("Model", name);
      auto wire = target && !target.isPort() ? dyn_cast<WireOp>(target.getOp()) : WireOp();
      require(wire && wire.getName() == wireName && target.getField() == field,
              "stable output InnerRef does not resolve to the correct wire field");
      auto wireSymbol = wire.getInnerSymAttr();
      bool identity = false;
      for (auto p : wireSymbol)
        identity |= p.getName().getValue() == name && p.getFieldID() == field &&
                    p.getSymVisibility().getValue() == (name == "x_id" ? "private" : "public");
      require(identity, "output identity visibility changed");
    }
    auto active = resolve("Model", "active_id");
    require(active.isPort() && active.getPort() == 1,
            "retained output identity changed");
    for (StringRef ownerName : {"Top", "Parent"}) {
      auto target = resolve(ownerName, "instance_id");
      auto instance = target ? dyn_cast<InstanceOp>(target.getOp()) : InstanceOp();
      require(instance && instance.getNumResults() == 2 &&
                  instance->getAttrOfType<StringAttr>("test.metadata").getValue() == ownerName,
              "instance identity or metadata changed");
    }
    require(circuit->getAttr("test.stable_refs") == stableRefs,
            "stable InnerRefs were rewritten");
  };
  check(false);
  PassManager pm(&context);
  pm.addNestedPass<CircuitOp>(createLowerFIRRTLTypesPass());
  require(succeeded(pm.run(*root)) && succeeded(verify(*root)),
          "internalized output identities failed LowerTypes");
  check(true);
  llvm::outs() << "Preserved scalar and nested aggregate leaf InnerRefs, retained port "
                  "and two instance identities through internalization and LowerTypes\n";
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    runIdentities(context);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
