// See LICENSE for license details.
#include "goldengate/GlobalResetWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"

using namespace mlir;
using namespace circt::firrtl;

namespace {
bool isSource(Annotation annotation) {
  return annotation.isClass(goldengate::AnnotationClasses::GlobalResetSource) ||
         annotation.isClass(goldengate::AnnotationClasses::PublicGlobalResetSource);
}
bool isSink(Annotation annotation) {
  return annotation.isClass(goldengate::AnnotationClasses::GlobalResetSink) ||
         annotation.isClass(goldengate::AnnotationClasses::PublicGlobalResetSink);
}
struct Reference { FModuleOp module; Value value; };
std::optional<Reference> resolve(CircuitOp circuit, StringRef target, bool sink,
                               std::string &error) {
  std::string portError;
  if (auto port = goldengate::resolveAnnotationTarget(circuit, target, portError)) {
    auto module = dyn_cast<FModuleOp>(port->module.getOperation());
    if (!module || !port->port || port->fieldID.value_or(0) != 0 ||
        (sink && module.getPortDirection(*port->port) != Direction::Out)) {
      error = "global reset needs a lowered ground reference; port sinks must be outputs: " + target.str();
      return std::nullopt;
    }
    return Reference{module, module.getBodyBlock()->getArgument(*port->port)};
  }
  auto *op = goldengate::resolveInternalAnnotationTarget(circuit, target, error);
  if (!op || !isa<WireOp, NodeOp, RegOp, RegResetOp>(op) ||
      (sink && !isa<WireOp>(op))) {
    error = "global reset needs a local lowered declaration; internal sinks must be wires: " + target.str();
    return std::nullopt;
  }
  auto module = op->getParentOfType<FModuleOp>();
  if (!module || op->getBlock() != module.getBodyBlock()) {
    error = "global reset declaration must be at module scope: " + target.str();
    return std::nullopt;
  }
  return Reference{module, op->getResult(0)};
}
} // namespace

LogicalResult goldengate::wireLocalGlobalReset(CircuitOp circuit,
                                              unsigned &wired, std::string &error) {
  wired = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = "global reset wiring needs retained annotations"; return failure(); }
  SmallVector<Attribute> retained;
  SmallVector<StringAttr> sources, sinks;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!isSource(annotation) && !isSink(annotation)) {
      retained.push_back(attr);
      continue;
    }
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) { error = "global reset annotation has no target"; return failure(); }
    (isSource(annotation) ? sources : sinks).push_back(target);
  }
  // Scala's parameterized wiring transform requires at most one source even
  // when sinks are absent. It does not resolve targets when either side is absent.
  if (sources.size() > 1) { error = "received multiple global reset source annotations"; return failure(); }
  if (sources.empty() || sinks.empty()) {
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
    return success();
  }
  auto source = resolve(circuit, sources.front().getValue(), false, error);
  if (!source) return failure();
  auto isBool = [](Value value) {
    auto type = dyn_cast<UIntType>(value.getType());
    return type && type.getWidth() == 1;
  };
  if (!isBool(source->value)) { error = "global reset source must be UInt<1>"; return failure(); }
  SmallVector<Value> destinations;
  llvm::DenseSet<Value> seen;
  SmallVector<Operation *> oldDrivers;
  for (auto target : sinks) {
    auto sink = resolve(circuit, target.getValue(), true, error);
    if (!sink) return failure();
    if (sink->module != source->module || !isBool(sink->value) ||
        sink->value == source->value) {
      error = "global reset wiring currently needs distinct UInt<1> source and sinks in one module";
      return failure();
    }
    if (!seen.insert(sink->value).second) continue;
    destinations.push_back(sink->value);
    for (auto &use : sink->value.getUses()) {
      auto *op = use.getOwner();
      bool driver = false;
      if (auto connect = dyn_cast<StrictConnectOp>(op)) driver = connect.getDest() == sink->value;
      if (auto connect = dyn_cast<ConnectOp>(op)) driver = connect.getDest() == sink->value;
      if (!driver) continue;
      if (op->getBlock() != source->module.getBodyBlock()) {
        error = "global reset wiring requires ExpandWhens before replacing sink drivers";
        return failure();
      }
      oldDrivers.push_back(op);
    }
  }
  // Replace placeholder drivers, retaining the source operation and its
  // passthrough blocker logic. All validation above is read-only.
  for (auto *op : oldDrivers) op->erase();
  OpBuilder b(circuit.getContext());
  b.setInsertionPointToEnd(source->module.getBodyBlock());
  for (auto sink : destinations)
    b.create<StrictConnectOp>(source->module.getLoc(), sink, source->value);
  wired = destinations.size();
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
  return success();
}
