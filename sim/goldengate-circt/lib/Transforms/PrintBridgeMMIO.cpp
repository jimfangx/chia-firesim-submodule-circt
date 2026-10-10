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
#include "goldengate/BlockDevTokenEngine.h"
#include "goldengate/BlockDevRequestQueue.h"
#include "goldengate/BlockDevDataQueue.h"
#include "goldengate/BlockDevReadResponseQueue.h"
#include "goldengate/BlockDevWriteAckQueue.h"
#include "goldengate/BlockDevMMIOBank.h"
#include "goldengate/BlockDevWriteLatency.h"
#include "goldengate/BlockDevReadLatency.h"
#include "goldengate/BlockDevResponseScheduler.h"
#include "goldengate/FASEDTokenEngine.h"
#include "goldengate/FASEDRequestLimits.h"
#include "goldengate/FASEDLatencyRegisters.h"
#include "goldengate/FASEDFunctionalModelRegister.h"
#include "goldengate/FASEDResponseErrors.h"
#include "goldengate/FASEDStatistics.h"
#include "goldengate/FASEDHostOutstanding.h"
#include "goldengate/FASEDIngressAWQueue.h"
#include "goldengate/FASEDIngressWQueue.h"
#include "goldengate/FASEDIngressCredits.h"
#include "goldengate/FASEDIngressOrder.h"
#include "goldengate/FASEDIngressIssue.h"
#include "goldengate/FASEDIngressDeadlock.h"
#include "goldengate/FASEDReadBuffer.h"
#include "goldengate/FASEDReadScheduler.h"
#include "goldengate/FASEDWriteEgress.h"
#include "goldengate/FASEDResponseReleaser.h"
#include "goldengate/FASEDTimingCycle.h"
#include "goldengate/FASEDReadLatency.h"
#include "goldengate/FASEDWriteLatency.h"
#include "goldengate/FASEDTimingAWQueue.h"
#include "goldengate/FASEDWritePairing.h"
#include "goldengate/FASEDWriteRetirement.h"
#include "goldengate/FASEDWriteAdmission.h"
#include "goldengate/FASEDReadAdmission.h"
#include "goldengate/FASEDIngressARQueue.h"
#include "goldengate/TSITokenEngine.h"
#include "goldengate/TSIWordQueues.h"
#include "goldengate/TSIMMIOBank.h"
#include "goldengate/SimulationMaster.h"
#include "goldengate/SimulationMasterControl.h"
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

