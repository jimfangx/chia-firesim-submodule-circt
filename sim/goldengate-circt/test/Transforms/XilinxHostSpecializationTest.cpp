// See LICENSE for license details.
#include "goldengate/XilinxHostSpecialization.h"
#include "goldengate/XDCEmission.h"
#include "goldengate/AnnotationEmission.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, StringRef reason) {
  if (!value) throw std::runtime_error(reason.str());
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream stream(text);
  op->print(stream, OpPrintingFlags().useLocalScope()); return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>, out %O: !firrtl.clock) {
        %gate:3 = firrtl.instance gate @Alias(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock)
        firrtl.strictconnect %gate#0, %clock : !firrtl.clock
        firrtl.strictconnect %gate#1, %enable : !firrtl.uint<1>
        firrtl.strictconnect %O, %gate#2 : !firrtl.clock
        firrtl.when %enable : !firrtl.uint<1> {
          %nested:3 = firrtl.instance nested @Another(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock)
          firrtl.strictconnect %nested#0, %clock : !firrtl.clock
          firrtl.strictconnect %nested#1, %enable : !firrtl.uint<1>
        }
        %other:3 = firrtl.instance other @Unrelated(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock)
      }
      firrtl.extmodule private @Alias(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock) attributes {defname = "AbstractClockGate"}
      firrtl.extmodule private @Another(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock) attributes {defname = "AbstractClockGate"}
      firrtl.extmodule private @Unrelated(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock) attributes {defname = "AnotherGate"}
    }
  })mlir", &context);
  require(bool(root), "fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr("test.Retained")),
    b.getNamedAttr("target", b.getStringAttr("~Top|Top>gate.O"))})}));
  circuit.walk([&](InstanceOp instance) {
    if (instance.getName() == "gate") {
      instance->setAttr("goldengate.generatedClockConstraint", b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr("target_clock")),
        b.getNamedAttr("mfmr", b.getI64IntegerAttr(3))}));
      instance.setInnerSymAttr(circt::hw::InnerSymAttr::get(b.getStringAttr("stable_gate")));
      instance.setAnnotationsAttr(b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test.InstanceAnnotation"))})}));
    }
  });
  require(succeeded(verify(*root)), "fixture verify"); return root;
}
FExtModuleOp external(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FExtModuleOp>())
    if (module.getName() == name) return module;
  return {};
}
void checkSpecialization(CircuitOp circuit) {
  auto raw = circuit->getAttr("rawAnnotations");
  std::map<Operation *, std::string> original;
  struct InstanceState {InstanceOp op; DictionaryAttr attrs; SmallVector<Value> values;};
  SmallVector<InstanceState> instances;
  for (auto module : circuit.getOps<FModuleLike>()) original[module.getOperation()] = dump(module);
  circuit.walk([&](InstanceOp instance) {
    auto target = external(circuit, instance.getModuleName());
    if (target && target.getDefname() == "AbstractClockGate") {
      NamedAttrList attrs(instance->getAttrs()); attrs.set("moduleName", FlatSymbolRefAttr::get(circuit.getContext(), "BUFGCE"));
      instances.push_back({instance, attrs.getDictionary(circuit.getContext()), SmallVector<Value>(instance.getResults())});
    }
  });
  require(instances.size() > 0, "expected abstract instances");
  std::string error;
  require(succeeded(goldengate::specializeXilinxClockGates(circuit, error)), error);
  require(succeeded(verify(circuit)), "specialized IR verify");
  require(circuit->getAttr("rawAnnotations") == raw, "retained annotation changed");
  auto buffer = external(circuit, "BUFGCE");
  require(buffer && buffer.getDefname() == "BUFGCE" && buffer.getNumPorts() == 3 && buffer.getParameters().empty(), "BUFGCE declaration");
  for (auto &state : instances) {
    require(state.op->getAttrDictionary() == state.attrs, "instance attributes changed");
    for (auto [index, value] : llvm::enumerate(state.values))
      require(state.op.getResult(index) == value, "SSA result replaced");
  }
  // Normalize only the module-symbol operand. Every connection and all
  // unrelated operations must retain exactly their previous representation.
  for (auto &[op, before] : original) {
    std::string after = dump(op);
    for (auto alias : {"Alias", "Another", "AbstractClockGate"}) {
      auto target = external(circuit, alias);
      if (!target || target.getDefname() != "AbstractClockGate") continue;
      std::string from = "@" + std::string(alias) + "(";
      size_t offset = 0;
      while ((offset = before.find(from, offset)) != std::string::npos) {
        // External declarations stay unchanged.
        if (isa<FExtModuleOp>(op)) break;
        before.replace(offset, from.size(), "@BUFGCE("); offset += 8;
      }
    }
    require(before == after, "module changed beyond instance specialization");
  }
  llvm::outs() << "Specialized " << instances.size() << " gate instances; " << original.size()
               << " old modules, SSA results, connections, symbols and annotations retained\n";
}

