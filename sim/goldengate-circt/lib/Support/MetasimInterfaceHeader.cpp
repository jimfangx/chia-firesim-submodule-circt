// See LICENSE for license details.
// Oracle: FPGATopImp.genHeader and SimulationMapping.generateHeaderAnnos.
// All widths come from FIRRTL types; the original circuit name is supplied
// before wrapping changes its identity. Validate before changing annotations.
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

using namespace mlir;
using namespace circt::firrtl;
namespace {
struct Leaf { unsigned width; bool input; };
using Leaves = std::map<std::string, Leaf>;
bool flatten(Type type, std::string path, bool input, Leaves &leaves) {
  if (auto bundle = dyn_cast<BundleType>(type)) {
    for (auto field : bundle.getElements())
      if (!flatten(field.type, path + "." + field.name.getValue().str(),
                   input != field.isFlip, leaves)) return false;
    return true;
  }
  auto uint = dyn_cast<UIntType>(type);
  return uint && uint.getWidth() && *uint.getWidth() > 0 &&
         leaves.emplace(path, Leaf{unsigned(*uint.getWidth()), input}).second;
}
struct AXIConfig { unsigned id, addr, data; };
bool analyzeAXI(PortInfo port, bool master, AXIConfig &config) {
  Leaves leaves;
  if (port.direction != (master ? Direction::Out : Direction::In) ||
      !flatten(port.type, "", !master, leaves)) return false;
  auto width = [&](StringRef path, bool input) -> unsigned {
    auto found = leaves.find(path.str());
    return found != leaves.end() && found->second.input == input
               ? found->second.width : 0;
  };
  config = {width(".aw.bits.id", !master),
            width(".aw.bits.addr", !master),
            width(".w.bits.data", !master)};
  if (!config.id || !config.addr || !config.data || config.data % 8 ||
      width(".ar.bits.id", !master) != config.id ||
      width(".b.bits.id", master) != config.id ||
      width(".r.bits.id", master) != config.id ||
      width(".ar.bits.addr", !master) != config.addr ||
      width(".r.bits.data", master) != config.data ||
      width(".w.bits.strb", !master) != config.data / 8) return false;
  for (auto channel : {"aw", "w", "ar", "b", "r"}) {
    bool request = StringRef(channel) == "aw" || StringRef(channel) == "w" ||
                   StringRef(channel) == "ar";
    bool input = request != master;
    if (width((Twine(".") + channel + ".valid").str(), input) != 1 ||
        width((Twine(".") + channel + ".ready").str(), !input) != 1)
      return false;
  }
  return true;
}
std::string cString(StringRef text) {
  std::string result = "\"";
  for (unsigned char c : text.bytes()) {
    if (c == '\\' || c == '"') { result += '\\'; result += char(c); }
    else if (c >= 32 && c < 127) result += char(c);
    else { // Three-digit octal prevents an adjacent digit extending the escape.
      result += '\\'; result += char('0' + (c >> 6));
      result += char('0' + ((c >> 3) & 7)); result += char('0' + (c & 7));
    }
  }
  return result + '"';
}
} // namespace

