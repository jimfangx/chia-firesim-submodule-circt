// See LICENSE for license details.
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/SingleClockBridge.h"
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
// Use the real clock producer; the small surrounding graph supplies the
// allocation/binding contracts from ControlWidgetWrites/ControlReadDispatch.
OwningOpRef<ModuleOp> clockFixture(MLIRContext &ctx, unsigned lanes = 1) {
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"GGFAMEPipeWrapper\" { firrtl.module @GGFAMEPipeWrapper("
      "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
      "in %ticks: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: vector<uint<1>, " + std::to_string(lanes) + ">>) {} }}", &ctx);
  require(bool(root), "clock fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto clock = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("base")),
    b.getNamedAttr("multiplier",b.getI64IntegerAttr(1)),b.getNamedAttr("divisor",b.getI64IntegerAttr(1))});
  SmallVector<Attribute> clockValues, mfmrs, sinks;
  for (unsigned i = 0; i < lanes; ++i) {
    NamedAttrList c(clock);
    c.set("name",b.getStringAttr("clock"+std::to_string(i)));
    c.set("divisor",b.getI64IntegerAttr(i+1));
    clockValues.push_back(c.getDictionary(&ctx));
    mfmrs.push_back(b.getI64IntegerAttr(i+1));
    sinks.push_back(b.getStringAttr("~GGFAMEPipeWrapper|GGFAMEPipeWrapper>ticks.bits["+std::to_string(i)+"]"));
  }
  auto clocks = b.getArrayAttr(clockValues);
  auto info = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::TargetClockChannel)),
    b.getNamedAttr("clockInfo",clocks),b.getNamedAttr("perClockMFMR",b.getArrayAttr(mfmrs))});
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
    b.getNamedAttr("globalName",b.getStringAttr("clockBridge_clocks")),b.getNamedAttr("channelInfo",info),
    b.getNamedAttr("sinks",b.getArrayAttr(sinks))});
  auto key = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridges.ClockParameters")),
    b.getNamedAttr("clocks",clocks)});
  auto bridge = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.ClockBridgeModule")),b.getNamedAttr("widgetConstructorKey",key),
    b.getNamedAttr("channelMapping",b.getDictionaryAttr({b.getNamedAttr("clocks",b.getStringAttr("clockBridge_clocks"))}))});
  c->setAttr("rawAnnotations",b.getArrayAttr({channel,bridge})); return root;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0, unsigned lanes = 1) {
  auto root = clockFixture(ctx,lanes); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addClockBridge(c, error)), error);
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto top = named(c, "GGClockBridgeWrapper");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(loc, b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{
        {b.getStringAttr("aw_addr"), UIntType::get(&ctx, 25, false), Direction::In}});
  auto row = b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 1 ? "Other_2" : "ClockBridgeModule_2")),
      b.getNamedAttr("slave", b.getI32IntegerAttr(0)),
      b.getNamedAttr("start", b.getI64IntegerAttr(bad == 2 ? 1025 : bad == 3 ? (1 << 25)-8 : 1024)),
      b.getNamedAttr("size", b.getI64IntegerAttr(bad == 4 ? 16 : 32))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("Other_0")),
    b.getNamedAttr("slave", b.getI32IntegerAttr(1)), b.getNamedAttr("start", b.getI64IntegerAttr(1040)),
    b.getNamedAttr("size", b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions", bad == 5 ? b.getArrayAttr({row,other}) : b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({
    b.getNamedAttr("name", b.getStringAttr("ClockBridgeModule_2")),
    b.getNamedAttr("port", b.getStringAttr("clockBridge_ctrl")),
    b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 6 ? 1 : 0))});
  SmallVector<FModuleOp> helpers{decoder};
  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper", "goldengate.controlWriteBindings"),
                    std::make_pair("GGControlReadDispatchWrapper", "goldengate.controlReadBindings")}) {
    auto m = b.create<FModuleOp>(loc, b.getStringAttr(pair.first), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    m->setAttr(pair.second, bad == 7 && StringRef(pair.first) == "GGControlReadDispatchWrapper" ?
        b.getArrayAttr({}) : bad == 8 ? b.getArrayAttr({binding,binding}) : b.getArrayAttr({binding}));
    helpers.push_back(m);
  }
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) b.create<InstanceOp>(loc, m, m.getName());
  auto bank = named(c, "GGSingleClockBridge");
  if (bad == 9) b.create<InstanceOp>(loc, bank, "duplicateClock");
  if (bad == 10) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    b.create<FModuleOp>(loc, b.getStringAttr("DisconnectedTop"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    c.setNameAttr(b.getStringAttr("DisconnectedTop"));
  }
  if (bad == 11) bank->removeAttr("goldengate.mmioRegisters");
  if (bad == 12 || bad == 13) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> changed(regs.begin(),regs.end()); NamedAttrList d(cast<DictionaryAttr>(changed[0]));
    d.set(bad == 12 ? "offset" : "readable", bad == 12 ? Attribute(b.getI32IntegerAttr(4)) : Attribute(b.getBoolAttr(false)));
    changed[0] = d.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(changed));
  }
  if (bad == 14) for (auto r : bank.getOps<RegOp>()) if (r.getName() == "hCycle_mmreg") r.setNameAttr(b.getStringAttr("wrong_snapshot"));
  if (bad == 15) for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "tCycleFastest") r.setNameAttr(b.getStringAttr("wrong_counter"));
  if (bad == 16) for (auto bits : bank.getOps<BitsPrimOp>()) if (bits.getLo() == 0 && bits.getHi() == 0) { bits.setLoAttr(b.getI32IntegerAttr(1)); bits.setHiAttr(b.getI32IntegerAttr(1)); }
  auto output = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("// retained header\n"))});
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> annotations(raw.begin(),raw.end());
  if (bad != 17) annotations.push_back(output);
  if (bad == 18) annotations.push_back(output);
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules;
  for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareClockBridgeHeader(c, error)), error);
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
    require(argc == 1 || argc == 3, "usage: ClockBridgeHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".hCycle_0 = 1024", ".hCycle_1 = 1028", ".hCycle_latch = 1032", ".tCycle_0 = 1036",
                     ".tCycle_1 = 1040", ".tCycle_latch = 1044", "},\n  2,\n  args",
                     "#include \"bridges/clock.h\"", "#ifdef GET_BRIDGE_CONSTRUCTOR",
                     "offsetof(CLOCKBRIDGEMODULE_struct, tCycle_latch) == 5 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text), "allocation, widget index or driver ABI differs");
    for (unsigned lanes : {2U,3U}) {
      auto rational = fixture(ctx,0,lanes);
      require(run(*rational->getOps<CircuitOp>().begin())==body,
              "rational clock lanes changed six-word driver ABI");
    }
    auto before = dump(*root); std::string error;
    require(failed(goldengate::prepareClockBridgeHeader(c,error)) && dump(*root) == before, "duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 18; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareClockBridgeHeader(nc,error)) && !error.empty() && dump(*negative) == before,
              "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "ClockBridge header: varied base/index, live snapshots/latches, preserved hardware/annotations; nineteen atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error); body = run(bc);
      for (auto text : {".hCycle_0 = 512", ".hCycle_1 = 516", ".hCycle_latch = 520", ".tCycle_0 = 524",
                       ".tCycle_1 = 528", ".tCycle_latch = 532", "},\n  0,\n  args"})
        require(StringRef(body).contains(text),"recorded U250 allocation differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: ClockBridge constructor composed with metasim and master header\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
