// See LICENSE for license details.
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/PeekPokeCycleEngine.h"
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/LoadMemHeader.h"
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
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
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
OwningOpRef<ModuleOp> cycleFixture(MLIRContext &context, unsigned maximum = 2,
                            unsigned fieldWidth = 1, unsigned latency = 0,
                            bool hasSource = false) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGUARTBridgeControlWrapper" {
      firrtl.module @GGUARTBridgeControlWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %resetToken: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  auto dict = [&](std::initializer_list<NamedAttribute> values) { return b.getDictionaryAttr(values); };
  auto str = [&](llvm::StringRef name, llvm::StringRef value) { return b.getNamedAttr(name, b.getStringAttr(value)); };
  auto key = dict({str("class", "firesim.lib.bridges.PeekPokeKey"),
      b.getNamedAttr("maxChannelDecoupling", b.getI64IntegerAttr(maximum)),
      b.getNamedAttr("peeks", b.getArrayAttr({})),
      b.getNamedAttr("pokes", b.getArrayAttr({dict({str("name", "reset"),
          b.getNamedAttr("fieldWidth", b.getI64IntegerAttr(fieldWidth)),
          b.getNamedAttr("tpe", dict({str("typeString", "UInt")}))})}))});
  auto bridge = dict({str("class", "firesim.lib.bridgeutils.BridgeIOAnnotation"),
      str("widgetClass", "midas.widgets.PeekPokeBridgeModule"),
      b.getNamedAttr("widgetConstructorKey", key),
      b.getNamedAttr("channelMapping", dict({str("reset", "peekPokeBridge_reset")})),
      str("target", "~FireSim|FireSim>peekPokeBridge")});
  NamedAttrList channel(dict({str("class", "midas.passes.fame.FAMEChannelConnectionAnnotation"),
      str("globalName", "peekPokeBridge_reset"),
      b.getNamedAttr("channelInfo", dict({str("class", "midas.passes.fame.PipeChannel"),
          b.getNamedAttr("latency", b.getI64IntegerAttr(latency))})),
      b.getNamedAttr("sinks", b.getArrayAttr({b.getStringAttr("~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>resetToken.bits")}))}));
  if (hasSource) channel.set("sources", b.getArrayAttr({b.getStringAttr("~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>other")}));
  auto copied = dict({str("class", "test.Annotation"), str("target", "~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>other")});
  c->setAttr("rawAnnotations", b.getArrayAttr({bridge, channel.getDictionary(&context), copied}));
  return root;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = cycleFixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addPeekPokeCycleEngine(c, error)), error);
  require(succeeded(goldengate::addPeekPokeMMIOBank(c, error)), error);
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto top = named(c, "GGPeekPokeMMIOWrapper");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(loc, b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{
        {b.getStringAttr("aw_addr"), UIntType::get(&ctx, 25, false), Direction::In}});
  auto row = b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 1 ? "Other_2" : "PeekPokeBridgeModule_2")),
      b.getNamedAttr("slave", b.getI32IntegerAttr(0)),
      b.getNamedAttr("start", b.getI64IntegerAttr(bad == 2 ? 1025 : bad == 3 ? (1 << 25)-4 : 1024)),
      b.getNamedAttr("size", b.getI64IntegerAttr(bad == 4 ? 24 : 32))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("Other_0")),
    b.getNamedAttr("slave", b.getI32IntegerAttr(1)), b.getNamedAttr("start", b.getI64IntegerAttr(1028)),
    b.getNamedAttr("size", b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions", bad == 5 ? b.getArrayAttr({row,other}) : b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({
    b.getNamedAttr("name", b.getStringAttr("PeekPokeBridgeModule_2")),
    b.getNamedAttr("port", b.getStringAttr("peekPokeBridge_ctrl")),
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
  auto bank = named(c, "GGPeekPokeMMIOBank");
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
  if (bad == 14) for (auto r : bank.getOps<RegOp>()) if (r.getName() == "target_reset_i") r.setNameAttr(b.getStringAttr("wrong_reset"));
  if (bad == 15) for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "DONE") r.setNameAttr(b.getStringAttr("wrong_done"));
  if (bad == 16) for (auto bits : bank.getOps<BitsPrimOp>()) if (bits.getLo() == 0 && bits.getHi() == 0) { bits.setLoAttr(b.getI32IntegerAttr(1)); bits.setHiAttr(b.getI32IntegerAttr(1)); }
  if (bad >= 19 && bad <= 24) {
    auto engine = named(c, "GGPeekPokeCycleEngine");
    NamedAttrList key(engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
    if (bad == 19) key.set("maxChannelDecoupling", b.getI64IntegerAttr(3));
    if (bad == 20) key.set("peeks", b.getArrayAttr({b.getDictionaryAttr({})}));
    if (bad == 21) key.set("pokes", b.getArrayAttr({}));
    if (bad == 22 || bad == 24) {
      NamedAttrList poke(cast<DictionaryAttr>(cast<ArrayAttr>(key.get("pokes"))[0]));
      if (bad == 22) poke.set("name", b.getStringAttr("other"));
      else poke.set("fieldWidth", b.getI64IntegerAttr(33));
      key.set("pokes", b.getArrayAttr({poke.getDictionary(&ctx)}));
    }
    if (bad == 23) key.erase("class");
    engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(&ctx));
  }
  if (bad == 25) for (auto r : bank.getOps<RegResetOp>())
    r->setOperand(2, bank.getBodyBlock()->getArgument(1));
  if (bad == 26) for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    if (field && field.getFieldName() == "bits" && slot && slot.getIndex() == 4)
      for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == "PRECISE_PEEKABLE")
        connect->setOperand(1, reg.getResult());
  }
  if (bad == 27) for (auto connect : bank.getOps<ConnectOp>()) {
    auto instance = connect.getDest().getDefiningOp<InstanceOp>();
    auto slot = connect.getSrc().getDefiningOp<SubindexOp>();
    if (instance && slot) slot.setIndexAttr(b.getI32IntegerAttr(5));
  }
  if (bad == 28) named(c, "GGPeekPokeCycleEngine")->removeAttr("goldengate.bridgeConstructor");
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
  require(succeeded(goldengate::preparePeekPokeHeader(c, error)), error);
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
    require(argc == 1 || argc == 3, "usage: PeekPokeHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".STEP = 1036", ".DONE = 1040", ".PRECISE_PEEKABLE = 1048",
                     "},\n  2,\n  args,\n  peek_poke_t::PortMap{{\"reset\", peek_poke_t::Port{.address = 1044, .chunks = 1}}}",
                     "peek_poke_t::PortMap{}", "#include \"bridges/peek_poke.h\"", "#ifdef GET_BRIDGE_CONSTRUCTOR",
                     "offsetof(PEEKPOKEBRIDGEMODULE_struct, PRECISE_PEEKABLE) == 2 * sizeof(uint64_t)",
                     "sizeof(PEEKPOKEBRIDGEMODULE_struct) == 3 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text), "allocation, widget index or constructor port maps differ");
    auto before = dump(*root); std::string error;
    require(failed(goldengate::preparePeekPokeHeader(c,error)) && dump(*root) == before, "duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 28; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::preparePeekPokeHeader(nc,error)) && !error.empty() && dump(*negative) == before,
              "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "PeekPoke header: varied base/index; driver substruct and input/output maps; preserved hardware/annotations; 29 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareResetPulseHeader(bc,error)),error);
      require(succeeded(goldengate::prepareLoadMemHeader(bc,error)),error); body = run(bc);
      for (auto text : {".STEP = 460", ".DONE = 464", ".PRECISE_PEEKABLE = 472",
                       "},\n  0,\n  args,\n  peek_poke_t::PortMap{{\"reset\", peek_poke_t::Port{.address = 468, .chunks = 1}}}"})
        require(StringRef(body).contains(text),"recorded U250 allocation/port maps differ");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: PeekPoke constructor composed with five existing header sections\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
