// See LICENSE for license details.
#include "goldengate/PrintStubs.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"

using namespace mlir;
using namespace circt::firrtl;

namespace {
// Scala PrintSynthesis accepts WRef clocks. Bind the corresponding native SSA
// port or named declaration; a computed clock requires a prior named node.
std::string clockReference(FModuleOp module, Value clock) {
  if (auto arg = dyn_cast<BlockArgument>(clock))
    return module.getPortName(arg.getArgNumber()).str();
  auto *op = clock.getDefiningOp();
  if (op && isa<WireOp, NodeOp, RegOp, RegResetOp>(op))
    return op->getAttrOfType<StringAttr>("name").getValue().str();
  return {};
}
} // namespace

LogicalResult goldengate::synthesizePrintStubs(
    CircuitOp circuit, SmallVectorImpl<PrintStub> &stubs, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "PrintSynthesis needs retained annotations";
    return failure();
  }
  llvm::DenseSet<Operation *> selected;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::SynthPrintf)) continue;
    auto target = annotation.getMember<StringAttr>("target");
    auto *op = target ? resolveInternalAnnotationTarget(circuit, target.getValue(), error) : nullptr;
    if (!op || !isa<PrintFOp>(op)) {
      error = "SynthPrintf target must resolve to a local printf: " +
              (target ? target.getValue().str() : std::string("<missing>"));
      return failure();
    }
    selected.insert(op); // Scala's exists/find synthesizes duplicate records once.
  }
  SmallVector<PrintStub> pending;
  // Traverse operations, preserving statement order independently of annotation order.
  for (auto module : circuit.getOps<FModuleOp>()) {
    module.walk([&](PrintFOp print) {
      if (!selected.contains(print.getOperation())) return;
      auto clock = print.getClock();
      auto prefix = "~" + circuit.getName().str() + "|" + module.getName().str() + ">";
      auto name = clock.getParentBlock() == module.getBodyBlock()
          ? clockReference(module, clock) : std::string();
      auto conditionType = dyn_cast<UIntType>(print.getCond().getType());
      bool valid = print->getBlock() == module.getBodyBlock() &&
          isa<ClockType>(clock.getType()) && !name.empty() && conditionType &&
          conditionType.getWidth() == 1 && print.getCond().getParentBlock() == module.getBodyBlock();
      for (auto arg : print.getSubstitutions())
        valid &= isa<UIntType, SIntType>(arg.getType()) &&
                 arg.getParentBlock() == module.getBodyBlock();
      if (!valid) {
        error = "PrintSynthesis needs module-scope integer operands and a named clock after ExpandWhens: " + prefix + print.getName().str();
        return;
      }
      pending.push_back({print, {}, clock, {}, prefix + name, print.getFormatString().str()});
    });
  }
  if (pending.size() != selected.size()) return failure();

  OpBuilder b(circuit.getContext());
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  FModuleOp current;
  circt::Namespace names;
  for (auto &stub : pending) {
    auto module = stub.print->getParentOfType<FModuleOp>();
    if (module != current) {
      current = module;
      names = circt::Namespace();
      for (auto name : module.getPortNamesAttr())
        names.newName(cast<StringAttr>(name).getValue());
      module.walk([&](Operation *op) {
        if (auto name = op->getAttrOfType<StringAttr>("name"))
          names.newName(name.getValue());
      });
    }
    auto loc = stub.print.getLoc();
    b.setInsertionPointAfter(stub.print);
    SmallVector<BundleType::BundleElement> fields{
        {b.getStringAttr("enable"), false, UIntType::get(b.getContext(), 1)}};
    for (auto [index, arg] : llvm::enumerate(stub.print.getSubstitutions()))
      fields.push_back({b.getStringAttr("args_" + std::to_string(index)), false,
                        cast<FIRRTLBaseType>(arg.getType())});
    auto bundle = b.create<WireOp>(loc, BundleType::get(b.getContext(), fields),
        names.newName(stub.print.getName().str() + "_wire"));
    auto enable = b.create<SubfieldOp>(loc, bundle.getResult(), 0);
    b.create<StrictConnectOp>(loc, enable.getResult(), stub.print.getCond());
    for (auto [index, arg] : llvm::enumerate(stub.print.getSubstitutions())) {
      auto field = b.create<SubfieldOp>(loc, bundle.getResult(), index + 1);
      b.create<StrictConnectOp>(loc, field.getResult(), arg);
    }
    stub.bundle = bundle;
    stub.target = "~" + circuit.getName().str() + "|" + module.getName().str() + ">" + bundle.getName().str();
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::BridgeTopWiring)),
        b.getNamedAttr("target", b.getStringAttr(stub.target)),
        b.getNamedAttr("clock", b.getStringAttr(stub.clockTarget))}));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  stubs.append(pending.begin(), pending.end());
  return success();
}
