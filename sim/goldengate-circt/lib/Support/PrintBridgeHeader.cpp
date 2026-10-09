// See LICENSE for license details.
// Oracle: PrintBridgeModule.genHeader and PrintRecord.argumentWidths.
// Required input invariants: bound queued hosts, ordered integer payloads and
// analyzed rational bridge clocks. Annotations consumed/produced: none.
// IR mutations: none. Analyses required: resolved annotation target identity.
// Analyses preserved: all. Output invariant: decoder offsets/widths match the
// host payload; allocation remains explicit; failure preserves caller text.
#include "goldengate/PrintBridgeHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <functional>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void literal(llvm::raw_ostream &out, StringRef text) {
  out << '"';
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') out << '\\' << char(ch);
    // Fixed-length octal escapes cannot absorb a following digit. Escape '?'
    // too so arbitrary printf text cannot form a C++ trigraph.
    else if (ch >= 32 && ch < 127 && ch != '?') out << char(ch);
    else out << '\\' << char('0' + ((ch >> 6) & 7))
             << char('0' + ((ch >> 3) & 7)) << char('0' + (ch & 7));
  }
  out << '"';
}
}
LogicalResult goldengate::preparePrintBridgeDecoderHeader(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts, std::string &header, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("Print decoder requires retained annotations");
  std::string result;
  llvm::raw_string_ostream out(result);
  out << "// Generated Print decoder boundary; platform allocation supplies MMIO and stream indices.\n"
         "#pragma once\n#include <memory>\n#include <string>\n#include <vector>\n#include \"bridges/synthesized_prints.h\"\n"
         "namespace goldengate_print {\n";
  llvm::StringSet<> seen;
  FModuleOp top;
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == "GGPrintBridgeHostWrapper") top = m;
  auto bindings = top ? top->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr{};
  if (!hosts.empty() && (!bindings || bindings.size() != hosts.size()))
    return reject("Print decoder requires the complete bound host collection");
  for (auto [slot, item] : llvm::enumerate(hosts)) {
    FModuleOp host = item;
    if (!host || host->getParentOp() != circuit || !seen.insert(host.getName()).second ||
        host.getNumPorts() != 13 || host.getPortName(3) != "hBits" ||
        host.getPortDirection(3) != Direction::In)
      return reject("Print decoder requires distinct queued hosts in this circuit");
    auto binding = dyn_cast<DictionaryAttr>(bindings[slot]);
    if (!binding || binding.getAs<StringAttr>("hostModule") != host.getName())
      return reject("Print decoder host order differs from active bindings");
    auto info = host->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    auto records = info ? info.getAs<ArrayAttr>("records") : ArrayAttr{};
    auto bits = info ? info.getAs<IntegerAttr>("tokenBits") : IntegerAttr{};
    auto bytes = info ? info.getAs<IntegerAttr>("tokenBytes") : IntegerAttr{};
    auto idle = info ? info.getAs<IntegerAttr>("idleCycleBits") : IntegerAttr{};
    auto mask = info ? info.getAs<IntegerAttr>("idleCycleMask") : IntegerAttr{};
    auto depth = info ? info.getAs<IntegerAttr>("queueDepth") : IntegerAttr{};
    auto bag = dyn_cast<BundleType>(host.getPortType(3));
    if (!records || records.empty() || !bits || bits.getInt() < 8 || bits.getInt() > (1LL << 30) ||
        (bits.getInt() & (bits.getInt() - 1)) || !bytes || bytes.getInt() != bits.getInt() / 8 ||
        !idle || idle.getInt() != std::min<int64_t>(16, bits.getInt()) - 1 ||
        !mask || mask.getInt() != ((1LL << idle.getInt()) - 1) * 2 ||
        !depth || depth.getInt() != 6144 || !bag || !bag.isPassive() ||
        bag.getElements().size() != records.size() + 1 ||
        bag.getElements()[0].name != info.getAs<StringAttr>("resetPortName") ||
        bag.getElements()[0].type != UIntType::get(circuit.getContext(), 1))
      return reject("Print decoder token geometry or reset payload differs from queued host");
    DictionaryAttr bridge;
    for (auto attr : raw) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      if (!d || d.getAs<StringAttr>("class") != AnnotationClasses::BridgeIO ||
          d.getAs<StringAttr>("widgetClass") != AnnotationClasses::PrintBridgeModule) continue;
      auto target = d.getAs<StringAttr>("target");
      auto resolved = target ? resolveAnnotationTarget(circuit, target.getValue(), error) : std::nullopt;
      if (!resolved || resolved->module != host || !resolved->port ||
          *resolved->port != 3 || resolved->fieldID != 0) continue;
      if (bridge) return reject("ambiguous Print decoder bridge identity");
      bridge = d;
    }
    auto clock = bridge ? bridge.getAs<DictionaryAttr>("clockInfo") : DictionaryAttr{};
    auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr{};
    auto constructorRecords = key ? key.getAs<ArrayAttr>("printPorts") : ArrayAttr{};
    if (!key || key.getAs<StringAttr>("class") != AnnotationClasses::PrintBridgeParameters ||
        key.getAs<StringAttr>("resetPortName") != info.getAs<StringAttr>("resetPortName") ||
        !constructorRecords || constructorRecords.size() != records.size())
      return reject("Print decoder constructor differs from bound host");
    auto name = clock ? clock.getAs<StringAttr>("name") : StringAttr{};
    auto mult = clock ? clock.getAs<IntegerAttr>("multiplier") : IntegerAttr{};
    auto div = clock ? clock.getAs<IntegerAttr>("divisor") : IntegerAttr{};
    if (!name || name.getValue().empty() || name.getValue().contains('\0') ||
        !mult || mult.getInt() <= 0 || mult.getInt() > UINT32_MAX ||
        !div || div.getInt() <= 0 || div.getInt() > UINT32_MAX)
      return reject("Print decoder requires a bound bridge with a valid rational clock");
    out << "inline std::unique_ptr<synthesized_prints_t> make_print_bridge_" << slot
        << "(simif_t &sim, StreamEngine &stream, const PRINTBRIDGEMODULE_struct &mmio,\n"
           "    unsigned widget_index, const std::vector<std::string> &args, unsigned stream_index) {\n"
           "  return std::make_unique<synthesized_prints_t>(sim, stream, mmio, widget_index, args,\n"
           "    std::vector<synthesized_prints_t::Print>{\n";
    uint64_t offset = 1;
    for (auto [i, attr] : llvm::enumerate(records)) {
      auto record = dyn_cast<DictionaryAttr>(attr);
      auto format = record ? record.getAs<StringAttr>("format") : StringAttr{};
      auto storedOffset = record ? record.getAs<IntegerAttr>("offset") : IntegerAttr{};
      auto storedWidth = record ? record.getAs<IntegerAttr>("width") : IntegerAttr{};
      auto widths = record ? record.getAs<ArrayAttr>("argumentWidths") : ArrayAttr{};
      auto type = dyn_cast<BundleType>(bag.getElements()[i + 1].type);
      auto constructor = dyn_cast<DictionaryAttr>(constructorRecords[i]);
      if (!record || !format || format.getValue().contains('\0') || !storedOffset ||
          storedOffset.getInt() != int64_t(offset) || !storedWidth || !widths || !type ||
          type.getElements().empty() || widths.size() + 1 != type.getElements().size() ||
          record.getAs<StringAttr>("name") != bag.getElements()[i + 1].name ||
          type.getElements()[0].name != "enable" ||
          type.getElements()[0].type != UIntType::get(circuit.getContext(), 1))
        return reject("Print decoder record order, offset or enable differs from payload");
      if (!constructor || constructor.getAs<StringAttr>("name") != record.getAs<StringAttr>("name") ||
          constructor.getAs<StringAttr>("format") != format)
        return reject("Print decoder format or order differs from constructor");
      uint64_t width = 1;
      out << "      {" << offset << "U, "; literal(out, format.getValue());
      out << ", std::vector<unsigned>{";
      for (auto [j, value] : llvm::enumerate(widths)) {
        auto w = dyn_cast<IntegerAttr>(value);
        auto field = dyn_cast<FIRRTLBaseType>(type.getElements()[j + 1].type);
        if (!w || w.getInt() < 0 || w.getInt() > UINT32_MAX || !field ||
            !isa<UIntType, SIntType>(field) || field.getBitWidthOrSentinel() != w.getInt())
          return reject("Print decoder argument width differs from payload");
        width += w.getInt(); out << (j ? ", " : "") << w.getInt() << "U";
      }
      if (storedWidth.getInt() != int64_t(width))
        return reject("Print decoder record width differs from payload");
      offset += width; out << "}},\n";
    }
    uint64_t expectedBits = 8;
    while (expectedBits < offset + 1) expectedBits <<= 1;
    if (expectedBits != uint64_t(bits.getInt()))
      return reject("Print decoder padded token width differs from payload");
    out << "    }, " << bytes.getInt() << "U, " << mask.getInt() << "U, stream_index, "
        << depth.getInt() << "U, ClockInfo{";
    literal(out, name.getValue());
    out << ", " << mult.getInt() << "U, " << div.getInt() << "U});\n}\n";
  }
  out << "} // namespace goldengate_print\n"; out.flush(); header = std::move(result);
  return success();
}

