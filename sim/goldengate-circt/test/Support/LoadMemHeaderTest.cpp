// See LICENSE for license details.
#include "goldengate/LoadMemHeader.h"
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/LoadMemWriter.h"
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
// Generate all six actual LoadMem transformations, then attach an independent
// allocation graph. This exercises the emitter against actual FIRRTL state.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGCPUStreamWriteResponseBufferWrapper" {
      firrtl.module @GGCPUStreamWriteResponseBufferWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %cpu_stream_b_valid: !firrtl.uint<1>, in %other: !firrtl.uint<8>) {}
    } })", &ctx);
  require(bool(root),"LoadMem fixture parse"); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  c->setAttr("rawAnnotations",b.getArrayAttr({})); std::string error;
  for (auto transform : {goldengate::addLoadMemWriter,goldengate::addLoadMemRequests,
      goldengate::addLoadMemWriteMMIO,goldengate::addLoadMemWriteData,
      goldengate::addLoadMemReadRequests,goldengate::addLoadMemReadData})
    require(succeeded(transform(c,error)),error);
  auto top=named(c,"GGLoadMemReadDataWrapper"); auto loc=c.getLoc();
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder=b.create<FModuleOp>(loc,b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{
      {b.getStringAttr("aw_addr"),UIntType::get(&ctx,25,false),Direction::In}});
  auto row=b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(bad==1?"Other_2":"LoadMemWidget_2")),
    b.getNamedAttr("slave",b.getI32IntegerAttr(0)),
    b.getNamedAttr("start",b.getI64IntegerAttr(bad==2?1025:bad==3?(1<<25)-32:1024)),
    b.getNamedAttr("size",b.getI64IntegerAttr(bad==4?32:64))});
  auto other=b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("Other_0")),
    b.getNamedAttr("slave",b.getI32IntegerAttr(1)),b.getNamedAttr("start",b.getI64IntegerAttr(1056)),
    b.getNamedAttr("size",b.getI64IntegerAttr(64))});
  decoder->setAttr("goldengate.controlRegions",bad==5?b.getArrayAttr({row,other}):b.getArrayAttr({row}));
  auto binding=b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("LoadMemWidget_2")),
    b.getNamedAttr("port",b.getStringAttr("loadmem_ctrl")),b.getNamedAttr("slave",b.getI32IntegerAttr(bad==6?1:0))});
  auto leaf = [&](StringRef name, unsigned width) {
    return BundleType::get(&ctx,{{b.getStringAttr("bits"),false,
      BundleType::get(&ctx,{{b.getStringAttr(name),false,UIntType::get(&ctx,width,false)}})}});
  };
  auto memory=BundleType::get(&ctx,{{b.getStringAttr("w"),false,leaf("data",bad==25?128:64)},
    {b.getStringAttr("r"),true,leaf("data",64)}, {b.getStringAttr("aw"),false,leaf("addr",34)},
    {b.getStringAttr("ar"),false,leaf("addr",34)}});
  auto platform=b.create<FModuleOp>(loc,b.getStringAttr("FPGATop"),ConventionAttr::get(&ctx,Convention::Internal),
    ArrayRef<PortInfo>{{b.getStringAttr("mem_0"),memory,Direction::Out}});
  SmallVector<FModuleOp> helpers{decoder};
  if (bad!=26) helpers.push_back(platform);

  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper","goldengate.controlWriteBindings"),
                   std::make_pair("GGControlReadDispatchWrapper","goldengate.controlReadBindings")}) {
    auto m=b.create<FModuleOp>(loc,b.getStringAttr(pair.first),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    m->setAttr(pair.second,bad==7 && StringRef(pair.first)=="GGControlReadDispatchWrapper"?
      b.getArrayAttr({}):bad==8?b.getArrayAttr({binding,binding}):b.getArrayAttr({binding})); helpers.push_back(m);
  }
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) b.create<InstanceOp>(loc,m,m.getName());
  auto bank=named(c,"GGLoadMemWriteMMIOBank");
  if (bad==9) b.create<InstanceOp>(loc,bank,"duplicateBank");
  if (bad==10) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    b.create<FModuleOp>(loc,b.getStringAttr("DisconnectedTop"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    c.setNameAttr(b.getStringAttr("DisconnectedTop"));
  }
  if (bad==11) bank->removeAttr("goldengate.mmioRegisters");
  if (bad==12 || bad==13 || bad==14) {
    auto regs=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> changed(regs.begin(),regs.end()); NamedAttrList d(cast<DictionaryAttr>(changed[0]));
    if (bad==12) d.set("offset",b.getI32IntegerAttr(4));
    if (bad==13) d.set("readable",b.getBoolAttr(false));
    if (bad==14) d.erase("name");
    changed[0]=d.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(changed));
  }
  if (bad==15) for (auto r : bank.getOps<RegOp>()) if (r.getName()=="W_ADDRESS_H") r.setNameAttr(b.getStringAttr("wrong_high"));
  if (bad==16) for (auto r : named(c,"GGLoadMemReadRequests").getOps<RegOp>()) r.setNameAttr(b.getStringAttr("wrong_read_high"));
  auto pack=named(c,"GGLoadMemWriteDataFIFO"), unpack=named(c,"GGLoadMemReadDataFIFO");
  if (bad==17) pack->setAttr("goldengate.inputWidth",b.getI32IntegerAttr(64));
  if (bad==18) unpack->setAttr("goldengate.outputWidth",b.getI32IntegerAttr(64));
  if (bad==19) {
    SmallVector<Attribute> types(pack.getPortTypes().begin(),pack.getPortTypes().end());
    types[7]=TypeAttr::get(UIntType::get(&ctx,128,false)); pack.setPortTypes(types);
  }
  if (bad==20) b.create<InstanceOp>(loc,pack,"duplicateFIFO");
  if (bad==21) for (auto mux : bank.getOps<MuxPrimOp>()) mux->setOperand(0,bank.getBodyBlock()->getArgument(1));
  if (bad==22) {
    auto m=named(c,"GGLoadMemWriteDataWrapper");
    for (auto i : m.getOps<InstanceOp>()) if (i.getModuleName()==pack.getName())
      for (auto conn : m.getOps<StrictConnectOp>()) if (conn.getDest()==i.getResult(3)) conn->setOperand(1,m.getBodyBlock()->getArgument(1));
  }
  auto output=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("// retained header\n"))});
  SmallVector<Attribute> annotations{b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Retained"))})};
  if (bad!=23) annotations.push_back(output); if (bad==24) annotations.push_back(output);
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules;
  for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareLoadMemHeader(c, error)), error);
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
    require(argc==1 || argc==3,"usage: LoadMemHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root=fixture(ctx); auto c=*root->getOps<CircuitOp>().begin(); auto body=run(c);
    const char *fields[]{"W_ADDRESS_H","W_ADDRESS_L","W_LENGTH","ZERO_OUT_DRAM","W_DATA",
                         "ZERO_FINISHED","R_ADDRESS_H","R_ADDRESS_L","R_DATA"};
    for (unsigned i=0;i<9;++i) require(StringRef(body).contains("."+std::string(fields[i])+" = "+std::to_string(1024+i*4)),"wrong address order");
    require(StringRef(body).contains("},\n  2,\n  args,\n  conf_target.mem,\n  2U") &&
      StringRef(body).contains("#ifdef GET_CORE_CONSTRUCTOR\nregistry.add_widget(new loadmem_t("),"wrong core constructor/index/chunk count");
    auto before=dump(*root); std::string error;
    require(failed(goldengate::prepareLoadMemHeader(c,error)) && dump(*root)==before,"duplicate emission changed IR");
    for (unsigned bad=1;bad<=26;++bad) {
      auto negative=fixture(ctx,bad); auto nc=*negative->getOps<CircuitOp>().begin(); before=dump(*negative);
      require(failed(goldengate::prepareLoadMemHeader(nc,error)) && !error.empty() && dump(*negative)==before,
              "invalid boundary accepted or mutated: "+std::to_string(bad));
    }
    llvm::outs()<<"LoadMem header: actual six-pass fixture, varied allocation/index, preserved IR/annotations; 27 atomic rejections passed\n";
    if (argc==3) {
      auto boundary=parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc=*boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareResetPulseHeader(bc,error)),error); body=run(bc);
      for (unsigned i=0;i<9;++i) require(StringRef(body).contains("."+std::string(fields[i])+" = "+std::to_string(384+i*4)),"recorded U250 allocation differs");
      require(StringRef(body).contains("},\n  0,\n  args,\n  conf_target.mem,\n  2U"),"recorded U250 constructor differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs()<<"Recorded U250 boundary: LoadMem constructor composed with metasim, master, clock and reset headers\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs()<<e.what()<<'\n';return 1; }
}
