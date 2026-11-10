// See LICENSE for license details.
// Semantic oracle: midas/passes/xilinx/package.scala. Match external-module
// defnames, including aliases, rather than assuming an AbstractClockGate symbol.
#include "goldengate/XilinxHostSpecialization.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::specializeXilinxClockGates(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef message) {
    error = message.str();
    return failure();
  };
  auto *context = circuit.getContext();
  SmallVector<PortInfo> ports{
      {StringAttr::get(context, "I"), ClockType::get(context), Direction::In},
      {StringAttr::get(context, "CE"), UIntType::get(context, 1), Direction::In},
      {StringAttr::get(context, "O"), ClockType::get(context), Direction::Out}};
  llvm::DenseMap<Attribute, FExtModuleOp> abstractModules;
  FExtModuleOp existingBuffer;
  for (auto module : circuit.getOps<FModuleLike>()) {
    if (module.getModuleName() == "BUFGCE") {
      existingBuffer = dyn_cast<FExtModuleOp>(module.getOperation());
      if (!existingBuffer || !existingBuffer->hasAttr("goldengate.xilinxClockGate") ||
          existingBuffer.getDefname() != "BUFGCE" ||
          existingBuffer.getConvention() != Convention::Internal ||
          existingBuffer.getNumPorts() != ports.size() ||
          !existingBuffer.getParameters().empty() || !existingBuffer.getLayers().empty() ||
          !existingBuffer.getAnnotations().empty())
        return reject("Xilinx specialization requires an unused BUFGCE symbol or its canonical generated declaration");
      for (auto [index, port] : llvm::enumerate(ports))
        if (existingBuffer.getPortName(index) != port.name.getValue() ||
            existingBuffer.getPortType(index) != port.type ||
            existingBuffer.getPortDirection(index) != port.direction ||
            !existingBuffer.getAnnotationsForPort(index).empty() || existingBuffer.hasPortSymbolAttr(index))
          return reject("generated BUFGCE declaration has an incompatible schema");
      continue;
    }
    auto external = dyn_cast<FExtModuleOp>(module.getOperation());
    if (!external || external.getDefname() != "AbstractClockGate")
      continue;
    if (external.getNumPorts() != ports.size() ||
        !external.getParameters().empty())
      return reject("AbstractClockGate requires three ports and no parameters");
    for (auto [index, port] : llvm::enumerate(ports))
      if (external.getPortName(index) != port.name.getValue() ||
          external.getPortType(index) != port.type ||
          external.getPortDirection(index) != port.direction)
        return reject("AbstractClockGate requires I:Clock, CE:UInt<1>, O:Clock");
    abstractModules[external.getModuleNameAttr()] = external;
  }
  SmallVector<InstanceOp> instances;
  bool invalid = false;
  circuit.walk([&](InstanceOp instance) {
    auto found = abstractModules.find(instance.getModuleNameAttr().getAttr());
    if (found == abstractModules.end())
      return;
    if (!instance->getParentOfType<FModuleOp>() ||
        instance.getNumResults() != ports.size()) {
      invalid = true;
      return;
    }
    for (auto [index, port] : llvm::enumerate(ports))
      if (instance.getPortNameStr(index) != port.name.getValue() ||
          instance.getResult(index).getType() != port.type ||
          instance.getPortDirection(index) != port.direction)
        invalid = true;
    instances.push_back(instance);
  });
  if (invalid)
    return reject("abstract clock instance disagrees with its module schema");

  // Like the Scala oracle, keep the now-unused abstract declarations, and
  // define BUFGCE even when no abstract instances occur in this circuit.
  OpBuilder builder(context);
  builder.setInsertionPointToEnd(circuit.getBodyBlock());
  if (!existingBuffer) {
    auto buffer = builder.create<FExtModuleOp>(
        circuit.getLoc(), builder.getStringAttr("BUFGCE"),
        ConventionAttr::get(context, Convention::Internal), ports, "BUFGCE");
    buffer->setAttr("goldengate.xilinxClockGate", builder.getUnitAttr());
  }
  for (auto instance : instances)
    instance.setModuleName("BUFGCE");
  return success();
}
