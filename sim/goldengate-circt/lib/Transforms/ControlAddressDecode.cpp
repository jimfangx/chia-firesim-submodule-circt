// See LICENSE for license details.
// Golden Gate widget regions are half-open byte ranges. Construct address
// comparisons and one-hot/encoded selections directly as CIRCT FIRRTL ops.
// The encoder's no-match value selects NastiRouter's stateful error endpoint.
#include "goldengate/ControlRegionGeometry.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AssertionBridge.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringMap.h"
#include <set>
#include <algorithm>
#include <functional>
#include <map>
using namespace mlir;
using namespace circt::firrtl;

namespace {
// These symbols are assigned in BridgeIO constructor order by the native
// TracerV trigger/control passes. Do not infer the number of widgets from the
// CPU stream list: a stale list could otherwise hide an entire control bank.
LogicalResult tracerVBanks(CircuitOp circuit, SmallVectorImpl<FModuleOp> &banks,
                          std::string &error) {
  std::map<unsigned, FModuleOp> configs, adapters;
  auto reject = [&]() {
    error = "Rocket control catalog requires contiguous paired TracerV configuration/MCR banks";
    return failure();
  };
  for (auto module : circuit.getOps<FModuleLike>()) {
    StringRef name = module.getName();
    for (auto prefix : {StringRef("GGTracerVTriggerConfig"), StringRef("GGTracerVMCRFile")}) {
      if (!name.starts_with(prefix)) continue;
      auto suffix = name.drop_front(prefix.size()); unsigned index = 0;
      if (!suffix.empty() && (!suffix.consume_front("_") || suffix.getAsInteger(10, index) ||
          !index || suffix != std::to_string(index))) return reject();
      auto native = dyn_cast<FModuleOp>(module.getOperation());
      auto &entries = prefix == "GGTracerVTriggerConfig" ? configs : adapters;
      if (!native || !entries.emplace(index, native).second) return reject();
    }
  }
  if (configs.size() != adapters.size()) return reject();
  SmallVector<FModuleOp> ordered;
  for (unsigned i = 0; i < configs.size(); ++i) {
    if (!configs.count(i) || !adapters.count(i)) return reject();
    ordered.push_back(configs.at(i));
  }
  banks.assign(ordered.begin(), ordered.end());
  return success();
}
// One independent MCRFile per NIC, even though all use one bank definition.
// Names are assigned by mapNICBridgeControl in retained constructor order.
LogicalResult nicAdapters(CircuitOp circuit, SmallVectorImpl<FModuleOp> &ordered,
                          std::string &error) {
  std::map<unsigned, FModuleOp> adapters;
  for (auto m : circuit.getOps<FModuleLike>()) {
    auto name = m.getName();
    if (!name.starts_with("GGNICMCRFile")) continue;
    auto suffix = name.drop_front(StringRef("GGNICMCRFile").size()); unsigned i = 0;
    auto native = dyn_cast<FModuleOp>(m.getOperation());
    if ((!suffix.empty() && (!suffix.consume_front("_") || suffix.getAsInteger(10, i) ||
         !i || suffix != std::to_string(i))) || !native ||
         !adapters.emplace(i, native).second) {
      error = "NIC catalog needs canonical independent MCR adapter indices"; return failure();
    }
  }
  ordered.clear();
  for (unsigned i = 0; i < adapters.size(); ++i) {
    auto it = adapters.find(i);
    if (it == adapters.end()) { error = "NIC catalog has missing adapters"; return failure(); }
    ordered.push_back(it->second);
  }
  return success();
}

}

