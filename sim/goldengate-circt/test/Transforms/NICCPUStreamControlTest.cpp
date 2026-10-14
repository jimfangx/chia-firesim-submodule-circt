// See LICENSE for license details.
#include "goldengate/CPUStreamCountBank.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef msg) { if (!ok) throw std::runtime_error(msg.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m:c.getOps<FModuleOp>()) if(m.getName()==name)return m;
  throw std::runtime_error("missing module: "+name.str());
}
std::string dump(Operation *op) {std::string s;llvm::raw_string_ostream os(s);op->print(os);return s;}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "Counts" {
    firrtl.module @Counts(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %nicOut: !firrtl.uint<12>, out %trace: !firrtl.uint<13>, out %nicIn: !firrtl.uint<12>) {}
  } })",&ctx);
  require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  c->setAttr("rawAnnotations",b.getArrayAttr({}));std::string error;
  require(succeeded(goldengate::addCPUStreamCountBank(c,{
    {"SIMPLENICBRIDGEMODULE_0_to_cpu_stream","nicOut",12},
    {"TRACERVBRIDGEMODULE_0_to_cpu_stream","trace",13},
    {"SIMPLENICBRIDGEMODULE_0_from_cpu_stream","nicIn",12}},error)),error);
  // The count boundary survives intervening NIC/CPU wrappers in production.
  auto top=named(c,"GGCPUStreamCountWrapper");top.setName("GGNICBridgeControlWrapper");c.setName(top.getName());
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGNICBridgeControlWrapper"),
      b.getStringAttr("~GGNICBridgeControlWrapper|GGNICBridgeControlWrapper>hostReset"),
      b.getStringAttr("~GGNICBridgeControlWrapper|GGCPUStreamCountBank>count_0")}))})}));return root;
}
void write(ModuleOp root,llvm::StringRef path){std::error_code ec;llvm::raw_fd_ostream os(path,ec);require(!ec,"output open failed");root.print(os);os<<'\n';}
void standalone(CircuitOp production,llvm::StringRef path) {
  OpBuilder b(production.getContext());auto loc=production.getLoc();auto root=ModuleOp::create(loc);
  b.setInsertionPointToEnd(root.getBody());auto c=b.create<CircuitOp>(loc,b.getStringAttr("GGNICCPUControlTest"));
  auto bank=named(production,"GGCPUStreamCountBank"),adapter=named(production,"GGCPUStreamMCRFile");
  b.setInsertionPointToEnd(c.getBodyBlock());b.insert(bank->clone());b.insert(adapter->clone());
  auto ps=bank.getPorts();ps[0].name=b.getStringAttr("hostClock");ps[1].name=b.getStringAttr("hostReset");
  ps.back()=PortInfo(b.getStringAttr("cpuStream_ctrl"),adapter.getPortType(2),Direction::In);
  auto top=b.create<FModuleOp>(loc,b.getStringAttr("GGNICCPUControlTest"),bank.getConventionAttr(),ps);
  b.setInsertionPointToStart(top.getBodyBlock());auto regs=b.create<InstanceOp>(loc,named(c,"GGCPUStreamCountBank"),"counts");
  auto cr=b.create<InstanceOp>(loc,named(c,"GGCPUStreamMCRFile"),"crFile");
  for(unsigned p=0;p+1<ps.size();++p)b.create<StrictConnectOp>(loc,regs.getResult(p),top.getArgument(p));
  b.create<StrictConnectOp>(loc,cr.getResult(0),top.getArgument(0));b.create<StrictConnectOp>(loc,cr.getResult(1),top.getArgument(1));
  b.create<ConnectOp>(loc,cr.getResult(2),top.getArguments().back());b.create<ConnectOp>(loc,cr.getResult(3),regs.getResults().back());
  require(succeeded(verify(root)),"standalone verification failed");if(!path.empty())write(root,path);root.erase();
}
void rejected(MLIRContext &ctx) {
  for(unsigned mode=0;mode<11;++mode){auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    auto bank=named(c,"GGCPUStreamCountBank"),top=named(c,"GGNICBridgeControlWrapper");OpBuilder b(&ctx);
    if(mode==0)bank->removeAttr("goldengate.mmioRegisters");
    if(mode==1){auto rows=llvm::to_vector(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));rows.pop_back();bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));}
    if(mode==2)c.setName("Wrong");
    if(mode==3)c->removeAttr("rawAnnotations");
    if(mode==4){SmallVector<bool> ds(bank.getPortDirections().begin(),bank.getPortDirections().end());ds.back()=!ds.back();bank.setPortDirections(ds);}
    if(mode==5){auto ts=llvm::to_vector(top.getPortTypes());ts.back()=TypeAttr::get(UIntType::get(&ctx,32));top->setAttr("portTypes",b.getArrayAttr(ts));}
    if(mode==6)c->setAttr("rawAnnotations",b.getArrayAttr({b.getStringAttr("~GGNICBridgeControlWrapper|GGNICBridgeControlWrapper>cpuStream_mcr.read[2].bits")}));
    if(mode==7){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGCPUStreamMCRFile"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    auto before=dump(*root);std::string error;
    require(failed(goldengate::attachCPUStreamControl(c,"GGNICBridgeControlWrapper",mode==8?"":"GGNICCPUStreamControlWrapper",mode==9?3:25,mode==10?0:12,error)),"invalid boundary accepted");
    require(before==dump(*root),"rejection mutated IR");
  }
}
}
int main(int argc,char **argv){try {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  auto root=argc==4?parseSourceFile<ModuleOp>(argv[1],&ctx):fixture(ctx);require(bool(root),"input parse failed");
  auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::attachCPUStreamControl(c,"GGNICBridgeControlWrapper","GGNICCPUStreamControlWrapper",25,12,error)),error);
  require(succeeded(verify(*root)),"production verification failed");
  auto adapter=named(c,"GGCPUStreamMCRFile");unsigned indices=0;
  for(auto r:adapter.getOps<RegOp>())if(r.getName()=="wIndex"||r.getName()=="rIndex"){
    require(cast<UIntType>(r.getResult().getType()).getWidth()==2,"three-word index differs");++indices;}
  require(indices==2,"missing indices");
  if(argc==4){write(*root,argv[2]);standalone(c,argv[3]);}else{
    auto targets=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
    require(cast<StringAttr>(targets[0])=="~GGNICCPUStreamControlWrapper","circuit transfer differs");
    require(cast<StringAttr>(targets[1])=="~GGNICCPUStreamControlWrapper|GGNICCPUStreamControlWrapper>hostReset","copied target differs");
    require(cast<StringAttr>(targets[2])=="~GGNICCPUStreamControlWrapper|GGCPUStreamCountBank>count_0","internal target differs");
    standalone(c,{});rejected(ctx);
  }
  llvm::outs()<<"PASS NIC CPU stream registry-sized control and production cloning";
  if(argc!=4)llvm::outs()<<", annotation identities and 11 atomic rejection cases";
  llvm::outs()<<'\n';return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
