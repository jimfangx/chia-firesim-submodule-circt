// See LICENSE for license details.
// Oracle: SimpleNICBridgeModule LoopbackNIC branch, CPUManagedStreamEngine,
// HasWidgets allocation/control and FPGATop's independent AXI4Buffer channels.
#include "goldengate/NICLoopbackControl.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "goldengate/NICTokenQueues.h"
#include "goldengate/NICCPUStreamQueues.h"
#include "goldengate/NICMMIOBank.h"
#include "goldengate/NICHeader.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/CPUStreamWrite.h"
#include "goldengate/CPUStreamPort.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlReadDispatch.h"
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlReadArbiter.h"
#include "goldengate/ControlWriteArbiter.h"
#include "goldengate/ControlWriteTracker.h"
#include "goldengate/SimulationMasterControl.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/HW/HWTypeInterfaces.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/BitVector.h"
#include <functional>
#include <set>
#include <tuple>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;
namespace {
FModuleOp find(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>()) if (module.getName() == name) return module;
  return {};
}
// Prove observational non-use through ordinary wiring and the actual FAME
// pipe's scalar data registers/expressions. A shared pipe module is analyzed
// in its exact instance context: an unrelated live Pipe16 must not make this
// pauser live, and a live use of this instance must never be dropped.
bool unusedPayload(CircuitOp circuit, circt::FieldRef initial) {
  auto maxField = [](Type type) -> uint64_t {
    auto aggregate = dyn_cast<circt::hw::FieldIDTypeInterface>(type);
    return aggregate ? aggregate.getMaxFieldID() : 0;
  };
  auto flipped = [](Type type, unsigned field) {
    bool flip = false;
    while (field) {
      if (auto bundle = dyn_cast<BundleType>(type)) {
        auto index = bundle.getIndexForFieldID(field);
        auto element = bundle.getElement(index);
        flip ^= element.isFlip; field -= bundle.getFieldID(index); type = element.type;
      } else if (auto vector = dyn_cast<FVectorType>(type)) {
        auto index = vector.getIndexForFieldID(field);
        field -= vector.getFieldID(index); type = vector.getElementType();
      } else break;
    }
    return flip;
  };
  using Context = SmallVector<InstanceOp>;
  using Key = std::tuple<void *, unsigned, std::vector<Operation *>>;
  std::set<Key> visited;
  std::function<bool(circt::FieldRef, Context)> unused =
      [&](circt::FieldRef ref, Context context) {
    auto root = ref.getValue(); unsigned field = ref.getFieldID();
    std::vector<Operation *> path;
    for (auto instance : context) path.push_back(instance.getOperation());
    // Revisiting data feedback is harmless only because every outgoing use
    // of each reached state/expression is scanned by its first invocation.
    if (!visited.insert({root.getAsOpaquePointer(), field, path}).second) return true;
    if (auto argument = dyn_cast<BlockArgument>(root)) {
      auto module = dyn_cast<FModuleOp>(argument.getOwner()->getParentOp());
      if (!module) return false;
      auto index = argument.getArgNumber();
      if ((module.getPortDirection(index) == Direction::Out) != flipped(root.getType(), field)) {
        if (context.empty()) return false; // Observable output of active top.
        auto instance = context.pop_back_val();
        if (instance.getModuleName() != module.getName() ||
            !unused(circt::FieldRef(instance.getResult(index), field), context)) return false;
        context.push_back(instance);
      }
    } else if (auto instance = root.getDefiningOp<InstanceOp>()) {
      auto module = find(circuit, instance.getModuleName());
      auto index = cast<OpResult>(root).getResultNumber();
      if (!module) return false; // External/opaque consumer.
      if ((module.getPortDirection(index) == Direction::In) != flipped(root.getType(), field)) {
        if (llvm::is_contained(context, instance)) return false;
        auto nested = context; nested.push_back(instance);
        if (!unused(circt::FieldRef(module.getArgument(index), field), nested)) return false;
      }
    }
    if (auto *definition = root.getDefiningOp())
      if (!isa<InstanceOp, RegOp, RegResetOp, WireOp, NodeOp>(definition) &&
          !isExpression(definition)) return false; // Memory/opaque state.
    auto reads = [&](Value operand) {
      auto use = getFieldRefFromValue(operand);
      auto type = dyn_cast<FIRRTLBaseType>(operand.getType());
      return use.getValue() == root && type && field >= use.getFieldID() &&
          field <= use.getFieldID() + maxField(type);
    };
    for (auto &op : *root.getParentBlock()) {
      if (op.getNumRegions()) {
        // Unsupported region data/control capture rejects. Unrelated target
        // conditionals cannot make an otherwise dead payload observable.
        bool captured = false;
        op.walk([&](Operation *nested) {
          for (auto operand : nested->getOperands()) captured |= reads(operand);
        });
        if (captured) return false;
        continue;
      }
      if (isa<SubfieldOp, SubindexOp>(op)) continue;
      if (auto node = dyn_cast<NodeOp>(op)) {
        auto source = getFieldRefFromValue(node.getInput());
        if (reads(node.getInput()) &&
            !unused(circt::FieldRef(node.getResult(), field - source.getFieldID()), context)) return false;
        continue;
      }
      if (isa<ConnectOp, StrictConnectOp>(op)) {
        if (!reads(op.getOperand(1))) continue;
        auto source = getFieldRefFromValue(op.getOperand(1));
        auto dest = getFieldRefFromValue(op.getOperand(0));
        if (!unused(circt::FieldRef(dest.getValue(), dest.getFieldID() + field - source.getFieldID()), context)) return false;
        continue;
      }
      bool used = false;
      for (auto operand : op.getOperands()) used |= reads(operand);
      if (!used) continue;
      // A tainted condition may schedule state or a handshake. No clock,
      // reset, enable, memory, assertion or other side effect is admitted.
      if (auto mux = dyn_cast<MuxPrimOp>(op)) if (reads(mux.getSel())) return false;
      if (!isExpression(&op) || !op.getNumResults()) return false;
      for (auto result : op.getResults()) {
        if (!isa<UIntType, SIntType>(result.getType()) ||
            !unused(circt::FieldRef(result, 0), context)) return false;
      }
    }
    return true;
  };
  return unused(initial, {});
}
LogicalResult closeMonitors(CircuitOp circuit, std::string &error) {
  auto top = find(circuit, circuit.getName()); llvm::BitVector erase(top.getNumPorts());
  SmallVector<Operation *> connects; unsigned masters = 0;
  for (auto [index, port] : llvm::enumerate(top.getPorts())) {
    auto name = port.name.getValue();
    if (name == "ctrl") { ++masters; continue; }
    if (name.starts_with("nicBridge_") || name == "cpuStream_ctrl") {
      error = "NIC loopback composition retained an unbound widget slave"; return failure();
    }
    if (!name.starts_with("ctrl_")) continue;
    if (port.direction != Direction::Out) { error = "NIC loopback composition retained a routing input"; return failure(); }
    auto argument = top.getArgument(index);
    for (auto &use : argument.getUses()) {
      auto *op = use.getOwner();
      if (!isa<ConnectOp, StrictConnectOp>(op) || op->getOperand(0) != argument) {
        error = "NIC loopback control monitor has a non-connect use"; return failure();
      }
      connects.push_back(op);
    }
    erase.set(index);
  }
  if (masters != 1) { error = "NIC loopback composition requires one control master"; return failure(); }
  for (auto *op : connects) op->erase(); top.erasePorts(erase); return success();
}
}
LogicalResult goldengate::validateNICLoopbackPauserInputs(CircuitOp circuit,
    std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC loopback requires retained annotations");
  SmallVector<DictionaryAttr> bridges;
  for (auto attr : raw) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto cls = row ? row.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("NIC loopback has a malformed annotation");
    if (cls != AnnotationClasses::BridgeIO || row.getAs<StringAttr>("widgetClass") !=
        "firechip.goldengateimplementations.SimpleNICBridgeModule") continue;
    if (row.get("widgetConstructorKey"))
      return reject("NIC loopback requires constructor-free NIC annotations");
    bridges.push_back(row);
  }
  // SFC omits these assignments. Admit them only when the model demonstrably
  // ignores their payloads. Never replace a live pauser with chosen constants.
  for (auto bridge : bridges) {
    auto mapping = bridge.getAs<DictionaryAttr>("channelMapping");
    for (StringRef name : {"nic_pauser_threshold", "nic_pauser_quanta", "nic_pauser_refresh"}) {
      auto global = mapping ? mapping.getAs<StringAttr>(name) : StringAttr{}; DictionaryAttr channel;
      for (auto attr : raw) {
        auto row = dyn_cast<DictionaryAttr>(attr);
        if (global && row.getAs<StringAttr>("class") == AnnotationClasses::ChannelConnection && row.getAs<StringAttr>("globalName") == global) {
          if (channel) return reject("NIC loopback pauser channel identity is ambiguous"); channel = row;
        }
      }
      auto sinks = channel ? channel.getAs<ArrayAttr>("sinks") : ArrayAttr{};
      auto target = sinks && sinks.size() == 1 ? dyn_cast<StringAttr>(sinks[0]) : StringAttr{};
      auto resolved = target ? resolveAnnotationTarget(circuit, target.getValue(), error) : std::nullopt;
      auto module = resolved ? dyn_cast<FModuleOp>(resolved->module.getOperation()) : FModuleOp{};
      if (!resolved || !module || !resolved->port || !resolved->fieldID ||
          !unusedPayload(circuit, circt::FieldRef(module.getArgument(*resolved->port), *resolved->fieldID)))
        return reject("NIC loopback requires provably unused pauser payloads; live usePauser is unsupported");
    }
  }
  return success();
}