LogicalResult goldengate::mapPrintBridgeRocketSimulationMaster(CircuitOp circuit,
                                                              std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGControlWriteTrackerWrapper")
    return reject("Rocket Print master requires the completed response tracker boundary");
  auto find = [](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto top = find(circuit, circuit.getName());
  auto bound = find(circuit, "GGPrintBridgeHostWrapper");
  auto decoder = find(circuit, "GGControlAddressDecode");
  auto bank = find(circuit, "GGSimulationMasterBank");
  // Response catalogs belong to the read arbiter, not the later AW tracker.
  auto arbiter = find(circuit, "GGControlReadArbiterWrapper");
  auto hosts = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  auto reads = arbiter ? arbiter->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings") : ArrayAttr{};
  if (!top || !bank || !hosts || hosts.empty() || !regions || regions.size() != hosts.size() + 11 ||
      !reads || reads.size() != hosts.size() + 7)
    return reject("Rocket Print master requires the live expanded banks and response catalog");
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 3)
    return reject("Rocket Print master requires the three Master.scala registers");
  const StringRef registerNames[]{"INIT_DONE", "PRESENCE_READ", "PRESENCE_WRITE"};
  for (auto [i, attr] : llvm::enumerate(registers)) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto offset = row ? row.getAs<IntegerAttr>("offset") : IntegerAttr{};
    auto readable = row ? row.getAs<BoolAttr>("readable") : BoolAttr{};
    auto writeable = row ? row.getAs<BoolAttr>("writeable") : BoolAttr{};
    if (!row || row.getAs<StringAttr>("name") != registerNames[i] || !offset ||
        offset.getValue().getBitWidth() > 64 || offset.getInt() != int64_t(4 * i) ||
        !readable || !readable.getValue() || !writeable || !writeable.getValue())
      return reject("Rocket Print master register identity, offset or permissions differ");
  }
  SmallVector<FModuleOp> printHosts;
  llvm::StringSet<> names;
  for (auto attr : hosts) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto hostName = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    auto host = hostName ? find(circuit, hostName.getValue()) : FModuleOp{};
    if (!name || name.getValue() != "PrintBridgeModule_" + std::to_string(printHosts.size()) ||
        !host || !names.insert(host.getName()).second)
      return reject("Rocket Print master requires instantiated constructor order");
    printHosts.push_back(host);
  }
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlMMIORegion> allocated;
  if (failed(allocateRocketControlMMIORegions(circuit, 25, printHosts, widgets, allocated, error)))
    return failure();
  if (allocated.size() != regions.size()) return reject("Rocket Print master allocation count differs");
  // The old eleven-bank address is no longer correct once Print is present.
  // Comparing the entire live allocation also catches stale count-bank sizes.
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
      return reject("Rocket Print master region differs from the live register allocation");
  }
  llvm::StringSet<> boundNames;
  for (auto attr : reads) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr{};
    if (!name || name.getValue() == "SimulationMaster_0" || !boundNames.insert(name.getValue()).second ||
        !slave || slave.getValue().getBitWidth() > 64 || slave.getInt() < 0 ||
        uint64_t(slave.getInt()) >= allocated.size() || allocated[slave.getInt()].name != name.getValue())
      return reject("Rocket Print master response bank identity differs from the live allocation");
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(attachSimulationMasterBank(*staged, find(*staged, "GGSimulationMasterBank"), error)) ||
      failed(mapSimulationMasterControl(*staged, 25, 12, error)) ||
      failed(bindSimulationMasterControl(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print master produced invalid FIRRTL IR");
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

// TSIBridge.scala binds five FAME channel ports before attaching its two queues.
// Allocate from the actual expanded bank catalog, then stage all operations so
// a missing channel, malformed bank or late wrapper collision leaves IR intact.
LogicalResult goldengate::mapPrintBridgeRocketTSI(CircuitOp circuit,
                                                std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGSimulationMasterBoundWrapper")
    return reject("Rocket Print TSI requires the completed SimulationMaster boundary");
  auto find = [](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto bound = find(circuit, "GGPrintBridgeHostWrapper");
  auto decoder = find(circuit, "GGControlAddressDecode");
  auto bank = find(circuit, "GGTSIMMIOBank");
  auto hosts = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  auto words = bank ? bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr{};
  if (!hosts || hosts.empty() || !regions || regions.size() != hosts.size() + 11 ||
      !words || words.size() != 9)
    return reject("Rocket Print TSI requires the expanded allocation and nine TSI words");
  const StringRef names[]{"in_bits", "in_valid", "in_ready", "out_bits", "out_valid",
                         "out_ready", "step_size", "done", "start"};
  for (auto [i, attr] : llvm::enumerate(words)) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto offset = row ? row.getAs<IntegerAttr>("offset") : IntegerAttr{};
    auto read = row ? row.getAs<BoolAttr>("readable") : BoolAttr{};
    auto write = row ? row.getAs<BoolAttr>("writeable") : BoolAttr{};
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || name.getValue() != names[i] || !offset ||
        offset.getValue().getBitWidth() > 64 || offset.getInt() != int64_t(4 * i) ||
        !read || !read.getValue() || !write || !write.getValue())
      return reject("Rocket Print TSI register identity, offset or permissions differ");
  }
  SmallVector<FModuleOp> printHosts;
  llvm::StringSet<> identities;
  for (auto attr : hosts) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto symbol = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    auto host = symbol ? find(circuit, symbol.getValue()) : FModuleOp{};
    if (!name || name.getValue() != "PrintBridgeModule_" + std::to_string(printHosts.size()) ||
        !host || !identities.insert(host.getName()).second)
      return reject("Rocket Print TSI requires instantiated constructor order");
    printHosts.push_back(host);
  }
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlMMIORegion> allocated;
  if (failed(allocateRocketControlMMIORegions(circuit, 25, printHosts, widgets, allocated, error)))
    return failure();
  if (allocated.size() != regions.size()) return reject("Rocket Print TSI allocation count differs");
  for (auto [i, region] : llvm::enumerate(allocated)) {
    auto row = dyn_cast<DictionaryAttr>(regions[i]);
    auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr{};
    auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr{};
    auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr{};
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || name.getValue() != region.name || !start || !size || !slave ||
        start.getValue().getBitWidth() > 64 || size.getValue().getBitWidth() > 64 ||
        slave.getValue().getBitWidth() > 64 || start.getInt() != int64_t(region.start) ||
        size.getInt() != int64_t(region.size) || slave.getInt() != int64_t(i))
      return reject("Rocket Print TSI region differs from the live register allocation");
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addTSITokenEngine(*staged, error)) || failed(addTSIWordQueues(*staged, error)) ||
      failed(attachTSIMMIOBank(*staged, find(*staged, "GGTSIMMIOBank"), error)) ||
      failed(mapTSIBridgeControl(*staged, 25, 12, error)) ||
      failed(bindTSIBridgeControl(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print TSI produced invalid FIRRTL IR");
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

// BlockDevBridgeModule.scala binds nine FAME channels, four functional queues,
// and target-cycle timing. Preserve the allocated bank while completing that path.
// Allocate from the actual expanded bank catalog, then stage all operations so
// a missing channel, malformed bank or late wrapper collision leaves IR intact.
LogicalResult goldengate::mapPrintBridgeRocketBlockDev(CircuitOp circuit,
                                                std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGTSIBridgeBoundWrapper")
    return reject("Rocket Print BlockDev requires the completed TSI boundary");
  auto find = [](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto bound = find(circuit, "GGPrintBridgeHostWrapper");
  auto decoder = find(circuit, "GGControlAddressDecode");
  auto bank = find(circuit, "GGBlockDevMMIOBank");
  auto hosts = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  auto words = bank ? bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr{};
  if (!hosts || hosts.empty() || !regions || regions.size() != hosts.size() + 11 ||
      !words || words.size() != 26)
    return reject("Rocket Print BlockDev requires the expanded allocation and 26 BlockDev words");
  const StringRef names[]{"read_latency", "write_latency", "bdev_nsectors", "bdev_max_req_len",
      "bdev_req_valid", "bdev_req_write", "bdev_req_offset", "bdev_req_len", "bdev_req_tag", "bdev_req_ready",
      "bdev_data_valid", "bdev_data_data_upper", "bdev_data_data_lower", "bdev_data_tag", "bdev_data_ready",
      "bdev_rresp_data_upper", "bdev_rresp_data_lower", "bdev_rresp_tag", "bdev_rresp_valid", "bdev_rresp_ready",
      "bdev_wack_tag", "bdev_wack_valid", "bdev_wack_ready", "bdev_reqs_pending", "bdev_wack_stalled", "bdev_rresp_stalled"};
  for (auto [i, attr] : llvm::enumerate(words)) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto offset = row ? row.getAs<IntegerAttr>("offset") : IntegerAttr{};
    auto read = row ? row.getAs<BoolAttr>("readable") : BoolAttr{};
    auto write = row ? row.getAs<BoolAttr>("writeable") : BoolAttr{};
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || name.getValue() != names[i] || !offset ||
        offset.getValue().getBitWidth() > 64 || offset.getInt() != int64_t(4 * i) ||
        !read || read.getValue() != (i != 2 && i != 3) || !write || !write.getValue())
      return reject("Rocket Print BlockDev register identity, offset or permissions differ");
  }
  SmallVector<FModuleOp> printHosts;
  llvm::StringSet<> identities;
  for (auto attr : hosts) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto symbol = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    auto host = symbol ? find(circuit, symbol.getValue()) : FModuleOp{};
    if (!name || name.getValue() != "PrintBridgeModule_" + std::to_string(printHosts.size()) ||
        !host || !identities.insert(host.getName()).second)
      return reject("Rocket Print BlockDev requires instantiated constructor order");
    printHosts.push_back(host);
  }
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlMMIORegion> allocated;
  if (failed(allocateRocketControlMMIORegions(circuit, 25, printHosts, widgets, allocated, error)))
    return failure();
  if (allocated.size() != regions.size()) return reject("Rocket Print BlockDev allocation count differs");
  for (auto [i, region] : llvm::enumerate(allocated)) {
    auto row = dyn_cast<DictionaryAttr>(regions[i]);
    auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr{};
    auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr{};
    auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr{};
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || name.getValue() != region.name || !start || !size || !slave ||
        start.getValue().getBitWidth() > 64 || size.getValue().getBitWidth() > 64 ||
        slave.getValue().getBitWidth() > 64 || start.getInt() != int64_t(region.start) ||
        size.getInt() != int64_t(region.size) || slave.getInt() != int64_t(i))
      return reject("Rocket Print BlockDev region differs from the live register allocation");
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addBlockDevTokenEngine(*staged, error)) ||
      failed(addBlockDevRequestQueue(*staged, error)) || failed(addBlockDevDataQueue(*staged, error)) ||
      failed(addBlockDevReadResponseQueue(*staged, error)) || failed(addBlockDevWriteAckQueue(*staged, error)) ||
      failed(attachBlockDevMMIOBank(*staged, find(*staged, "GGBlockDevMMIOBank"), error)) ||
      failed(mapBlockDevBridgeControl(*staged, 25, 12, error)) || failed(bindBlockDevBridgeControl(*staged, error)) ||
      failed(addBlockDevWriteLatency(*staged, error)) || failed(addBlockDevReadLatency(*staged, error)) ||
      failed(addBlockDevResponseScheduler(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print BlockDev produced invalid FIRRTL IR");
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

// Requires the completed expanded Print/BlockDev boundary and its live MMIO
// allocation. Consumes FASED BridgeIO/channel endpoints via the native token
// pass; produces completed host endpoints and explicit ingress/egress ports.
// Mutates only new FIRRTL modules, raw annotation targets and circuit identity.
// No cached analysis is preserved. Timing, host-memory and MMIO binding follow.
LogicalResult goldengate::mapPrintBridgeRocketFASEDIngress(CircuitOp circuit,
                                                        std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevResponseSchedulerWrapper")
    return reject("Rocket Print FASED ingress requires the completed BlockDev boundary");
  auto find = [](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto bound = find(circuit, "GGPrintBridgeHostWrapper");
  auto decoder = find(circuit, "GGControlAddressDecode");
  auto hosts = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  if (!hosts || hosts.empty() || !regions || regions.size() != hosts.size() + 11)
    return reject("Rocket Print FASED ingress requires the expanded allocation");
  SmallVector<FModuleOp> printHosts;
  llvm::StringSet<> identities;
  for (auto attr : hosts) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto symbol = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    auto host = symbol ? find(circuit, symbol.getValue()) : FModuleOp{};
    if (!name || name.getValue() != "PrintBridgeModule_" + std::to_string(printHosts.size()) ||
        !host || !identities.insert(host.getName()).second)
      return reject("Rocket Print FASED ingress requires instantiated constructor order");
    printHosts.push_back(host);
  }
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlMMIORegion> allocated;
  if (failed(allocateRocketControlMMIORegions(circuit, 25, printHosts, widgets, allocated, error)))
    return failure();
  if (allocated.size() != regions.size()) return reject("Rocket Print FASED ingress allocation count differs");
  for (auto [i, region] : llvm::enumerate(allocated)) {
    auto row = dyn_cast<DictionaryAttr>(regions[i]);
    auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr{};
    auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr{};
    auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr{};
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || name.getValue() != region.name || !start || !size || !slave ||
        start.getValue().getBitWidth() > 64 || size.getValue().getBitWidth() > 64 ||
        slave.getValue().getBitWidth() > 64 || start.getInt() != int64_t(region.start) ||
        size.getInt() != int64_t(region.size) || slave.getInt() != int64_t(i))
      return reject("Rocket Print FASED ingress region differs from the live register allocation");
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDTokenEngine(*staged, error)) ||
      failed(addFASEDHostOutstanding(*staged, error)) ||
      failed(addFASEDIngressAWQueue(*staged, error)) ||
      failed(addFASEDIngressWQueue(*staged, error)) ||
      failed(addFASEDIngressARQueue(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED ingress produced invalid FIRRTL IR");
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

// Both ingress issue and read egress use the same instantiated Print registry
// and live Rocket allocator. Reject stale region metadata before staging IR.
static LogicalResult validateRocketPrintFASEDAllocation(CircuitOp circuit,
                                                       std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto find = [](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto bound = find(circuit, "GGPrintBridgeHostWrapper");
  auto decoder = find(circuit, "GGControlAddressDecode");
  auto hosts = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  if (!hosts || hosts.empty() || !regions || regions.size() != hosts.size() + 11)
    return reject("Rocket Print FASED requires the expanded allocation");
  SmallVector<FModuleOp> printHosts;
  llvm::StringSet<> identities;
  for (auto attr : hosts) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto symbol = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    auto host = symbol ? find(circuit, symbol.getValue()) : FModuleOp{};
    if (!name || name.getValue() != "PrintBridgeModule_" + std::to_string(printHosts.size()) ||
        !host || !identities.insert(host.getName()).second)
      return reject("Rocket Print FASED requires instantiated constructor order");
    printHosts.push_back(host);
  }
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlMMIORegion> allocated;
  if (failed(allocateRocketControlMMIORegions(circuit, 25, printHosts, widgets, allocated, error)))
    return failure();
  if (allocated.size() != regions.size()) return reject("Rocket Print FASED allocation count differs");
  for (auto [i, region] : llvm::enumerate(allocated)) {
    auto row = dyn_cast<DictionaryAttr>(regions[i]);
    auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr{};
    auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr{};
    auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr{};
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || name.getValue() != region.name || !start || !size || !slave ||
        start.getValue().getBitWidth() > 64 || size.getValue().getBitWidth() > 64 ||
        slave.getValue().getBitWidth() > 64 || start.getInt() != int64_t(region.start) ||
        size.getInt() != int64_t(region.size) || slave.getInt() != int64_t(i))
      return reject("Rocket Print FASED region differs from the live register allocation");
  }
  return success();
}

// Requires the expanded Print/FASED ingress boundary and live MMIO allocation.
// Consume raw AW/W/AR dequeue and host transaction ports through native credits,
// ordering and issue operations. Deadlock assertions use actual enqueue valid /
// ready signals and qualified reset. Stage the complete batch before mutation;
// commit the assertion context through existing ingress modules while preserving
// their operation identities and every unrelated bank/decoder. Analyses invalid.
LogicalResult goldengate::mapPrintBridgeRocketFASEDIssue(CircuitOp circuit,
                                                        std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDIngressARQueueWrapper")
    return reject("Rocket Print FASED issue requires the completed FASED ingress boundary");
  auto find = [](CircuitOp c, StringRef name) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDIngressCredits(*staged, error)) ||
      failed(addFASEDIngressOrder(*staged, error)) ||
      failed(addFASEDIngressIssue(*staged, error)) ||
      failed(addFASEDIngressDeadlock(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED issue produced invalid FIRRTL IR");
  // Deadlock context is appended along this existing, uniquely instantiated
  // ingress chain. Retain its module operations while transferring staged ports
  // and bodies, including updated instances; symbol references keep their names.
  const StringRef changed[]{"GGFASEDIngressARQueueWrapper", "GGFASEDIngressWQueueWrapper",
      "GGFASEDIngressAWWrapper", "GGFASEDIngressAW"};
  for (auto name : changed) {
    auto original = find(circuit, name), replacement = find(*staged, name);
    original->setAttrs(replacement->getAttrs());
    original->getRegion(0).takeBody(replacement->getRegion(0));
  }
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

// ReadEgress's recorded non-ROB branch: host R is accepted unconditionally;
// sixteen synchronous eight-beat queues retain data/last by AXI ID. Compose on
// a clone and publish only after verification. Existing module operations,
// constructor/channel catalogs, and all allocated MMIO banks remain intact.
LogicalResult goldengate::mapPrintBridgeRocketFASEDReadBuffer(CircuitOp circuit,
                                                             std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDIngressIssueWrapper")
    return reject("Rocket Print FASED read buffer requires the completed ingress issue boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDReadBuffer(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED read buffer produced invalid FIRRTL IR");
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

// Compose ReadEgress request state on the expanded Print/Rocket read buffer.
// The qualified token fire drives request capture and beat retirement; egress
// reset clears only active validity, and a new request wins over retirement.
// Publish new verified modules together, retaining existing module operations.
LogicalResult goldengate::mapPrintBridgeRocketFASEDReadScheduler(CircuitOp circuit,
                                                                std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDReadBufferWrapper")
    return reject("Rocket Print FASED read scheduler requires the completed read buffer boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDReadScheduler(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED read scheduler produced invalid FIRRTL IR");
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

// Compose WriteEgress acknowledgment state on the expanded Print/Rocket boundary.
// Host B responses always accept; retry samples old per-ID acknowledgment
// counters even without token fire, and same-ID enqueue/retirement cancel.
// Publish new verified modules together, retaining existing module operations.
LogicalResult goldengate::mapPrintBridgeRocketFASEDWriteEgress(CircuitOp circuit,
                                                              std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDReadSchedulerWrapper")
    return reject("Rocket Print FASED write egress requires the completed read scheduler boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDWriteEgress(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED write egress produced invalid FIRRTL IR");
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

// Compose AXI4Releaser's response occupancy on the expanded Print/Rocket circuit.
// Read retirement requires an accepted last beat; simultaneous replacement keeps
// occupancy set. State/reset are qualified by targetFire, acceptance is not.
// Publish verified additions atomically while preserving existing operations.
LogicalResult goldengate::mapPrintBridgeRocketFASEDResponseReleaser(CircuitOp circuit,
                                                              std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDWriteEgressWrapper")
    return reject("Rocket Print FASED response releaser requires the completed write egress boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDResponseReleaser(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED response releaser produced invalid FIRRTL IR");
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

// TimingModel's model clock advances/reset only when targetFire enables the gate.
// LatencyPipe deadlines use zero-extended UInt32 latencies, less one target cycle.
// Retain earlier module operations and publish verified additions atomically.
LogicalResult goldengate::mapPrintBridgeRocketFASEDTimingCycle(CircuitOp circuit,
                                                              std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDResponseReleaserWrapper")
    return reject("Rocket Print FASED timing cycle requires the completed response releaser boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDTimingCycle(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED timing cycle produced invalid FIRRTL IR");
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

// LatencyPipe accepts AR independently of capacity and diagnoses overflow.
// Queue state/RAM writes use targetFire; reset clears pointers, not memory.
LogicalResult goldengate::mapPrintBridgeRocketFASEDReadLatency(CircuitOp circuit,
                                                             std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDTimingCycleWrapper")
    return reject("Rocket Print FASED read latency requires the completed model-cycle boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDReadLatency(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED read latency produced invalid FIRRTL IR");
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

// Completed writes carry the oldest AW ID; ready diagnoses overflow.
// AW/W pairing remains an explicit input to this queue composition.
LogicalResult goldengate::mapPrintBridgeRocketFASEDWriteLatency(CircuitOp circuit,
                                                             std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDReadLatencyWrapper")
    return reject("Rocket Print FASED write latency requires the completed read-latency boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDWriteLatency(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED write latency produced invalid FIRRTL IR");
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

// Keep accepted AW IDs ordered until the model pairs each write.
LogicalResult goldengate::mapPrintBridgeRocketFASEDTimingAWQueue(CircuitOp circuit,
                                                             std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDWriteLatencyWrapper")
    return reject("Rocket Print FASED timing AW queue requires the completed write-latency boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDTimingAWQueue(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED timing AW queue produced invalid FIRRTL IR");
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

// Produce write completion from accepted AW and final W handshakes.
LogicalResult goldengate::mapPrintBridgeRocketFASEDWritePairing(CircuitOp circuit,
                                                             std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDTimingAWQueueWrapper")
    return reject("Rocket Print FASED write pairing requires the completed timing AW queue boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDWritePairing(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED write pairing produced invalid FIRRTL IR");
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

// Retire both pending counters on the target-visible B handshake. This stage
// appends observation ports to existing modules, so validate the complete
// mutation on a clone before applying it in place to preserve module identities.
LogicalResult goldengate::mapPrintBridgeRocketFASEDWriteRetirement(CircuitOp circuit,
                                                                std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDWritePairingWrapper")
    return reject("Rocket Print FASED write retirement requires the completed write-pairing boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  FModuleOp engine;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == "GGFASEDTokenEngine") engine = m;
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr{};
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr{};
  auto has = [&](StringRef name, int value) {
    auto attr = widths ? widths.getAs<IntegerAttr>(name) : IntegerAttr{};
    return attr && attr.getValue().getBitWidth() <= 64 && attr.getInt() == value;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("Rocket Print FASED write retirement requires the recorded 35/64/4-bit profile");
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(bindFASEDWriteRetirement(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED write retirement produced invalid FIRRTL IR");
  // bindFASEDWriteRetirement checks every rejection condition before mutation.
  // The unchanged original has just passed those checks and IR verification.
  return bindFASEDWriteRetirement(circuit, error);
}

// Admission adds only a wrapper; move that verified addition from staging so
// every pre-existing module operation and its users retain their identities.
LogicalResult goldengate::mapPrintBridgeRocketFASEDWriteAdmission(CircuitOp circuit,
                                                               std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDWriteRetirementWrapper")
    return reject("Rocket Print FASED write admission requires the completed retirement boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  FModuleOp engine;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == "GGFASEDTokenEngine") engine = m;
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr{};
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr{};
  auto has = [&](StringRef name, int value) {
    auto attr = widths ? widths.getAs<IntegerAttr>(name) : IntegerAttr{};
    return attr && attr.getValue().getBitWidth() <= 64 && attr.getInt() == value;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("Rocket Print FASED write admission requires the recorded 35/64/4-bit profile");
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(bindFASEDWriteAdmission(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED write admission produced invalid FIRRTL IR");
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

// Observe accepted final R through the existing hierarchy and close AR ready.
// Verify the full mutation on a clone, then preserve original module identities.
LogicalResult goldengate::mapPrintBridgeRocketFASEDReadAdmission(CircuitOp circuit,
                                                                std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDWriteAdmissionWrapper")
    return reject("Rocket Print FASED read admission requires the completed write-admission boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  FModuleOp engine;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == "GGFASEDTokenEngine") engine = m;
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr{};
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr{};
  auto has = [&](StringRef name, int value) {
    auto attr = widths ? widths.getAs<IntegerAttr>(name) : IntegerAttr{};
    return attr && attr.getValue().getBitWidth() <= 64 && attr.getInt() == value;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("Rocket Print FASED read admission requires the recorded 35/64/4-bit profile");
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(bindFASEDReadAdmission(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED read admission produced invalid FIRRTL IR");
  // bindFASEDReadAdmission checks every rejection condition before mutation.
  // The unchanged original has just passed those checks and IR verification.
  return bindFASEDReadAdmission(circuit, error);
}

// Requires the expanded Print allocation and completed read/write admission.
// Consumes/produces no annotations; explicitly retargets the retained archive.
// Adds only a wrapper, using the allocated bank's existing module identity.
// Preserves model/channel identities, constructor keys, banks and allocation.
// Output: host-reset MMIO words drive the four-bit admission maxima; decoded
// MCR words 2/3 remain exposed for subsequent control-fanout composition.
LogicalResult goldengate::mapPrintBridgeRocketFASEDRequestLimits(CircuitOp circuit,
                                                               std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDReadAdmissionWrapper")
    return reject("Rocket Print FASED request limits require the completed read-admission boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp bank;
  for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == "GGFASEDRequestLimits") bank = m;
  if (!bank) return reject("Rocket Print FASED request limits require the allocated request-limit bank");
  if (failed(attachFASEDRequestLimits(*staged, bank, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED request limits produced invalid FIRRTL IR");
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

// Requires: completed request-limit boundary, valid expanded Rocket/Print MMIO
// allocation, retained annotations and a free, allocated latency bank.
// Annotations consumed/produced: none; explicitly retarget the retained archive.
// Mutates: append a wrapper consuming latency inputs and exporting decoded MCR.
// Analyses required: Rocket control allocation; no cached analysis is used.
// Preserves: prior module/port identities, bank definitions and constructor keys.
// Output: host-clock/reset bank drives full-width read/write timing latencies;
// failed attachment is atomic and retained ports keep their type/direction/order.
LogicalResult goldengate::mapPrintBridgeRocketFASEDLatencyRegisters(CircuitOp circuit,
                                                               std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDRequestLimitsWrapper")
    return reject("Rocket Print FASED latency registers require the completed request-limit boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp bank;
  for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == "GGFASEDLatencyRegisters") bank = m;
  if (!bank) return reject("Rocket Print FASED latency registers require the allocated latency-register bank");
  if (failed(attachFASEDLatencyRegisters(*staged, bank, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED latency registers produced invalid FIRRTL IR");
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

// Requires: completed latency-register boundary, valid expanded Rocket/Print MMIO
// allocation, retained annotations and a free, allocated functional-model bank.
// Annotations consumed/produced: none; explicitly retarget the retained archive.
// Mutates: append a wrapper consuming ingress relaxation and exporting decoded MCR.
// Analyses required: Rocket control allocation; no cached analysis is used.
// Preserves: prior module/port identities, bank definitions and constructor keys.
// Output: host-clock/reset bank drives full-width MMIO readback and bit-zero ingress relaxation;
// failed attachment is atomic and retained ports keep their type/direction/order.
LogicalResult goldengate::mapPrintBridgeRocketFASEDFunctionalModelRegister(CircuitOp circuit,
                                                               std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDLatencyRegistersWrapper")
    return reject("Rocket Print FASED functional-model register requires the completed latency-register boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp bank;
  for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == "GGFASEDFunctionalModelRegister") bank = m;
  if (!bank) return reject("Rocket Print FASED functional-model register requires the allocated functional-model register bank");
  if (failed(attachFASEDFunctionalModelRegister(*staged, bank, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED functional-model register produced invalid FIRRTL IR");
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

// Requires: completed functional-model register boundary, valid expanded Rocket/
// Print MMIO allocation, retained annotations and a free allocated error bank.
// Annotations consumed/produced: none; explicitly retarget the retained archive.
// Mutates: append a wrapper observing accepted host R/B responses and exporting MCR.
// Analyses required: Rocket control allocation; no cached analysis is used.
// Preserves: prior module/port identities, bank definitions and constructor keys.
// Output: host-reset UInt<2> error state is read-only and zero-extended to UInt<32>;
// accepted B errors capture R resp, as in SFC. Failed attachment is atomic and
// copied ports retain their type/direction/order; response ready comes from sim.
LogicalResult goldengate::mapPrintBridgeRocketFASEDResponseErrors(CircuitOp circuit,
                                                               std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDFunctionalModelRegisterWrapper")
    return reject("Rocket Print FASED response errors require the completed functional-model register boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp bank;
  for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == "GGFASEDResponseErrors") bank = m;
  if (!bank) return reject("Rocket Print FASED response errors require the allocated response-error bank");
  if (failed(attachFASEDResponseErrors(*staged, bank, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED response errors produced invalid FIRRTL IR");
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

// Requires: completed response-error boundary, expanded Rocket/Print allocation,
// retained archive, free allocated statistics bank and unique response hierarchy.
// Annotations consumed/produced: none; native attachment retargets the archive.
// Mutates: append accepted-R observation to 13 modules and 12 sim instances;
// attach four read-only counters and export their MCR fragment in a new wrapper.
// Analyses required: native FASED response/request boundaries and MMIO allocation.
// Preserves: existing module identities, original port arguments/types/order,
// constructor keys and banks. Hierarchy analyses must be recomputed afterward.
// Output: AW/AR transactions and all W/R beats count only on targetFire; model
// reset is gated by targetFire. Validate and verify on a clone before changing
// existing modules. Native attachment preflights every failure before mutation;
// replaying that verified attachment preserves identities in the live circuit.
LogicalResult goldengate::mapPrintBridgeRocketFASEDStatistics(CircuitOp circuit,
                                                            std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDResponseErrorsWrapper")
    return reject("Rocket Print FASED statistics require the completed response-error boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  FModuleOp bank;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == "GGFASEDStatistics") bank = m;
  if (!bank) return reject("Rocket Print FASED statistics require the allocated statistics bank");
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp stagedBank;
  for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == bank.getName()) stagedBank = m;
  if (failed(attachFASEDStatistics(*staged, stagedBank, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED statistics produced invalid FIRRTL IR");
  // Moving only the new wrapper would discard the staged R-observation wiring.
  // Repeat the fully preflighted mutation to retain original module/port handles.
  return attachFASEDStatistics(circuit, bank, error);
}
