// See LICENSE for license details.
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TracerVTokenEngine.h"
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
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
// Use native queue, read, count and empty-write transforms. Only the control
// allocator/binding modules are synthetic; count and stream forwarding are real.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGTracerVBridgeControlWrapper" {
    firrtl.module @GGTracerVBridgeControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>) {}
  } })", &ctx);
  require(bool(root),"fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); std::string error;
  auto output = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("previous body\n"))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("value",b.getI32IntegerAttr(9))});
  c->setAttr("rawAnnotations",b.getArrayAttr({other,output}));
  require(succeeded(goldengate::addTracerVStreamQueue(c,error)),error);
  require(succeeded(goldengate::addCPUStreamRead(c,error)),error);
  require(succeeded(goldengate::addCPUStreamCountBank(c,error)),error);
  auto top = named(c,c.getName()), bank = named(c,"GGCPUStreamCountBank");
  auto queue = named(c,"GGTracerVStreamQueue6144"), read = named(c,"GGCPUStreamRead");
  auto eroot = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGCPUStreamControlWrapper" {
    firrtl.module @GGCPUStreamControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %cpu_stream_r_bits_id: !firrtl.uint<16>) {}
  } })",&ctx);
  auto ec = *eroot->getOps<CircuitOp>().begin(); ec->setAttr("rawAnnotations",b.getArrayAttr({}));
  require(succeeded(goldengate::addEmptyCPUStreamWrite(ec,error)),error);
  b.setInsertionPointToEnd(c.getBodyBlock()); auto empty = cast<FModuleOp>(b.clone(*named(ec,"GGEmptyCPUStreamWrite")));
  auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  auto decoder = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{
      {b.getStringAttr("aw_addr"),uint(25),Direction::In}});
  auto row = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("CPUManagedStreamEngine_2")),
      b.getNamedAttr("slave",b.getI32IntegerAttr(0)),b.getNamedAttr("start",b.getI64IntegerAttr(2048)),b.getNamedAttr("size",b.getI64IntegerAttr(4))});
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("CPUManagedStreamEngine_2")),
      b.getNamedAttr("port",b.getStringAttr("cpuStream_ctrl")),b.getNamedAttr("slave",b.getI32IntegerAttr(0))});
  SmallVector<FModuleOp> helpers{decoder,empty};
  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper","goldengate.controlWriteBindings"),
                   std::make_pair("GGControlReadDispatchWrapper","goldengate.controlReadBindings")}) {
    auto m = b.create<FModuleOp>(c.getLoc(),b.getStringAttr(pair.first),top.getConventionAttr(),ArrayRef<PortInfo>{});
    m->setAttr(pair.second,b.getArrayAttr({binding})); helpers.push_back(m);
  }
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) b.create<InstanceOp>(c.getLoc(),m,m.getName());
  // Vary stream identity independently of widget index and check C++ escaping.
  auto stream = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"); NamedAttrList sp(stream);
  sp.set("name",b.getStringAttr("trace\"stream\\domain"));
  if (bad == 1) sp.set("depth",b.getI64IntegerAttr(6143));
  if (bad == 2) sp.set("widthBytes",b.getI64IntegerAttr(32));
  if (bad == 3) sp.set("index",b.getI64IntegerAttr(1));
  if (bad == 4) sp.set("name",b.getStringAttr(""));
  queue->setAttr("goldengate.streamParameters",sp.getDictionary(&ctx));
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"); NamedAttrList word(cast<DictionaryAttr>(regs[0]));
  word.set("name",b.getStringAttr("trace\"stream\\domain_count"));
  if (bad == 5) word.set("offset",b.getI32IntegerAttr(4));
  if (bad == 6) word.set("readable",b.getBoolAttr(false));
  if (bad == 7) word.set("writeable",b.getBoolAttr(true));
  if (bad == 8) word.set("name",b.getStringAttr("wrong_count"));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr({word.getDictionary(&ctx)}));
  if (bad == 9) read->setAttr("goldengate.streamAddressSpaceBits",b.getI64IntegerAttr(18));
  if (bad == 10) empty->setAttr("goldengate.fromHostCPUStreamCount",b.getI32IntegerAttr(1));
  if (bad == 11) for (auto mem : queue.getOps<MemOp>()) mem.setDepthAttr(b.getI64IntegerAttr(1024));
  if (bad == 12) for (auto bits : read.getOps<BitsPrimOp>()) if (bits.getHi() == 63) bits.setLoAttr(b.getI32IntegerAttr(18));
  if (bad == 13) for (auto eq : read.getOps<EQPrimOp>()) {
    auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
    if (bits && bits.getHi() == 63) { b.setInsertionPoint(eq); eq->setOperand(1,b.create<ConstantOp>(c.getLoc(),uint(45),APInt(45,1))); }
  }
  if (bad >= 14 && bad <= 18) {
    NamedAttrList r(row);
    if (bad == 14) r.set("start",b.getI64IntegerAttr(2049));
    if (bad == 15) r.set("start",b.getI64IntegerAttr(1<<25));
    if (bad == 16) r.set("size",b.getI64IntegerAttr(0));
    if (bad == 17) r.set("name",b.getStringAttr("Other_2"));
    if (bad == 18) r.set("slave",b.getI32IntegerAttr(1));
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({r.getDictionary(&ctx)}));
  }
  if (bad == 19) { NamedAttrList overlap(row); overlap.set("name",b.getStringAttr("Other_1"));
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({row,overlap.getDictionary(&ctx)})); }
  if (bad == 20) named(c,"GGControlReadDispatchWrapper")->removeAttr("goldengate.controlReadBindings");
  if (bad == 21) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),queue,"duplicateQueue"); }
  if (bad == 22) for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == empty.getName()) { i.erase(); break; }
  if (bad == 23) bank->removeAttr("goldengate.mmioRegisters");
  if (bad == 24) for (auto pad : bank.getOps<PadPrimOp>()) {
    b.setInsertionPoint(pad); pad->setOperand(0,b.create<ConstantOp>(c.getLoc(),uint(13),APInt(13,0))); }
  if (bad == 25) for (auto a : bank.getOps<AssertOp>()) { a.erase(); break; }
  if (bad == 26) for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == bank.getName()) {
    for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == i.getResult(2)) {
      b.setInsertionPoint(conn); conn->setOperand(1,b.create<ConstantOp>(c.getLoc(),uint(13),APInt(13,0))); break; } }
  if (bad == 27) {
    auto wrapper = named(c,"GGCPUStreamReadWrapper");
    for (auto i : wrapper.getOps<InstanceOp>()) if (i.getModuleName() == read.getName())
      for (auto conn : wrapper.getOps<ConnectOp>()) if (conn.getDest() == i.getResult(2)) { conn.erase(); break; }
  }
  if (bad == 28) for (auto conn : empty.getOps<StrictConnectOp>()) if (conn.getDest() == empty.getBodyBlock()->getArgument(2)) {
    b.setInsertionPoint(conn); conn->setOperand(1,b.create<ConstantOp>(c.getLoc(),uint(1),APInt(1,1))); break; }
  if (bad == 29) c->setAttr("rawAnnotations",b.getArrayAttr({other}));
  if (bad == 30) c->setAttr("rawAnnotations",b.getArrayAttr({other,output,output}));
  if (bad == 31) c->removeAttr("rawAnnotations");
  return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules; for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareCPUManagedStreamHeader(c,error)),error);
  auto after = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(before.size() == after.size(),"changed annotation count");
  unsigned changed = 0; std::string body;
  for (unsigned i = 0; i < before.size(); ++i) if (before[i] != after[i]) {
    ++changed; auto old = cast<DictionaryAttr>(before[i]), updated = cast<DictionaryAttr>(after[i]);
    body = updated.getAs<StringAttr>("body").getValue().str();
    require(StringRef(body).starts_with(old.getAs<StringAttr>("body").getValue()),"lost existing header");
    for (auto a : old) if (a.getName() != "body") require(updated.get(a.getName()) == a.getValue(),"changed output metadata");
  }
  require(changed == 1,"changed unrelated annotations"); unsigned i = 0;
  for (auto m : c.getOps<FModuleLike>()) require(modules[i++] == dump(m.getOperation()),"changed hardware");
  return body;
}
}
int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 3,"usage: CPUManagedStreamHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.disableMultithreading();
    ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    require(StringRef(body).contains("  2,\n  args,\n  std::vector<CPUManagedStreams::StreamParameters>{},"),"sink order/widget identity");
    require(StringRef(body).contains("std::string(\"trace\\\"stream\\\\domain\"), 0ULL, 2048ULL, 6144U, 64U)"),"descriptor/escaping differs");
    require(StringRef(body).contains("GET_MANAGED_STREAM_CONSTRUCTOR") && !StringRef(body).contains("_struct"),"constructor guard/substruct differs");
    std::string before = dump(*root), error;
    require(failed(goldengate::prepareCPUManagedStreamHeader(c,error)) && dump(*root) == before,"duplicate constructor mutated IR");
    for (unsigned bad = 1; bad <= 31; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareCPUManagedStreamHeader(nc,error)) && !error.empty() && dump(*negative) == before,
          "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "CPU managed stream header: live count/data forwarding, varied allocation, escaped name; 32 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error); body = run(bc);
      require(StringRef(body).contains("std::string(\"TRACERVBRIDGEMODULE_0_to_cpu_stream\"), 0ULL, 568ULL, 6144U, 64U)"),"recorded U250 descriptor differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: CPU-managed stream constructor emitted\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