LogicalResult goldengate::addNICLoopbackPlatformQueues(CircuitOp circuit,
    std::string &error) {
  // Unlike the standalone selected diagnostic, preserve every other platform
  // bridge and leave shared MMIO/CPU/driver assembly to the normal pipeline.
  if (failed(validateNICLoopbackPauserInputs(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  auto before = staged->getName();
  if (failed(addNICLoopbackTokenQueues(*staged, error))) return failure();
  if (staged->getName() == before) return success(); // No enabled NIC.
  if (failed(addNICLoopbackCPUStreamQueues(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "NIC loopback platform queues produced invalid IR"; return failure();
  }
  // Replay the two deterministic, fully preflighted stages in place. The
  // production compiler retains handles to other hosts/models across this
  // stage; replacing all modules with a cloned circuit would invalidate them.
  // Neither transform has an external side effect or input-dependent failure
  // after the successful staged run above.
  if (failed(addNICLoopbackTokenQueues(circuit, error)) ||
      failed(addNICLoopbackCPUStreamQueues(circuit, error))) return failure();
  return success();
}

LogicalResult goldengate::compileNICLoopbackControl(CircuitOp circuit,
    unsigned addressBits, std::string &header, std::string &peerMap, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (addressBits != 32) return reject("NIC loopback composition requires F2 32-bit control addresses");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC loopback composition requires retained annotations");
  SmallVector<DictionaryAttr> bridges;
  for (auto attr : raw) {
    auto row = dyn_cast<DictionaryAttr>(attr); auto cls = row ? row.getAs<StringAttr>("class") : StringAttr{};
    if (!cls) return reject("NIC loopback composition has a malformed annotation");
    if (cls == AnnotationClasses::OutputFile &&
        (row.getAs<StringAttr>("fileSuffix") == ".const.h" || row.getAs<StringAttr>("fileSuffix") == ".peer2peer.const.yaml"))
      return reject("NIC loopback composition requires unconsumed driver outputs");
    if (cls != AnnotationClasses::BridgeIO) continue;
    if (row.getAs<StringAttr>("widgetClass") != "firechip.goldengateimplementations.SimpleNICBridgeModule" || row.get("widgetConstructorKey"))
      return reject("NIC loopback selected catalog cannot omit another bridge constructor");
    bridges.push_back(row);
  }
  if (bridges.empty()) return reject("NIC loopback composition requires an enabled NIC");
  if (failed(validateNICLoopbackPauserInputs(circuit, error))) return failure();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(addNICLoopbackTokenQueues(*staged, error)) ||
      staged->getName() != "GGNICLoopbackTokenQueuesWrapper" ||
      failed(addNICLoopbackCPUStreamQueues(*staged, error)) ||
      failed(addNICLoopbackMMIOBank(*staged, error)) ||
      failed(mapNICBridgeControl(*staged, addressBits, 12, error))) return failure();
  std::string nextHeader;
  if (failed(emitNICLoopbackHeader(*staged, nextHeader, error))) return failure();
  SmallVector<CPUStreamSourcePort> sources, sinks; SmallVector<CPUStreamCountPort> counts;
  if (failed(deriveCPUStreamPorts(*staged, sources, sinks, counts, error)) ||
      sources.size() != bridges.size() || sinks.size() != bridges.size() || counts.size() != 2*bridges.size())
    return reject("NIC loopback composition requires complete independent directional CPU allocations");
  if (failed(addCPUStreamRead(*staged, sources, error)) || failed(addCPUStreamCountBank(*staged, counts, error)) ||
      failed(addCPUStreamWrite(*staged, sinks, error)) ||
      failed(attachCPUStreamControl(*staged, "GGCPUStreamWriteWrapper", "GGNICLoopbackCPUStreamControlWrapper", addressBits, 12, error))) return failure();
  SmallVector<std::string> names; for (unsigned i = 0; i < bridges.size(); ++i) names.push_back("SimpleNICBridgeModule_" + std::to_string(i));
  SmallVector<ControlMMIOWidget> widgets; SmallVector<ControlWidgetPort> controls;
  for (auto [i, name] : llvm::enumerate(names)) {
    ControlMMIOWidget widget;
    std::string mcr = "GGNICMCRFile" + (i ? "_" + std::to_string(i) : "");
    if (failed(deriveControlMMIOWidget(*staged, name, mcr, {"GGNICMMIOBank"}, widget, error)) || widget.registerCount != 1)
      return reject("NIC loopback composition requires each actual singleton done bank");
    widgets.push_back(widget); controls.push_back({name, bridges.size() == 1 ? "nicBridge_ctrl" : "nicBridge_" + std::to_string(i) + "_ctrl"});
  }
  ControlMMIOWidget cpu;
  if (failed(deriveControlMMIOWidget(*staged, "CPUManagedStreamEngine_0", "GGCPUStreamMCRFile", {"GGCPUStreamCountBank"}, cpu, error))) return failure();
  widgets.push_back(cpu); controls.push_back({"CPUManagedStreamEngine_0", "cpuStream_ctrl"});
  SmallVector<ControlMMIORegion> regions;
  if (failed(allocateControlMMIORegions(addressBits, widgets, regions, error)) ||
      failed(addControlErrorSlave(*staged, addressBits, 12, "GGNICLoopbackCPUStreamControlWrapper", error)) ||
      failed(addControlAddressDecode(*staged, addressBits, regions, error)) ||
      failed(addControlWriteRoute(*staged, error)) || failed(addControlWriteDispatch(*staged, error)) ||
      failed(bindControlWidgetWrites(*staged, controls, error)) || failed(addControlReadDispatch(*staged, error)) ||
      failed(addControlReadTracker(*staged, error)) || failed(addControlReadArbiter(*staged, error)) ||
      failed(addControlWriteArbiter(*staged, error)) || failed(addControlWriteTracker(*staged, error)) ||
      failed(bindControlMaster(*staged, "GGControlWriteTrackerWrapper", error)) || failed(closeMonitors(*staged, error)) ||
      failed(attachCPUStreamWriteBuffer(*staged, "GGControlMasterWrapper", "GGNICLoopbackCPUStreamWriteBufferWrapper", error)) ||
      failed(attachCPUStreamReadBuffer(*staged, "GGNICLoopbackCPUStreamWriteBufferWrapper", "GGNICLoopbackCPUStreamReadBufferWrapper", error)) ||
      failed(attachCPUStreamResponseBuffer(*staged, "GGNICLoopbackCPUStreamReadBufferWrapper", "GGNICLoopbackCPUStreamResponseBufferWrapper", error)) ||
      failed(attachCPUStreamWriteResponseBuffer(*staged, "GGNICLoopbackCPUStreamResponseBufferWrapper", "GGNICLoopbackCPUStreamWriteResponseBufferWrapper", error)) ||
      failed(attachCPUStreamPort(*staged, "GGNICLoopbackCPUStreamWriteResponseBufferWrapper", "GGNICLoopbackCPUStreamPlatformWrapper", error))) return failure();
  OpBuilder b(circuit.getContext()); SmallVector<Attribute> nextRaw(staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations").begin(), staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations").end());
  nextRaw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::OutputFile)), b.getNamedAttr("fileSuffix", b.getStringAttr(".const.h")), b.getNamedAttr("body", b.getStringAttr(nextHeader))}));
  staged->getOperation()->setAttr("rawAnnotations", b.getArrayAttr(nextRaw));
  if (failed(prepareCPUManagedStreamHeader(*staged, error))) return failure();
  std::string nextPeer;
  for (auto attr : staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    if (row.getAs<StringAttr>("class") != AnnotationClasses::OutputFile) continue;
    if (row.getAs<StringAttr>("fileSuffix") == ".const.h") nextHeader = row.getAs<StringAttr>("body").getValue().str();
    if (row.getAs<StringAttr>("fileSuffix") == ".peer2peer.const.yaml") nextPeer = row.getAs<StringAttr>("body").getValue().str();
  }
  if (nextPeer.empty() || failed(verify(*staged))) return reject("NIC loopback composition produced invalid IR or missing CPU map");
  auto top = find(*staged, staged->getName()); top->setAttr("goldengate.nicLoopbackGlobalControl", b.getUnitAttr());
  circuit->setAttrs(staged->getOperation()->getAttrs()); auto &destination = circuit.getBodyBlock()->getOperations();
  destination.clear(); destination.splice(destination.end(), staged->getBodyBlock()->getOperations());
  header = std::move(nextHeader); peerMap = std::move(nextPeer); return success();
}
