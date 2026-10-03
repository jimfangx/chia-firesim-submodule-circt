// See LICENSE for license details.
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/BlockDevMMIOBank.h"
#include "goldengate/BlockDevHeader.h"
#include "goldengate/TSIHeader.h"
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
OwningOpRef<ModuleOp> bankFixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGBlockDevWriteAckQueueWrapper" {
      firrtl.module @GGBlockDevWriteAckQueueWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %blockdev_req_deq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<tag: uint<1>, len: uint<32>, offset: uint<32>, write: uint<1>>>,
        out %blockdev_data_deq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<tag: uint<1>, data: uint<64>>>,
        in %blockdev_rresp_enq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<tag: uint<1>, data: uint<64>>>,
        in %blockdev_wack_enq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        in %blockdev_info: !firrtl.bundle<nsectors: uint<32>, max_req_len: uint<32>>,
        in %blockdev_timing: !firrtl.bundle<returnWrite: uint<1>, readRespBusy: uint<1>, wAckStallN flip: uint<1>, rRespStallN flip: uint<1>, tCycle flip: uint<24>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"blockdev_req_deq.bits.offset", "blockdev_data_deq.valid", "blockdev_rresp_enq.bits.data",
      "blockdev_wack_enq.bits", "blockdev_info.nsectors", "blockdev_timing.wAckStallN", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGBlockDevWriteAckQueueWrapper|GGBlockDevWriteAckQueueWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
