// See LICENSE for license details.
// Widget.scala binds each widget's io.ctrl to its allocated Nasti slave.
// Connect requests and readiness through FIRRTL operations; preserve the
// independent AW/W routes and the MCRFile's independent request capture.
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlPortWidth.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AssertionBridge.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

namespace {
struct WidgetPort { const char *widget; const char *port; };
constexpr WidgetPort widgetPorts[]{
  {"TracerVBridgeModule_0", "tracerv_ctrl"},
  {"LoadMemWidget_0", "loadmem_ctrl"},
  {"PeekPokeBridgeModule_0", "peekPokeBridge_ctrl"},
  {"UARTBridgeModule_0", "uartBridge_ctrl"},
  {"ClockBridgeModule_0", "clockBridge_ctrl"},
  {"ResetPulseBridgeModule_0", "resetBridge_ctrl"},
  {"CPUManagedStreamEngine_0", "cpuStream_ctrl"}};
struct Binding { std::string widget; std::string port; unsigned slave; };
struct Field { const char *name; unsigned width; };
constexpr Field dataFields[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
bool dispatched(llvm::StringRef channel, llvm::StringRef field) {
  return channel == "aw" ? field == "addr" || field == "len" || field == "id"
                         : field == "data" || field == "last";
}
}

LogicalResult goldengate::bindControlWidgetWrites(CircuitOp circuit,
                                                 std::string &error) {
  SmallVector<ControlWidgetPort> widgets;
  for (auto w : widgetPorts) widgets.push_back({w.widget, w.port});
  return bindControlWidgetWrites(circuit, widgets, error);
}

LogicalResult goldengate::bindRocketControlWidgetWrites(CircuitOp circuit,
                                                       std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  FModuleOp bound, top, decoder;
  std::map<std::string, FModuleOp> modules;
  for (auto m : circuit.getOps<FModuleOp>()) {
    modules.emplace(m.getName().str(), m);
    if (m.getName() == "GGPrintBridgeHostWrapper") bound = m;
    if (m.getName() == circuit.getName()) top = m;
    if (m.getName() == "GGControlAddressDecode") decoder = m;
  }
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr{};
  if (!top || !regions) return reject("Rocket request binding requires an active top and allocation");
  std::set<std::string> allocatedPrints, allocatedTracers;
  std::set<std::string> allocatedNICs;
  for (auto attr : regions) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr{};
    if (!name) return reject("Rocket request allocation lacks a widget identity");
    if (name.getValue().starts_with("SimpleNICBridgeModule_")) {
      if (!allocatedNICs.insert(name.getValue().str()).second)
        return reject("Rocket request allocation duplicates a NIC bank");
    }
    if (name.getValue().starts_with("TracerVBridgeModule_") &&
        !allocatedTracers.insert(name.getValue().str()).second)
      return reject("Rocket request allocation duplicates a TracerV bank");
    if (name.getValue().starts_with("PrintBridgeModule_") &&
        !allocatedPrints.insert(name.getValue().str()).second)
      return reject("Rocket request allocation duplicates a Print bank");
  }
  // mapTracerVBridgeControls assigns these symbols in constructor order.
  // Discover actual adapters, rather than deriving a count from allocation:
  // an omitted region must not silently drop an instantiated control bank.
  std::map<unsigned, FModuleOp> tracers;
  constexpr StringLiteral tracerPrefix = "GGTracerVMCRFile";
  for (auto module : circuit.getOps<FModuleLike>()) {
    StringRef name = module.getName();
    if (!name.starts_with(tracerPrefix)) continue;
    auto suffix = name.drop_front(tracerPrefix.size()); unsigned index = 0;
    if (!suffix.empty() && (!suffix.consume_front("_") || suffix.getAsInteger(10, index) ||
        !index || suffix != std::to_string(index)))
      return reject("Rocket request binding requires canonical TracerV adapter indices");
    auto native = dyn_cast<FModuleOp>(module.getOperation());
    if (!native || index >= 63 || !tracers.emplace(index, native).second)
      return reject("Rocket request binding requires unique native TracerV adapters");
  }
  if (tracers.empty() || tracers.size() != allocatedTracers.size())
    return reject("Rocket request allocation must identify every instantiated TracerV bank");
  SmallVector<ControlWidgetPort> widgets;
  for (unsigned i = 0; i < tracers.size(); ++i) {
    auto suffix = i ? "_" + std::to_string(i) : "";
    auto widget = "TracerVBridgeModule_" + std::to_string(i);
    auto port = "tracerv_ctrl" + suffix;
    auto it = tracers.find(i);
    if (it == tracers.end() || !allocatedTracers.erase(widget))
      return reject("Rocket request binding requires contiguous allocated TracerV identities");
    auto adapter = it->second;
    if (adapter.getNumPorts() != 4 || adapter.getPortName(2) != "nasti" ||
        adapter.getPortDirection(2) != Direction::In)
      return reject("Rocket TracerV request binding requires its Nasti adapter");
    Value value;
    for (unsigned p = 0; p < top.getNumPorts(); ++p)
      if (top.getPortName(p) == port && top.getPortDirection(p) == Direction::In &&
          top.getPortType(p) == adapter.getPortType(2)) value = top.getArgument(p);
    // Equal bundle types cannot distinguish banks. Follow single whole-bundle
    // forwarding to the exact constructor's crFile operand, rejecting aliases,
    // fanout, detached ports and cycles before creating any wrapper.
    FModuleOp owner = top; bool reachesAdapter = false;
    std::set<std::pair<Operation *, unsigned>> visited;
    for (unsigned depth = 0; value && depth < 4096; ++depth) {
      auto argument = dyn_cast<BlockArgument>(value);
      if (!argument || !visited.emplace(owner.getOperation(), argument.getArgNumber()).second) break;
      Value dest; unsigned drivers = 0;
      for (auto &op : *owner.getBodyBlock()) {
        if (auto connect = dyn_cast<ConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
        if (auto connect = dyn_cast<StrictConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
      }
      auto result = dyn_cast_or_null<OpResult>(dest);
      auto instance = result ? dyn_cast<InstanceOp>(result.getOwner()) : InstanceOp{};
      if (drivers != 1 || !instance) break;
      auto child = modules.find(instance.getModuleName().str());
      if (child == modules.end() || result.getResultNumber() >= child->second.getNumPorts() ||
          child->second.getPortDirection(result.getResultNumber()) != Direction::In) break;
      if (child->second == adapter) {
        auto wrapper = "GGTracerVBridgeControlWrapper" +
            (i + 1 == tracers.size() ? "" : "_" + std::to_string(i));
        reachesAdapter = owner.getName() == wrapper && instance.getName() == "crFile" &&
                         result.getResultNumber() == 2;
        break;
      }
      owner = child->second; value = owner.getArgument(result.getResultNumber());
    }
    if (!reachesAdapter)
      return reject("Rocket TracerV request port does not reach its instantiated bank");
    widgets.push_back({widget, port});
  }
  for (auto w : ArrayRef(widgetPorts).drop_front()) widgets.push_back({w.widget, w.port});
  if (bound) {
    auto registry = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
    if (!registry || registry.empty() || registry.size() != allocatedPrints.size())
      return reject("Rocket request binding requires every allocated Print host");
    std::set<std::string> hosts, ports;
    for (auto [slot, attr] : llvm::enumerate(registry)) {
      auto row = dyn_cast<DictionaryAttr>(attr);
      auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
      auto port = row ? row.getAs<StringAttr>("controlPort") : StringAttr{};
      auto hostName = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
      auto expected = "PrintBridgeModule_" + std::to_string(slot);
      if (!name || name.getValue() != expected || !allocatedPrints.erase(expected) ||
          !port || port.getValue().empty() || !ports.insert(port.getValue().str()).second ||
          !hostName || !hosts.insert(hostName.getValue().str()).second)
        return reject("Rocket request binding requires unique constructor-ordered Print identities");
      auto it = modules.find(hostName.getValue().str());
      auto host = it == modules.end() ? FModuleOp{} : it->second;
      unsigned instances = 0;
      for (auto i : bound.getOps<InstanceOp>())
        if (i.getName() == expected && host && i.getModuleName() == host.getName()) ++instances;
      bool control = false;
      if (host && host.getNumPorts() == 13 && host->hasAttr("goldengate.printHost"))
        for (auto p : top.getPorts()) if (p.name == port.getValue())
          control = p.direction == Direction::In && p.type == host.getPortType(11);
      if (instances != 1 || !control)
        return reject("Rocket request binding differs from the instantiated Print AXI bank");
      // Equal AXI types do not identify a bank. Follow whole-bundle input
      // forwarding through the active wrapper hierarchy to this constructor's
      // actual host control operand; reject aliases, fanout and detached ports.
      Value value;
      for (unsigned p = 0; p < top.getNumPorts(); ++p)
        if (top.getPortName(p) == port.getValue()) value = top.getArgument(p);
      FModuleOp owner = top; bool reachesHost = false;
      std::set<std::pair<Operation *, unsigned>> visited;
      for (unsigned depth = 0; value && depth < 4096; ++depth) {
        auto argument = dyn_cast<BlockArgument>(value);
        if (!argument || !visited.emplace(owner.getOperation(), argument.getArgNumber()).second) break;
        Value dest; unsigned drivers = 0;
        for (auto &op : *owner.getBodyBlock()) {
          if (auto connect = dyn_cast<ConnectOp>(op))
            if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
          if (auto connect = dyn_cast<StrictConnectOp>(op))
            if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
        }
        auto result = dyn_cast_or_null<OpResult>(dest);
        auto instance = result ? dyn_cast<InstanceOp>(result.getOwner()) : InstanceOp{};
        if (drivers != 1 || !instance) break;
        auto child = modules.find(instance.getModuleName().str());
        if (child == modules.end() || result.getResultNumber() >= child->second.getNumPorts() ||
            child->second.getPortDirection(result.getResultNumber()) != Direction::In) break;
        if (owner == bound) {
          reachesHost = instance.getName() == expected && instance.getModuleName() == host.getName() &&
                        result.getResultNumber() == 11;
          break;
        }
        owner = child->second; value = owner.getArgument(result.getResultNumber());
      }
      if (!reachesHost) return reject("Rocket request port does not reach its instantiated Print bank");
      widgets.push_back({expected, port.getValue().str()});
    }
  }
  if (!allocatedPrints.empty()) return reject("Rocket request allocation contains unbound Print banks");
  std::map<unsigned, FModuleOp> nics;
  for (auto module : circuit.getOps<FModuleLike>()) {
    auto name = module.getName();
    if (!name.starts_with("GGNICMCRFile")) continue;
    auto suffix = name.drop_front(StringRef("GGNICMCRFile").size()); unsigned i = 0;
    auto native = dyn_cast<FModuleOp>(module.getOperation());
    if ((!suffix.empty() && (!suffix.consume_front("_") || suffix.getAsInteger(10, i) ||
         !i || suffix != std::to_string(i))) || !native || i >= 63 ||
        !nics.emplace(i, native).second)
      return reject("Rocket NIC request binding needs canonical independent adapter indices");
  }
  if (nics.size() != allocatedNICs.size())
    return reject("Rocket request allocation must identify every instantiated NIC bank");
  for (unsigned i = 0; i < nics.size(); ++i) {
    auto widget = "SimpleNICBridgeModule_" + std::to_string(i);
    auto port = nics.size() == 1 ? std::string("nicBridge_ctrl") : "nicBridge_" + std::to_string(i) + "_ctrl";
    auto it = nics.find(i);
    if (it == nics.end() || !allocatedNICs.erase(widget))
      return reject("Rocket request binding requires contiguous allocated NIC identities");
    ControlMMIOWidget registry;
    if (failed(deriveControlMMIOWidget(circuit, widget,
            it->second.getName(), {"GGNICMMIOBank"}, registry, error))) return failure();
    if (failed(validateNICControlMMIOMode(circuit, nics.size(), registry.registerCount, error)))
      return failure();
    auto find = [&](StringRef name) -> FModuleOp {
      auto found = modules.find(name.str());
      return found == modules.end() ? FModuleOp{} : found->second;
    };
    auto adapter = it->second, owner = top;
    if (adapter.getNumPorts() != 4 || adapter.getPortName(2) != "nasti" ||
        adapter.getPortDirection(2) != Direction::In)
      return reject("Rocket NIC request binding requires the instantiated Nasti adapter");
    // Follow the exact constructor's crFile; identical types cannot identify
    // independent register state. Reject aliases, detached ports and fanout.
    Value value;
    for (unsigned p = 0; p < top.getNumPorts(); ++p)
      if (top.getPortName(p) == port && top.getPortDirection(p) == Direction::In &&
          top.getPortType(p) == adapter.getPortType(2)) value = top.getArgument(p);
    bool reachesAdapter = false;
    std::set<std::pair<Operation *, unsigned>> visited;
    for (unsigned depth = 0; value && depth < 4096; ++depth) {
      auto argument = dyn_cast<BlockArgument>(value);
      if (!argument || !visited.emplace(owner.getOperation(), argument.getArgNumber()).second) break;
      Value dest; unsigned drivers = 0;
      for (auto &op : *owner.getBodyBlock()) {
        if (auto connect = dyn_cast<ConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
        if (auto connect = dyn_cast<StrictConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
      }
      auto result = dyn_cast_or_null<OpResult>(dest);
      auto instance = result ? dyn_cast<InstanceOp>(result.getOwner()) : InstanceOp{};
      if (drivers != 1 || !instance) break;
      auto child = find(instance.getModuleName());
      if (!child || result.getResultNumber() >= child.getNumPorts() ||
          child.getPortDirection(result.getResultNumber()) != Direction::In) break;
      if (child == adapter) {
        auto expectedWrapper = "GGNICBridgeControlWrapper" +
            (i + 1 == nics.size() ? "" : "_" + std::to_string(i));
        reachesAdapter = owner.getName() == expectedWrapper &&
                         instance.getName() == "crFile" && result.getResultNumber() == 2;
        break;
      }
      owner = child; value = owner.getArgument(result.getResultNumber());
    }
    if (!reachesAdapter)
      return reject("Rocket NIC request port does not reach its instantiated NIC bank");
    widgets.push_back({widget, port});
  }
  SmallVector<FModuleOp> csvHosts;
  if (failed(discoverBoundAutoCounterBridgeHosts(circuit, csvHosts, error))) return failure();
  std::set<std::string> allocatedCSV;
  for (auto attr : regions) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
    if (name && name.getValue().starts_with("AutoCounterBridgeModule_") &&
        !allocatedCSV.insert(name.getValue().str()).second)
      return reject("Rocket request allocation duplicates a CSV bank");
  }
  if (allocatedCSV.size() != csvHosts.size())
    return reject("Rocket request allocation must identify every instantiated CSV bank");
  for (auto [index, host] : llvm::enumerate(csvHosts)) {
    auto widget = "AutoCounterBridgeModule_" + std::to_string(index);
    auto port = "autocounter_" + std::to_string(index) + "_ctrl";
    if (!allocatedCSV.erase(widget)) return reject("Rocket request allocation omits a CSV bank");
    Value value;
    for (unsigned p = 0; p < top.getNumPorts(); ++p)
      if (top.getPortName(p) == port && top.getPortDirection(p) == Direction::In &&
          top.getPortType(p) == host.getPortType(7)) value = top.getArgument(p);
    // A matching AXI type is insufficient: prove the port reaches this exact
    // logical widget through unique, whole-bundle forwarding at every wrapper.
    FModuleOp owner = top;
    bool reachesHost = false;
    std::set<std::pair<Operation *, unsigned>> visited;
    for (unsigned depth = 0; value && depth < 4096; ++depth) {
      auto argument = dyn_cast<BlockArgument>(value);
      if (!argument || !visited.emplace(owner.getOperation(), argument.getArgNumber()).second) break;
      Value dest; unsigned drivers = 0;
      for (auto &op : *owner.getBodyBlock()) {
        if (auto connect = dyn_cast<ConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
        if (auto connect = dyn_cast<StrictConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
      }
      auto result = dyn_cast_or_null<OpResult>(dest);
      auto instance = result ? dyn_cast<InstanceOp>(result.getOwner()) : InstanceOp();
      if (drivers != 1 || !instance) break;
      auto found = modules.find(instance.getModuleName().str());
      if (found == modules.end() || result.getResultNumber() >= found->second.getNumPorts() ||
          found->second.getPortDirection(result.getResultNumber()) != Direction::In) break;
      if (found->second == host) {
        reachesHost = owner->hasAttr("goldengate.autocounterHostBindings") &&
            instance.getName() == widget && result.getResultNumber() == 7;
        break;
      }
      owner = found->second; value = owner.getArgument(result.getResultNumber());
    }
    if (!reachesHost) return reject("Rocket request port does not reach its instantiated CSV bank");
    widgets.push_back({widget, port});
  }
  SmallVector<FModuleOp> assertionHosts;
  if (failed(discoverBoundAssertionBridgeHosts(circuit, assertionHosts, error))) return failure();
  std::set<std::string> allocatedAssertion;
  for (auto attr : regions) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
    if (name && name.getValue().starts_with("AssertBridgeModule_") &&
        !allocatedAssertion.insert(name.getValue().str()).second)
      return reject("Rocket request allocation duplicates a Assertion bank");
  }
  if (allocatedAssertion.size() != assertionHosts.size())
    return reject("Rocket request allocation must identify every instantiated Assertion bank");
  for (auto [index, host] : llvm::enumerate(assertionHosts)) {
    auto widget = "AssertBridgeModule_" + std::to_string(index);
    auto port = "assertion_" + std::to_string(index) + "_ctrl";
    if (!allocatedAssertion.erase(widget)) return reject("Rocket request allocation omits a Assertion bank");
    Value value;
    for (unsigned p = 0; p < top.getNumPorts(); ++p)
      if (top.getPortName(p) == port && top.getPortDirection(p) == Direction::In &&
          top.getPortType(p) == host.getPortType(7)) value = top.getArgument(p);
    // A matching AXI type is insufficient: prove the port reaches this exact
    // logical widget through unique, whole-bundle forwarding at every wrapper.
    FModuleOp owner = top;
    bool reachesHost = false;
    std::set<std::pair<Operation *, unsigned>> visited;
    for (unsigned depth = 0; value && depth < 4096; ++depth) {
      auto argument = dyn_cast<BlockArgument>(value);
      if (!argument || !visited.emplace(owner.getOperation(), argument.getArgNumber()).second) break;
      Value dest; unsigned drivers = 0;
      for (auto &op : *owner.getBodyBlock()) {
        if (auto connect = dyn_cast<ConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
        if (auto connect = dyn_cast<StrictConnectOp>(op))
          if (connect.getSrc() == value) { dest = connect.getDest(); ++drivers; }
      }
      auto result = dyn_cast_or_null<OpResult>(dest);
      auto instance = result ? dyn_cast<InstanceOp>(result.getOwner()) : InstanceOp();
      if (drivers != 1 || !instance) break;
      auto found = modules.find(instance.getModuleName().str());
      if (found == modules.end() || result.getResultNumber() >= found->second.getNumPorts() ||
          found->second.getPortDirection(result.getResultNumber()) != Direction::In) break;
      if (found->second == host) {
        reachesHost = owner->hasAttr("goldengate.assertionHostBindings") &&
            instance.getName() == widget && result.getResultNumber() == 7;
        break;
      }
      owner = found->second; value = owner.getArgument(result.getResultNumber());
    }
    if (!reachesHost) return reject("Rocket request port does not reach its instantiated Assertion bank");
    widgets.push_back({widget, port});
  }
  return bindControlWidgetWrites(circuit, widgets, error);
}

LogicalResult goldengate::bindControlWidgetWrites(CircuitOp circuit,
    ArrayRef<ControlWidgetPort> widgets, std::string &error) {
  constexpr llvm::StringLiteral wrapperName="GGControlWidgetWriteWrapper";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!="GGControlWriteDispatchWrapper")
    return reject("widget writes require the control write dispatch wrapper");
  FModuleOp inner,decoder;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()==wrapperName)return reject("widget write wrapper exists");
    if(m.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getModuleName()=="GGControlAddressDecode")decoder=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto regions=decoder?decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions"):ArrayAttr();
  if(!inner||!raw||!regions||regions.empty()||regions.size()>63)
    return reject("widget writes require retained annotations and 1..63 decoded regions");
  // Widget.scala zips sortedWidgets with the interconnect slaves. Resolve
  // identity through the decoder's allocation rather than assuming the
  // baseline ordering: adding a register bank can shift existing slaves.
  std::map<std::string,unsigned> allocated;
  for(auto [i,row]:llvm::enumerate(regions)) {
    auto region=dyn_cast<DictionaryAttr>(row);
    auto name=region?region.getAs<StringAttr>("name"):StringAttr();
    auto index=region?region.getAs<IntegerAttr>("slave"):IntegerAttr();
    if(!name||name.getValue().empty()||!index||index.getValue().isNegative()||
       index.getValue().getActiveBits()>32||index.getValue().getZExtValue()!=i||
       !allocated.emplace(name.getValue().str(),i).second)
      return reject("widget write allocation needs unique names and ordered slave indices");
  }
  SmallVector<Binding> bindings;
  std::set<std::string> widgetNames, portNames;
  if (widgets.empty()) return reject("widget write bindings must not be empty");
  auto addressWidth = controlPortWidth(inner, widgets.front().port+".aw.bits.addr");
  if (!addressWidth) return reject("widget writes require a typed control address boundary");
  const unsigned addressBits = *addressWidth;
  const Field addressFields[]{
    {"addr",addressBits},{"len",8},{"size",3},{"burst",2},{"lock",1},
    {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
  for(auto widget:widgets) {
    if (widget.widget.empty() || widget.port.empty() ||
        !widgetNames.insert(widget.widget).second || !portNames.insert(widget.port).second)
      return reject("widget writes require unique nonempty widget and port identities");
    auto found=allocated.find(widget.widget);
    if(found==allocated.end())return reject("widget write allocation is missing an implemented widget");
    bindings.push_back({widget.widget,widget.port,found->second});
  }
  bool instantiated=false;
  circuit.walk([&](InstanceOp i){instantiated|=i.getModuleName()==inner.getName();});
  if(instantiated)return reject("widget writes require an uninstantiated top");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto payload=[&](ArrayRef<Field> fields) {
    SmallVector<BundleType::BundleElement> elems;
    for(auto f:fields)elems.push_back({b.getStringAttr(f.name),false,uint(f.width)});
    return BundleType::get(ctx,elems);
  };
  auto token=[&](FIRRTLBaseType bits) {
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,bits}});
  };
  const Field bfields[]{{"resp",2},{"id",12},{"user",1}};
  const Field rfields[]{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}};
  auto controlType=BundleType::get(ctx,{
    {b.getStringAttr("aw"),false,token(payload(addressFields))},
    {b.getStringAttr("w"),false,token(payload(dataFields))},
    {b.getStringAttr("b"),true,token(payload(bfields))},
    {b.getStringAttr("ar"),false,token(payload(addressFields))},
    {b.getStringAttr("r"),true,token(payload(rfields))}});
  SmallVector<BundleType::BundleElement> remaining;
  for(auto e:controlType.getElements())if(e.name!="aw"&&e.name!="w")remaining.push_back(e);
  auto remainingType=BundleType::get(ctx,remaining);
  std::map<std::string,unsigned> old,copied,metadata;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old.emplace(p.name.getValue().str(),i);
  std::set<std::string> consumed,controls;
  // Complete preflight before adding any operation or changing annotations.
  for(auto binding:bindings) {
    auto found=old.find(binding.port);
    if(found==old.end()||inner.getPortType(found->second)!=controlType||
       inner.getPortDirection(found->second)!=Direction::In)
      return reject("widget writes require matching Nasti control bundles");
    controls.insert(binding.port);
    std::string prefix="ctrl_write_dispatch_slave_"+std::to_string(binding.slave)+"_";
    const Field fields[]{{"aw_ready",1},{"w_ready",1},{"aw_valid",1},
      {"aw_bits_addr",addressBits},{"aw_bits_len",8},{"aw_bits_id",12},
      {"w_valid",1},{"w_bits_data",32},{"w_bits_last",1}};
    for(auto f:fields) {
      auto n=prefix+f.name;auto it=old.find(n);
      auto dir=StringRef(f.name).ends_with("ready")?Direction::In:Direction::Out;
      if(it==old.end()||inner.getPortType(it->second)!=uint(f.width)||
         inner.getPortDirection(it->second)!=dir)
        return reject("widget writes are missing an exact dispatch slave boundary");
      consumed.insert(n);
    }
  }
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())) {
    auto n=p.name.getValue().str();copied[n]=ports.size();
    if(controls.count(n))p.type=remainingType;
    ports.push_back(p);
  }
  for(auto ch:{"aw","w"})for(auto f:ch==StringRef("aw")?ArrayRef<Field>(addressFields):ArrayRef<Field>(dataFields)) {
    if(dispatched(ch,f.name))continue;
    std::string n="ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name;
    if(old.count(n))return reject("widget write metadata boundary already exists");
    metadata[n]=ports.size();ports.push_back({b.getStringAttr(n),uint(f.width),Direction::In});
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  SmallVector<Attribute> catalog;
  for(auto x:bindings)catalog.push_back(b.getDictionaryAttr({
    b.getNamedAttr("name",b.getStringAttr(x.widget)),b.getNamedAttr("port",b.getStringAttr(x.port)),
    b.getNamedAttr("slave",b.getI32IntegerAttr(x.slave))}));
  wrapper->setAttr("goldengate.controlWriteBindings",b.getArrayAttr(catalog));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim");
  auto arg=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  for(auto [n,i]:copied) {
    Value v=sim.getResult(old.at(n)),external=arg(i);
    if(controls.count(n)) {
      for(auto e:remaining) {
        Value a=field(v,e.name),z=field(external,e.name);
        // ConnectOp observes all nested flips, including response ready.
        b.create<ConnectOp>(loc,e.isFlip?z:a,e.isFlip?a:z);
      }
    } else {
      auto dir=inner.getPortDirection(old.at(n));
      b.create<ConnectOp>(loc,dir==Direction::In?v:external,dir==Direction::In?external:v);
    }
  }
  for(auto binding:bindings)for(auto ch:{"aw","w"}) {
    std::string prefix="ctrl_write_dispatch_slave_"+std::to_string(binding.slave)+"_"+ch;
    Value channel=field(sim.getResult(old.at(binding.port)),ch);
    connect(sim.getResult(old.at(prefix+"_ready")),field(channel,"ready"));
    connect(field(channel,"valid"),sim.getResult(old.at(prefix+"_valid")));
    Value bits=field(channel,"bits");
    for(auto f:ch==StringRef("aw")?ArrayRef<Field>(addressFields):ArrayRef<Field>(dataFields)) {
      Value source=dispatched(ch,f.name)?sim.getResult(old.at(prefix+"_bits_"+f.name)):
        arg(metadata.at("ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name));
      connect(field(bits,f.name),source);
    }
  }
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),mp="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return a;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto port=ref.take_front(ref.find_first_of(".[")).str();
        bool transfer=copied.count(port);
        if(controls.count(port)) {
          ref=ref.drop_front(port.size());
          transfer=ref.starts_with(".ar.")||ref==".ar"||ref.starts_with(".b.")||ref==".b"||ref.starts_with(".r.")||ref==".r";
        }
        // Whole control-bundle and consumed AW/W targets still resolve in
        // the inner module. Only surviving AR/B/R fields move to the new top.
        if(transfer)suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto arr=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> xs;for(auto x:arr)xs.push_back(retarget(x));return b.getArrayAttr(xs);}
    if(auto dict=dyn_cast<DictionaryAttr>(a)){NamedAttrList xs;for(auto x:dict)xs.set(x.getName(),retarget(x.getValue()));return xs.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(wrapperName));return success();
}
