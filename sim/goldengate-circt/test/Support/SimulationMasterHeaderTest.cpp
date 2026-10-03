// See LICENSE for license details.
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/SimulationMaster.h"
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
void require(bool ok, StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
std::string dump(Operation *op) {
  std::string s; llvm::raw_string_ostream out(s);
  op->print(out, OpPrintingFlags().useLocalScope()); return s;
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing fixture module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"GGControlWriteTrackerWrapper\" attributes {rawAnnotations = []} {"
      "firrtl.module @GGControlWriteTrackerWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>) {} }}", &ctx);
  require(bool(root), "fixture parse");
  auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addSimulationMasterBank(c, error)), error);
  OpBuilder b(&ctx); auto loc = c.getLoc();
  auto top = named(c, "GGSimulationMasterWrapper");
  top->setAttr("sym_name", b.getStringAttr("GGSimulationMasterBoundWrapper"));
  c.setNameAttr(b.getStringAttr("GGSimulationMasterBoundWrapper"));
  top->setAttr("goldengate.simulationMasterSlave", b.getI32IntegerAttr(bad == 1 ? 2 : 0));
  b.setInsertionPointToEnd(c.getBodyBlock());
  SmallVector<PortInfo> ports{{b.getStringAttr("aw_addr"), UIntType::get(&ctx, 25, false), Direction::In}};
  auto decoder = b.create<FModuleOp>(loc, b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto row = b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 2 ? "Other_2" : "SimulationMaster_2")),
      b.getNamedAttr("slave", b.getI32IntegerAttr(0)),
      b.getNamedAttr("start", b.getI64IntegerAttr(bad == 3 ? 1025 : bad == 4 ? (1 << 25) - 8 : 1024)),
      b.getNamedAttr("size", b.getI64IntegerAttr(bad == 5 ? 8 : 16))});
  decoder->setAttr("goldengate.controlRegions", b.getArrayAttr({row}));
  b.setInsertionPointToStart(top.getBodyBlock());
  b.create<InstanceOp>(loc, decoder, "decoder");
  auto bank = named(c, "GGSimulationMasterBank");
  if (bad == 6) bank->removeAttr("goldengate.mmioRegisters");
  if (bad == 7 || bad == 8) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> entries(regs.begin(), regs.end());
    NamedAttrList first(cast<DictionaryAttr>(entries[0]));
    first.set(bad == 7 ? "offset" : "readable", bad == 7 ? Attribute(b.getI32IntegerAttr(4)) : Attribute(b.getBoolAttr(false)));
    entries[0] = first.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(entries));
  }
  if (bad == 9) {
    for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "INIT_DONE") r.setNameAttr(b.getStringAttr("WrongIdentity"));
  }
  if (bad == 10) b.create<InstanceOp>(loc, bank, "secondMaster");
  if (bad == 11) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    b.create<FModuleOp>(loc, b.getStringAttr("DisconnectedTop"),
        ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    c.setNameAttr(b.getStringAttr("DisconnectedTop"));
  }
  auto output = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix", b.getStringAttr(".const.h")),
      b.getNamedAttr("body", b.getStringAttr("// retained metasim header\n"))});
  auto retained = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Retained"))});
  c->setAttr("rawAnnotations", bad == 12 ? b.getArrayAttr({retained}) :
      bad == 13 ? b.getArrayAttr({retained, output, output}) : b.getArrayAttr({retained, output}));
  return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules;
  for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareSimulationMasterHeader(c, error)), error);
  auto updated = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == updated.size(), "changed annotation count");
  unsigned changed = 0; std::string result;
  for (unsigned i = 0; i < raw.size(); ++i) {
    if (raw[i] == updated[i]) continue;
    ++changed; auto before = cast<DictionaryAttr>(raw[i]), after = cast<DictionaryAttr>(updated[i]);
    result = after.getAs<StringAttr>("body").getValue().str();
    require(StringRef(result).starts_with(before.getAs<StringAttr>("body").getValue()), "lost existing header body");
    for (auto a : before) if (a.getName() != "body") require(after.get(a.getName()) == a.getValue(), "changed output metadata");
  }
  require(changed == 1, "changed unrelated annotations");
  unsigned i = 0;
  for (auto m : c.getOps<FModuleLike>()) require(modules[i++] == dump(m.getOperation()), "changed hardware semantics");
  return result;
}
}
int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 3, "usage: SimulationMasterHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
    auto body = run(c);
    for (auto text : {".INIT_DONE = 1024", ".PRESENCE_READ = 1028", ".PRESENCE_WRITE = 1032",
                     "},\n  2,\n  args", "#include \"bridges/master.h\"", "#ifdef GET_CORE_CONSTRUCTOR",
                     "offsetof(SIMULATIONMASTER_struct, PRESENCE_WRITE) == 2 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text), "allocation, widget index or driver ABI mismatch");
    auto before = dump(*root); std::string error;
    require(failed(goldengate::prepareSimulationMasterHeader(c, error)) && dump(*root) == before, "duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 13; ++bad) {
      auto negative = fixture(ctx, bad); auto nc = *negative->getOps<CircuitOp>().begin();
      before = dump(*negative);
      require(failed(goldengate::prepareSimulationMasterHeader(nc, error)) && dump(*negative) == before,
              "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "SimulationMaster header: varied allocation/index, live bank read-slot identity, preserved hardware/annotations; fourteen atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1], &ctx);
      require(bool(boundary), "boundary parse"); auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc, "FireSim", error)), error);
      body = run(bc);
      for (auto text : {".INIT_DONE = 544", ".PRESENCE_READ = 548", ".PRESENCE_WRITE = 552", "},\n  0,\n  args"})
        require(StringRef(body).contains(text), "recorded U250 allocation differs");
      require(succeeded(goldengate::emitOutputFiles(bc, argv[2], "FireSim-generated", error)), error);
      llvm::outs() << "Recorded CIRCT U250 boundary: SimulationMaster constructor and metasim .const.h emitted\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