// Exercise the actual MMIO transform; the token engine and control allocation
// are typed stand-ins. The optional boundary mode checks the complete compiler IR.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = bankFixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevMMIOBank(c,error)),error);
  auto bank = named(c,"GGBlockDevMMIOBank"), top = named(c,"GGBlockDevMMIOWrapper");
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(loc,b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{
    {b.getStringAttr("aw_addr"),uint(25),Direction::In}});
  auto row = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(bad == 1 ? "Other_2" : "BlockDevBridgeModule_2")),
    b.getNamedAttr("slave",b.getI32IntegerAttr(0)),b.getNamedAttr("start",b.getI64IntegerAttr(bad == 2 ? 1025 : bad == 3 ? (1<<25)-4 : 1024)),
    b.getNamedAttr("size",b.getI64IntegerAttr(bad == 4 ? 100 : 128))});
  auto overlap = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("Other_0")),b.getNamedAttr("slave",b.getI32IntegerAttr(1)),
    b.getNamedAttr("start",b.getI64IntegerAttr(1028)),b.getNamedAttr("size",b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions",bad == 5 ? b.getArrayAttr({row,overlap}) : b.getArrayAttr({row}));
  auto binding = b.create<FModuleOp>(loc,b.getStringAttr("GGBlockDevBridgeBoundWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if (bad != 7) binding->setAttr("goldengate.blockdevSlave",b.getI32IntegerAttr(bad == 6 ? 1 : 0));
  auto engine = b.create<FModuleOp>(loc,b.getStringAttr("GGBlockDevTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{
    {b.getStringAttr("clock"),ClockType::get(&ctx),Direction::In},{b.getStringAttr("reset"),uint(1),Direction::In}});
  engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr(bad == 8 ? "WrongKey" : "firechip.bridgeinterfaces.BlockDeviceConfig")),
    b.getNamedAttr("nTrackers",b.getI32IntegerAttr(bad == 9 ? 2 : 1))}));
  b.setInsertionPointToStart(engine.getBodyBlock());
  auto zero = b.create<ConstantOp>(loc,uint(24),APInt(24,0));
  b.create<RegResetOp>(loc,uint(24),engine.getBodyBlock()->getArgument(0),engine.getBodyBlock()->getArgument(1),zero,bad == 10 ? "missingCycle" : "tCycle");
  b.setInsertionPointToStart(top.getBodyBlock());
  b.create<InstanceOp>(loc,decoder,"decode"); b.create<InstanceOp>(loc,binding,"bound");
  if (bad != 11) b.create<InstanceOp>(loc,engine,"tokens");
  if (bad == 12) b.create<InstanceOp>(loc,engine,"duplicateTokens");
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> annotations(raw.begin(),raw.end());
  auto output = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("existing body\n"))});
  if (bad != 13) annotations.push_back(output);
  if (bad == 14) annotations.push_back(output);
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations));
  if (bad >= 15 && bad <= 18) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"); SmallVector<Attribute> entries(regs.begin(),regs.end());
    NamedAttrList entry(cast<DictionaryAttr>(entries[bad == 17 ? 2 : 0]));
    if (bad == 15) entry.set("name",b.getStringAttr("wrong"));
    if (bad == 16) entry.set("offset",b.getI64IntegerAttr(8));
    if (bad == 17) entry.set("readable",b.getBoolAttr(true));
    if (bad == 18) entry.set("writeable",b.getBoolAttr(false));
    entries[bad == 17 ? 2 : 0] = entry.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(entries));
  }
  if (bad == 19 || bad == 20) for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == "read_latency") {
    b.setInsertionPoint(reg);
    auto wire = b.create<WireOp>(loc,bad == 19 ? FIRRTLType(ClockType::get(&ctx)) : FIRRTLType(uint(1)),"wrongDomain");
    reg->setOperand(bad == 19 ? 0 : 1,wire.getResult()); break;
  }
  if (bad == 21) for (auto reg : bank.getOps<RegOp>()) if (reg.getName() == "nsectorReg") reg->setAttr("name",b.getStringAttr("missing"));
  if (bad == 22) { auto conn = *bank.getOps<StrictConnectOp>().begin(); conn.erase(); }
  if (bad == 23) for (auto bits : bank.getOps<BitsPrimOp>()) if (bits.getHi() == 63) { bits->setAttr("lo",b.getI32IntegerAttr(0)); break; }
  if (bad == 24) for (auto cat : bank.getOps<CatPrimOp>()) { Value left=cat.getLhs(); cat->setOperand(0,cat.getRhs()); cat->setOperand(1,left); break; }
  if (bad == 25) for (auto conn : bank.getOps<StrictConnectOp>()) {
    auto mux = conn.getSrc().getDefiningOp<MuxPrimOp>();
    if (mux) { mux->setOperand(2,mux.getHigh()); break; }
  }
  if (bad == 26) {
    auto conn = *bank.getOps<StrictConnectOp>().begin(); b.setInsertionPointAfter(conn); b.clone(*conn);
  }
  if (bad == 27) for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == "read_latency") {
    b.setInsertionPoint(reg); auto wrong = b.create<ConstantOp>(loc,uint(24),APInt(24,0)); reg->setOperand(2,wrong); break;
  }
  return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules;
  for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareBlockDevHeader(c, error)), error);
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
    require(argc == 1 || argc == 3,"usage: BlockDevHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".read_latency = 1024", ".bdev_nsectors = 1032", ".bdev_rresp_stalled = 1124",
        "registry.add_widget(new blockdev_t(","},\n  2,\n  args,\n  1,\n  24\n));",
        "#include \"bridges/blockdev.h\"","sizeof(BLOCKDEVBRIDGEMODULE_struct) == 26 * sizeof(uint64_t)",
        "offsetof(BLOCKDEVBRIDGEMODULE_struct, bdev_rresp_stalled) == 25 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text),"BlockDev allocation/constructor ABI differs");
    std::string error, before = dump(*root);
    require(failed(goldengate::prepareBlockDevHeader(c,error)) && dump(*root) == before,"duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 27; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareBlockDevHeader(nc,error)) && !error.empty() && dump(*negative) == before,
          "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "BlockDev header: varied base/index, 26-word ABI, constructor parameters, preserved hardware/annotations; 28 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareResetPulseHeader(bc,error)),error);
      require(succeeded(goldengate::prepareLoadMemHeader(bc,error)),error);
      require(succeeded(goldengate::preparePeekPokeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareUARTHeader(bc,error)),error);
      require(succeeded(goldengate::prepareTSIHeader(bc,error)),error); body = run(bc);
      for (auto text : {".read_latency = 0", ".write_latency = 4", ".bdev_nsectors = 8",
                       ".bdev_rresp_stalled = 100", "},\n  0,\n  args,\n  1,\n  24\n));"})
        require(StringRef(body).contains(text),"recorded U250 BlockDev allocation differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: BlockDev constructor composed with eight existing header sections\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