LogicalResult goldengate::deriveControlMMIORegistry(CircuitOp circuit,
    StringRef widgetName, ArrayRef<StringRef> registerModules,
    ControlMMIOWidget &widget, std::string &error) {
  auto reject = [&](StringRef why) {
    error = "control widget '" + widgetName.str() + "': " + why.str();
    return failure();
  };
  if (widgetName.empty() || registerModules.empty())
    return reject("requires widget and register module identities");
  auto find = [&](StringRef name) -> FModuleOp {
    for (auto module : circuit.getOps<FModuleOp>())
      if (module.getName() == name) return module;
    return {};
  };
  llvm::StringSet<> modules, names;
  std::set<uint64_t> offsets;
  for (auto name : registerModules) {
    if (name.empty() || !modules.insert(name).second)
      return reject("register module identities must be nonempty and unique");
    auto module = find(name);
    auto rows = module ? module->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr();
    if (!rows || rows.empty()) return reject("register module or nonempty registry is missing");
    for (auto attr : rows) {
      auto row = dyn_cast<DictionaryAttr>(attr);
      auto label = row ? row.getAs<StringAttr>("name") : StringAttr();
      auto offset = row ? row.getAs<IntegerAttr>("offset") : IntegerAttr();
      auto read = row ? row.getAs<BoolAttr>("readable") : BoolAttr();
      auto write = row ? row.getAs<BoolAttr>("writeable") : BoolAttr();
      if (!label || label.getValue().empty() || !names.insert(label.getValue()).second ||
          !offset || offset.getValue().getBitWidth() > 64 || offset.getValue().isNegative() ||
          offset.getValue().getZExtValue() % 4 || !read || !write ||
          (!read.getValue() && !write.getValue()))
        return reject("registry needs unique names, aligned nonnegative offsets and permissions");
      if (!offsets.insert(offset.getValue().getZExtValue()).second)
        return reject("register offsets overlap");
    }
  }
  uint64_t count = offsets.size();
  if (*offsets.begin() != 0 || *offsets.rbegin() / 4 != count - 1)
    return reject("register registry has missing words");
  widget = {widgetName, count};
  return success();
}

LogicalResult goldengate::deriveControlMMIOWidget(CircuitOp circuit,
    StringRef widgetName, StringRef mcrModule, ArrayRef<StringRef> registerModules,
    ControlMMIOWidget &widget, std::string &error, Direction mcrDirection) {
  ControlMMIOWidget registry;
  if (failed(deriveControlMMIORegistry(circuit, widgetName, registerModules, registry, error)))
    return failure();
  auto reject = [&](StringRef why) {
    error = "control widget '" + widgetName.str() + "': " + why.str(); return failure();
  };
  auto find = [&](StringRef name) -> FModuleOp {
    for (auto module : circuit.getOps<FModuleOp>()) if (module.getName()==name) return module;
    return {};
  };
  uint64_t count=registry.registerCount;
  // Validate the implemented MCR bank, not just the collateral registry. This
  // catches a stale schema before it can change global region/slave allocation.
  auto adapter = find(mcrModule);
  if (!adapter) return reject("MCRFile module is missing");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)},
      {b.getStringAttr("bits"), false, uint(32)}});
  auto lanes = FVectorType::get(token, count);
  auto expected = BundleType::get(ctx, {{b.getStringAttr("read"), false, lanes},
      {b.getStringAttr("write"), true, lanes},
      {b.getStringAttr("wstrb"), true, uint(4)}});
  unsigned matched = 0;
  for (auto port : adapter.getPorts()) if (port.name == "mcr") {
    if (port.direction != mcrDirection || port.type != expected)
      return reject("MCRFile lanes differ from the register registry");
    ++matched;
  }
  if (matched != 1) return reject("requires exactly one MCRFile bank port");
  widget = {widgetName, count};
  return success();
}

LogicalResult goldengate::validateNICControlMMIOMode(CircuitOp circuit,
    unsigned widgets, uint64_t registerCount, std::string &error) {
  auto reject = [&]() {
    error = "NIC control bank requires the six-word normal ABI or live singleton loopback done ABI";
    return failure();
  };
  if (!widgets || (registerCount != 1 && registerCount != 6)) return reject();
  FModuleOp bank, bindingOwner;
  ArrayAttr bindings;
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module.getName() == "GGNICMMIOBank") bank = module;
    if (auto rows = module->getAttrOfType<ArrayAttr>("goldengate.nicMMIOBindings")) {
      if (bindingOwner) return reject();
      bindingOwner = module; bindings = rows;
    }
  }
  // Legacy normal-mode decoded-bank fixtures omit binding metadata. A
  // singleton can never use that compatibility path.
  if (!bank) return reject();
  if (registerCount == 6 && !bindings) return success();
  auto registers = bank ? bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr{};
  if (!registers || registers.size() != registerCount || !bindings || bindings.size() != widgets)
    return reject();
  if (registerCount == 1) {
    // SFC genROReg uses attach's default ReadWrite permissions: writes can
    // override this sampled done register. Preserve that actual ABI.
    auto row = dyn_cast<DictionaryAttr>(registers[0]);
    auto offset = row ? row.getAs<IntegerAttr>("offset") : IntegerAttr{};
    auto read = row ? row.getAs<BoolAttr>("readable") : BoolAttr{};
    auto write = row ? row.getAs<BoolAttr>("writeable") : BoolAttr{};
    if (!row || row.getAs<StringAttr>("name") != "done" || !offset || offset.getInt() != 0 ||
        !read || !read.getValue() || !write || !write.getValue()) return reject();
  }
  std::set<std::string> identities;
  for (auto [index, attr] : llvm::enumerate(bindings)) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto widget = row ? row.getAs<IntegerAttr>("widget") : IntegerAttr{};
    auto name = row ? row.getAs<StringAttr>("registers") : StringAttr{};
    auto mcr = row ? row.getAs<StringAttr>("mcr") : StringAttr{};
    auto mode = row ? row.getAs<StringAttr>("mode") : StringAttr{};
    auto expectedMCR = widgets == 1 ? std::string("nicBridge_mcr") :
        "nicBridge_" + std::to_string(index) + "_mcr";
    if (!widget || widget.getInt() != int64_t(index) || !name ||
        !identities.insert(name.getValue().str()).second || !mcr || mcr.getValue() != expectedMCR ||
        !mode || mode.getValue() != (registerCount == 1 ? "compile_time_loopback" : "normal"))
      return reject();
    unsigned instances = 0;
    for (auto instance : bindingOwner.getOps<InstanceOp>())
      if (instance.getName() == name.getValue() && instance.getModuleName() == bank.getName()) ++instances;
    if (instances != 1) return reject();
  }
  return success();
}

