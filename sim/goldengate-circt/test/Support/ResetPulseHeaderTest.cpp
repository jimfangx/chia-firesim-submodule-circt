// See LICENSE for license details.
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/ResetPulseBridge.h"
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
// The actual bridge transform supplies the typed register bank. Only the small
// allocation graph is synthetic, with a base/index distinct from the U250 oracle.
OwningOpRef<ModuleOp> resetFixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(
    "module { firrtl.circuit \"GGClockBridgeControlWrapper\" { firrtl.module @GGClockBridgeControlWrapper("
    "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
    "in %resetTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>) {} }}", &ctx);
  require(bool(root), "reset fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto key = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridges.ResetPulseBridgeParameters")),
    b.getNamedAttr("activeHigh", b.getBoolAttr(false)),
    b.getNamedAttr("maxPulseLength", b.getI64IntegerAttr(5)),
    b.getNamedAttr("defaultPulseLength", b.getI64IntegerAttr(2))});
  auto bridge = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("widgetClass", b.getStringAttr("midas.widgets.ResetPulseBridgeModule")),
    b.getNamedAttr("widgetConstructorKey", key),
    b.getNamedAttr("channelMapping", b.getDictionaryAttr({b.getNamedAttr("reset", b.getStringAttr("resetBridge_reset"))}))});
  auto info = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
    b.getNamedAttr("latency", b.getI64IntegerAttr(0))});
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
    b.getNamedAttr("globalName", b.getStringAttr("resetBridge_reset")),b.getNamedAttr("channelInfo", info),
    b.getNamedAttr("sinks", b.getArrayAttr({b.getStringAttr("~GGClockBridgeControlWrapper|GGClockBridgeControlWrapper>resetTokens.bits")}))});
  c->setAttr("rawAnnotations", b.getArrayAttr({channel,bridge})); return root;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = resetFixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addResetPulseBridge(c, error)), error);
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto top = named(c, "GGResetPulseBridgeWrapper");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(loc, b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{
        {b.getStringAttr("aw_addr"), UIntType::get(&ctx, 25, false), Direction::In}});
  auto row = b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 1 ? "Other_2" : "ResetPulseBridgeModule_2")),
      b.getNamedAttr("slave", b.getI32IntegerAttr(0)),
      b.getNamedAttr("start", b.getI64IntegerAttr(bad == 2 ? 1025 : bad == 3 ? (1 << 25)-4 : 1024)),
      b.getNamedAttr("size", b.getI64IntegerAttr(bad == 4 ? 4 : 8))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("Other_0")),
    b.getNamedAttr("slave", b.getI32IntegerAttr(1)), b.getNamedAttr("start", b.getI64IntegerAttr(1028)),
    b.getNamedAttr("size", b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions", bad == 5 ? b.getArrayAttr({row,other}) : b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({
    b.getNamedAttr("name", b.getStringAttr("ResetPulseBridgeModule_2")),
    b.getNamedAttr("port", b.getStringAttr("resetBridge_ctrl")),
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
  auto bank = named(c, "GGResetPulseBridge");
  if (bad == 9) b.create<InstanceOp>(loc, bank, "duplicateReset");
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
  if (bad == 14) for (auto r : bank.getOps<RegOp>()) if (r.getName() == "pulseLength") r.setNameAttr(b.getStringAttr("wrong_pulse"));
  if (bad == 15) for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "doneInit") r.setNameAttr(b.getStringAttr("wrong_init"));
  if (bad == 16) for (auto bits : bank.getOps<BitsPrimOp>()) if (bits.getLo() == 0 && bits.getHi() == 0) { bits.setLoAttr(b.getI32IntegerAttr(1)); bits.setHiAttr(b.getI32IntegerAttr(1)); }
  if (bad >= 19 && bad <= 24) {
    NamedAttrList key(bank->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
    if (bad == 19) key.set("defaultPulseLength", b.getI64IntegerAttr(6));
    if (bad == 20) key.set("maxPulseLength", b.getI64IntegerAttr(0));
    if (bad == 21) key.set("maxPulseLength", b.getI64IntegerAttr(1023));
    if (bad == 22) key.set("activeHigh", b.getBoolAttr(true));
    if (bad == 23) key.erase("class");
    if (bad == 24) key.set("defaultPulseLength", b.getI64IntegerAttr(-1));
    bank->setAttr("goldengate.bridgeConstructor", key.getDictionary(&ctx));
  }
  if (bad == 25) for (auto r : bank.getOps<RegResetOp>())
    r->setOperand(2, bank.getBodyBlock()->getArgument(1));
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
  require(succeeded(goldengate::prepareResetPulseHeader(c, error)), error);
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
    require(argc == 1 || argc == 3, "usage: ResetPulseHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".pulseLength = 1024", ".doneInit = 1028", "},\n  2,\n  args,\n  5U,\n  2U",
                     "#include \"bridges/reset_pulse.h\"", "#ifdef GET_BRIDGE_CONSTRUCTOR",
                     "offsetof(RESETPULSEBRIDGEMODULE_struct, doneInit) == 1 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text), "allocation, widget index or constructor ABI differs");
    auto before = dump(*root); std::string error;
    require(failed(goldengate::prepareResetPulseHeader(c,error)) && dump(*root) == before, "duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 25; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareResetPulseHeader(nc,error)) && !error.empty() && dump(*negative) == before,
              "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "ResetPulse header: varied base/index/parameters and polarity; preserved hardware/annotations; 26 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error); body = run(bc);
      for (auto text : {".pulseLength = 560", ".doneInit = 564", "},\n  0,\n  args,\n  1023U,\n  50U"})
        require(StringRef(body).contains(text),"recorded U250 allocation/constructor differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: ResetPulse constructor composed with metasim, master and clock headers\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
