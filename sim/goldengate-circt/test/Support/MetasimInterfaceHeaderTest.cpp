// See LICENSE for license details.
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, StringRef message) {
  if (!condition) throw std::runtime_error(message.str());
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text);
  op->print(out, OpPrintingFlags().useLocalScope()); return text;
}
std::string axi(unsigned id, unsigned addr, unsigned data, unsigned bad) {
  auto uint = [](unsigned width) { return "uint<" + std::to_string(width) + ">"; };
  auto channel = [](std::string bits) {
    return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<" + bits + ">>";
  };
  std::string address = "id: " + uint(id) + ", addr: " + uint(addr);
  std::string b = "id: " + uint(id + (bad == 1));
  std::string r = "id: " + uint(id) + ", data: " + uint(data + (bad == 2 ? 8 : 0));
  std::string w = "data: " + uint(data) + ", strb: " + uint(data / 8);
  return "!firrtl.bundle<aw: " + channel(address) + ", w: " + channel(w) +
         ", ar: " + channel(address) + ", b flip: " + channel(b) + ", r flip: " + channel(r) + ">";
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned bad = 0) {
  auto text = "module { firrtl.circuit \"F1Shim\" attributes {rawAnnotations = [{class = \"test.Retained\", target = \"~Old|Old>port\"}]} { "
      "firrtl.module @FPGATop(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, in %ctrl: " +
      axi(3, 20, 64, bad) + ", out %mem_0: " + axi(5, 40, 128, 0) +
      ", in %cpu_managed_axi4: " + axi(7, 48, 256, 0) + ") {} "
      "firrtl.module @F1Shim(out %io_qsfp_tx: !firrtl.vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>, 2>, "
      "in %io_qsfp_rx: !firrtl.vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<" +
      std::to_string(bad == 3 ? 256 : 512) + ">>, 2>) {} } }";
  auto module = parseSourceString<ModuleOp>(text, &context);
  require(bool(module), "fixture parse"); return module;
}
std::string run(CircuitOp circuit, StringRef name) {
  SmallVector<std::string> bodies;
  for (auto module : circuit.getOps<FModuleLike>()) bodies.push_back(dump(module.getOperation()));
  auto archive = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  std::string error;
  require(succeeded(goldengate::prepareMetasimInterfaceHeader(circuit, name, error)), error);
  auto result = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(result.size() == archive.size() + 2, "header annotation count");
  for (unsigned i = 0; i < archive.size(); ++i)
    require(archive[i] == result[i], "changed existing annotation");
  unsigned i = 0;
  for (auto module : circuit.getOps<FModuleLike>())
    require(bodies[i++] == dump(module.getOperation()), "changed circuit semantics");
  auto verilog = cast<DictionaryAttr>(result[result.size() - 2]);
  require(verilog.getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::OutputFile &&
          verilog.getAs<StringAttr>("fileSuffix").getValue() == ".const.vh", "Verilog header output annotation");
  auto output = cast<DictionaryAttr>(result[result.size() - 1]);
  require(output.getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::OutputFile &&
          output.getAs<StringAttr>("fileSuffix").getValue() == ".const.h", "header output annotation");
  return output.getAs<StringAttr>("body").getValue().str();
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 3, "usage: MetasimInterfaceHeaderTest [boundary.mlir output-directory]");
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto positive = fixture(context);
    auto circuit = *positive->getOps<CircuitOp>().begin();
    auto body = run(circuit, "Quoted\"\\\nTarget");
    auto annotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    auto verilog = cast<DictionaryAttr>(annotations[annotations.size() - 2])
                       .getAs<StringAttr>("body").getValue();
    for (auto expected : {"`define CTRL_ID_BITS 3\n", "`define CTRL_ADDR_BITS 20\n",
         "`define CTRL_DATA_BITS 64\n", "`define MEM_ID_BITS 5\n",
         "`define MEM_ADDR_BITS 40\n", "`define MEM_DATA_BITS 128\n",
         "`define CPU_MANAGED_AXI4_ID_BITS 7\n", "`define CPU_MANAGED_AXI4_ADDR_BITS 48\n",
         "`define CPU_MANAGED_AXI4_DATA_BITS 256\n", "`define CPU_MANAGED_AXI4_PRESENT 1\n",
         "`define QSFP_DATA_BITS 512\n", "`define MEM_HAS_CHANNEL0 1\n"})
      require(verilog.contains(expected), "Verilog interface width or presence macro");
    require(!verilog.contains("MEM_HAS_CHANNEL1") && !verilog.contains("FPGA_MANAGED_AXI4_PRESENT") &&
            !verilog.contains("QSFP_HAS_CHANNEL") && !verilog.contains("MEM_HAS_CHANNEL-1"),
            "unexpected metasim endpoint macro");
    require(verilog.contains("`ifndef __QUOTED___TARGET_H\n") &&
            verilog.contains("`endif // __QUOTED___TARGET_H\n"),
            "Verilog guard must escape punctuation and newlines");
    for (auto expected : {".ctrl = AXI4Config{3, 20, 64}", ".mem = AXI4Config{5, 40, 128}",
         ".cpu_managed = AXI4Config{7, 48, 256}", ".mem_num_channels = 1",
         ".fpga_managed = std::nullopt", ".qsfp = FPGATopQSFPConfig{512, 0}",
         ".target_name = \"Quoted\\\"\\\\\\012Target\"", "#undef GET_METASIM_INTERFACE_CONFIG"})
      require(StringRef(body).contains(expected), "typed configuration or C string escaping");
    std::string error, before = dump(*positive);
    require(failed(goldengate::prepareMetasimInterfaceHeader(circuit, "FireSim", error)) &&
            dump(*positive) == before, "duplicate output changed IR");
    for (unsigned bad = 1; bad <= 8; ++bad) {
      auto negative = fixture(context, bad);
      auto c = *negative->getOps<CircuitOp>().begin();
      if (bad == 4) c->removeAttr("rawAnnotations");
      if (bad == 5) c.setNameAttr(StringAttr::get(&context, "OtherTop"));
      if (bad == 7 || bad == 8) {
        OpBuilder builder(&context);
        auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
        SmallVector<Attribute> retained(raw.begin(), raw.end());
        retained.push_back(builder.getDictionaryAttr({
            builder.getNamedAttr("class", builder.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
            builder.getNamedAttr("fileSuffix", builder.getStringAttr(bad == 7 ? ".const.vh" : ".const.h")),
            builder.getNamedAttr("body", builder.getStringAttr("existing\n"))}));
        c->setAttr("rawAnnotations", builder.getArrayAttr(retained));
      }
      before = dump(*negative);
      require(failed(goldengate::prepareMetasimInterfaceHeader(c, bad == 6 ? "" : "FireSim", error)) &&
              dump(*negative) == before, "invalid input changed IR");
    }
    llvm::outs() << "Metasim header: varied typed widths, original target escaping, zero target QSFP endpoints; ID/data/QSFP mismatches and missing metadata fail without mutation\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1], &context);
      require(bool(boundary), "boundary parse");
      auto c = *boundary->getOps<CircuitOp>().begin();
      auto result = run(c, "FireSim");
      for (auto expected : {"AXI4Config{12, 25, 32}", "AXI4Config{16, 34, 64}",
                           "AXI4Config{16, 64, 512}", "FPGATopQSFPConfig{256, 0}"})
        require(StringRef(result).contains(expected), "recorded U250 boundary configuration");
      require(succeeded(goldengate::emitOutputFiles(c, argv[2], "FireSim-generated", error)), error);
      llvm::outs() << "Recorded CIRCT U250 boundary: metasim .const.h and .const.vh emitted with original FireSim target name\n";
    }
    return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