LogicalResult goldengate::deriveRocketControlMMIOCatalog(CircuitOp circuit,
    ArrayRef<FModuleOp> printHosts, SmallVectorImpl<ControlMMIOWidget> &widgets,
    std::string &error) {
  SmallVector<ControlMMIOWidget> catalog;
  auto bank = [&](StringRef name, StringRef mcr, ArrayRef<StringRef> registers,
                  Direction direction = Direction::In) {
    ControlMMIOWidget descriptor;
    if (failed(deriveControlMMIOWidget(circuit, name, mcr, registers,
                                     descriptor, error, direction))) return failure();
    catalog.push_back(descriptor); return success();
  };
  if (failed(bank("SimulationMaster_0", "GGSimulationMasterBank",
                  {"GGSimulationMasterBank"}, Direction::Out)) ||
      failed(bank("PeekPokeBridgeModule_0", "GGPeekPokeMCRFile", {"GGPeekPokeMMIOBank"})) ||
      failed(bank("ResetPulseBridgeModule_0", "GGResetPulseBridgeMCRFile", {"GGResetPulseBridge"})) ||
      failed(bank("BlockDevBridgeModule_0", "GGBlockDevMMIOBank", {"GGBlockDevMMIOBank"}, Direction::Out)) ||
      failed(bank("UARTBridgeModule_0", "GGUARTMCRFile", {"GGUARTMMIOBank"}))) return failure();
  // FASED's adapter is assembled later. Explicit platform capacity selects the
  // FRFCFS bank; maxFlight alone cannot distinguish Shuttle FRFCFS from LBP.
  // Its 68 words allocate 512 bytes and reorder the complete widget address map.
  ControlMMIOWidget fased;
  auto capacityRaw = circuit->getAttr("goldengate.fasedBaseCapacity");
  auto capacity = dyn_cast_or_null<IntegerAttr>(capacityRaw);
  if (capacityRaw && (!capacity || capacity.getValue().getBitWidth() > 64 ||
      (capacity.getInt() != 16 && capacity.getInt() != 32))) {
    error = "control catalog requires a validated FASED base capacity 16 or 32";
    return failure();
  }
  bool frfcfs = capacity && capacity.getInt() == 32;
  for (auto m : circuit.getOps<FModuleLike>()) {
    bool wrongBank = frfcfs ? m.getModuleName() == "GGFASEDLatencyRegisters" ||
                            m.getModuleName() == "GGFASEDRequestLimits"
                          : m.getModuleName() == "GGFASEDFRFCFSMMIO";
    if (wrongBank) {
      error = "FASED programming banks conflict with the explicit platform profile";
      return failure();
    }
  }
  if (frfcfs) {
    if (failed(deriveControlMMIOWidget(circuit, "FASEDMemoryTimingModel_0",
        "GGFASEDFRFCFSMMIO", {"GGFASEDFRFCFSMMIO"}, fased, error,
        Direction::Out))) return failure();
    if (fased.registerCount != 68) {
      error = "selected FRFCFS control bank requires 68 implemented register words";
      return failure();
    }
  } else if (failed(deriveControlMMIORegistry(circuit, "FASEDMemoryTimingModel_0",
        {"GGFASEDLatencyRegisters", "GGFASEDRequestLimits", "GGFASEDHistograms",
         "GGFASEDStatistics", "GGFASEDFunctionalModelRegister", "GGFASEDResponseErrors"},
        fased, error))) return failure();
  catalog.push_back(fased);
  SmallVector<FModuleOp> nics, tracers;
  if (failed(nicAdapters(circuit, nics, error)) ||
      failed(tracerVBanks(circuit, tracers, error))) return failure();
  bool nicBank = llvm::any_of(circuit.getOps<FModuleOp>(), [](FModuleOp m) {
    return m.getName() == "GGNICMMIOBank";
  });
  if (nicBank != !nics.empty()) {
    error = "NIC catalog requires matching decoded bank and independent adapters"; return failure();
  }
  OpBuilder b(circuit.getContext());
  auto addNIC = [&](unsigned i) {
    auto name = b.getStringAttr("SimpleNICBridgeModule_" + std::to_string(i));
    if (failed(bank(name.getValue(), nics[i].getName(), {"GGNICMMIOBank"}))) return failure();
    return validateNICControlMMIOMode(circuit, nics.size(), catalog.back().registerCount, error);
  };
  auto addTracer = [&](unsigned i) {
    auto name = b.getStringAttr("TracerVBridgeModule_" + std::to_string(i));
    auto mcr = b.getStringAttr("GGTracerVMCRFile" + (i ? "_" + std::to_string(i) : ""));
    if (failed(bank(name.getValue(), mcr.getValue(), {tracers[i].getName()}))) return failure();
    if (catalog.back().registerCount != 15) {
      error = "Rocket control catalog requires fifteen implemented TracerV configuration words";
      return failure();
    }
    return success();
  };
  // Match the actual bridge registration order. Stable size sorting later
  // preserves this order for equal-size regions, as HasWidgets does in SFC.
  unsigned ni = 0, ti = 0;
  if (auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    for (auto a : raw) {
      auto row = dyn_cast<DictionaryAttr>(a);
      if (!row || row.getAs<StringAttr>("class") != "firesim.lib.bridgeutils.BridgeIOAnnotation") continue;
      auto cls = row.getAs<StringAttr>("widgetClass");
      if (cls == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
        if (ni >= nics.size() || row.get("widgetConstructorKey") || failed(addNIC(ni++))) {
          error = "NIC constructor catalog differs from live adapters"; return failure();
        }
      } else if (cls == "firechip.goldengateimplementations.TracerVBridgeModule") {
        if (ti >= tracers.size() || failed(addTracer(ti++))) {
          error = "TracerV constructor catalog differs from live adapters"; return failure();
        }
      }
    }
  // Legacy focused register fixtures have no BridgeIO constructors. A partial
  // real constructor list cannot silently hide a widget.
  if (ni || ti) {
    if (ni != nics.size() || ti != tracers.size()) {
      error = "Control catalog omits a live NIC or TracerV constructor"; return failure();
    }
  } else {
    for (unsigned i = 0; i < nics.size(); ++i) if (failed(addNIC(i))) return failure();
    for (unsigned i = 0; i < tracers.size(); ++i) if (failed(addTracer(i))) return failure();
  }
  if (failed(bank("TSIBridgeModule_0", "GGTSIMMIOBank", {"GGTSIMMIOBank"}, Direction::Out)) ||
      failed(bank("ClockBridgeModule_0", "GGClockBridgeMCRFile", {"GGSingleClockBridge"}))) return failure();
  SmallVector<FModuleOp> assertionHosts;
  SmallVector<ControlMMIOWidget> assertionCatalog;
  if (failed(discoverBoundAssertionBridgeHosts(circuit, assertionHosts, error)) ||
      failed(deriveAssertionControlMMIOCatalog(circuit, assertionHosts, assertionCatalog, error)))
    return failure();
  catalog.append(assertionCatalog.begin(), assertionCatalog.end());
  // AutoCounterTransform runs before PrintSynthesis and appends the CSV
  // constructors first. Both participate in HasWidgets' stable size sort.
  SmallVector<FModuleOp> csvHosts;
  SmallVector<ControlMMIOWidget> csvCatalog;
  if (failed(discoverBoundAutoCounterBridgeHosts(circuit, csvHosts, error)) ||
      failed(deriveAutoCounterControlMMIOCatalog(circuit, csvHosts, csvCatalog, error)))
    return failure();
  catalog.append(csvCatalog.begin(), csvCatalog.end());
  // PrintSynthesis.scala appends BridgeIO annotations to the original bridges.
  // Equal-sized Print/Clock banks must retain this registration order after
  // HasWidgets sorts by size; module traversal order is irrelevant.
  llvm::StringSet<> seen, configs, mcrs;
  for (auto [slot, item] : llvm::enumerate(printHosts)) {
    FModuleOp host = item;
    auto info = host ? host->getAttrOfType<DictionaryAttr>("goldengate.printHost") : DictionaryAttr{};
    auto config = info ? info.getAs<StringAttr>("configModule") : StringAttr{};
    auto mcr = info ? info.getAs<StringAttr>("mcrModule") : StringAttr{};
    auto queue = info ? info.getAs<StringAttr>("queueModule") : StringAttr{};
    if (!host || host->getParentOp() != circuit || host.getNumPorts() != 13 ||
        !config || !mcr || !queue || !seen.insert(host.getName()).second ||
        !configs.insert(config.getValue()).second || !mcrs.insert(mcr.getValue()).second) {
      error = "Rocket control catalog requires unique queued Print hosts from this circuit"; return failure();
    }
    auto name = b.getStringAttr("PrintBridgeModule_" + std::to_string(slot));
    if (failed(bank(name.getValue(), mcr.getValue(), {config.getValue()}))) return failure();
    if (catalog.back().registerCount != 6) {
      error = "Rocket control catalog requires six implemented Print configuration words"; return failure();
    }
  }
  for (auto module : circuit.getOps<FModuleOp>()) {
    auto info = module->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    if (info && info.getAs<StringAttr>("queueModule") && !seen.count(module.getName())) {
      error = "Rocket control catalog would omit a materialized queued Print host"; return failure();
    }
  }
  // A linked host catalog retains the construction descriptors on the direct
  // engines. Native assembly still has the original wrappers. Select by live
  // module identity; a partial imported catalog must fail registry validation.
  bool importedLoadMem = llvm::any_of(circuit.getOps<FModuleOp>(), [](FModuleOp m) {
    return m.getName() == "CatalogRocket_GGLoadMemWriteDataFIFO";
  });
  const StringRef loadmemRegisters[]{"GGLoadMemWriteMMIOBank",
      importedLoadMem ? "CatalogRocket_GGLoadMemWriteDataFIFO" : "GGLoadMemWriteDataWrapper",
      importedLoadMem ? "CatalogRocket_GGLoadMemReadRequests" : "GGLoadMemReadRequestWrapper",
      importedLoadMem ? "CatalogRocket_GGLoadMemReadDataFIFO" : "GGLoadMemReadDataWrapper"};
  if (failed(bank("LoadMemWidget_0", "GGLoadMemMCRFile", loadmemRegisters))) return failure();
  bool hasCPU = llvm::any_of(circuit.getOps<FModuleOp>(), [](FModuleOp m) {
    return m.getName() == "GGCPUStreamCountBank" || m.getName() == "GGCPUStreamMCRFile";
  });
  if (hasCPU && failed(bank("CPUManagedStreamEngine_0", "GGCPUStreamMCRFile",
                            {"GGCPUStreamCountBank"}))) return failure();
  widgets.assign(catalog.begin(), catalog.end()); return success();
}

LogicalResult goldengate::allocateRocketControlMMIORegions(CircuitOp circuit,
    unsigned addressBits, ArrayRef<FModuleOp> printHosts,
    SmallVectorImpl<ControlMMIOWidget> &widgets,
    SmallVectorImpl<ControlMMIORegion> &regions, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  SmallVector<ControlMMIOWidget> catalog;
  if (failed(deriveRocketControlMMIOCatalog(circuit, printHosts, catalog, error))) return failure();
  auto tracerCount = llvm::count_if(catalog, [](const auto &widget) {
    return widget.name.starts_with("TracerVBridgeModule_");
  });
  FModuleOp reader, writer, nicBank, counts, bound;
  for (auto m : circuit.getOps<FModuleOp>()) {
    if (m.getName() == "GGCPUStreamRead") reader = m;
    if (m.getName() == "GGCPUStreamWrite") writer = m;
    if (m.getName() == "GGNICMMIOBank") nicBank = m;
    if (m.getName() == "GGCPUStreamCountBank") counts = m;
    if (m.getName() == "GGPrintBridgeHostWrapper") bound = m;
  }
  auto sources = reader ? reader->getAttrOfType<ArrayAttr>("goldengate.sourceStreams") : ArrayAttr{};
  auto sinks = writer ? writer->getAttrOfType<ArrayAttr>("goldengate.sinkStreams") : ArrayAttr{};
  unsigned nicCount = llvm::count_if(catalog, [](const auto &w) {
    return w.name.starts_with("SimpleNICBridgeModule_");
  });
  bool nic = bool(nicBank);
  if (nic != bool(writer) || nic != bool(nicCount) || (nic && (!sinks || sinks.size() != nicCount)))
    return reject("Rocket MMIO allocation requires the complete duplex NIC catalog");
  auto words = counts ? counts->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr{};
  if (!reader && !counts && printHosts.empty() && !tracerCount && !nicCount) {
    SmallVector<ControlMMIORegion> allocated;
    if (failed(allocateControlMMIORegions(addressBits, catalog, allocated, error))) return failure();
    widgets.assign(catalog.begin(), catalog.end());
    regions.assign(allocated.begin(), allocated.end());
    return success();
  }
  if (!sources || !words || sources.size() != printHosts.size() + tracerCount + nicCount ||
      words.size() != sources.size() + (nic ? sinks.size() : 0))
    return reject("Rocket MMIO allocation requires one count word per registered DMA source and sink");
  if (!printHosts.empty()) {
    auto bindings = bound ? bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
    if (!bindings || bindings.size() != printHosts.size())
      return reject("Rocket MMIO allocation requires the bound Print constructor catalog");
    llvm::StringMap<StringAttr> hostNames;
    for (auto attr : bindings) {
      auto row = dyn_cast<DictionaryAttr>(attr);
      auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
      auto host = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
      if (!name || !host || !hostNames.try_emplace(name.getValue(), host).second)
        return reject("Rocket MMIO allocation requires unique complete Print bindings");
    }
    for (auto [i, item] : llvm::enumerate(printHosts)) {
      FModuleOp host = item;
      std::string name = "PrintBridgeModule_" + std::to_string(i);
      auto found = hostNames.find(name); InstanceOp instance;
      for (auto child : bound.getOps<InstanceOp>()) if (child.getName() == name) instance = child;
      if (!host || found == hostNames.end() || found->second != host.getName() ||
          !instance || instance.getModuleName() != host.getName())
        return reject("Rocket MMIO allocation Print order differs from instantiated constructor catalog");
    }
  }
  // CPUManagedStreamEngine attaches count registers in stream registration
  // order. Match metadata by index/name/offset, independently of row order.
  // A well-typed stale bank must not allocate a smaller global region.
  std::set<int64_t> indices;
  llvm::StringMap<DictionaryAttr> registers;
  for (auto attr : words) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name || !registers.try_emplace(name.getValue(), row).second)
      return reject("Rocket MMIO count register names must be complete and unique");
  }
  SmallVector<std::string> expectedSources;
  unsigned ni = 0, ti = 0;
  if (auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    for (auto a : raw) {
      auto row = dyn_cast<DictionaryAttr>(a);
      if (!row || row.getAs<StringAttr>("class") != "firesim.lib.bridgeutils.BridgeIOAnnotation") continue;
      auto cls = row.getAs<StringAttr>("widgetClass");
      if (cls == "firechip.goldengateimplementations.SimpleNICBridgeModule")
        expectedSources.push_back("SIMPLENICBRIDGEMODULE_" + std::to_string(ni++) + "_to_cpu_stream");
      if (cls == "firechip.goldengateimplementations.TracerVBridgeModule")
        expectedSources.push_back("TRACERVBRIDGEMODULE_" + std::to_string(ti++) + "_to_cpu_stream");
    }
  if (ni || ti) {
    if (ni != nicCount || ti != tracerCount)
      return reject("MMIO source catalog differs from constructor order");
  } else {
    for (unsigned i = 0; i < nicCount; ++i)
      expectedSources.push_back("SIMPLENICBRIDGEMODULE_" + std::to_string(i) + "_to_cpu_stream");
    for (unsigned i = 0; i < tracerCount; ++i)
      expectedSources.push_back("TRACERVBRIDGEMODULE_" + std::to_string(i) + "_to_cpu_stream");
  }
  for (unsigned i = 0; i < printHosts.size(); ++i)
    expectedSources.push_back("PRINTBRIDGEMODULE_" + std::to_string(i) + "_to_cpu_stream");
  bool f2Ack = reader->hasAttr("goldengate.f2CPUWordReads");
  if (f2Ack && failed(validateF2CPUStreamWordReads(reader, error))) return failure();
  for (auto attr : sources) {
    auto source = dyn_cast<DictionaryAttr>(attr);
    auto name = source ? source.getAs<StringAttr>("name") : StringAttr{};
    auto index = source ? source.getAs<IntegerAttr>("index") : IntegerAttr{};
    if (!index || index.getValue().getBitWidth() > 64 || index.getInt() < 0 ||
        uint64_t(index.getInt()) >= sources.size() || !indices.insert(index.getInt()).second)
      return reject("Rocket MMIO DMA indices must be unique and contiguous");
    auto i = index.getInt();
    const auto &expected = expectedSources[i];
    auto found = registers.find(expected + "_count");
    auto word = found == registers.end() ? DictionaryAttr{} : found->second;
    auto offset = word ? word.getAs<IntegerAttr>("offset") : IntegerAttr{};
    auto read = word ? word.getAs<BoolAttr>("readable") : BoolAttr{};
    auto write = word ? word.getAs<BoolAttr>("writeable") : BoolAttr{};
    if (!name || name != expected || !offset || offset.getValue().getBitWidth() > 64 || offset.getInt() != i * 4 ||
        !read || !read.getValue() || !write || write.getValue() != f2Ack)
      return reject("Rocket MMIO count register differs from ordered read-only DMA source");
  }
  // CPUManagedStreamEngine registers all sources before all sinks; the sink
  // index belongs to its own address space, while its MMIO word follows sources.
  for (unsigned i = 0; i < nicCount; ++i) {
    auto sink = dyn_cast<DictionaryAttr>(sinks[i]);
    auto name = sink ? sink.getAs<StringAttr>("name") : StringAttr{};
    auto index = sink ? sink.getAs<IntegerAttr>("index") : IntegerAttr{};
    auto expected = "SIMPLENICBRIDGEMODULE_" + std::to_string(i) + "_from_cpu_stream";
    auto found = registers.find(expected + "_count");
    auto word = found == registers.end() ? DictionaryAttr{} : found->second;
    auto offset = word ? word.getAs<IntegerAttr>("offset") : IntegerAttr{};
    auto read = word ? word.getAs<BoolAttr>("readable") : BoolAttr{};
    auto write = word ? word.getAs<BoolAttr>("writeable") : BoolAttr{};
    if (!name || name != expected || !index ||
        index.getValue().getBitWidth() > 64 || index.getInt() != i || !offset ||
        offset.getValue().getBitWidth() > 64 || offset.getInt() != int64_t((sources.size() + i) * 4) ||
        !read || !read.getValue() || !write || write.getValue())
      return reject("Rocket MMIO sink count differs from ordered read-only DMA sink");
  }
  SmallVector<ControlMMIORegion> allocated;
  if (failed(allocateControlMMIORegions(addressBits, catalog, allocated, error))) return failure();
  widgets.assign(catalog.begin(), catalog.end());
  regions.assign(allocated.begin(), allocated.end());
  return success();
}