// Project the rewritten gate and its actual CE expression, using registers
// and named control wires as cut-point inputs. No gate/control equations are
// reconstructed here: the FIRRTL operations are cloned from candidate IR.
OwningOpRef<ModuleOp> gateBoundary(CircuitOp circuit) {
  InstanceOp gate;
  circuit.walk([&](InstanceOp instance) {
    if (instance->hasAttr("goldengate.generatedClockConstraint")) {
      require(!gate, "multiple hub clocks in baseline"); gate = instance;
    }
  });
  require(bool(gate) && gate.getModuleName() == "BUFGCE", "specialized hub clock absent");
  auto model = gate->getParentOfType<FModuleOp>(); Value drivers[2];
  model.walk([&](StrictConnectOp connect) {
    for (unsigned i = 0; i < 2; ++i) if (connect.getDest() == gate.getResult(i)) {
      require(!drivers[i], "multiple gate drivers"); drivers[i] = connect.getSrc();
    }
  });
  require(drivers[0] && drivers[1], "missing gate drivers");
  SmallVector<Value> leaves; llvm::DenseMap<Value, unsigned> leafIndices;
  SmallVector<PortInfo> ports; auto *context = circuit.getContext();
  std::function<void(Value)> collect = [&](Value value) {
    auto op = value.getDefiningOp();
    if (!op || isa<RegOp, RegResetOp, WireOp>(op)) {
      if (leafIndices.count(value)) return;
      StringAttr name;
      if (auto arg = dyn_cast<BlockArgument>(value)) name = model.getPortNameAttr(arg.getArgNumber());
      else name = op->getAttrOfType<StringAttr>("name");
      require(bool(name), "unnamed control cut point");
      leafIndices[value] = leaves.size(); leaves.push_back(value);
      ports.push_back({name, value.getType(), Direction::In}); return;
    }
    require(isa<AndPrimOp, NotPrimOp, NodeOp, ConstantOp>(op), "unexpected clock-control operation");
    for (auto operand : op->getOperands()) collect(operand);
  };
  collect(drivers[0]); collect(drivers[1]);
  ports.push_back({StringAttr::get(context, "O"), ClockType::get(context), Direction::Out});
  auto loc = circuit.getLoc(); OwningOpRef<ModuleOp> root = ModuleOp::create(loc); OpBuilder b(context);
  b.setInsertionPointToEnd(root->getBody()); auto projected = b.create<CircuitOp>(loc, b.getStringAttr("ClockGateBoundary"));
  b.setInsertionPointToEnd(projected.getBodyBlock());
  b.clone(*external(circuit, "BUFGCE"));
  auto top = b.create<FModuleOp>(loc, b.getStringAttr("ClockGateBoundary"), ConventionAttr::get(context, Convention::Internal), ports);
  b.setInsertionPointToEnd(top.getBodyBlock()); IRMapping mapping;
  for (auto [index, value] : llvm::enumerate(leaves)) mapping.map(value, top.getArgument(index));
  std::function<Value(Value)> clone = [&](Value value) -> Value {
    if (mapping.contains(value)) return mapping.lookup(value);
    auto op = value.getDefiningOp();
    for (auto operand : op->getOperands()) clone(operand);
    b.clone(*op, mapping); return mapping.lookup(value);
  };
  Value I = clone(drivers[0]), CE = clone(drivers[1]);
  auto actualGate = cast<InstanceOp>(b.clone(*gate));
  b.create<StrictConnectOp>(loc, actualGate.getResult(0), I);
  b.create<StrictConnectOp>(loc, actualGate.getResult(1), CE);
  b.create<StrictConnectOp>(loc, top.getArgument(ports.size()-1), actualGate.getResult(2));
  require(succeeded(verify(*root)), "projected gate verify"); return root;
}
void write(Operation *op, StringRef path) {
  std::error_code error; llvm::raw_fd_ostream stream(path, error);
  require(!error, "boundary output open"); op->print(stream); stream << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::chirrtl::CHIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin(); checkSpecialization(circuit);
    std::string error; auto before = dump(*root);
    require(failed(goldengate::specializeXilinxClockGates(circuit, error)) && before == dump(*root), "repeat specialization changed IR");
    for (unsigned mode = 0; mode < 5; ++mode) {
      auto bad = fixture(context); auto bc = *bad->getOps<CircuitOp>().begin(); OpBuilder b(&context); auto alias = external(bc, "Alias");
      if (mode < 2) {
        b.setInsertionPointToEnd(bc.getBodyBlock());
        if (mode == 0) b.create<FModuleOp>(bc.getLoc(), b.getStringAttr("BUFGCE"), ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
        else b.create<FExtModuleOp>(bc.getLoc(), b.getStringAttr("BUFGCE"), ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{}, "OtherBuffer");
      } else {
        auto text = dump(*bad);
        auto from = mode == 2 ? "in CE: !firrtl.uint<1>" : mode == 3 ? "out O: !firrtl.clock" : "in I: !firrtl.clock";
        auto to = mode == 2 ? "in CE: !firrtl.uint<2>" : mode == 3 ? "in O: !firrtl.clock" : "in I: !firrtl.uint<1>";
        auto offset = text.find(from, text.find("firrtl.extmodule private @Alias"));
        require(offset != std::string::npos, "negative schema fixture"); text.replace(offset, strlen(from), to);
        bad = parseSourceString<ModuleOp>(text, ParserConfig(&context, false)); require(bool(bad), "negative parse"); bc = *bad->getOps<CircuitOp>().begin();
      }
      before = dump(*bad);
      require(failed(goldengate::specializeXilinxClockGates(bc, error)) && !error.empty() && before == dump(*bad), "invalid specialization changed IR");
    }
    auto empty = parseSourceString<ModuleOp>("module { firrtl.circuit \"Empty\" { firrtl.module @Empty() {} } }", &context);
    require(bool(empty), "empty parse"); auto ec = *empty->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::specializeXilinxClockGates(ec, error)) && succeeded(verify(*empty)) && external(ec, "BUFGCE"), "zero-instance declaration");
    llvm::outs() << "Aliased/nested instances, unrelated defname, empty circuit; six atomic rejections passed\n";
    if (argc == 3) {
      auto real = parseSourceFile<ModuleOp>(argv[1], &context); require(bool(real), "real boundary parse"); auto rc = *real->getOps<CircuitOp>().begin();
      checkSpecialization(rc); llvm::SmallString<256> path(argv[2]);
      llvm::sys::path::append(path, "post-xilinx-host-specialization.mlir"); write(*real, path);
      auto projected = gateBoundary(rc); path = argv[2]; llvm::sys::path::append(path, "clock-gate-boundary.mlir"); write(*projected, path);
      path = argv[2]; llvm::sys::path::append(path, "post-xilinx-host-specialization-all.json");
      require(succeeded(goldengate::emitAllAnnotations(rc, path, error)), error);
      require(succeeded(goldengate::prepareXDCOutput(rc, error)), error);
      require(succeeded(goldengate::emitOutputFiles(rc, argv[2], "FireSim-generated", error)), error);
      llvm::outs() << "Real hub clock/control operations projected; XDC and DDR collateral emitted\n";
    }
    return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
