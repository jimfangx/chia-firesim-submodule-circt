// See LICENSE for license details.
#include "goldengate/NICMMIOBank.h"
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
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module: " + name.str());
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGNICCPUStreamResponseBufferWrapper" {
      firrtl.module @GGNICCPUStreamResponseBufferWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %nic_macAddr: !firrtl.uint<48>, in %nic_rlimit_inc: !firrtl.uint<8>,
        in %nic_rlimit_period: !firrtl.uint<8>, in %nic_rlimit_size: !firrtl.uint<8>,
        in %nic_pauser_threshold: !firrtl.uint<16>, in %nic_pauser_quanta: !firrtl.uint<16>,
        in %nic_pauser_refresh: !firrtl.uint<16>, out %nic_done: !firrtl.uint<1>,
        in %other: !firrtl.uint<8>) {}
    } })", &ctx);
  require(bool(root), "fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Nested")),
      b.getNamedAttr("targets", b.getArrayAttr({
          b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper"),
          b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper|GGNICCPUStreamResponseBufferWrapper>other"),
          b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper|GGNICCPUStreamResponseBufferWrapper>nic_done") }))})}));
  std::string error; require(succeeded(goldengate::addNICMMIOBank(c,error)),error);
  return root;
}
void write(ModuleOp root, llvm::StringRef path) {
  std::error_code ec; llvm::raw_fd_ostream os(path,ec); require(!ec,"cannot open output"); root.print(os); os << '\n';
}
void check(CircuitOp c) {
  auto inner=named(c,"GGNICMMIOWrapper"), wrapper=named(c,"GGNICBridgeControlWrapper"), adapter=named(c,"GGNICMCRFile");
  require(wrapper.getNumPorts()==inner.getNumPorts(),"public port count differs");
  InstanceOp sim, cr;
  for(auto i:wrapper.getOps<InstanceOp>()) { if(i.getName()=="sim")sim=i; if(i.getName()=="crFile")cr=i; }
  require(sim&&cr,"missing production instances");
  for(unsigned p=0;p<inner.getNumPorts();++p) {
    Value dest,src;
    if(inner.getPortName(p)=="nicBridge_mcr") {
      require(wrapper.getPortName(p)=="nicBridge_ctrl"&&wrapper.getPortDirection(p)==Direction::In,"Nasti slave not public");
      dest=cr.getResult(3);src=sim.getResult(p);
    } else {
      require(wrapper.getPortName(p)==inner.getPortName(p) &&
          wrapper.getPortType(p)==inner.getPortType(p) &&
          wrapper.getPortDirection(p)==inner.getPortDirection(p),"copied port identity differs");
      bool input=inner.getPortDirection(p)==Direction::In;
      dest=input?sim.getResult(p):wrapper.getBodyBlock()->getArgument(p);
      src=input?wrapper.getBodyBlock()->getArgument(p):sim.getResult(p);
    }
    bool found=false;
    for(auto op:wrapper.getOps<ConnectOp>())found|=op.getDest()==dest&&op.getSrc()==src;
    for(auto op:wrapper.getOps<StrictConnectOp>())found|=op.getDest()==dest&&op.getSrc()==src;
    require(found,"copied port or decoded bank route differs");
  }
  require(std::distance(adapter.getOps<RegResetOp>().begin(),adapter.getOps<RegResetOp>().end())==4,"four reset transaction flags required");
  require(std::distance(adapter.getOps<RegOp>().begin(),adapter.getOps<RegOp>().end())==5,"unreset captures differ");
  for(auto r:adapter.getOps<RegOp>())if(r.getName()=="wIndex"||r.getName()=="rIndex")
    require(cast<UIntType>(r.getResult().getType()).getWidth()==3,"six words require three index bits");
  require(std::distance(adapter.getOps<AssertOp>().begin(),adapter.getOps<AssertOp>().end())==2,"AW/AR single beat assertions required");
}
// Clone the actual production helper bodies and connect them at their decoded
// boundary. Expose only register outputs and Nasti for behavioral comparison.
void standalone(CircuitOp production, llvm::StringRef path) {
  OpBuilder b(production.getContext()); auto loc=production.getLoc();
  auto root=ModuleOp::create(loc);b.setInsertionPointToEnd(root.getBody());
  auto c=b.create<CircuitOp>(loc,b.getStringAttr("GGNICControlTest"));
  auto bank=named(production,"GGNICMMIOBank"),adapter=named(production,"GGNICMCRFile");
  b.setInsertionPointToEnd(c.getBodyBlock());b.insert(bank->clone());b.insert(adapter->clone());
  auto ports=bank.getPorts();ports[0].name=b.getStringAttr("hostClock");ports[1].name=b.getStringAttr("hostReset");
  ports[10]=PortInfo(b.getStringAttr("nicBridge_ctrl"),adapter.getPortType(2),Direction::In);
  auto top=b.create<FModuleOp>(loc,b.getStringAttr("GGNICControlTest"),bank.getConventionAttr(),ports);
  b.setInsertionPointToStart(top.getBodyBlock());
  auto registers=b.create<InstanceOp>(loc,named(c,"GGNICMMIOBank"),"nicRegisters");
  auto cr=b.create<InstanceOp>(loc,named(c,"GGNICMCRFile"),"crFile");
  auto arg=[&](unsigned p){return top.getBodyBlock()->getArgument(p);};
  for(unsigned p=0;p<10;++p) {
    bool input=ports[p].direction==Direction::In;
    b.create<StrictConnectOp>(loc,input?registers.getResult(p):arg(p),input?arg(p):registers.getResult(p));
  }
  b.create<StrictConnectOp>(loc,cr.getResult(0),arg(0));b.create<StrictConnectOp>(loc,cr.getResult(1),arg(1));
  b.create<ConnectOp>(loc,cr.getResult(2),arg(10));b.create<ConnectOp>(loc,cr.getResult(3),registers.getResult(10));
  require(succeeded(verify(root)),"standalone production control verification failed");
  if(!path.empty())write(root,path);
  root.erase();
}
void reject(MLIRContext &ctx) {
  for(unsigned bad=0;bad<12;++bad) {
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGNICMMIOWrapper");
    auto bank=named(c,"GGNICMMIOBank");OpBuilder b(&ctx);
    if(bad==0)c.setName("WrongTop");
    if(bad==1)c->removeAttr("rawAnnotations");
    if(bad==2)bank->removeAttr("goldengate.mmioRegisters");
    if(bad==3)bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr({}));
    if(bad==4) { SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end()); types.back()=TypeAttr::get(UIntType::get(&ctx,32));top->setAttr("portTypes",b.getArrayAttr(types)); }
    if(bad==5) {SmallVector<bool> ds(top.getPortDirections().begin(),top.getPortDirections().end());ds.back()=!ds.back();top.setPortDirections(ds);}
    if(bad==6) {b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGNICMCRFile"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==7) {SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[2]=b.getStringAttr("nicBridge_ctrl");top.setPortNames(names);}
    if(bad==8) {b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad==9)c->setAttr("rawAnnotations",b.getArrayAttr({b.getStringAttr("~GGNICMMIOWrapper|GGNICMMIOWrapper>nicBridge_mcr.read[5].bits")}));
    std::string before,after,error; {llvm::raw_string_ostream os(before);root->print(os);}
    require(failed(goldengate::mapNICBridgeControl(c,bad==10?4:25,bad==11?0:12,error)),"invalid NIC Nasti boundary accepted");
    {llvm::raw_string_ostream os(after);root->print(os);}require(before==after,"rejection mutated IR");
  }
}
}
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root=argc==4?parseSourceFile<ModuleOp>(argv[1],&ctx):fixture(ctx);
    require(bool(root),"input parse failed");auto c=*root->getOps<CircuitOp>().begin();std::string error;
    require(succeeded(goldengate::mapNICBridgeControl(c,25,12,error)),error);
    require(succeeded(verify(*root)),"NIC Nasti verification failed");check(c);
    if(argc==4){write(*root,argv[2]);standalone(c,argv[3]);}
    else {
      standalone(c,{});
      auto targets=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
      require(cast<StringAttr>(targets[0])=="~GGNICBridgeControlWrapper","circuit identity differs");
      require(cast<StringAttr>(targets[1])=="~GGNICBridgeControlWrapper|GGNICBridgeControlWrapper>other","copied identity differs");
      require(cast<StringAttr>(targets[2])=="~GGNICBridgeControlWrapper|GGNICCPUStreamResponseBufferWrapper>nic_done","consumed done identity differs");
      reject(ctx);
    }
    llvm::outs()<<"NIC Nasti: registry-sized transport and production routes passed";
    if(argc!=4)llvm::outs()<<", identities and 12 atomic rejections passed";
    llvm::outs()<<'\n';return 0;
  } catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
