// See LICENSE for license details.
#include "goldengate/FAMEAnnotations.h"
#include "goldengate/AnnotationClasses.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace mlir;
using namespace circt::firrtl;

namespace {
bool isDontTouch(Attribute attr) {
  auto dict = dyn_cast<DictionaryAttr>(attr);
  auto cls = dict ? dict.getAs<StringAttr>("class") : StringAttr();
  return cls && cls.getValue() == goldengate::AnnotationClasses::DontTouch;
}
ArrayAttr filterAttached(ArrayAttr annotations) {
  SmallVector<Attribute> retained;
  for (Attribute attr : annotations)
    if (!isDontTouch(attr))
      retained.push_back(attr);
  return ArrayAttr::get(annotations.getContext(), retained);
}
} // namespace

LogicalResult goldengate::consumeFAMEModelDontTouches(
    CircuitOp circuit, ArrayRef<FModuleOp> models, std::string &error) {
  auto reject = [&](llvm::StringRef reason) {
    error = reason.str();
    return failure();
  };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw)
    return reject("FAME DontTouch consumption needs retained annotations");
  llvm::SmallPtrSet<Operation *, 4> selected;
  for (auto model : models) {
    if (!model || model->getParentOp() != circuit.getOperation() ||
        model.getName() == circuit.getName() ||
        !selected.insert(model.getOperation()).second)
      return reject("FAME DontTouch consumption requires distinct non-top models");
  }
  SmallVector<Attribute> retained;
  for (Attribute attr : raw) {
    bool consume = false;
    if (isDontTouch(attr)) {
      auto target = cast<DictionaryAttr>(attr).getAs<StringAttr>("target");
      if (!target)
        return reject("DontTouchAnnotation has no reference target");
      llvm::StringRef spelling = target.getValue();
      if (!spelling.consume_front("~"))
        return reject("DontTouchAnnotation target must begin with '~'");
      auto circuitAndLocal = spelling.split('|');
      auto moduleAndRef = circuitAndLocal.second.split('>');
      // ReferenceTarget.moduleTarget names the root module, even for an
      // instance path. Do not resolve the leaf: FAME removes this protection
      // regardless of whether the reference becomes a wire or channel port.
      auto rootModule = moduleAndRef.first.split('/').first;
      if (circuitAndLocal.first.empty() || rootModule.empty() ||
          moduleAndRef.second.empty() || moduleAndRef.second.contains('>'))
        return reject("DontTouchAnnotation is not a reference target");
      if (circuitAndLocal.first == circuit.getName())
        for (auto model : models)
          consume |= rootModule == model.getName();
    }
    if (!consume)
      retained.push_back(attr);
  }
  // All preflight checks above precede both archive and attached IR changes.
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
  for (auto model : models) {
    model.walk([&](Operation *op) {
      if (auto annotations = op->getAttrOfType<ArrayAttr>("annotations"))
        op->setAttr("annotations", filterAttached(annotations));
      // Instance-port protections inside this body are also rooted in the
      // transformed model. Parent instances live outside this walk and keep
      // their wrapper-rooted metadata.
      if (auto ports = op->getAttrOfType<ArrayAttr>("portAnnotations")) {
        SmallVector<Attribute> updated;
        for (Attribute annotations : ports)
          updated.push_back(filterAttached(cast<ArrayAttr>(annotations)));
        op->setAttr("portAnnotations", ArrayAttr::get(circuit.getContext(), updated));
      }
    });
  }
  return success();
}
