// See LICENSE for license details.
#include "goldengate/CombDependencyAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/LowerTypes.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::vector<goldengate::LocalChannelDependency> analyze(FModuleOp model) {
  // One channel per port makes missed and spurious dependencies observable.
  SmallVector<goldengate::ModelPortGroup> groups;
  for (auto [i, port] : llvm::enumerate(model.getPorts()))
    groups.push_back({port.name.getValue().str(), model, port.direction,
                      std::nullopt, {static_cast<unsigned>(i)}});
  SmallVector<goldengate::ModelChannelBinding> bindings;
  for (auto [i, group] : llvm::enumerate(groups))
    bindings.push_back({group.name, &group, {}, {static_cast<unsigned>(i)}});
  return goldengate::analyzeLocalChannelDependencies(model, bindings);
}
void expect(FModuleOp model, StringRef output, std::set<std::string> inputs) {
  auto dependencies = analyze(model);
  auto row = llvm::find_if(dependencies, [&](auto &d) {
    return d.outputChannel == output;
  });
  require(row != dependencies.end(), "missing output " + output.str());
  require(row->unresolvedPorts.empty() && row->unresolvedCauses.empty(),
          "unresolved output " + output.str());
  require(std::set<std::string>(row->inputChannels.begin(),
                               row->inputChannels.end()) == inputs,
          "wrong dependencies for " + output.str());
}
void conditionalDrivers(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Model" {
      firrtl.module @Model(in %clock: !firrtl.clock,
          in %select: !firrtl.uint<1>, in %nested: !firrtl.uint<1>,
          in %a: !firrtl.uint<8>, in %b: !firrtl.uint<8>,
          out %chosen: !firrtl.uint<8>, out %overwritten: !firrtl.uint<8>,
          out %state: !firrtl.uint<8>) {
        %q = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<8>
        firrtl.strictconnect %q, %a : !firrtl.uint<8>
        firrtl.strictconnect %chosen, %a : !firrtl.uint<8>
        firrtl.strictconnect %overwritten, %a : !firrtl.uint<8>
        firrtl.when %select : !firrtl.uint<1> {
          firrtl.strictconnect %chosen, %b : !firrtl.uint<8>
          firrtl.strictconnect %overwritten, %a : !firrtl.uint<8>
          firrtl.when %nested : !firrtl.uint<1> {
            firrtl.strictconnect %chosen, %a : !firrtl.uint<8>
            firrtl.strictconnect %q, %b : !firrtl.uint<8>
          }
        }
        firrtl.strictconnect %overwritten, %b : !firrtl.uint<8>
        firrtl.strictconnect %state, %q : !firrtl.uint<8>
      }
    }
  })mlir", &context);
  require(bool(root), "conditional fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  auto retained = b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DontTouch)),
      b.getNamedAttr("target", b.getStringAttr("~Model|Model>q"))})});
  circuit->setAttr("rawAnnotations", retained);
  auto unnormalized = analyze(model);
  require(!unnormalized.empty() &&
              !unnormalized.front().unresolvedCauses.empty(),
          "unnormalized when was silently analyzed");
  std::string error;
  require(succeeded(goldengate::normalizeFAMEInput(
              *root, circuit, error)), error);
  require(succeeded(verify(*root)), "conditional normalization invalid");
  require(circuit->getAttr("rawAnnotations") == retained,
          "conditional normalization changed retained register identity");
  bool hasWhen = false;
  circuit.walk([&](WhenOp) { hasWhen = true; });
  require(!hasWhen, "FAME normalization left unresolved when semantics");
  expect(model, "chosen", {"select", "nested", "a", "b"});
  expect(model, "overwritten", {"b"});
  expect(model, "state", {});
}
void repeatedDrivers(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Model" {
      firrtl.module @Model(in %a: !firrtl.uint<1>, in %b: !firrtl.uint<1>,
                          out %out: !firrtl.uint<1>) {
        firrtl.strictconnect %out, %a : !firrtl.uint<1>
        firrtl.strictconnect %out, %b : !firrtl.uint<1>
      }
    }
  })mlir", &context);
  require(bool(root), "multiple-driver fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  auto dependencies = analyze(model);
  require(dependencies.size() == 1 && !dependencies[0].unresolvedCauses.empty(),
          "last-connect priority was silently replaced by a dependency union");
  OpBuilder b(&context);
  circuit->setAttr("rawAnnotations", b.getArrayAttr({}));
  std::string error;
  require(succeeded(goldengate::normalizeFAMEInput(*root, circuit, error)), error);
  expect(model, "out", {"b"});
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    conditionalDrivers(context);
    repeatedDrivers(context);
    require(argc <= 3, "expected optional input MLIR and normalized output MLIR");
    if (argc >= 2) {
      // Optional immutable Rocket extraction, normalized by the real tool.
      auto root = parseSourceFile<ModuleOp>(argv[1], &context);
      require(bool(root), "golden module MLIR parse failed");
      auto circuit = *root->getOps<CircuitOp>().begin();
      std::string error;
      require(succeeded(goldengate::normalizeFAMEInput(*root, circuit, error)), error);
      require(succeeded(verify(*root)), "golden normalization invalid");
      FModuleOp model;
      for (auto candidate : circuit.getOps<FModuleOp>())
        if (candidate.getName() == "Queue1_AXI4BundleW") model = candidate;
      require(bool(model), "missing golden module");
      expect(model, "io_deq_valid", {"io_enq_valid"});
      expect(model, "io_enq_ready", {});
      expect(model, "io_count", {});
      // Also exercise recursive instance tracing through the probe wrapper.
      auto probe = *circuit.getOps<FModuleOp>().begin();
      expect(probe, "io_deq_valid", {"io_enq_valid"});
      if (argc == 3) {
        std::error_code ec;
        llvm::raw_fd_ostream out(argv[2], ec);
        require(!ec, "cannot write normalized golden MLIR: " + ec.message());
        root->print(out);
        out << '\n';
      }
      llvm::outs() << "Rocket Queue1_AXI4BundleW: io_deq_valid <- {io_enq_valid}; "
                      "io_enq_ready <- {} matched SFC RTL; "
                      "io_count <- {} matched SFC FIRRTL register boundary\n";
    }
    llvm::outs() << "Nested when conditions, last-connect override and register "
                    "boundaries passed\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
