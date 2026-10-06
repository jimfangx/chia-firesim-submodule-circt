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
          "unresolved output " + output.str() + ": " +
              (row->unresolvedCauses.empty() ? "" : row->unresolvedCauses.front()));
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

void selectedMemoryData(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Model" {
      firrtl.module @Model(in %clock: !firrtl.clock,
          in %address: !firrtl.uint<1>, in %enable: !firrtl.uint<1>,
          out %async: !firrtl.uint<8>, out %sync: !firrtl.uint<8>,
          out %alias: !firrtl.uint<8>) {
        %read = firrtl.mem Undefined {depth = 2 : i64, name = "async_ram",
          portNames = ["r"], readLatency = 0 : i32, writeLatency = 1 : i32}
          : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock,
                           data flip: vector<uint<8>, 2>>
        %addr = firrtl.subfield %read[addr] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        %en = firrtl.subfield %read[en] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        %clk = firrtl.subfield %read[clk] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        firrtl.strictconnect %addr, %address : !firrtl.uint<1>
        firrtl.strictconnect %en, %enable : !firrtl.uint<1>
        firrtl.strictconnect %clk, %clock : !firrtl.clock
        %data = firrtl.subfield %read[data] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        %element = firrtl.subindex %data[1] : !firrtl.vector<uint<8>, 2>
        firrtl.strictconnect %async, %element : !firrtl.uint<8>
        %data_alias = firrtl.node %data : !firrtl.vector<uint<8>, 2>
        %alias_element = firrtl.subindex %data_alias[0] : !firrtl.vector<uint<8>, 2>
        firrtl.strictconnect %alias, %alias_element : !firrtl.uint<8>
        %sync_read = firrtl.mem Undefined {depth = 2 : i64, name = "sync_ram",
          portNames = ["r"], readLatency = 1 : i32, writeLatency = 1 : i32}
          : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock,
                           data flip: vector<uint<8>, 2>>
        %sync_addr = firrtl.subfield %sync_read[addr] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        %sync_en = firrtl.subfield %sync_read[en] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        %sync_clk = firrtl.subfield %sync_read[clk] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        firrtl.strictconnect %sync_addr, %address : !firrtl.uint<1>
        firrtl.strictconnect %sync_en, %enable : !firrtl.uint<1>
        firrtl.strictconnect %sync_clk, %clock : !firrtl.clock
        %sync_data = firrtl.subfield %sync_read[data] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, data flip: vector<uint<8>, 2>>
        %sync_element = firrtl.subindex %sync_data[0] : !firrtl.vector<uint<8>, 2>
        firrtl.strictconnect %sync, %sync_element : !firrtl.uint<8>
      }
    }
  })mlir", &context);
  require(bool(root), "memory fixture parse failed");
  require(succeeded(verify(*root)), "memory fixture is invalid");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  expect(model, "async", {"address", "enable"});
  expect(model, "alias", {"address", "enable"});
  expect(model, "sync", {});
  auto memory = *model.getOps<MemOp>().begin();
  memory.setRuw(RUWAttr::New);
  auto dependencies = analyze(model);
  require(llvm::any_of(dependencies.front().unresolvedCauses, [](auto &cause) {
    return StringRef(cause).ends_with(":new read-under-write memory");
  }), "new read-under-write was silently treated as stored-state-only data");
}

void asynchronousReadWrite(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Model" {
      firrtl.module @Model(in %clock: !firrtl.clock,
          in %address: !firrtl.uint<1>, in %enable: !firrtl.uint<1>,
          in %mode: !firrtl.uint<1>, in %write: !firrtl.uint<8>,
          out %out: !firrtl.uint<8>) {
        %rw = firrtl.mem Undefined {depth = 2 : i64, name = "ram",
          portNames = ["rw"], readLatency = 0 : i32, writeLatency = 1 : i32}
          : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock,
              rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %addr = firrtl.subfield %rw[addr] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %en = firrtl.subfield %rw[en] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %clk = firrtl.subfield %rw[clk] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %data = firrtl.subfield %rw[rdata] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %wmode = firrtl.subfield %rw[wmode] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %wdata = firrtl.subfield %rw[wdata] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %wmask = firrtl.subfield %rw[wmask] : !firrtl.bundle<addr: uint<1>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>
        %one = firrtl.constant 1 : !firrtl.uint<1>
        firrtl.strictconnect %addr, %address : !firrtl.uint<1>
        firrtl.strictconnect %en, %enable : !firrtl.uint<1>
        firrtl.strictconnect %clk, %clock : !firrtl.clock
        firrtl.strictconnect %wmode, %mode : !firrtl.uint<1>
        firrtl.strictconnect %wdata, %write : !firrtl.uint<8>
        firrtl.strictconnect %wmask, %one : !firrtl.uint<1>
        firrtl.strictconnect %out, %data : !firrtl.uint<8>
      }
    }
  })mlir", &context);
  require(bool(root) && succeeded(verify(*root)), "readwrite fixture invalid");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  auto dependencies = analyze(model);
  require(dependencies.size() == 1 &&
      llvm::any_of(dependencies.front().unresolvedCauses, [](auto &cause) {
        return StringRef(cause).ends_with(":asynchronous readwrite memory");
      }), "asynchronous readwrite omitted write mode without a blocker");
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    conditionalDrivers(context);
    repeatedDrivers(context);
    selectedMemoryData(context);
    asynchronousReadWrite(context);
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
      for (StringRef field : {"data", "strb", "last"}) {
        expect(model, "io_deq_bits_" + field.str(),
               {"io_enq_bits_" + field.str()});
      }
      // Also exercise recursive instance tracing through the probe wrapper.
      auto probe = *circuit.getOps<FModuleOp>().begin();
      expect(probe, "io_deq_valid", {"io_enq_valid"});
      for (StringRef field : {"data", "strb", "last"})
        expect(probe, "io_deq_bits_" + field.str(),
               {"io_enq_bits_" + field.str()});
      if (argc == 3) {
        std::error_code ec;
        llvm::raw_fd_ostream out(argv[2], ec);
        require(!ec, "cannot write normalized golden MLIR: " + ec.message());
        root->print(out);
        out << '\n';
      }
      llvm::outs() << "Rocket Queue1_AXI4BundleW: io_deq_valid <- {io_enq_valid}; "
                      "io_enq_ready <- {} matched SFC RTL; "
                      "io_count <- {} matched SFC FIRRTL register boundary\n"
                      "Payload data/strb/last each depends only on matching "
                      "enqueue field, including through the probe instance; "
                      "matched SFC RTL\n";
    }
    llvm::outs() << "Nested when conditions, last-connect override and register "
                    "boundaries passed; static memory selections and aliases "
                    "follow async address/enable and stop at sync state; "
                    "unsupported memory modes rejected\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