LogicalResult goldengate::preparePrintBridgeAllocatedHeader(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts, std::string &header, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGControlMasterWrapper" || hosts.empty())
    return reject("allocated Print header requires the selected control master and queued hosts");
  std::string decoderText;
  if (failed(preparePrintBridgeDecoderHeader(circuit, hosts, decoderText, error))) return failure();
  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("allocated Print header requires an uninstantiated top");
  auto live = [&](StringRef name) -> FModuleOp {
    auto *target = graph.lookup(StringAttr::get(circuit.getContext(), name));
    if (!target) return {};
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
    llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
    bool recursive = false;
    std::function<unsigned(circt::igraph::InstanceGraphNode *)> count = [&](auto *node) {
      if (active.count(node)) { recursive = true; return 2U; }
      auto prior = counts.find(node); if (prior != counts.end()) return prior->second;
      active.insert(node); unsigned n = node == target;
      for (auto *edge : *node) n = std::min(2U, n + count(edge->getTarget()));
      active.erase(node); counts[node] = n; return n;
    };
    if (count(top) != 1 || recursive) return {};
    return dyn_cast<FModuleOp>(target->getModule().getOperation());
  };
  auto bound = live("GGPrintBridgeHostWrapper"), decoder = live("GGControlAddressDecode");
  auto reads = live("GGControlReadDispatchWrapper"), writes = live("GGControlWidgetWriteWrapper");
  auto transport = live("GGCPUStreamRead");
  if (!bound || !decoder || !reads || !writes || !transport)
    return reject("allocated Print header requires unique live hosts, decoder and control/stream bindings");
  auto catalog = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
  auto readBindings = reads->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings");
  auto writeBindings = writes->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
  auto actual = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto sources = transport->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
  if (!catalog || catalog.size() != hosts.size() || !readBindings || readBindings != writeBindings ||
      readBindings.size() != hosts.size() + 1 || !actual || actual.size() != readBindings.size() || !sources)
    return reject("allocated Print header requires a complete matching Print/count catalog");
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<StringAttr> controlPorts;
  SmallVector<unsigned> streamIndices;
  const StringRef fields[] = {"startCycleL", "startCycleH", "endCycleL", "endCycleH", "doneInit", "flushNarrowPacket"};
  for (auto [slot, item] : llvm::enumerate(hosts)) {
    FModuleOp host = item;
    auto row = dyn_cast<DictionaryAttr>(catalog[slot]);
    auto widget = row ? row.getAs<StringAttr>("widgetName") : StringAttr{};
    auto control = row ? row.getAs<StringAttr>("controlPort") : StringAttr{};
    auto stream = row ? row.getAs<StringAttr>("streamPort") : StringAttr{};
    auto info = host->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    auto configName = info ? info.getAs<StringAttr>("configModule") : StringAttr{};
    auto mcrName = info ? info.getAs<StringAttr>("mcrModule") : StringAttr{};
    auto config = configName ? live(configName.getValue()) : FModuleOp{};
    if (!widget || widget.getValue() != "PrintBridgeModule_" + std::to_string(slot) ||
        !control || !stream || !config || !mcrName || !live(mcrName.getValue()) || live(host.getName()) != host)
      return reject("allocated Print header widget or configuration identity differs from live hosts");
    InstanceOp instance;
    for (auto i : bound.getOps<InstanceOp>()) if (i.getName() == widget.getValue()) instance = i;
    if (!instance || instance.getModuleName() != host.getName())
      return reject("allocated Print header widget differs from its bound host instance");
    ControlMMIOWidget descriptor;
    if (failed(deriveControlMMIOWidget(circuit, widget.getValue(), mcrName.getValue(),
          {configName.getValue()}, descriptor, error))) return failure();
    auto registers = config->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    if (descriptor.registerCount != 6 || !registers)
      return reject("allocated Print header requires six configuration words");
    // ABI member order is fixed; metadata row order is not. Use byte offsets.
    for (auto attr : registers) {
      auto reg = cast<DictionaryAttr>(attr); unsigned word = reg.getAs<IntegerAttr>("offset").getInt() / 4;
      if (word >= 6 || reg.getAs<StringAttr>("name") != fields[word] ||
          !reg.getAs<BoolAttr>("readable").getValue() || !reg.getAs<BoolAttr>("writeable").getValue())
        return reject("allocated Print header register ABI or permissions differ");
    }
    std::optional<unsigned> index;
    for (auto [i, attr] : llvm::enumerate(sources)) {
      auto source = dyn_cast<DictionaryAttr>(attr);
      if (!source || source.getAs<StringAttr>("name") != "PRINTBRIDGEMODULE_" + std::to_string(slot) + "_to_cpu_stream") continue;
      if (index || source.getAs<StringAttr>("port") != stream)
        return reject("allocated Print header stream identity is ambiguous or differs from the bound queue");
      index = i;
    }
    if (!index) return reject("allocated Print header has no CPU stream for its host");
    widgets.push_back(descriptor); controlPorts.push_back(control); streamIndices.push_back(*index);
  }
  ControlMMIOWidget counts;
  if (!live("GGCPUStreamMCRFile") || !live("GGCPUStreamCountBank") ||
      failed(deriveControlMMIOWidget(circuit, "CPUManagedStreamEngine_0", "GGCPUStreamMCRFile",
          {"GGCPUStreamCountBank"}, counts, error))) return reject("allocated Print header requires the live CPU count bank");
  widgets.push_back(counts); controlPorts.push_back(StringAttr::get(circuit.getContext(), "cpuStream_ctrl"));
  SmallVector<ControlMMIORegion> regions;
  if (failed(allocateControlMMIORegions(25, widgets, regions, error))) return failure();
  for (auto [i, region] : llvm::enumerate(regions)) {
    auto row = dyn_cast<DictionaryAttr>(actual[i]);
    if (!row || row.getAs<StringAttr>("name") != region.name ||
        !row.getAs<IntegerAttr>("start") || row.getAs<IntegerAttr>("start").getInt() != int64_t(region.start) ||
        !row.getAs<IntegerAttr>("size") || row.getAs<IntegerAttr>("size").getInt() != int64_t(region.size) ||
        !row.getAs<IntegerAttr>("slave") || row.getAs<IntegerAttr>("slave").getInt() != int64_t(i))
      return reject("allocated Print header MMIO regions differ from materialized register allocation");
    unsigned matches = 0;
    auto widget = llvm::find_if(widgets, [&](const auto &w) { return w.name == region.name; });
    for (auto attr : readBindings) {
      auto binding = dyn_cast<DictionaryAttr>(attr);
      if (!binding || binding.getAs<StringAttr>("name") != region.name) continue;
      if (binding.getAs<StringAttr>("port") != controlPorts[widget - widgets.begin()] ||
          !binding.getAs<IntegerAttr>("slave") || binding.getAs<IntegerAttr>("slave").getInt() != int64_t(i))
        return reject("allocated Print header read/write binding differs from MMIO allocation");
      ++matches;
    }
    if (matches != 1) return reject("allocated Print header has a missing or duplicate control binding");
  }
  // Reuse the CPU emitter's SSA/instance-path checks of each live queue, count
  // word and DMA selector. Run it on a clone with a scratch output annotation;
  // this read-only boundary must not consume existing platform collateral.
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  OpBuilder b(circuit.getContext()); SmallVector<Attribute> raw;
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    if (d && d.getAs<StringAttr>("class") == AnnotationClasses::OutputFile && d.getAs<StringAttr>("fileSuffix") == ".const.h") continue;
    raw.push_back(attr);
  }
  raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix", b.getStringAttr(".const.h")), b.getNamedAttr("body", b.getStringAttr(""))}));
  staged->getOperation()->setAttr("rawAnnotations", b.getArrayAttr(raw));
  if (failed(prepareCPUManagedStreamHeader(*staged, error, CPUStreamHeaderBoundary::SelectedOutgoing))) return failure();
  auto cpu = cast<DictionaryAttr>(staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations").getValue().back()).getAs<StringAttr>("body");
  std::string result; llvm::raw_string_ostream out(result);
  out << "// Selected Print/count allocation; include under the standard Golden Gate GET_* guards.\n"
         "#ifdef GET_INCLUDES\n#include <cstddef>\n#include <cstdint>\n#include \"print-bridge-decoders.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (auto [i, field] : llvm::enumerate(fields))
    out << "static_assert(offsetof(PRINTBRIDGEMODULE_struct, " << field << ") == " << i << " * sizeof(uint64_t), \"invalid " << field << "\");\n";
  out << "static_assert(sizeof(PRINTBRIDGEMODULE_struct) == 6 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // GET_SUBSTRUCT_CHECKS\n#ifdef GET_BRIDGE_CONSTRUCTOR\n";
  for (auto [slot, host] : llvm::enumerate(hosts)) {
    auto region = llvm::find_if(regions, [&](const auto &r) { return r.name == widgets[slot].name; });
    out << "registry.add_widget(goldengate_print::make_print_bridge_" << slot
        << "(simif, *registry.get_stream_engine(), PRINTBRIDGEMODULE_struct{\n";
    for (auto [i, field] : llvm::enumerate(fields))
      out << "  /* " << field << " */ " << region->start + 4 * i << "ULL,\n";
    out << "}, " << slot << "U, args, " << streamIndices[slot] << "U).release());\n";
  }
  out << "#endif // GET_BRIDGE_CONSTRUCTOR\n" << cpu.getValue(); out.flush();
  header = std::move(result); return success();
}
