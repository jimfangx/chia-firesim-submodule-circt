// See LICENSE for license details.
#include "goldengate/FAMEAnnotations.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
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

LogicalResult goldengate::transferFAMEWrapperDontTouch(
    CircuitOp circuit, llvm::StringRef oldTarget,
    llvm::StringRef payloadTarget, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  std::string prefix = "~" + circuit.getName().str() + "|" +
                       circuit.getName().str() + ">";
  auto oldPort = oldTarget;
  if (!raw || !oldPort.consume_front(prefix) || oldPort.empty() ||
      oldPort.find_first_of(".[]/>|") != llvm::StringRef::npos) {
    error = "FAME DontTouch transfer requires a local wrapper ground port";
    return failure();
  }
  auto replacement = resolveAnnotationTarget(circuit, payloadTarget, error);
  auto payload = payloadTarget;
  if (!replacement || !replacement->port || !replacement->fieldID ||
      *replacement->fieldID == 0 ||
      replacement->module.getModuleName() != circuit.getName() ||
      !payload.consume_front(prefix)) {
    error = "FAME DontTouch replacement must resolve to a wrapper payload field";
    return failure();
  }
  auto firstField = payload.find('.');
  auto field = firstField == llvm::StringRef::npos
                   ? llvm::StringRef() : payload.drop_front(firstField);
  if (field != ".bits" && !field.starts_with(".bits.")) {
    error = "FAME DontTouch replacement is not a channel bits field";
    return failure();
  }
  SmallVector<Attribute> updated;
  for (Attribute attr : raw) {
    if (isDontTouch(attr)) {
      auto dict = cast<DictionaryAttr>(attr);
      auto target = dict.getAs<StringAttr>("target");
      if (!target) {
        error = "DontTouchAnnotation has no reference target";
        return failure();
      }
      if (target.getValue() == oldTarget) {
        NamedAttrList transferred(dict);
        transferred.set("target", StringAttr::get(circuit.getContext(), payloadTarget));
        attr = transferred.getDictionary(circuit.getContext());
      }
    }
    updated.push_back(attr);
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), updated));
  return success();
}
