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
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlReadArbiter.h"
#include "goldengate/ControlWriteArbiter.h"
#include "goldengate/ControlWriteTracker.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringMap.h"
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

namespace {
using namespace goldengate;
LogicalResult mapControlResponses(CircuitOp circuit, bool rocket,
                                  std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGControlReadDispatchWrapper")
    return reject("Print control responses require the selected AR dispatch wrapper");
  FModuleOp top, bound, decoder;
  for (auto m : circuit.getOps<FModuleOp>()) {
    if (m.getName() == circuit.getName()) top = m;
    if (m.getName() == "GGPrintBridgeHostWrapper") bound = m;
    if (m.getName() == "GGControlAddressDecode") decoder = m;
  }
  auto hosts = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  auto bindings = top ? top->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings") : ArrayAttr{};
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  if (!hosts || hosts.empty() || !bindings || bindings.size() != hosts.size() + (rocket ? 7 : 1) ||
      !regions || regions.size() != hosts.size() + (rocket ? 11 : 1))
    return reject("Print control responses require the complete selected or Rocket bank catalog");
  auto writes = [&]() -> ArrayAttr {
    for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == "GGControlWidgetWriteWrapper")
      return m->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
    return {};
  }();
  if (writes != bindings)
    return reject("Print control responses require identical AW/W and AR bank bindings");
  // Selected composition consumes all banks. Rocket composition leaves only
  // Master/FASED/TSI/BlockDev exposed, as in the native platform pipeline.
  llvm::StringMap<StringAttr> expected;
  SmallVector<FModuleOp> printHosts;
  llvm::StringSet<> hostNames;
  for (auto attr : hosts) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto port = row ? row.getAs<StringAttr>("controlPort") : StringAttr{};
    if (!name || !port || name.getValue().empty() || port.getValue().empty() ||
        !expected.try_emplace(name.getValue(), port).second)
      return reject("Print control responses require unique complete Print identities");
    if (rocket) {
      auto hostName = row.getAs<StringAttr>("hostModule"); FModuleOp host;
      if (hostName) for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == hostName) host = m;
      if (!host || !hostNames.insert(host.getName()).second ||
          name.getValue() != "PrintBridgeModule_" + std::to_string(printHosts.size()))
        return reject("Rocket Print responses require instantiated constructor order");
      printHosts.push_back(host);
    }
  }
  if (!expected.try_emplace("CPUManagedStreamEngine_0",
        StringAttr::get(circuit.getContext(), "cpuStream_ctrl")).second)
    return reject("Print control responses collide with the CPU count bank");
  if (rocket) {
    const std::pair<const char *, const char *> early[]{
        {"TracerVBridgeModule_0", "tracerv_ctrl"}, {"LoadMemWidget_0", "loadmem_ctrl"},
        {"PeekPokeBridgeModule_0", "peekPokeBridge_ctrl"}, {"UARTBridgeModule_0", "uartBridge_ctrl"},
        {"ClockBridgeModule_0", "clockBridge_ctrl"}, {"ResetPulseBridgeModule_0", "resetBridge_ctrl"}};
    for (auto [name, port] : early)
      if (!expected.try_emplace(name, StringAttr::get(circuit.getContext(), port)).second)
        return reject("Rocket Print response bank identity collides");
    SmallVector<goldengate::ControlMMIOWidget> widgets;
    SmallVector<goldengate::ControlMMIORegion> allocated;
    if (failed(goldengate::allocateRocketControlMMIORegions(circuit, 25, printHosts,
            widgets, allocated, error))) return failure();
    if (allocated.size() != regions.size()) return reject("Rocket response allocation count differs");
    for (auto [i, region] : llvm::enumerate(allocated)) {
      auto row = dyn_cast<DictionaryAttr>(regions[i]);
      auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
      auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr{};
      auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr{};
      auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr{};
      if (!name || name != region.name || !start || !size || !slave ||
          start.getValue().getBitWidth() > 64 || size.getValue().getBitWidth() > 64 ||
          slave.getValue().getBitWidth() > 64 || start.getInt() != int64_t(region.start) ||
          size.getInt() != int64_t(region.size) || slave.getInt() != int64_t(i))
        return reject("Rocket response region differs from the live register allocation");
    }
  }
  for (auto attr : bindings) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    auto port = row ? row.getAs<StringAttr>("port") : StringAttr{};
    auto it = name ? expected.find(name.getValue()) : expected.end();
    if (it == expected.end() || port != it->second)
      return reject("Print control response binding differs from the instantiated bank catalog");
    expected.erase(it);
  }
  if (!expected.empty()) return reject("Print control response bank is unbound");
  if (rocket) {
    // Catalog equality is insufficient if both rows are stale. The response
    // slave must be the same slave already connected to the bank's AR port.
    auto path = [](Value value) -> std::string {
      std::string suffix;
      while (auto field = value.getDefiningOp<SubfieldOp>()) {
        suffix = "." + field.getFieldName().str() + suffix; value = field.getInput();
      }
      if (auto inst = value.getDefiningOp<InstanceOp>())
        return inst.getName().str() + "." + inst.getPortNameStr(cast<OpResult>(value).getResultNumber()).str() + suffix;
      return {};
    };
    llvm::StringMap<std::string> ar;
    for (auto connect : top.getOps<ConnectOp>()) {
      auto dest = path(connect.getDest()), src = path(connect.getSrc());
      if (!dest.empty() && !src.empty() && !ar.try_emplace(dest, src).second)
        return reject("Rocket responses require uniquely driven AR request banks");
    }
    for (auto attr : bindings) {
      auto row = cast<DictionaryAttr>(attr);
      auto port = row.getAs<StringAttr>("port"); auto slave = row.getAs<IntegerAttr>("slave");
      auto name = row.getAs<StringAttr>("name");
      if (!slave || slave.getValue().getBitWidth() > 64 || slave.getInt() < 0 ||
          uint64_t(slave.getInt()) >= regions.size() ||
          cast<DictionaryAttr>(regions[slave.getInt()]).getAs<StringAttr>("name") != name)
        return reject("Rocket response binding differs from its allocated slave identity");
      auto found = ar.find("sim." + port.getValue().str() + ".ar");
      if (found == ar.end() || found->second != "controlReadDispatch.slave_" + std::to_string(slave.getInt()) + "_ar")
        return reject("Rocket response bank differs from its connected AR request slave");
    }
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addControlReadTracker(*staged, error)) ||
      failed(addControlReadArbiter(*staged, error)) ||
      failed(addControlWriteArbiter(*staged, error)) ||
      failed(addControlWriteTracker(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Print control responses produced invalid FIRRTL IR");
  llvm::StringSet<> originalNames;
  for (auto m : circuit.getOps<FModuleLike>()) originalNames.insert(m.getModuleName());
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleLike>(&op))
      if (!originalNames.count(m.getModuleName()))
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}
} // namespace

LogicalResult goldengate::mapPrintBridgeControlResponses(CircuitOp circuit,
                                                        std::string &error) {
  return mapControlResponses(circuit, false, error);
}

LogicalResult goldengate::mapPrintBridgeRocketControlResponses(CircuitOp circuit,
                                                              std::string &error) {
  return mapControlResponses(circuit, true, error);
}
