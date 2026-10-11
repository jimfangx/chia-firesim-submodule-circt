// See LICENSE for license details.
// Compose Widget.scala's size-sorted control allocation and io.ctrl binding
// for the implemented Print hosts and their live CPU stream count bank.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/HostMemoryWriteArbiter.h"
#include "goldengate/HostMemoryWriteResponses.h"
#include "goldengate/HostMemoryReadArbiter.h"
#include "goldengate/HostMemoryReadResponses.h"
#include "goldengate/HostMemoryOutputBuffer.h"
#include "goldengate/HostMemoryPort.h"
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
#include "goldengate/FASEDHistograms.h"
#include "goldengate/FASEDMMIOBank.h"
#include "goldengate/FASEDHostMemory.h"
#include "goldengate/FASEDAddressTranslation.h"
#include "goldengate/FASEDReadDeinterleaver.h"
#include "goldengate/FASEDHostMemoryBuffer.h"
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

// Requires: completed statistics boundary, expanded Rocket/Print allocation,
// retained archive, unused allocated histogram bank and exact pending counts.
// Annotations consumed/produced: none; explicitly transfer copied top targets
// and retarget the retained archive using the native attachment contract.
// Mutates: append a wrapper sampling pending reads and AW before the target edge
// and exporting ten read-only MCR words. Existing modules/banks are unchanged.
// Analyses required: Rocket control allocation and native histogram boundary.
// Preserves: prior module/port/argument identities, constructors and drivers;
// hierarchy analyses must be recomputed after wrapper attachment.
// Output: first bound 0,2,4,8 increments on targetFire; fifth bins remain zero.
// Model reset requires targetFire; host reset only masks read-only assertions.
// Stage and verify before committing, so rejection leaves live IR unchanged.
LogicalResult goldengate::mapPrintBridgeRocketFASEDHistograms(CircuitOp circuit,
                                                            std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDStatisticsWrapper")
    return reject("Rocket Print FASED histograms require the completed statistics boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp bank;
  for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == "GGFASEDHistograms") bank = m;
  if (!bank) return reject("Rocket Print FASED histograms require the allocated histogram bank");
  if (failed(attachFASEDHistograms(*staged, bank, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED histograms produced invalid FIRRTL IR");
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

// Requires: completed histogram boundary, expanded Rocket/Print allocation,
// retained archive and six typed MCR fragments with the recorded register maps.
// Annotations consumed/produced: none. Copied top targets and indexed fragment
// lanes transfer; whole-fragment references remain on the retained inner module.
// Mutates: append one wrapper joining 21 read/write lanes and six strobe routes;
// remove the six fragment ports from the new top, without duplicating state.
// Analyses required: Rocket allocation and native fragment registry/type checks.
// Preserves: existing modules, port/argument identities, constructors and banks.
// Hierarchy analyses must be recomputed after attachment.
// Output: ordered byte offsets 0..80; ready flows opposite valid/bits. Stage and
// verify before committing so even a failed IR verifier leaves live IR intact.
LogicalResult goldengate::mapPrintBridgeRocketFASEDMMIOBank(CircuitOp circuit,
                                                         std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDHistogramsWrapper")
    return reject("Rocket Print FASED MMIO bank requires the completed histogram boundary");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDMMIOBank(*staged, error))) return failure();
  if (failed(verify(*staged))) return reject("Rocket Print FASED MMIO bank produced invalid FIRRTL IR");
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

// Requires: unused completed FASED MMIO top, retained annotations and the live
// expanded Rocket/Print allocation. Consumes the decoded MCR boundary and the
// catalog-selected AW/W/AR and B/R slave ports; no annotation classes consumed.
// Produces: native MCRFile transaction state and resolved fasedSlave metadata.
// Mutates: append the transport and two wrappers after staged verification.
// Analyses required: register registry and Rocket allocation; hierarchy invalid.
// Preserves: existing operation/port/argument identities, Print controls/streams,
// all bank state and constructor/channel metadata. Copied targets transfer;
// consumed slave references stay inner, unexpected decoded references reject.
// Output: 21-word local decode, independently captured AW/W, held B/R flags/IDs,
// live read data, zero legacy strobe and retirement through arbiter readiness.
LogicalResult goldengate::mapPrintBridgeRocketFASEDControl(CircuitOp circuit,
                                                          std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDMMIOWrapper")
    return reject("Rocket Print FASED control requires the completed MMIO bank");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(mapFASEDBridgeControl(*staged, 25, 12, error))) return failure();
  const StringRef fragments[]{"GGFASEDLatencyRegisters", "GGFASEDRequestLimits",
      "GGFASEDHistograms", "GGFASEDStatistics", "GGFASEDFunctionalModelRegister",
      "GGFASEDResponseErrors"};
  ControlMMIOWidget widget;
  if (failed(deriveControlMMIOWidget(*staged, "FASEDMemoryTimingModel_0",
          "GGFASEDMCRFile", fragments, widget, error))) return failure();
  // The adapter and allocator must describe the same actual bank, including
  // names and permissions, rather than merely agreeing on the rounded size.
  SmallVector<DictionaryAttr> registered(widget.registerCount), assembled(widget.registerCount);
  for (auto m : staged->getOps<FModuleOp>()) {
    bool fragment = llvm::is_contained(fragments, m.getName());
    if (!fragment && m.getName() != "GGFASEDMMIOWrapper") continue;
    auto rows = m->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    for (auto attr : rows) {
      auto row = cast<DictionaryAttr>(attr);
      unsigned word = row.getAs<IntegerAttr>("offset").getValue().getZExtValue() / 4;
      if (word >= widget.registerCount)
        return reject("Rocket Print FASED assembled registry exceeds the allocated bank");
      (fragment ? registered : assembled)[word] = row;
    }
  }
  for (auto [word, row] : llvm::enumerate(registered)) {
    if (!row || !assembled[word])
      return reject("Rocket Print FASED assembled registry is incomplete");
    for (auto key : {"name", "readable", "writeable"})
      if (row.get(key) != assembled[word].get(key))
        return reject("Rocket Print FASED assembled registry differs from its allocated fragments");
  }
  if (failed(bindFASEDBridgeControl(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print FASED control produced invalid FIRRTL IR");
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

// Requires: unused completed FASED control top, retained annotations, live
// Rocket/Print allocation and no exported widget/slave control boundaries.
// Consumes: scalar AW/W and B/R master ports and aggregate AR; no annotation
// classes consumed or produced. Copied targets transfer, consumed targets stay
// inner. Mutates: append one stateless FIRRTL wrapper after staged verification.
// Analyses required: live MMIO registry/allocation; hierarchy analysis invalid.
// Preserves: prior operations, ports, arguments, clock/reset, constructor/channel
// metadata and all request/response tracker state. Output: complete host ctrl
// bundle with independent AW/W readiness and B/R acceptance through arbiters.
LogicalResult goldengate::mapPrintBridgeRocketControlMaster(CircuitOp circuit,
                                                          std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDBridgeBoundWrapper")
    return reject("Rocket Print control master requires the completed FASED control binding");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  for (auto m : circuit.getOps<FModuleOp>()) {
    if (m.getName() != circuit.getName()) continue;
    for (auto p : m.getPorts()) {
      auto name = p.name.getValue();
      if (name.ends_with("_ctrl") ||
          name.starts_with("ctrl_write_dispatch_slave_") ||
          name.starts_with("ctrl_read_dispatch_slave_") ||
          name.starts_with("ctrl_write_arb_in_") ||
          name.starts_with("ctrl_read_arb_in_"))
        return reject("Rocket Print control master has an unbound widget control slave");
    }
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(bindControlMaster(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print control master produced invalid FIRRTL IR");
  // Commit only the new wrapper: replacing the cloned circuit would invalidate
  // every previously resolved module, port and argument used by later mapping.
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGControlMasterWrapper")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// Requires: unused completed control master, live Rocket/Print allocation,
// retained constructor/annotation archive and exact 35/64/4 NASTI boundaries.
// Consumes: split host request/read-response/write-response ports. No annotation
// classes consumed or produced; mapped leaf and copied targets transfer, whole
// split-bundle and discarded metadata targets remain on the inner module.
// Mutates: append one stateless FIRRTL wrapper after staged verification.
// Analyses required: live MMIO allocation; hierarchy analysis invalidated.
// Preserves: all existing operation/port/argument identities, state, control,
// clock/reset and channel/constructor metadata. Output: five-channel host AXI
// memory master before address translation, buffering and LoadMem arbitration.
LogicalResult goldengate::mapPrintBridgeRocketFASEDHostMemory(CircuitOp circuit,
                                                           std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGControlMasterWrapper")
    return reject("Rocket Print host memory requires the completed control master");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(bindFASEDHostMemory(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print host memory produced invalid FIRRTL IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGFASEDHostMemoryWrapper")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// Requires: unused expanded host memory master, live Rocket/Print allocation,
// retained constructor/archive and the recorded MainMemory_0 address sets.
// Consumes: the 35-bit host address boundary; no annotation classes consumed or
// produced. Copied targets transfer; memory references retain inner semantics.
// Mutates: append the verified FIRRTL translation helper and wrapper atomically.
// Analyses required: live MMIO allocation; hierarchy analysis invalidated.
// Preserves: all prior module/port/argument identities, state, control, clock,
// channel and constructor metadata. Output: 34-bit host memory master with
// valid-qualified, host-reset-gated bounds assertions before deinterleaving.
LogicalResult goldengate::mapPrintBridgeRocketFASEDAddressTranslation(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDHostMemoryWrapper")
    return reject("Rocket Print translation requires the assembled host memory master");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDAddressTranslation(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print translation produced invalid FIRRTL IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGFASEDAddressTranslation" ||
          m.getName() == "GGFASEDAddressTranslationWrapper")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// Requires: active expanded translation top, live Rocket/Print allocation and
// the recorded sixteen-ID eight-beat constructor. No annotations consumed.
// Mutates: insert verified native queues/helper at the unique pre-translation
// master connection. Preserves: all existing module/port/argument identities,
// annotation archive, constructor/channel metadata and control/clock bindings.
// Hierarchy analysis invalidated; output retains the translated host interface.
LogicalResult goldengate::mapPrintBridgeRocketFASEDReadDeinterleaver(
    CircuitOp circuit, std::string &error) {
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDReadDeinterleaver(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "Rocket Print deinterleaver produced invalid FIRRTL IR";
    return failure();
  }
  FModuleOp top, helper;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  InstanceOp translation;
  for (auto i : top.getOps<InstanceOp>()) {
    if (i.getName() == "translation") translation = i;
  }
  ConnectOp boundary;
  for (auto c : top.getOps<ConnectOp>())
    if (c.getDest() == translation.getResult(2)) boundary = c;
  // Staged validation proves this exact unique boundary and host bindings.
  // Move only new definitions, then replace the one connection in the live top.
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGFASEDReadDeinterleaver" ||
          m.getName() == "GGFASEDDeinterleaveQueue8") {
        if (m.getName() == "GGFASEDReadDeinterleaver") helper = m;
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
      }
  OpBuilder b(boundary); auto loc = boundary.getLoc();
  auto deinterleaver = b.create<InstanceOp>(loc, helper, "deinterleaver");
  for (auto c : llvm::make_early_inc_range(top.getOps<StrictConnectOp>()))
    for (unsigned i = 0; i < 2; ++i)
      if (c.getDest() == translation.getResult(i))
        b.create<StrictConnectOp>(loc, deinterleaver.getResult(i), c.getSrc());
  b.create<ConnectOp>(loc, deinterleaver.getResult(2), boundary.getSrc());
  b.create<ConnectOp>(loc, translation.getResult(2), deinterleaver.getResult(3));
  boundary.erase();
  return success();
}

LogicalResult goldengate::mapPrintBridgeRocketFASEDHostMemoryBuffer(
    CircuitOp circuit, std::string &error) {
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addFASEDHostMemoryBuffer(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "Rocket Print host memory buffer produced invalid FIRRTL IR";
    return failure();
  }
  FModuleOp top, helper;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  InstanceOp translation;
  for (auto i : top.getOps<InstanceOp>())
    if (i.getName() == "translation") translation = i;
  ConnectOp boundary;
  for (auto c : top.getOps<ConnectOp>())
    if (c.getSrc() == translation.getResult(3)) boundary = c;
  // Validate the entire composition on a clone before moving only new
  // definitions. Keep all existing operations and target identities live.
  const StringRef definitions[]{"GGFASEDHostMemoryBuffer", "GGFASEDMemoryAddressQueue2",
      "GGFASEDMemoryWriteQueue2", "GGFASEDMemoryAckQueue2", "GGFASEDMemoryReadQueue2"};
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (llvm::is_contained(definitions, m.getName())) {
        if (m.getName() == definitions[0]) helper = m;
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
      }
  OpBuilder b(boundary); auto loc = boundary.getLoc();
  auto buffer = b.create<InstanceOp>(loc, helper, "memory_buffer");
  for (auto c : llvm::make_early_inc_range(top.getOps<StrictConnectOp>()))
    for (unsigned i = 0; i < 2; ++i)
      if (c.getDest() == translation.getResult(i))
        b.create<StrictConnectOp>(loc, buffer.getResult(i), c.getSrc());
  b.create<ConnectOp>(loc, buffer.getResult(2), boundary.getSrc());
  b.create<ConnectOp>(loc, boundary.getDest(), buffer.getResult(3));
  boundary.erase();
  return success();
}

LogicalResult goldengate::mapPrintBridgeRocketLoadMemEngine(
    CircuitOp circuit, std::string &error) {
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("Print LoadMem engine requires the preserved annotation archive");
  constexpr StringLiteral oldName = "GGFASEDAddressTranslationWrapper";
  constexpr StringLiteral newName = "GGPrintLoadMemWrapper";
  constexpr StringLiteral engineName = "GGPrintLoadMemEngine";
  if (circuit.getName() != oldName)
    return reject("Print LoadMem engine requires the buffered translated top");
  FModuleOp inner;
  const StringRef names[]{"GGLoadMemWriter", "GGLoadMemRequestQueue2", "GGLoadMemRequestArbiter",
      "GGLoadMemWriteMMIOBank", "GGLoadMemWriteDataFIFO", "GGLoadMemReadRequests", "GGLoadMemReadDataFIFO"};
  const unsigned counts[]{20, 4, 12, 10, 9, 16, 9};
  SmallVector<FModuleOp> helpers(7); unsigned matches[7]{};
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == newName || m.getModuleName() == engineName)
      return reject("Print LoadMem engine or wrapper already exists");
    auto f = dyn_cast<FModuleOp>(m.getOperation()); if (!f) continue;
    if (f.getName() == oldName) inner = f;
    for (unsigned j = 0; j < 7; ++j)
      if (f.getName() == names[j] || f.getName().ends_with(("_" + names[j]).str())) {
        helpers[j] = f; ++matches[j];
      }
  }
  for (unsigned j = 0; j < 7; ++j)
    if (matches[j] != 1 || helpers[j].getNumPorts() != counts[j])
      return reject("Print LoadMem engine needs the unique native writer/queue/bank catalog");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(32)}});
  auto mcrType = BundleType::get(ctx, {{b.getStringAttr("read"), false, FVectorType::get(token, 9)},
      {b.getStringAttr("write"), true, FVectorType::get(token, 9)}, {b.getStringAttr("wstrb"), true, uint(4)}});
  // Check the entire catalog interface before creating any indexed subfields.
  // A matching name/count alone cannot prove the recorded Rocket geometry.
  auto request = BundleType::get(ctx, {{b.getStringAttr("zero"), false, uint(1)},
      {b.getStringAttr("addr"), false, uint(34)}, {b.getStringAttr("len"), false, uint(34)}});
  auto requestToken = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, request}});
  SmallVector<BundleType::BundleElement> bankFields;
  for (unsigned word : {0U, 1U, 2U, 3U, 5U}) {
    bankFields.push_back({b.getStringAttr("read_" + std::to_string(word)), false, token});
    bankFields.push_back({b.getStringAttr("write_" + std::to_string(word)), true, token});
  }
  bankFields.push_back({b.getStringAttr("wstrb"), true, uint(4)});
  auto bankType = BundleType::get(ctx, bankFields);
  SmallVector<SmallVector<PortInfo>> expected(7);
  auto add = [&](unsigned j, StringRef name, Type type, Direction dir) {
    expected[j].push_back({b.getStringAttr(name), type, dir});
  };
  for (unsigned j : {0U, 1U, 3U, 4U, 5U, 6U}) {
    add(j, "clock", ClockType::get(ctx), Direction::In); add(j, "reset", uint(1), Direction::In);
  }
  const StringRef wn[]{"req_ready", "req_valid", "req_bits_zero", "req_bits_addr", "req_bits_len",
      "data_ready", "data_valid", "data_bits", "mem_aw_ready", "mem_aw_valid", "mem_aw_bits_addr",
      "mem_aw_bits_len", "mem_w_ready", "mem_w_valid", "mem_w_bits_data", "mem_w_bits_last", "mem_b_ready", "mem_b_valid"};
  const unsigned ww[]{1,1,1,34,34,1,1,64,1,1,34,8,1,1,64,1,1,1};
  for (unsigned p = 0; p < 18; ++p) add(0, wn[p], uint(ww[p]),
      llvm::is_contained(ArrayRef<unsigned>({0,5,9,10,11,13,14,15,16}), p) ? Direction::Out : Direction::In);
  add(1, "enq", requestToken, Direction::In); add(1, "deq", requestToken, Direction::Out);
  const StringRef an[]{"in_0_ready", "in_0_valid", "in_0_bits_zero", "in_0_bits_addr", "in_0_bits_len",
      "in_1_ready", "in_1_valid", "out_ready", "out_valid", "out_bits_zero", "out_bits_addr", "out_bits_len"};
  const unsigned aw[]{1,1,1,34,34,1,1,1,1,1,34,34};
  for (unsigned p = 0; p < 12; ++p) add(2, an[p], uint(aw[p]),
      p == 0 || p == 5 || p >= 8 ? Direction::Out : Direction::In);
  const StringRef bn[]{"write_ready", "write_valid", "write_bits_addr", "write_bits_len", "zero_ready", "zero_valid", "zero_finished"};
  const unsigned bw[]{1,1,34,34,1,1,1};
  for (unsigned p = 0; p < 7; ++p) add(3, bn[p], uint(bw[p]),
      p == 0 || p == 4 || p == 6 ? Direction::In : Direction::Out);
  add(3, "mcr", bankType, Direction::Out);
  const StringRef fn[]{"in_ready", "in_valid", "in_bits", "out_ready", "out_valid", "out_bits", "count"};
  for (unsigned j : {4U, 6U}) {
    const unsigned fw[]{1,1,j == 4 ? 32U : 64U,1,1,j == 4 ? 64U : 32U,j == 4 ? 6U : 2U};
    for (unsigned p = 0; p < 7; ++p) add(j, fn[p], uint(fw[p]),
        p == 0 || p >= 4 ? Direction::Out : Direction::In);
  }
  const StringRef rn[]{"write_6_ready", "write_6_valid", "write_6_bits", "read_6_valid", "read_6_bits",
      "write_7_ready", "write_7_valid", "write_7_bits", "read_7_valid", "read_7_bits", "read_7_ready",
      "mem_ar_ready", "mem_ar_valid", "mem_ar_bits_addr"};
  const unsigned rw[]{1,1,32,1,32,1,1,32,1,32,1,1,1,34};
  for (unsigned p = 0; p < 14; ++p) add(5, rn[p], uint(rw[p]),
      llvm::is_contained(ArrayRef<unsigned>({0,3,4,5,8,9,12,13}), p) ? Direction::Out : Direction::In);
  for (unsigned j = 0; j < 7; ++j)
    for (auto [p, info] : llvm::enumerate(helpers[j].getPorts())) {
      auto want = expected[j][p];
      if (info.name != want.name || info.type != want.type || info.direction != want.direction)
        return reject("Print LoadMem native catalog port name/type/direction differs");
    }
  std::optional<unsigned> mcr, clock, reset;
  if (!inner) return reject("Print LoadMem engine has no active module");
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "loadmem_mcr" && p.type == mcrType && p.direction == Direction::In) mcr = i;
    if (p.name == "hostClock" && p.type == ClockType::get(ctx) && p.direction == Direction::In) clock = i;
    if (p.name == "hostReset" && p.type == uint(1) && p.direction == Direction::In) reset = i;
    if (p.name.getValue().starts_with("loadmem_mem_")) return reject("Print LoadMem AXI boundary already exists");
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == oldName; });
  if (!mcr || !clock || !reset || used)
    return reject("Print LoadMem engine needs exact host clock/reset and nine-word MCR boundary");
  const StringRef children[]{"sim", "translation", "deinterleaver", "memory_buffer"};
  const StringRef modules[]{"GGFASEDHostMemoryWrapper", "GGFASEDAddressTranslation",
      "GGFASEDReadDeinterleaver", "GGFASEDHostMemoryBuffer"};
  unsigned seen[4]{};
  for (auto inst : inner.getOps<InstanceOp>()) {
    bool found = false;
    for (unsigned j = 0; j < 4; ++j)
      if (inst.getName() == children[j] && inst.getModuleName() == modules[j]) {
        found = true; ++seen[j];
      }
    if (!found) return reject("Print LoadMem engine requires the translated/deinterleaved/buffered hierarchy");
  }
  if (llvm::any_of(seen, [](unsigned n) { return n != 1; }))
    return reject("Print LoadMem engine requires all four existing buffered top instances");
  // Construct on a clone, including type verification, before committing any
  // live operations. Catalog namespaces are resolved by unique helper identity.
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  SmallVector<FModuleOp> h;
  for (auto prior : helpers) for (auto m : staged->getOps<FModuleOp>())
    if (m.getName() == prior.getName()) h.push_back(m);
  FModuleOp old;
  for (auto m : staged->getOps<FModuleOp>()) if (m.getName() == oldName) old = m;
  SmallVector<PortInfo> ep{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), uint(1), Direction::In}, {b.getStringAttr("mcr"), mcrType, Direction::Out}};
  // Native writer and reader ports already carry the recorded 34/64 geometry.
  for (auto p : ArrayRef<PortInfo>(h[0].getPorts()).drop_front(10)) { p.name = b.getStringAttr(("loadmem_" + p.name.getValue()).str()); ep.push_back(p); }
  for (auto p : ArrayRef<PortInfo>(h[5].getPorts()).drop_front(13)) { p.name = b.getStringAttr(("loadmem_" + p.name.getValue()).str()); ep.push_back(p); }
  ep.push_back({b.getStringAttr("loadmem_mem_r_ready"), uint(1), Direction::Out});
  ep.push_back({b.getStringAttr("loadmem_mem_r_valid"), uint(1), Direction::In});
  ep.push_back({b.getStringAttr("loadmem_mem_r_bits_data"), uint(64), Direction::In});
  b.setInsertionPointToEnd(staged->getBodyBlock());
  auto engine = b.create<FModuleOp>(loc, b.getStringAttr(engineName), old.getConventionAttr(), ep);
  b.setInsertionPointToStart(engine.getBodyBlock());
  const StringRef instNames[]{"writer", "write_requests", "request_arbiter", "registers", "write_data", "read_requests", "read_data"};
  SmallVector<InstanceOp> i;
  for (unsigned j = 0; j < 7; ++j) i.push_back(b.create<InstanceOp>(loc, h[j], instNames[j]));
  auto arg = [&](unsigned p) { return engine.getArgument(p); };
  auto field = [&](Value v, StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto lane = [&](StringRef group, unsigned word) -> Value {
    return b.create<SubindexOp>(loc, field(arg(2), group), word);
  };
  for (unsigned j : {0U, 1U, 3U, 4U, 5U, 6U})
    for (unsigned p = 0; p < 2; ++p) connect(i[j].getResult(p), arg(p));
  // Queued writes have priority over whole-DRAM zero fill; completion is
  // writer req.ready. Reuse the native queue/arbiter instead of duplicating it.
  connect(i[3].getResult(2), field(i[1].getResult(2), "ready"));
  connect(field(i[1].getResult(2), "valid"), i[3].getResult(3));
  auto bits = field(i[1].getResult(2), "bits");
  connect(field(bits, "zero"), constant(1, 0));
  connect(field(bits, "addr"), i[3].getResult(4)); connect(field(bits, "len"), i[3].getResult(5));
  connect(field(i[1].getResult(3), "ready"), i[2].getResult(0));
  connect(i[2].getResult(1), field(i[1].getResult(3), "valid"));
  const StringRef requestFields[]{"zero", "addr", "len"};
  for (unsigned p = 0; p < 3; ++p)
    connect(i[2].getResult(2 + p), field(field(i[1].getResult(3), "bits"), requestFields[p]));
  connect(i[3].getResult(6), i[2].getResult(5)); connect(i[2].getResult(6), i[3].getResult(7));
  connect(i[2].getResult(7), i[0].getResult(2)); connect(i[0].getResult(3), i[2].getResult(8));
  for (unsigned p = 0; p < 3; ++p) connect(i[0].getResult(4 + p), i[2].getResult(9 + p));
  connect(i[3].getResult(8), i[0].getResult(2));
  connect(i[4].getResult(5), i[0].getResult(7));
  connect(i[0].getResult(8), i[4].getResult(6)); connect(i[0].getResult(9), i[4].getResult(7));
  for (unsigned p = 10; p < 20; ++p) {
    bool input = h[0].getPortDirection(p) == Direction::In;
    connect(input ? i[0].getResult(p) : arg(p - 7), input ? arg(p - 7) : i[0].getResult(p));
  }
  for (unsigned p = 13; p < 16; ++p) {
    bool input = h[5].getPortDirection(p) == Direction::In;
    connect(input ? i[5].getResult(p) : arg(p), input ? arg(p) : i[5].getResult(p));
  }
  connect(arg(16), i[6].getResult(2)); connect(i[6].getResult(3), arg(17)); connect(i[6].getResult(4), arg(18));
  // Assemble decoded MCR words using the same permissions/handshakes as
  // LoadMem.scala. Address/status register and queue state stays in the catalog.
  for (unsigned word : {0U, 1U, 2U, 3U, 5U}) {
    auto rd = field(i[3].getResult(9), "read_" + std::to_string(word));
    auto wr = field(i[3].getResult(9), "write_" + std::to_string(word));
    b.create<ConnectOp>(loc, lane("read", word), rd);
    b.create<ConnectOp>(loc, wr, lane("write", word));
  }
  connect(field(i[3].getResult(9), "wstrb"), field(arg(2), "wstrb"));
  auto rd4 = lane("read", 4), wr4 = lane("write", 4);
  connect(field(wr4, "ready"), i[4].getResult(2)); connect(i[4].getResult(3), field(wr4, "valid"));
  connect(i[4].getResult(4), field(wr4, "bits"));
  connect(field(rd4, "valid"), constant(1, 0)); connect(field(rd4, "bits"), constant(32, 0));
  for (unsigned word = 6; word <= 7; ++word) {
    auto rd = lane("read", word), wr = lane("write", word); unsigned p = word == 6 ? 2 : 7;
    connect(field(wr, "ready"), i[5].getResult(p)); connect(i[5].getResult(p + 1), field(wr, "valid"));
    connect(i[5].getResult(p + 2), field(wr, "bits"));
    connect(field(rd, "valid"), i[5].getResult(p + 3)); connect(field(rd, "bits"), i[5].getResult(p + 4));
    if (word == 7) connect(i[5].getResult(12), field(rd, "ready"));
  }
  auto rd8 = lane("read", 8), wr8 = lane("write", 8);
  connect(i[6].getResult(5), field(rd8, "ready"));
  connect(field(rd8, "valid"), i[6].getResult(6)); connect(field(rd8, "bits"), i[6].getResult(7));
  connect(field(wr8, "ready"), constant(1, 0));
  auto active = b.create<NotPrimOp>(loc, arg(1));
  b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, field(rd4, "ready")), active,
      "Can only write to this decoupled sink", ValueRange{}, "");
  b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, field(wr8, "valid")), active,
      "Can only read from this decoupled source", ValueRange{}, "");
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [p, info] : llvm::enumerate(old.getPorts())) if (p != *mcr) { copied.push_back(p); ports.push_back(info); }
  llvm::append_range(ports, ArrayRef<PortInfo>(ep).drop_front(3));
  b.setInsertionPointToEnd(staged->getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(newName), old.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, old, "sim"), loadmem = b.create<InstanceOp>(loc, engine, "loadmem");
  for (auto [p, index] : llvm::enumerate(copied)) {
    bool input = old.getPortDirection(index) == Direction::In;
    b.create<ConnectOp>(loc, input ? sim.getResult(index) : wrapper.getArgument(p), input ? wrapper.getArgument(p) : sim.getResult(index));
    if (index == *clock || index == *reset) connect(loadmem.getResult(index == *clock ? 0 : 1), wrapper.getArgument(p));
  }
  b.create<ConnectOp>(loc, sim.getResult(*mcr), loadmem.getResult(2));
  for (unsigned p = 3; p < ep.size(); ++p) {
    auto outer = wrapper.getArgument(copied.size() + p - 3); bool input = ep[p].direction == Direction::In;
    connect(input ? loadmem.getResult(p) : outer, input ? outer : loadmem.getResult(p));
  }
  if (failed(verify(*staged))) return reject("Print LoadMem engine catalog types or composed FIRRTL differ");
  // Copied targets move to the new public top. The consumed MCR identity
  // remains on the retained inner top; recurse through nested annotation data.
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue(); std::string oldPrefix = "~" + oldName.str(), newPrefix = "~" + newName.str();
      if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return a;
      std::string suffix = "|" + v.str(); StringRef ref(suffix); std::string mp = "|" + oldName.str() + ">";
      if (ref.consume_front(mp)) {
        auto root = ref.take_front(ref.find_first_of(".["));
        for (auto p : copied) if (root == old.getPortName(p)) { suffix.replace(0, mp.size(), "|" + newName.str() + ">"); break; }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto xs = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto x : xs) out.push_back(retarget(x)); return b.getArrayAttr(out); }
    if (auto xs = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto x : xs) out.set(x.getName(), retarget(x.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  auto raw = retarget(circuit->getAttr("rawAnnotations"));
  for (auto m : {engine, wrapper}) m->moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", raw); circuit.setName(newName);
  return success();
}

// Requires the verified LoadMem attachment and live expanded control allocation.
// AW/W scalar targets stay on the retained inner top; copied ports and the
// remaining FASED channels move to the new top. No annotation classes consumed.
// Stage the source reservation queue, arbiter and wrapper atomically. Preserve
// all existing operations/ports/SSA arguments; rebuild hierarchy/target analyses.
LogicalResult goldengate::mapPrintBridgeRocketHostMemoryWriteArbiter(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGPrintLoadMemWrapper")
    return reject("Rocket Print host write arbitration requires the LoadMem attachment");
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("Rocket Print host write arbitration requires the annotation archive");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addHostMemoryWriteArbiter(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print host write arbitration produced invalid FIRRTL IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGHostMemoryWriteWrapper" ||
          m.getName() == "GGHostMemoryWriteArbiter" ||
          m.getName() == "GGHostMemoryWriteSourceQueue2")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// Join host B to the expanded Print/LoadMem/FASED AW/W boundary. The shared
// FIRRTL router implements AXI4Xbar's ID selection, truncation and selected
// sink backpressure. Consumed B targets remain on the retained inner module.
// Stage and verify both additions before changing any live operation identity.
LogicalResult goldengate::mapPrintBridgeRocketHostMemoryWriteResponses(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGHostMemoryWriteWrapper")
    return reject("Rocket Print host write responses require the AW/W attachment");
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("Rocket Print host write responses require the annotation archive");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(routeHostMemoryWriteResponses(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print host write responses produced invalid FIRRTL IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGHostMemoryWriteResponseWrapper" ||
          m.getName() == "GGHostMemoryWriteResponseRouter")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// Attach host AR arbitration to the expanded Print/LoadMem/FASED boundary.
// Stage the shared CIRCT pass and verify before moving only the two additions;
// existing operations and their SSA values retain their live identities.
LogicalResult goldengate::mapPrintBridgeRocketHostMemoryReadArbiter(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGHostMemoryWriteResponseWrapper")
    return reject("Rocket Print host read arbitration requires the AW/W/B attachment");
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("Rocket Print host read arbitration requires the annotation archive");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addHostMemoryReadArbiter(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print host read arbitration produced invalid FIRRTL IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGHostMemoryReadWrapper" ||
          m.getName() == "GGHostMemoryReadArbiter")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// Complete the expanded Print host master with the shared AXI4Xbar R router.
// Required: recorded allocation and uninstantiated AW/W/B/AR wrapper; the shared
// pass validates constructor, types and unique scalar/aggregate connections.
// Consume no annotations; preserve prior analyses/bodies and consumed R targets,
// transfer copied ports and whole AW/W/B/AR targets in the recursive archive.
// Stage and verify additions before committing; rejection preserves live SSA.
LogicalResult goldengate::mapPrintBridgeRocketHostMemoryReadResponses(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGHostMemoryReadWrapper")
    return reject("Rocket Print host read responses require the AW/W/B/AR attachment");
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("Rocket Print host read responses require the annotation archive");
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(routeHostMemoryReadResponses(*staged, error))) return failure();
  if (failed(verify(*staged)))
    return reject("Rocket Print host read responses produced invalid FIRRTL IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGHostMemoryReadResponseWrapper" ||
          m.getName() == "GGHostMemoryReadResponseRouter")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}

// FPGATop's final AXI4Buffer applies to the unified Print/Rocket host master.
// Stage the shared queue pass and verify the complete expanded circuit before
// transferring new definitions. Keep live top arguments, channel subfields,
// connections and the recursive annotation archive at their original identities.
LogicalResult goldengate::mapPrintBridgeRocketHostMemoryOutputBuffer(
    CircuitOp circuit, std::string &error) {
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addHostMemoryOutputBuffer(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "Rocket Print host output buffer produced invalid FIRRTL IR";
    return failure();
  }
  FModuleOp top, helper;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  Value host, clock, reset;
  for (auto [i, p] : llvm::enumerate(top.getPorts())) {
    auto arg = top.getArguments()[i];
    if (p.name == "host_mem") host = arg;
    if (p.name == "hostClock") clock = arg;
    if (p.name == "hostReset") reset = arg;
  }
  SmallVector<SubfieldOp> channels;
  for (auto &use : host.getUses()) channels.push_back(cast<SubfieldOp>(use.getOwner()));
  const StringRef definitions[]{"GGHostMemoryOutputBuffer", "GGHostMemoryAddressQueue2",
      "GGHostMemoryWriteQueue2", "GGHostMemoryAckQueue2", "GGHostMemoryReadQueue2"};
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (llvm::is_contained(definitions, m.getName())) {
        if (m.getName() == definitions[0]) helper = m;
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
      }
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->begin()); auto loc = top.getLoc();
  auto buffer = b.create<InstanceOp>(loc, helper, "host_memory_buffer");
  b.create<StrictConnectOp>(loc, buffer.getResult(0), clock);
  b.create<StrictConnectOp>(loc, buffer.getResult(1), reset);
  b.create<ConnectOp>(loc, host, buffer.getResult(3));
  for (auto field : channels) field->setOperand(0, buffer.getResult(2));
  return success();
}

// FPGATop.scala's mem IO <> memAXI4Nodes converts the internal five-bit IDs
// to the platform's sixteen-bit IDs. Stage the shared pass on the complete
// expanded Print circuit; commit only verified additions and target transfers.
// Every prior module, operation and SSA argument retains its live identity.
LogicalResult goldengate::mapPrintBridgeRocketHostMemoryPort(
    CircuitOp circuit, std::string &error) {
  if (failed(validateRocketPrintFASEDAllocation(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(specializeHostMemoryPort(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "Rocket Print host memory platform port produced invalid FIRRTL IR";
    return failure();
  }
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleOp>(&op))
      if (m.getName() == "GGHostMemoryPlatformWrapper" ||
          m.getName() == "GGHostMemoryPortAdapter")
        op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  circuit.setName(staged->getName());
  return success();
}
