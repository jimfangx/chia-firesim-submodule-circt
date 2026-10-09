// See LICENSE for license details.
// Golden Gate widget regions are half-open byte ranges. Construct address
// comparisons and one-hot/encoded selections directly as CIRCT FIRRTL ops.
// The encoder's no-match value selects NastiRouter's stateful error endpoint.
#include "goldengate/ControlAddressDecode.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/StringSet.h"
#include <set>
#include <algorithm>
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

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
  // FASED's adapter is assembled later; its six sparse fragments collectively
  // define the complete bank at this allocation boundary.
  ControlMMIOWidget fased;
  if (failed(deriveControlMMIORegistry(circuit, "FASEDMemoryTimingModel_0",
      {"GGFASEDLatencyRegisters", "GGFASEDRequestLimits", "GGFASEDHistograms",
       "GGFASEDStatistics", "GGFASEDFunctionalModelRegister", "GGFASEDResponseErrors"},
      fased, error))) return failure();
  catalog.push_back(fased);
  if (failed(bank("TracerVBridgeModule_0", "GGTracerVMCRFile", {"GGTracerVTriggerConfig"})) ||
      failed(bank("TSIBridgeModule_0", "GGTSIMMIOBank", {"GGTSIMMIOBank"}, Direction::Out)) ||
      failed(bank("ClockBridgeModule_0", "GGClockBridgeMCRFile", {"GGSingleClockBridge"}))) return failure();
  // PrintSynthesis.scala appends BridgeIO annotations to the original bridges.
  // Equal-sized Print/Clock banks must retain this registration order after
  // HasWidgets sorts by size; module traversal order is irrelevant.
  OpBuilder b(circuit.getContext()); llvm::StringSet<> seen, configs, mcrs;
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
  if (failed(bank("LoadMemWidget_0", "GGLoadMemMCRFile",
      {"GGLoadMemWriteMMIOBank", "GGLoadMemWriteDataWrapper",
       "GGLoadMemReadRequestWrapper", "GGLoadMemReadDataWrapper"})) ||
      failed(bank("CPUManagedStreamEngine_0", "GGCPUStreamMCRFile", {"GGCPUStreamCountBank"}))) return failure();
  widgets.assign(catalog.begin(), catalog.end()); return success();
}

LogicalResult goldengate::allocateControlMMIORegions(unsigned addressBits,
    ArrayRef<ControlMMIOWidget> widgets,
    SmallVectorImpl<ControlMMIORegion> &regions, std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (!addressBits || addressBits > 63 || widgets.empty() || widgets.size() > 63)
    return reject("control allocation needs 1..63 address bits and 1..63 widgets");
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
  if (!addressBits || addressBits > 63 || regions.empty() || regions.size() > 63)
    return reject("control decoder needs 1..63 address bits and 1..63 regions");
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
  unsigned count = regions.size(), indexBits = std::max(1u, llvm::Log2_64_Ceil(count + 1));
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
