// See LICENSE for license details.
#include "goldengate/CoerceAsyncToSyncReset.h"
#include "mlir/IR/Builders.h"

using namespace mlir;
using namespace circt::firrtl;

namespace {
Type coerce(Type type) {
  if (isa<ResetType, AsyncResetType>(type))
    return UIntType::get(type.getContext(), 1, isConst(type));
  // CIRCT may retain aggregates on external module ports. Preserve field IDs,
  // flips and const qualifiers while changing only reset leaves.
  if (auto bundle = dyn_cast<BundleType>(type)) {
    SmallVector<BundleType::BundleElement> fields(bundle.getElements());
    for (auto &field : fields)
      field.type = cast<FIRRTLBaseType>(coerce(field.type));
    return BundleType::get(type.getContext(), fields, bundle.isConst());
  }
  if (auto vector = dyn_cast<FVectorType>(type))
    return FVectorType::get(cast<FIRRTLBaseType>(coerce(vector.getElementType())),
                            vector.getNumElements(), vector.isConst());
  return type;
}
} // namespace

void goldengate::coerceAsyncToSyncReset(CircuitOp circuit) {
  SmallVector<AsAsyncResetPrimOp> casts;
  circuit.walk([&](Operation *op) {
    if (auto module = dyn_cast<FModuleLike>(op)) {
      SmallVector<Attribute> types;
      for (auto attr : module.getPortTypes())
        types.push_back(TypeAttr::get(coerce(cast<TypeAttr>(attr).getValue())));
      op->setAttr(FModuleLike::getPortTypesAttrName(),
                  ArrayAttr::get(op->getContext(), types));
    }
    for (auto result : op->getResults())
      result.setType(coerce(result.getType()));
    for (auto &region : op->getRegions())
      for (auto &block : region)
        for (auto arg : block.getArguments())
          arg.setType(coerce(arg.getType()));
    if (auto castOp = dyn_cast<AsAsyncResetPrimOp>(op))
      casts.push_back(castOp);
  });
  for (auto castOp : casts) {
    OpBuilder builder(castOp);
    OperationState state(castOp.getLoc(), AsUIntPrimOp::getOperationName());
    state.addOperands(castOp->getOperands());
    state.addTypes(castOp->getResultTypes());
    state.addAttributes(castOp->getAttrs());
    auto replacement = builder.create(state);
    castOp->replaceAllUsesWith(replacement->getResults());
    castOp.erase();
  }
}