LogicalResult goldengate::prepareMetasimInterfaceHeader(
    CircuitOp circuit, StringRef targetName, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw || targetName.empty() || targetName.contains('\0'))
    return reject("metasim header requires retained annotations and original target name");
  FModuleOp top, shim;
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module.getName() == "FPGATop") top = module;
    if (module.getName() == "F1Shim") shim = module;
  }
  if (circuit.getName() != "F1Shim" || !top || !shim || top.getNumPorts() != 5)
    return reject("metasim header requires the U250 F1Shim/FPGATop boundary");
  AXIConfig ctrl{}, memory{}, cpu{};
  unsigned clocks = 0, resets = 0, controls = 0, memories = 0, cpus = 0;
  for (auto port : top.getPorts()) {
    auto name = port.name.getValue();
    if (name == "clock") {
      if (port.direction != Direction::In || !isa<ClockType>(port.type))
        return reject("invalid metasim host clock");
      ++clocks;
    } else if (name == "reset") {
      auto uint = dyn_cast<UIntType>(port.type);
      if (port.direction != Direction::In || !uint || uint.getWidth() != 1)
        return reject("invalid metasim host reset");
      ++resets;
    } else if (name == "ctrl" || name == "mem_0" || name == "cpu_managed_axi4") {
      bool master = name == "mem_0";
      auto &config = master ? memory : name == "ctrl" ? ctrl : cpu;
      if (!analyzeAXI(port, master, config))
        return reject("inconsistent metasim AXI4 widths or directions");
      if (master) ++memories; else if (name == "ctrl") ++controls; else ++cpus;
    } else return reject("metasim header has not ported additional memory, FPGA-managed or QSFP endpoints");
  }
  if (clocks != 1 || resets != 1 || controls != 1 || memories != 1 || cpus != 1)
    return reject("metasim header requires unique clock/reset/control/memory/CPU ports");
  // F1Shim exposes two physical QSFP lanes even when FPGATop has zero target
  // endpoints. Their payload width supplies FPGATopQSFPBitWidth, not the count.
  unsigned qsfpWidth = 0, qsfpPorts = 0;
  for (auto port : shim.getPorts()) {
    auto name = port.name.getValue();
    if (name != "io_qsfp_tx" && name != "io_qsfp_rx") continue;
    auto vector = dyn_cast<FVectorType>(port.type);
    if (!vector || vector.getNumElements() != 2)
      return reject("invalid U250 physical QSFP vector");
    Leaves leaves;
    bool input = name == "io_qsfp_rx";
    if (port.direction != (input ? Direction::In : Direction::Out) ||
        !flatten(vector.getElementType(), "", input, leaves) || leaves.size() != 3)
      return reject("invalid U250 physical QSFP handshake");
    auto bits = leaves.find(".bits"), valid = leaves.find(".valid"), ready = leaves.find(".ready");
    if (bits == leaves.end() || valid == leaves.end() || ready == leaves.end() ||
        bits->second.input != input || valid->second.input != input ||
        ready->second.input == input || valid->second.width != 1 || ready->second.width != 1 ||
        (qsfpWidth && qsfpWidth != bits->second.width))
      return reject("inconsistent physical QSFP payload width or direction");
    qsfpWidth = bits->second.width; ++qsfpPorts;
  }
  if (qsfpPorts != 2) return reject("missing physical QSFP width for metasim header");
  for (auto annotation : raw) {
    auto dict = dyn_cast<DictionaryAttr>(annotation);
    auto cls = dict ? dict.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed retained header annotation");
    if (cls.getValue() == AnnotationClasses::OutputFile) {
      auto suffix = dict.getAs<StringAttr>("fileSuffix");
      if (suffix && suffix.getValue() == ".const.h")
        return reject("driver header output already exists");
    }
  }
  std::string body;
  llvm::raw_string_ostream out(body);
  out << "// Golden Gate-generated Driver Header\n"
         "// Bridge constructor emission is still pending.\n"
         "#ifdef GET_METASIM_INTERFACE_CONFIG\n"
         "static constexpr TargetConfig conf_target{\n.ctrl = ";
  auto printAXI = [&](AXIConfig config) {
    out << "AXI4Config{" << config.id << ", " << config.addr << ", " << config.data << "}";
  };
  printAXI(ctrl); out << ",\n.mem = "; printAXI(memory);
  out << ",\n.mem_num_channels = " << memories << ",\n.cpu_managed = "; printAXI(cpu);
  out << ",\n.fpga_managed = std::nullopt,\n.qsfp = FPGATopQSFPConfig{"
      << qsfpWidth << ", 0},\n.target_name = " << cString(targetName)
      << "\n};\n#undef GET_METASIM_INTERFACE_CONFIG\n"
         "#endif // GET_METASIM_INTERFACE_CONFIG\n";
  out.flush();
  OpBuilder builder(circuit.getContext());
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations.push_back(builder.getDictionaryAttr({
      builder.getNamedAttr("class", builder.getStringAttr(AnnotationClasses::OutputFile)),
      builder.getNamedAttr("fileSuffix", builder.getStringAttr(".const.h")),
      builder.getNamedAttr("body", builder.getStringAttr(body))}));
  circuit->setAttr("rawAnnotations", builder.getArrayAttr(annotations));
  return success();
}