LogicalResult goldengate::allocateControlMMIORegions(unsigned addressBits,
    ArrayRef<ControlMMIOWidget> widgets,
    SmallVectorImpl<ControlMMIORegion> &regions, std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (!addressBits || addressBits > 63 || !goldengate::validControlRegionCount(widgets.size()))
    return reject("control allocation needs 1..63 address bits and a representable nonempty widget catalog");
  uint64_t limit = uint64_t(1) << addressBits;
  SmallVector<ControlMMIORegion> allocated;
  for (auto [i, widget] : llvm::enumerate(widgets)) {
    if (widget.name.empty()) return reject("control widget name must be nonempty");
    for (auto prior : widgets.take_front(i))
      if (prior.name == widget.name) return reject("control widget names must be unique");
    uint64_t size;
    if (widget.customSize) {
      // Scala customSize replaces the register-derived size; WidgetRegion
      // requires the supplied size to be a power of two.
      size = *widget.customSize;
      if (!llvm::isPowerOf2_64(size))
        return reject("control custom region size must be a positive power of two");
    } else {
      // Check before multiplying or rounding. The largest supported address
      // space is 2^63 bytes, whose last endpoint still fits uint64_t.
      if (!widget.registerCount || widget.registerCount > limit / 4)
        return reject("control register bank exceeds the address space or is empty");
      size = uint64_t(1) << llvm::Log2_64_Ceil(widget.registerCount * 4);
    }
    if (size > limit) return reject("control widget region exceeds the address space");
    allocated.push_back({widget.name, 0, size});
  }
  std::stable_sort(allocated.begin(), allocated.end(),
      [](const auto &a, const auto &b) { return a.size > b.size; });
  uint64_t start = 0;
  for (auto &region : allocated) {
    if (region.size > limit - start)
      return reject("control widgets collectively exceed the address space");
    region.start = start;
    start += region.size;
  }
  regions.assign(allocated.begin(), allocated.end());
  return success();
}

