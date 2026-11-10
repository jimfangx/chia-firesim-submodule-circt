// See LICENSE for license details.
#include "NICTokenQueuesFixture.h"
#include "goldengate/NICCPUStreamQueues.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/NICMMIOBank.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/NICHeader.h"
static FModuleOp named(CircuitOp c,StringRef n) {
  for(auto m:c.getOps<FModuleOp>()) if(m.getName()==n)return m;
  throw std::runtime_error("missing module "+n.str());
}
static std::string printed(ModuleOp m) {std::string s;llvm::raw_string_ostream o(s);m.print(o);return s;}
int main(int argc,char **argv) {
 try {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();std::string error;
  auto check=[&](LogicalResult result){if(failed(result))throw std::runtime_error(error);};
  auto root=fixture(ctx,true,1,2,true);auto c=*root->getOps<CircuitOp>().begin();
  check(goldengate::addNICLoopbackTokenQueues(c,error));
  for(unsigned bad=0;bad<6;++bad) {
    OwningOpRef<ModuleOp> copy(cast<ModuleOp>(root->clone()));auto cc=*copy->getOps<CircuitOp>().begin();auto top=named(cc,cc.getName());OpBuilder b(&ctx);
    if(bad==0)cc.setName("GGNICPacketAdaptersWrapper");
    if(bad==1)top->removeAttr("goldengate.nicTokenBindings");
    if(bad==2)top->setAttr("goldengate.nicTokenBindings",b.getArrayAttr({b.getStringAttr("malformed"),b.getStringAttr("malformed")}));
    if(bad==3) {
      SmallVector<Attribute> records;for(auto a:top->getAttrOfType<ArrayAttr>("goldengate.nicTokenBindings")) {
        NamedAttrList d(cast<DictionaryAttr>(a));d.set("mode",b.getStringAttr("normal"));records.push_back(d.getDictionary(&ctx));
      }top->setAttr("goldengate.nicTokenBindings",b.getArrayAttr(records));
    }
    if(bad==4) {SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[1]=b.getStringAttr("missingReset");top.setPortNames(names);}
    if(bad==5) {b.setInsertionPointToEnd(cc.getBodyBlock());b.create<FModuleOp>(cc.getLoc(),b.getStringAttr("GGNICCPUStreamQueue3072"),top.getConventionAttr(),SmallVector<PortInfo>{});}
    auto before=printed(*copy);require(failed(goldengate::addNICLoopbackCPUStreamQueues(cc,error)),"malformed boundary accepted");
    require(before==printed(*copy),"failed preflight changed IR");
  }
  check(goldengate::addNICLoopbackCPUStreamQueues(c,error));
  require(succeeded(verify(*root)),"invalid CPU stream composition");
  auto top=named(c,c.getName());unsigned queues=0;OpBuilder b(&ctx);
  for(auto i:top.getOps<InstanceOp>()) if(i.getModuleName()=="GGNICCPUStreamQueue3072") {
    ++queues;auto info=i->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");require(bool(info),"missing stream identity");
    bool incoming=info.getAs<StringAttr>("direction")=="fromHostCPU";
    Value disabled;
    for(auto f:top.getOps<SubfieldOp>())if(f.getInput()==i.getResult(incoming?3:2)&&f.getFieldName()==(incoming?"ready":"valid"))disabled=f;
    require(bool(disabled),"disabled endpoint missing");bool zero=false;
    for(auto con:top.getOps<StrictConnectOp>())if(con.getDest()==disabled) {
      auto val=con.getSrc().getDefiningOp<ConstantOp>();zero=val&&val.getValue().isZero();
    }require(zero,"loopback must disable its CPU token adapters");
  }
  require(queues==4,"two independent directional queues per widget required");
  SmallVector<goldengate::CPUStreamSourcePort> sources,sinks;SmallVector<goldengate::CPUStreamCountPort> counts;
  check(goldengate::deriveCPUStreamPorts(c,sources,sinks,counts,error));
  require(sources.size()==2&&sinks.size()==2&&counts.size()==4,"disabled streams must retain complete catalog allocation");
  for(unsigned w=0;w<2;++w) {
    require(sources[w].streamName=="SIMPLENICBRIDGEMODULE_"+std::to_string(w)+"_to_cpu_stream"&&sources[w].depth==3072,"source identity/depth");
    require(sinks[w].streamName=="SIMPLENICBRIDGEMODULE_"+std::to_string(w)+"_from_cpu_stream"&&sinks[w].depth==3072,"sink identity/depth");
  }
  auto before=printed(*root);require(failed(goldengate::addNICLoopbackCPUStreamQueues(c,error))&&before==printed(*root),"duplicate stream allocation mutated IR");
  check(goldengate::addNICLoopbackMMIOBank(c,error));
  check(goldengate::mapNICBridgeControl(c,32,12,error));
  require(succeeded(verify(*root)),"combined loopback token/CPU/MMIO invalid");
  check(goldengate::deriveCPUStreamPorts(c,sources,sinks,counts,error));
  std::string header="unchanged"; auto hardware=printed(*root);
  check(goldengate::emitNICLoopbackHeader(c,header,error));
  require(header.find("simplenic_loopback_t(simif, 0, args)")!=std::string::npos&&
      header.find("simplenic_loopback_t(simif, 1, args)")!=std::string::npos&&
      header.find("new simplenic_t(")==std::string::npos,"loopback driver mode/index mismatch");
  require(hardware==printed(*root),"loopback header emission changed hardware or annotations");
  for(unsigned bad=0;bad<10;++bad) {
    OwningOpRef<ModuleOp> copy(cast<ModuleOp>(root->clone()));auto cc=*copy->getOps<CircuitOp>().begin();OpBuilder builder(&ctx);
    if(bad==0)named(cc,"GGNICLoopbackTokenQueuesWrapper")->removeAttr("goldengate.nicTokenBindings");
    if(bad==1)named(cc,"GGNICMMIOBank")->removeAttr("goldengate.mmioRegisters");
    if(bad==2)for(auto r:named(cc,"GGNICMMIOBank").getOps<RegOp>())r.setName("wrong");
    if(bad==3)for(auto con:named(cc,"GGNICMMIOBank").getOps<StrictConnectOp>())if(isa<RegOp>(con.getDest().getDefiningOp())){con.erase();break;}
    auto q=named(cc,"GGNICCPUStreamQueuesWrapper");
    if(bad==4)for(auto i:q.getOps<InstanceOp>())if(i->hasAttr("goldengate.streamParameters")){i->removeAttr("goldengate.streamParameters");break;}
    if(bad==5)for(auto i:q.getOps<InstanceOp>())if(auto info=i->getAttrOfType<DictionaryAttr>("goldengate.streamParameters")){
      NamedAttrList row(info);row.set("name",builder.getStringAttr("wrong"));i->setAttr("goldengate.streamParameters",row.getDictionary(&ctx));break;}
    if(bad==6)for(auto con:q.getOps<StrictConnectOp>())if(auto zero=con.getSrc().getDefiningOp<ConstantOp>())if(zero.getValue().isZero()){
      builder.setInsertionPoint(con);con->setOperand(1,builder.create<ConstantOp>(cc.getLoc(),UIntType::get(&ctx,1,false),APInt(1,1)));break;}
    if(bad==7)named(cc,"GGNICMMIOWrapper")->removeAttr("goldengate.nicMMIOBindings");
    if(bad==8)for(auto m:named(cc,"GGNICCPUStreamQueue3072").getOps<MemOp>())m.setDepthAttr(builder.getI64IntegerAttr(3071));
    if(bad==9)for(auto i:q.getOps<InstanceOp>())if(auto info=i->getAttrOfType<DictionaryAttr>("goldengate.streamParameters")){
      NamedAttrList row(info);row.set("direction",builder.getStringAttr("wrong"));i->setAttr("goldengate.streamParameters",row.getDictionary(&ctx));break;}
    auto before=printed(*copy);std::string output="sentinel";
    require(failed(goldengate::emitNICLoopbackHeader(cc,output,error)),("invalid loopback driver accepted: "+std::to_string(bad)).c_str());
    require(output=="sentinel"&&before==printed(*copy),"failed loopback driver preflight changed output/IR");
  }
  if(argc==2){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"output open");root->print(os);os<<'\n';}
  llvm::outs()<<"PASS loopback CPU queue allocation, disabled adapter endpoints, live catalog, singleton MMIO composition and six atomic rejections\n";return 0;
 }catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}
}
