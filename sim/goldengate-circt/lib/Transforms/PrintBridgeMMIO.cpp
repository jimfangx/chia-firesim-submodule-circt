// See LICENSE for license details.
// Compose Widget.scala's size-sorted control allocation and io.ctrl binding
// for the implemented Print hosts and their live CPU stream count bank.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlReadDispatch.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::mapPrintBridgeControlDispatch(CircuitOp circuit,
                                                       std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGCPUStreamCountWrapper")
    return reject("Print control dispatch requires the allocated CPU count wrapper");
  auto find = [&](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto bound = find(circuit, "GGPrintBridgeHostWrapper");
  auto top = find(circuit, circuit.getName());
  auto registry = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  if (!registry || registry.empty() || !top)
    return reject("Print control dispatch requires the bound host registry and active top");
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlWidgetPort> controls;
  llvm::StringSet<> identities, ports, hosts;
  for (auto attr : registry) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto port = row ? row.getAs<StringAttr>("controlPort") : StringAttr{};
    auto hostName = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    if (!name || !port || !hostName || name.getValue().empty() || port.getValue().empty() ||
        !identities.insert(name.getValue()).second || !ports.insert(port.getValue()).second ||
        !hosts.insert(hostName.getValue()).second)
      return reject("Print control dispatch requires unique complete host identities");
    auto host = find(circuit, hostName.getValue());
    auto info = host ? host->getAttrOfType<DictionaryAttr>("goldengate.printHost") : DictionaryAttr{};
    auto config = info ? info.getAs<StringAttr>("configModule") : StringAttr{};
    auto mcr = info ? info.getAs<StringAttr>("mcrModule") : StringAttr{};
    InstanceOp instance;
    for (auto i : bound.getOps<InstanceOp>()) if (i.getName() == name.getValue()) instance = i;
    if (!host || host.getNumPorts() != 13 || !config || !mcr || !instance ||
        instance.getModuleName() != host.getName())
      return reject("Print control dispatch host differs from the instantiated queued bank");
    bool found = false;
    for (auto p : top.getPorts()) if (p.name == port.getValue())
      found = p.direction == Direction::In && p.type == host.getPortType(11);
    if (!found) return reject("Print control dispatch local AXI bank is absent from the active top");
    ControlMMIOWidget widget;
    if (failed(deriveControlMMIOWidget(circuit, name.getValue(), mcr.getValue(),
          {config.getValue()}, widget, error))) return failure();
    if (widget.registerCount != 6) return reject("Print control dispatch requires six configuration words");
    widgets.push_back(widget);
    controls.push_back({name.getValue().str(), port.getValue().str()});
  }
  // A later bank/dispatcher can fail after earlier wrappers have been created.
  // Stage the whole operation so even such failures preserve existing IR IDs.
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(mapCPUStreamControl(*staged, 25, 12, error))) return failure();
  ControlMMIOWidget counts;
  if (failed(deriveControlMMIOWidget(*staged, "CPUManagedStreamEngine_0", "GGCPUStreamMCRFile",
        {"GGCPUStreamCountBank"}, counts, error))) return failure();
  widgets.push_back(counts);
  controls.push_back({"CPUManagedStreamEngine_0", "cpuStream_ctrl"});
  SmallVector<ControlMMIORegion> regions;
  if (failed(allocateControlMMIORegions(25, widgets, regions, error)) ||
      failed(addControlErrorSlave(*staged, 25, 12, "GGCPUStreamControlWrapper", error)) ||
      failed(addControlAddressDecode(*staged, 25, regions, error)) ||
      failed(addControlWriteRoute(*staged, error)) ||
      failed(addControlWriteDispatch(*staged, error)) ||
      failed(bindControlWidgetWrites(*staged, controls, error)) ||
      failed(addControlReadDispatch(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Print control dispatch produced invalid FIRRTL IR");
  llvm::StringSet<> originalNames;
  for (auto m : circuit.getOps<FModuleLike>()) originalNames.insert(m.getModuleName());
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleLike>(&op))
      if (!originalNames.count(m.getModuleName())) op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}
