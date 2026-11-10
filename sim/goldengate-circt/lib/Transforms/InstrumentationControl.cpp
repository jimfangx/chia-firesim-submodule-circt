// See LICENSE for license details.
// Oracle: FPGATop registration order, HasWidgets.genCtrlIO, PrintBridge and
// CPUManagedStreamEngine. The complete selected catalog is CSV + Print + CPU.
#include "goldengate/InstrumentationControl.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlReadDispatch.h"
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlReadArbiter.h"
#include "goldengate/ControlWriteArbiter.h"
#include "goldengate/ControlWriteTracker.h"
#include "goldengate/SimulationMasterControl.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/BitVector.h"
#include <set>
using namespace mlir;
using namespace circt::firrtl;
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/PrintBridgeHeader.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/EmptyCPUStreamWrite.h"
#include "goldengate/CPUStreamPort.h"
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "llvm/Support/raw_ostream.h"
namespace {
FModuleOp find(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>()) if (module.getName() == name) return module;
  return {};
}
SmallVector<FModuleOp> remap(CircuitOp circuit, ArrayRef<FModuleOp> modules) {
  SmallVector<FModuleOp> result;
  for (auto module : modules) result.push_back(find(circuit, module.getName()));
  return result;
}
void commit(CircuitOp circuit, CircuitOp staged) {
  circuit->setAttrs(staged->getAttrs());
  auto &destination = circuit.getBodyBlock()->getOperations();
  destination.clear();
  destination.splice(destination.end(), staged.getBodyBlock()->getOperations());
}
// Generic stages expose diagnostic sink ports for unit tests. Close them only
// after all actual routing inputs have been consumed; retain the source nets.
LogicalResult closeMonitors(CircuitOp circuit, std::string &error) {
  auto top = find(circuit, circuit.getName());
  if (!top) { error = "instrumentation control has no final top"; return failure(); }
  llvm::BitVector erase(top.getNumPorts()); SmallVector<Operation *> connects;
  unsigned masters = 0;
  for (auto [index, port] : llvm::enumerate(top.getPorts())) {
    auto name = port.name.getValue();
    if (name == "ctrl") { ++masters; continue; }
    if (name.starts_with("autocounter_") || name.starts_with("print_") || name == "cpuStream_ctrl") {
      error = "instrumentation control retained an unbound widget slave"; return failure();
    }
    if (!name.starts_with("ctrl_")) continue;
    if (port.direction != Direction::Out) { error = "instrumentation control retained a routing input"; return failure(); }
    auto argument = top.getArgument(index);
    for (auto &use : argument.getUses()) {
      auto *op = use.getOwner();
      if (!isa<ConnectOp, StrictConnectOp>(op) || op->getOperand(0) != argument) {
        error = "instrumentation monitor output has a source or non-connect use"; return failure();
      }
      connects.push_back(op);
    }
    erase.set(index);
  }
  if (masters != 1) { error = "instrumentation control requires exactly one master"; return failure(); }
  for (auto *op : connects) op->erase();
  top.erasePorts(erase);
  return success();
}
}
LogicalResult goldengate::assembleInstrumentationControl(
    CircuitOp circuit, ArrayRef<FModuleOp> csvHosts, ArrayRef<FModuleOp> printHosts,
    std::string &header, std::string &decoderHeader, std::string &peerMap,
    std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGPrintBridgeHostWrapper" || csvHosts.empty() || printHosts.empty())
    return reject("instrumentation composition requires bound enabled CSV and Print hosts");
  auto top = find(circuit, circuit.getName());
  if (!top) return reject("instrumentation composition has no bound active top");
  for (auto port : top.getPorts()) if (port.name == "ctrl" || port.name.getValue().starts_with("ctrl_"))
    return reject("instrumentation composition reserves the global ctrl namespace");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("instrumentation composition requires retained constructors");
  unsigned csvCount = 0, printCount = 0;
  for (auto attr : raw) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto cls = row ? row.getAs<StringAttr>("class") : StringAttr{};
    if (!cls) return reject("instrumentation composition has a malformed retained annotation");
    if (cls != AnnotationClasses::BridgeIO) continue;
    auto kind = row.getAs<StringAttr>("widgetClass");
    if (!kind) return reject("instrumentation composition has a missing bridge widget class");
    if (kind == "midas.widgets.AutoCounterBridgeModule") ++csvCount;
    else if (kind == AnnotationClasses::PrintBridgeModule) ++printCount;
    else return reject("instrumentation selected catalog cannot omit another bridge constructor");
  }
  if (csvCount != csvHosts.size() || printCount != printHosts.size())
    return reject("instrumentation selected catalog omits an enabled constructor");
  SmallVector<ControlMMIOWidget> widgets;
  if (failed(deriveAutoCounterControlMMIOCatalog(circuit, csvHosts, widgets, error))) return failure();
  auto csvInfo = csvHosts.front()->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");
  unsigned addressBits = csvInfo.getAs<IntegerAttr>("addressBits").getInt();
  for (auto host : csvHosts) {
    auto info = host->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");
    auto address = info.getAs<IntegerAttr>("addressBits"), ids = info.getAs<IntegerAttr>("idBits");
    if (!address || address.getInt() != addressBits || !ids || ids.getInt() != 12)
      return reject("instrumentation composition requires uniform control widths and twelve-bit IDs");
  }
  std::string nextDecoder;
  if (failed(preparePrintBridgeDecoderHeader(circuit, printHosts, nextDecoder, error))) return failure();
  auto registry = top->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
  if (!registry || registry.size() != printHosts.size()) return reject("instrumentation Print registry differs from complete hosts");
  SmallVector<ControlWidgetPort> controls;
  for (auto [i, widget] : llvm::enumerate(widgets)) controls.push_back({widget.name.str(), "autocounter_" + std::to_string(i) + "_ctrl"});
  const StringRef fields[] = {"startCycleL", "startCycleH", "endCycleL", "endCycleH", "doneInit", "flushNarrowPacket"};
  for (auto [i, item] : llvm::enumerate(printHosts)) {
    FModuleOp host = item;
    auto row = dyn_cast<DictionaryAttr>(registry[i]);
    auto info = host ? host->getAttrOfType<DictionaryAttr>("goldengate.printHost") : DictionaryAttr{};
    auto name = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto hostName = row ? row.getAs<StringAttr>("hostModule") : StringAttr{};
    auto port = row ? row.getAs<StringAttr>("controlPort") : StringAttr{};
    auto mcr = info ? info.getAs<StringAttr>("mcrModule") : StringAttr{};
    auto configName = info ? info.getAs<StringAttr>("configModule") : StringAttr{};
    auto config = configName ? find(circuit, configName.getValue()) : FModuleOp{};
    auto regs = config ? config->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr{};
    if (!name || name.getValue() != "PrintBridgeModule_" + std::to_string(i) || hostName != host.getName() ||
        !port || !mcr || !config || !regs || regs.size() != 6 || host.getNumPorts() != 13)
      return reject("instrumentation Print host/configuration identity differs");
    auto ctrl = dyn_cast<BundleType>(host.getPortType(11));
    auto aw = ctrl ? ctrl.getElement("aw") : std::nullopt;
    auto awBundle = aw ? dyn_cast<BundleType>(aw->type) : BundleType{};
    auto bits = awBundle ? awBundle.getElement("bits") : std::nullopt;
    auto bitsBundle = bits ? dyn_cast<BundleType>(bits->type) : BundleType{};
    auto addr = bitsBundle ? bitsBundle.getElement("addr") : std::nullopt;
    auto ids = bitsBundle ? bitsBundle.getElement("id") : std::nullopt;
    if (!addr || addr->type != UIntType::get(circuit.getContext(), addressBits) || !ids || ids->type != UIntType::get(circuit.getContext(), 12))
      return reject("instrumentation Print AXI widths differ from CSV control");
    SmallVector<bool> seen(6, false);
    for (auto attr : regs) {
      auto reg = dyn_cast<DictionaryAttr>(attr); auto offset = reg ? reg.getAs<IntegerAttr>("offset") : IntegerAttr{};
      auto read = reg ? reg.getAs<BoolAttr>("readable") : BoolAttr{}, write = reg ? reg.getAs<BoolAttr>("writeable") : BoolAttr{};
      if (!offset || offset.getInt() < 0 || offset.getInt() % 4 || offset.getInt() >= 24 ||
          seen[offset.getInt()/4] || reg.getAs<StringAttr>("name") != fields[offset.getInt()/4] || !read || !read.getValue() || !write || !write.getValue())
        return reject("instrumentation Print register ABI differs from six actual SFC words");
      seen[offset.getInt()/4] = true;
    }
    ControlMMIOWidget widget;
    if (failed(deriveControlMMIOWidget(circuit, name.getValue(), mcr.getValue(), {configName.getValue()}, widget, error)) || widget.registerCount != 6)
      return reject("instrumentation Print register adapter differs from complete bank");
    widgets.push_back(widget); controls.push_back({name.getValue().str(), port.getValue().str()});
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  if (failed(mapPrintBridgeCPUStreams(*staged, {}, {}, error)) || failed(mapCPUStreamControl(*staged, addressBits, 12, error))) return failure();
  ControlMMIOWidget counts;
  if (failed(deriveControlMMIOWidget(*staged, "CPUManagedStreamEngine_0", "GGCPUStreamMCRFile", {"GGCPUStreamCountBank"}, counts, error))) return failure();
  widgets.push_back(counts); controls.push_back({"CPUManagedStreamEngine_0", "cpuStream_ctrl"});
  SmallVector<ControlMMIORegion> regions;
  if (failed(allocateControlMMIORegions(addressBits, widgets, regions, error)) ||
      failed(addControlErrorSlave(*staged, addressBits, 12, "GGCPUStreamControlWrapper", error)) ||
      failed(addControlAddressDecode(*staged, addressBits, regions, error)) ||
      failed(addControlWriteRoute(*staged, error)) || failed(addControlWriteDispatch(*staged, error)) ||
      failed(bindControlWidgetWrites(*staged, controls, error)) || failed(addControlReadDispatch(*staged, error)) ||
      failed(addControlReadTracker(*staged, error)) || failed(addControlReadArbiter(*staged, error)) ||
      failed(addControlWriteArbiter(*staged, error)) || failed(addControlWriteTracker(*staged, error)) ||
      failed(bindControlMaster(*staged, "GGControlWriteTrackerWrapper", error)) || failed(closeMonitors(*staged, error))) return failure();
  // Use the existing selected Print boundary names to compose the shared
  // transport helpers. No second Print-only catalog or local bus is created.
  if (failed(materializeEmptyCPUStreamWrite(*staged, "GGControlMasterWrapper", "GGSelectedPrintCPUStreamWriteWrapper", error)) ||
      failed(attachCPUStreamWriteBuffer(*staged, "GGSelectedPrintCPUStreamWriteWrapper", "GGSelectedPrintCPUStreamWriteBufferWrapper", error)) ||
      failed(attachCPUStreamReadBuffer(*staged, "GGSelectedPrintCPUStreamWriteBufferWrapper", "GGSelectedPrintCPUStreamReadBufferWrapper", error)) ||
      failed(attachCPUStreamResponseBuffer(*staged, "GGSelectedPrintCPUStreamReadBufferWrapper", "GGSelectedPrintCPUStreamResponseBufferWrapper", error)) ||
      failed(attachCPUStreamWriteResponseBuffer(*staged, "GGSelectedPrintCPUStreamResponseBufferWrapper", "GGSelectedPrintCPUStreamWriteResponseBufferWrapper", error)) ||
      failed(attachCPUStreamPort(*staged, "GGSelectedPrintCPUStreamWriteResponseBufferWrapper", "GGSelectedPrintCPUStreamPlatformWrapper", error))) return failure();
  auto stagedCSV = remap(*staged, csvHosts), stagedPrint = remap(*staged, printHosts);
  std::string nextHeader;
  if (failed(prepareAutoCounterBridgeHeader(*staged, stagedCSV, regions, nextHeader, error))) return failure();
  std::string printText; llvm::raw_string_ostream out(printText);
  out << "#ifdef GET_INCLUDES\n#include <cstddef>\n#include <cstdint>\n#include \"print-bridge-decoders.h\"\n#endif\n#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (auto [i, field] : llvm::enumerate(fields)) out << "static_assert(offsetof(PRINTBRIDGEMODULE_struct, " << field << ") == " << i << " * sizeof(uint64_t), \"invalid " << field << "\");\n";
  out << "static_assert(sizeof(PRINTBRIDGEMODULE_struct) == 6 * sizeof(uint64_t), \"invalid structure\");\n#endif\n#ifdef GET_BRIDGE_CONSTRUCTOR\n";
  // mapPrintBridgeCPUStreams validates the unique actual queue/count forwarding
  // and fixes Print source order. With no preceding stream, index == slot.
  for (auto [slot, host] : llvm::enumerate(stagedPrint)) {
    auto region = llvm::find_if(regions, [&](const auto &r) { return r.name == widgets[csvHosts.size()+slot].name; });
    if (region == regions.end()) return reject("instrumentation Print host has no allocated region");
    out << "registry.add_widget(goldengate_print::make_print_bridge_" << slot << "(simif, *registry.get_stream_engine(), PRINTBRIDGEMODULE_struct{\n";
    for (auto [i, field] : llvm::enumerate(fields)) out << "  /* " << field << " */ " << region->start + 4*i << "ULL,\n";
    out << "}, " << slot << "U, args, " << slot << "U).release());\n";
  }
  out << "#endif // GET_BRIDGE_CONSTRUCTOR\n"; out.flush(); nextHeader += printText;
  // Let the CPU emitter inspect the live queue, count SSA, selector, decoder
  // and shared read/write bindings. Its result is the allocation used above.
  OpBuilder b(circuit.getContext()); SmallVector<Attribute> nextRaw;
  for (auto attr : staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    if (row && row.getAs<StringAttr>("class") == AnnotationClasses::OutputFile &&
        (row.getAs<StringAttr>("fileSuffix") == ".const.h" || row.getAs<StringAttr>("fileSuffix") == ".peer2peer.const.yaml"))
      return reject("instrumentation composition requires unconsumed driver output annotations");
    nextRaw.push_back(attr);
  }
  nextRaw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::OutputFile)), b.getNamedAttr("fileSuffix", b.getStringAttr(".const.h")), b.getNamedAttr("body", b.getStringAttr(nextHeader))}));
  staged->getOperation()->setAttr("rawAnnotations", b.getArrayAttr(nextRaw));
  if (failed(prepareCPUManagedStreamHeader(*staged, error))) return failure();
  std::string nextPeer;
  for (auto attr : staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    if (!row || row.getAs<StringAttr>("class") != AnnotationClasses::OutputFile) continue;
    if (row.getAs<StringAttr>("fileSuffix") == ".const.h") nextHeader = row.getAs<StringAttr>("body").getValue().str();
    if (row.getAs<StringAttr>("fileSuffix") == ".peer2peer.const.yaml") nextPeer = row.getAs<StringAttr>("body").getValue().str();
  }
  if (nextPeer.empty()) return reject("instrumentation composition has no validated CPU stream address map");
  find(*staged, staged->getName())->setAttr("goldengate.instrumentationGlobalControl", b.getUnitAttr());
  if (failed(verify(*staged))) return reject("instrumentation composition produced invalid FIRRTL");
  commit(circuit, *staged); header = std::move(nextHeader); decoderHeader = std::move(nextDecoder); peerMap = std::move(nextPeer);
  return success();
}
LogicalResult goldengate::compileInstrumentationControl(
    CircuitOp circuit, unsigned addressBits, unsigned idBits,
    std::string &header, std::string &decoderHeader, std::string &peerMap,
    std::string &error) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  SmallVector<FModuleOp> csv, payloads, stages, controls, prints;
  if (failed(materializeAutoCounterBridgeHosts(*staged, addressBits, idBits, csv, error)) ||
      failed(bindAutoCounterBridgeHosts(*staged, csv, error)) ||
      failed(materializePrintBridgePayloads(*staged, payloads, error)) ||
      failed(materializePrintBridgeTokenStages(*staged, payloads, stages, error)) ||
      failed(materializePrintBridgeControls(*staged, payloads, stages, controls, error)) ||
      failed(materializePrintBridgeHosts(*staged, controls, addressBits, idBits, prints, error)) ||
      failed(bindPrintBridgeHosts(*staged, prints, error))) return failure();
  std::string nextHeader, nextDecoder, nextPeer;
  if (failed(assembleInstrumentationControl(*staged, csv, prints, nextHeader, nextDecoder, nextPeer, error))) return failure();
  commit(circuit, *staged); header = std::move(nextHeader); decoderHeader = std::move(nextDecoder); peerMap = std::move(nextPeer);
  return success();
}