LogicalResult goldengate::addControlAddressDecode(CircuitOp circuit,
    unsigned addressBits, ArrayRef<ControlMMIORegion> regions, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlDecodeWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlAddressDecode";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (!addressBits || addressBits > 63 || !goldengate::validControlRegionCount(regions.size()))
    return reject("control decoder needs 1..63 address bits and a representable nonempty region catalog");
  uint64_t limit = uint64_t(1) << addressBits;
  for (auto [i, region] : llvm::enumerate(regions)) {
    if (region.name.empty() || !region.size || region.start >= limit || region.size > limit - region.start)
      return reject("control decoder region name or bounds are invalid");
    for (auto prior : regions.take_front(i)) {
      if (prior.name == region.name) return reject("control decoder region names must be unique");
      if (region.start < prior.start + prior.size && prior.start < region.start + region.size)
        return reject("control decoder regions overlap");
    }
  }
  if (circuit.getName() != "GGControlErrorWrapper")
    return reject("control decoder requires the control error wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("control decoder helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("control decoder needs a top and retained annotations");
  for (auto port : inner.getPorts())
    if (port.name.getValue().starts_with("ctrl_decode_")) return reject("control decoder boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("control decoder requires an uninstantiated top");

  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(context, width, false); };
  unsigned count = regions.size(), indexBits = goldengate::controlRouteWidth(count);
  SmallVector<PortInfo> helperPorts;
  for (auto name : {"aw_addr", "ar_addr"})
    helperPorts.push_back({b.getStringAttr(name), uint(addressBits), Direction::In});
  for (auto name : {"aw_route", "ar_route"})
    helperPorts.push_back({b.getStringAttr(name), uint(count), Direction::Out});
  for (auto name : {"aw_target", "ar_target"})
    helperPorts.push_back({b.getStringAttr(name), uint(indexBits), Direction::Out});
  for (auto name : {"aw_error", "ar_error"})
    helperPorts.push_back({b.getStringAttr(name), uint(1), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(context, Convention::Internal), helperPorts);
  SmallVector<Attribute> entries;
  for (auto [i, r] : llvm::enumerate(regions)) entries.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(r.name)),
      b.getNamedAttr("start", b.getI64IntegerAttr(r.start)),
      b.getNamedAttr("size", b.getI64IntegerAttr(r.size)),
      b.getNamedAttr("slave", b.getI32IntegerAttr(i))}));
  helper->setAttr("goldengate.controlRegions", b.getArrayAttr(entries));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned width, uint64_t v) -> Value {
    return b.create<ConstantOp>(loc, uint(width), APInt(width, v));
  };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  for (unsigned channel = 0; channel < 2; ++channel) {
    // Widen by one bit so a region ending exactly at 2^addressBits is legal.
    Value address = b.create<PadPrimOp>(loc, arg(channel), addressBits + 1);
    Value target = constant(indexBits, count), route;
    for (auto [i, r] : llvm::enumerate(regions)) {
      Value low = b.create<NotPrimOp>(loc, b.create<LTPrimOp>(loc, address, constant(addressBits + 1, r.start)));
      Value high = b.create<LTPrimOp>(loc, address, constant(addressBits + 1, r.start + r.size));
      Value selected = b.create<AndPrimOp>(loc, low, high);
      route = route ? Value(b.create<CatPrimOp>(loc, selected, route)) : selected;
      target = b.create<MuxPrimOp>(loc, selected, constant(indexBits, i), target);
    }
    connect(arg(2 + channel), route); connect(arg(4 + channel), target);
    connect(arg(6 + channel), b.create<EQPrimOp>(loc, route, constant(count, 0)));
  }
  SmallVector<PortInfo> ports(inner.getPorts()); unsigned first = ports.size();
  for (auto port : helperPorts) {
    port.name = b.getStringAttr("ctrl_decode_" + port.name.getValue().str()); ports.push_back(port);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto decoder = b.create<InstanceOp>(loc, helper, "controlDecode");
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
        port.direction == Direction::In ? external : sim.getResult(i));
  }
  for (unsigned i = 0; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(first + i);
    if (helperPorts[i].direction == Direction::In) connect(decoder.getResult(i), external);
    else connect(external, decoder.getResult(i));
  }
  // Retain classes/constructors and internal model identities; transfer copied
  // top ports while changing the circuit prefix for all retained targets.
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto port : inner.getPorts()) if (name == port.name.getValue()) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}
