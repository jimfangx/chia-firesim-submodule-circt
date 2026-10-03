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

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
circt::hw::InnerSymAttr symbol(OpBuilder &b, StringRef name,
                               StringRef visibility, unsigned field = 0) {
  return circt::hw::InnerSymAttr::get(b.getContext(), {
      circt::hw::InnerSymPropertiesAttr::get(b.getContext(), b.getStringAttr(name),
                                            field, b.getStringAttr(visibility))});
}
void annotate(OpBuilder &b, Operation *op, unsigned count, unsigned port) {
  SmallVector<Attribute> annotations(count, b.getArrayAttr({}));
  annotations[port] = b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.UnmappedClockMetadata"))})});
  op->setAttr("portAnnotations", b.getArrayAttr(annotations));
}
void runOutputs(MLIRContext &context, bool symbols, bool emptyBody,
                unsigned rejection) {
  auto root = parseSourceString<ModuleOp>(R"mlir(
module { firrtl.circuit "Top" {
  firrtl.module @Top(in %hostClock: !firrtl.clock, out %clockA: !firrtl.clock,
      in %data: !firrtl.uint<8>, out %clockB: !firrtl.clock,
      out %result: !firrtl.uint<8>) {}
  firrtl.module @Model(in %clock: !firrtl.clock, out %clockA: !firrtl.clock,
      in %data: !firrtl.uint<8>, out %clockB: !firrtl.clock,
      out %result: !firrtl.uint<8>) {}
} })mlir", &context);
  require(bool(root), "output-clock fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>();
  auto top = *modules.begin();
  auto model = *std::next(modules.begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  auto instanceSymbol = symbol(b, "instance_id", "private");
  instance.setInnerSymAttr(instanceSymbol);
  instance->setAttr("test.metadata", b.getStringAttr("retained"));
  for (unsigned i = 0; i < top.getNumPorts(); ++i) {
    auto t = top.getBodyBlock()->getArgument(i);
    auto m = instance.getResult(i);
    b.create<StrictConnectOp>(top.getLoc(), i == 0 || i == 2 ? m : t,
                             i == 0 || i == 2 ? t : m);
  }
  b.setInsertionPointToEnd(model.getBodyBlock());
  NodeOp clockConsumer;
  if (!emptyBody) {
    for (unsigned i : {1u, 3u})
      b.create<StrictConnectOp>(model.getLoc(), model.getBodyBlock()->getArgument(i),
                               model.getBodyBlock()->getArgument(0));
    clockConsumer = b.create<NodeOp>(model.getLoc(), model.getBodyBlock()->getArgument(3),
                                     "clockConsumer");
    b.create<StrictConnectOp>(model.getLoc(), model.getBodyBlock()->getArgument(4),
                             model.getBodyBlock()->getArgument(2));
  }
  auto aSymbol = symbol(b, "clock_a_id", "private");
  auto bSymbol = symbol(b, "clock_b_id", "public");
  if (symbols) {
    model.setPortSymbolsAttr(1, aSymbol);
    model.setPortSymbolsAttr(3, bSymbol);
  }
  auto dataSymbol = symbol(b, "data_id", "public");
  model.setPortSymbolsAttr(2, dataSymbol);
  top.setPortSymbolsAttr(2, dataSymbol); // Independent module namespaces.
  auto reference = [&](StringRef module, StringRef name) {
    return circt::hw::InnerRefAttr::get(b.getStringAttr(module), b.getStringAttr(name));
  };
  SmallVector<Attribute> refs{reference("Top", "instance_id"), reference("Top", "data_id"),
                              reference("Model", "data_id")};
  if (symbols) refs.append({reference("Model", "clock_a_id"), reference("Model", "clock_b_id")});
  circuit->setAttr("test.stable_refs", b.getArrayAttr(refs));
  auto resolve = [&](circt::hw::InnerRefAttr ref) {
    SymbolTable modules(circuit);
    circt::hw::InnerSymbolTableCollection tables;
    circt::hw::InnerRefNamespace names{modules, tables};
    return names.lookup(ref);
  };
  if (symbols)
    require(resolve(reference("Model", "clock_a_id")).isPort() &&
                resolve(reference("Model", "clock_b_id")).getPort() == 3,
            "initial clock references are wrong");
  require(succeeded(verify(*root)), "initial clock fixture is invalid");
  // Fail on the second clock after the first plan has been collected.
  if (rejection == 1) top.setPortSymbolsAttr(3, symbol(b, "wrapper_clock", "public"));
  if (rejection == 2) model.setPortSymbolsAttr(3, symbol(b, "clock_b_id", "public", 1));
  if (rejection == 3) annotate(b, model, 5, 3);
  if (rejection == 4) annotate(b, top, 5, 3);
  if (rejection == 5) annotate(b, instance, 5, 3);
  if (rejection == 6) b.create<WireOp>(model.getLoc(), ClockType::get(&context), "clockB");
  if (rejection == 7) {
    b.setInsertionPointToEnd(top.getBodyBlock());
    b.create<InstanceOp>(top.getLoc(), model, "second_model");
  }
  if (rejection == 8) {
    b.setInsertionPointToEnd(top.getBodyBlock());
    b.create<NodeOp>(top.getLoc(), top.getBodyBlock()->getArgument(3), "extraClockUser");
  }
  auto before = dump(*root);
  std::string error;
  auto result = goldengate::internalizeFAMEOutputClocks(top, model, "model", error);
  if (rejection) {
    require(failed(result) && !error.empty(), "unsafe clock-output plan accepted");
    require(dump(*root) == before, "rejected clock-output plan mutated IR");
    return;
  }
  require(succeeded(result) && succeeded(verify(*root)), "clock internalization failed");
  auto check = [&] {
    require(model.getNumPorts() == 3 && top.getNumPorts() == 3,
            "clock output port removal is wrong");
    for (auto module : {top, model})
      require(module.getPortName(0) == (module == top ? "hostClock" : "clock") &&
                  module.getPortName(1) == "data" && module.getPortName(2) == "result",
              "unrelated port order changed");
    unsigned wires = 0;
    for (auto wire : model.getOps<WireOp>()) {
      auto name = wire.getName();
      require(isa<ClockType>(wire.getResult().getType()) && (name == "clockA" || name == "clockB"),
              "replacement wire has wrong name/type");
      ++wires;
      auto expected = name == "clockA" ? aSymbol : bSymbol;
      if (symbols) {
        require(wire.getInnerSymAttr() == expected, "clock symbol/visibility changed");
        auto target = resolve(reference("Model", expected.getSymName()));
        require(target && !target.isPort() && target.getOp() == wire && target.getField() == 0,
                "stable clock InnerRef did not resolve to its wire");
      } else require(!wire.getInnerSymAttr(), "symbol fabricated for anonymous clock");
      if (!emptyBody) {
        bool driven = false;
        for (OpOperand &use : wire.getResult().getUses())
          if (auto c = dyn_cast<StrictConnectOp>(use.getOwner()))
            if (c.getDest() == wire.getResult()) {
              require(c.getSrc() == model.getBodyBlock()->getArgument(0), "clock driver changed");
              driven = true;
            }
        require(driven, "clock wire driver lost");
        if (name == "clockB") require(clockConsumer.getInput() == wire.getResult(), "clock user lost");
      }
    }
    require(wires == 2, "replacement clock wires missing");
    auto newInstance = *top.getOps<InstanceOp>().begin();
    require(newInstance.getInnerSymAttr() == instanceSymbol &&
                newInstance->getAttrOfType<StringAttr>("test.metadata").getValue() == "retained",
            "unrelated instance metadata lost");
    require(resolve(reference("Top", "instance_id")).getOp() == newInstance,
            "stable instance reference lost");
    for (auto module : {top, model}) {
      auto target = resolve(reference(module.getName(), "data_id"));
      require(target.isPort() && target.getOp() == module && target.getPort() == 1 &&
                  module.getPorts()[1].sym == dataSymbol,
              "unrelated data identity changed");
    }
    require(circuit->getAttr("test.stable_refs") == b.getArrayAttr(refs), "InnerRefs changed");
  };
  check();
  PassManager pm(&context);
  pm.addNestedPass<CircuitOp>(createLowerFIRRTLTypesPass());
  require(succeeded(pm.run(*root)) && succeeded(verify(*root)), "LowerTypes failed");
  check();
}
void runInput(MLIRContext &context, unsigned rejection) {
  auto root = parseSourceString<ModuleOp>(R"mlir(
module { firrtl.circuit "Top" {
  firrtl.module @Top(in %clock: !firrtl.clock, in %data: !firrtl.uint<8>) {}
  firrtl.module @Model(in %clock: !firrtl.clock, in %data: !firrtl.uint<8>) {}
} })mlir", &context);
  require(bool(root), "input-clock fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>();
  auto top = *modules.begin();
  auto model = *std::next(modules.begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  auto identity = symbol(b, "instance_id", "private");
  instance.setInnerSymAttr(identity);
  instance->setAttr("test.metadata", b.getStringAttr("retained"));
  for (unsigned i = 0; i < 2; ++i)
    b.create<StrictConnectOp>(top.getLoc(), instance.getResult(i), top.getBodyBlock()->getArgument(i));
  if (rejection == 1) top.setPortSymbolsAttr(0, symbol(b, "top_clock", "public"));
  if (rejection == 2) model.setPortSymbolsAttr(0, symbol(b, "model_clock", "public"));
  if (rejection == 3) annotate(b, instance, 2, 0);
  auto before = dump(*root);
  std::string error;
  auto result = goldengate::removeFAMETargetClockPort(top, model, "model", "clock", "clock", error);
  if (rejection) {
    require(failed(result) && !error.empty(), "unsafe input-clock deletion accepted");
    require(dump(*root) == before, "rejected input-clock deletion mutated IR");
    return;
  }
  require(succeeded(result) && succeeded(verify(*root)), "input-clock deletion failed");
  auto replacement = *top.getOps<InstanceOp>().begin();
  require(replacement.getInnerSymAttr() == identity && replacement->getAttr("test.metadata") &&
              top.getNumPorts() == 1 && model.getNumPorts() == 1 &&
              top.getPortName(0) == "data" && model.getPortName(0) == "data",
          "input-clock deletion lost unrelated metadata/ports");
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (bool symbols : {false, true})
      for (bool empty : {false, true}) runOutputs(context, symbols, empty, 0);
    for (unsigned rejection = 1; rejection <= 8; ++rejection)
      runOutputs(context, true, false, rejection);
    for (unsigned rejection = 0; rejection <= 3; ++rejection) runInput(context, rejection);
    llvm::outs() << "4 clock-output identity/LowerTypes cases and input-clock deletion passed; "
                    "11 unsafe plans rejected without mutation\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
