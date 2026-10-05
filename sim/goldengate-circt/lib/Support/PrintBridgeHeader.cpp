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
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>

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
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == circuit.getName()) top = m;
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
