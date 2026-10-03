// See LICENSE for license details.
#include "goldengate/TargetUtils.h"

using namespace circt::firrtl;

namespace {
// Select a typed local reference using CIRCT field IDs. Ports and internal
// declarations share the same bundle/vector path semantics.
bool selectField(FIRRTLBaseType &type, llvm::StringRef path,
                 uint64_t &fieldID, std::string &groundName,
                 std::string &error) {
  while (!path.empty()) {
    if (path.consume_front(".")) {
      auto end = path.find_first_of(".[");
      llvm::StringRef field = path.take_front(end);
      if (field.empty()) {
        error = "empty aggregate field in target";
        return false;
      }
      if (auto bundle = mlir::dyn_cast<BundleType>(type)) {
        auto index = bundle.getElementIndex(field);
        if (!index) {
          error = "target bundle field does not exist: " + field.str();
          return false;
        }
        fieldID += bundle.getFieldID(*index);
        type = bundle.getElements()[*index].type;
      } else if (auto vector = mlir::dyn_cast<FVectorType>(type)) {
        unsigned index;
        if (field.getAsInteger(10, index) ||
            index >= vector.getNumElements()) {
          error = "target vector index is invalid: " + field.str();
          return false;
        }
        fieldID += vector.getFieldID(index);
        type = vector.getElementType();
      } else {
        error = "target selects a field of a ground value";
        return false;
      }
      groundName += "_" + field.str();
      path = path.drop_front(field.size());
    } else if (path.consume_front("[")) {
      auto close = path.find(']');
      llvm::StringRef indexText = path.take_front(close);
      auto vector = mlir::dyn_cast<FVectorType>(type);
      unsigned index;
      if (!vector || close == llvm::StringRef::npos ||
          indexText.getAsInteger(10, index) ||
          index >= vector.getNumElements()) {
        error = "target vector index is invalid: " + indexText.str();
        return false;
      }
      fieldID += vector.getFieldID(index);
      type = vector.getElementType();
      groundName += "_" + indexText.str();
      path = path.drop_front(close + 1);
    } else {
      error = "invalid aggregate target path";
      return false;
    }
  }
  return true;
}
} // namespace

std::optional<goldengate::GGTarget>
goldengate::resolveAnnotationTarget(CircuitOp circuit, llvm::StringRef spelling,
                                    std::string &error) {
  if (!spelling.consume_front("~")) {
    error = "target must begin with '~'";
    return std::nullopt;
  }
  auto circuitAndLocal = spelling.split('|');
  if (circuitAndLocal.second.empty() ||
      circuitAndLocal.first != circuit.getName()) {
    error = "target has no module or names a different circuit";
    return std::nullopt;
  }
  auto moduleAndPort = circuitAndLocal.second.split('>');
  if (moduleAndPort.first.empty() || moduleAndPort.second.contains('>') ||
      moduleAndPort.first.contains('/')) {
    error = "hierarchical targets are not supported yet";
    return std::nullopt;
  }
  FModuleLike module;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto candidate = mlir::dyn_cast<FModuleLike>(&op);
    if (candidate && candidate.getModuleName() == moduleAndPort.first) {
      module = candidate;
      break;
    }
  }
  if (!module) {
    error = "target module does not exist: " + moduleAndPort.first.str();
    return std::nullopt;
  }
  if (moduleAndPort.second.empty())
    return GGTarget{circuit, module, std::nullopt};
  llvm::StringRef portAndPath = moduleAndPort.second;
  auto firstField = portAndPath.find_first_of(".[");
  llvm::StringRef portName = portAndPath.take_front(firstField);
  for (unsigned i = 0, n = module.getPorts().size(); i != n; ++i) {
    if (module.getPortName(i) != portName)
      continue;
    auto type = mlir::dyn_cast<FIRRTLBaseType>(module.getPorts()[i].type);
    if (!type) {
      error = "target port is not a FIRRTL base type: " + portName.str();
      return std::nullopt;
    }
    uint64_t fieldID = 0;
    std::string groundName = portName.str();
    if (!selectField(type, portAndPath.drop_front(portName.size()), fieldID,
                     groundName, error))
      return std::nullopt;
    return GGTarget{circuit, module, i, fieldID, std::move(groundName)};
  }
  error = "target port does not exist: " + portName.str();
  return std::nullopt;
}

mlir::Operation *goldengate::resolveInternalAnnotationTarget(
    CircuitOp circuit, llvm::StringRef spelling, std::string &error) {
  llvm::StringRef target = spelling;
  if (!target.consume_front("~")) {
    error = "target must begin with '~'";
    return nullptr;
  }
  auto circuitAndLocal = target.split('|');
  auto moduleAndRef = circuitAndLocal.second.split('>');
  if (circuitAndLocal.first != circuit.getName() ||
      moduleAndRef.first.empty() || moduleAndRef.second.empty() ||
      moduleAndRef.first.contains('/') ||
      moduleAndRef.second.find_first_of(".[]>/") != llvm::StringRef::npos) {
    error = "target is not a local declaration reference";
    return nullptr;
  }
  std::string moduleError;
  std::string moduleTarget = "~" + circuit.getName().str() + "|" +
                             moduleAndRef.first.str();
  auto resolved = resolveAnnotationTarget(circuit, moduleTarget, moduleError);
  if (!resolved) {
    error = moduleError;
    return nullptr;
  }
  mlir::Operation *match = nullptr;
  bool ambiguous = false;
  resolved->module.getOperation()->walk([&](mlir::Operation *op) {
    if (!mlir::isa<NodeOp, WireOp, RegOp, RegResetOp, MemOp>(op))
      return;
    auto name = op->getAttrOfType<mlir::StringAttr>("name");
    if (!name || name.getValue() != moduleAndRef.second)
      return;
    if (match)
      ambiguous = true;
    match = op;
  });
  if (ambiguous) {
    error = "target declaration name is ambiguous: " +
            moduleAndRef.second.str();
    return nullptr;
  }
  if (!match)
    error = "target declaration does not exist: " +
            moduleAndRef.second.str();
  return match;
}

std::optional<goldengate::GGInternalTarget>
goldengate::resolveInternalFieldTarget(CircuitOp circuit,
                                        llvm::StringRef spelling,
                                        std::string &error) {
  auto split = spelling.split('>');
  auto local = split.second;
  auto root = local.take_front(local.find_first_of(".["));
  auto *op = resolveInternalAnnotationTarget(
      circuit, split.first.str() + ">" + root.str(), error);
  if (!op)
    return std::nullopt;
  // Memory references need separate multi-result/data-field target handling.
  if (!mlir::isa<NodeOp, WireOp, RegOp, RegResetOp>(op)) {
    error = "target is not a wire, node, or register";
    return std::nullopt;
  }
  auto type = mlir::dyn_cast<FIRRTLBaseType>(op->getResult(0).getType());
  uint64_t fieldID = 0;
  std::string groundName = root.str();
  if (!type || !selectField(type, local.drop_front(root.size()), fieldID,
                            groundName, error))
    return std::nullopt;
  return GGInternalTarget{op->getParentOfType<FModuleOp>(), op, fieldID, type};
}
