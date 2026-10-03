// See LICENSE for license details.
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/UARTSerialEngine.h"
#include "goldengate/UARTHeader.h"
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
OwningOpRef<ModuleOp> serialFixture(MLIRContext &context, unsigned div) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGResetPulseBridgeControlWrapper" {
      firrtl.module @GGResetPulseBridgeControlWrapper(
        out %resetTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %rxTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %txTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context); NamedAttrList mapping;
  const llvm::StringRef locals[]{"reset", "uart_txd", "uart_rxd"};
  const llvm::StringRef ports[]{"resetTokens", "txTokens", "rxTokens"};
  SmallVector<Attribute> annos;
  for (unsigned j = 0; j < 3; ++j) {
    auto name = b.getStringAttr("ep_1_" + locals[j].str()); mapping.set(locals[j], name);
    NamedAttrList channel;
    channel.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection));
    channel.set("globalName", name);
    channel.set("channelInfo", b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
      b.getNamedAttr("latency", b.getI64IntegerAttr(1))}));
    channel.set("clock", b.getStringAttr("~GGResetPulseBridgeControlWrapper|GGResetPulseBridgeControlWrapper>hostClock"));
    channel.set(j == 2 ? "sinks" : "sources", b.getArrayAttr({b.getStringAttr(
      "~GGResetPulseBridgeControlWrapper|GGResetPulseBridgeControlWrapper>" + ports[j].str() + ".bits")}));
    annos.push_back(channel.getDictionary(&context));
  }
  auto bridge = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
      b.getNamedAttr("widgetClass", b.getStringAttr("firechip.goldengateimplementations.UARTBridgeModule")),
      b.getNamedAttr("target", b.getStringAttr("~Original|Original>ep_1")),
      b.getNamedAttr("widgetConstructorKey", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("firechip.bridgeinterfaces.UARTKey")),
        b.getNamedAttr("div", b.getI64IntegerAttr(div))})),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&context))});
  annos.push_back(bridge);
  annos.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DontTouch)),
    b.getNamedAttr("target", b.getStringAttr("~GGResetPulseBridgeControlWrapper|GGResetPulseBridgeControlWrapper>other"))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
// Exercise the real UART transforms with a baud key, base and instance number
// distinct from the U250 reference. Only the control allocation graph is synthetic.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = serialFixture(ctx, 7); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addUARTSerialEngine(c, error)), error);
  require(succeeded(goldengate::addUARTByteQueues(c, error)), error);
  require(succeeded(goldengate::addUARTMMIOBank(c, error)), error);
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto top = named(c, "GGUARTMMIOWrapper");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(loc, b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{
        {b.getStringAttr("aw_addr"), UIntType::get(&ctx, 25, false), Direction::In}});
  auto row = b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 1 ? "Other_2" : "UARTBridgeModule_2")),
      b.getNamedAttr("slave", b.getI32IntegerAttr(0)),
      b.getNamedAttr("start", b.getI64IntegerAttr(bad == 2 ? 1025 : bad == 3 ? (1 << 25)-4 : 1024)),
      b.getNamedAttr("size", b.getI64IntegerAttr(bad == 4 ? 20 : 32))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("Other_0")),
    b.getNamedAttr("slave", b.getI32IntegerAttr(1)), b.getNamedAttr("start", b.getI64IntegerAttr(1028)),
    b.getNamedAttr("size", b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions", bad == 5 ? b.getArrayAttr({row,other}) : b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({
    b.getNamedAttr("name", b.getStringAttr("UARTBridgeModule_2")),
    b.getNamedAttr("port", b.getStringAttr("uartBridge_ctrl")),
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
  auto bank = named(c, "GGUARTMMIOBank");
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
  if (bad == 14) for (auto r : bank.getOps<RegOp>()) if (r.getName() == "out_bits") r.setNameAttr(b.getStringAttr("wrong_byte"));
  if (bad == 15) for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "out_ready") r.setNameAttr(b.getStringAttr("wrong_pulse"));
  if (bad == 16) for (auto bits : bank.getOps<BitsPrimOp>()) if (bits.getLo() == 0 && bits.getHi() == 0) { bits.setLoAttr(b.getI32IntegerAttr(1)); bits.setHiAttr(b.getI32IntegerAttr(1)); }
  auto engine = named(c, "GGUARTSerialEngine");
  if (bad == 19 || bad == 20) {
    NamedAttrList key(engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
    if (bad == 19) key.set("div", b.getI64IntegerAttr(0)); else key.erase("class");
    engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(&ctx));
  }
  if (bad == 21) engine->removeAttr("goldengate.bridgeConstructor");
  if (bad == 22) for (auto r : bank.getOps<RegResetOp>()) r->setOperand(2, bank.getBodyBlock()->getArgument(1));
  if (bad == 23) for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    if (field && field.getFieldName() == "bits" && slot && slot.getIndex() == 0)
      for (auto pad : bank.getOps<PadPrimOp>()) {
        auto reg = pad.getInput().getDefiningOp<RegOp>();
        if (reg && reg.getName() == "in_bits") connect->setOperand(1, pad.getResult());
      }
  }
  if (bad == 24) for (auto mux : bank.getOps<MuxPrimOp>()) {
    auto reg = mux.getLow().getDefiningOp<RegOp>();
    if (reg && reg.getName() == "in_bits") mux->setOperand(2, bank.getBodyBlock()->getArgument(1));
  }
  if (bad == 25) for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    if (field && field.getInput() == bank.getBodyBlock()->getArgument(3) && field.getFieldName() == "valid")
      for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "out_ready") connect->setOperand(1,r.getResult());
  }
  if (bad == 26) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> changed(regs.begin(),regs.end()); NamedAttrList d(cast<DictionaryAttr>(changed[0]));
    d.set("writeable", b.getBoolAttr(false)); changed[0] = d.getDictionary(&ctx);
    bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(changed));
  }
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
  require(succeeded(goldengate::prepareUARTHeader(c, error)), error);
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
    require(argc == 1 || argc == 3, "usage: UARTHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".out_bits = 1024", ".out_valid = 1028", ".out_ready = 1032",
                     ".in_bits = 1036", ".in_valid = 1040", ".in_ready = 1044",
                     "registry.add_widget(new uart_t(", "},\n  2,\n  args\n));",
                     "#include \"bridges/uart.h\"", "#ifdef GET_BRIDGE_CONSTRUCTOR",
                     "offsetof(UARTBRIDGEMODULE_struct, in_ready) == 5 * sizeof(uint64_t)",
                     "sizeof(UARTBRIDGEMODULE_struct) == 6 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text), "allocation, widget index or UART constructor differs");
    auto before = dump(*root); std::string error;
    require(failed(goldengate::prepareUARTHeader(c,error)) && dump(*root) == before, "duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 26; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareUARTHeader(nc,error)) && !error.empty() && dump(*negative) == before,
              "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "UART header: varied base/index and baud key; six-address ABI; preserved hardware/annotations; 27 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareResetPulseHeader(bc,error)),error);
      require(succeeded(goldengate::prepareLoadMemHeader(bc,error)),error);
      require(succeeded(goldengate::preparePeekPokeHeader(bc,error)),error); body = run(bc);
      for (auto text : {".out_bits = 480", ".out_valid = 484", ".out_ready = 488",
                       ".in_bits = 492", ".in_valid = 496", ".in_ready = 500", "},\n  0,\n  args\n));"})
        require(StringRef(body).contains(text),"recorded U250 UART allocation differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: UART constructor composed with six existing header sections\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
