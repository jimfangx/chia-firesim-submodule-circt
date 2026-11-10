// See LICENSE for license details.
#include "NICTokenQueuesFixture.h"
#include "goldengate/NICMMIOBank.h"
#include "mlir/Parser/Parser.h"
FModuleOp named(CircuitOp c,StringRef name) {
  for(auto m:c.getOps<FModuleOp>())if(m.getName()==name)return m;
  throw std::runtime_error("module missing: "+name.str());
}
void dump(ModuleOp root,StringRef path) {
  std::error_code ec;llvm::raw_fd_ostream os(path,ec);require(!ec,"output open");root.print(os);os<<'\n';
}
// Extract the emitted production banks/adapters, changing only the previously
// checked tFire status boundary to scalar inputs for exact MMIO comparison.
void standalone(CircuitOp production,StringRef path) {
  OpBuilder b(production.getContext());auto loc=production.getLoc();
  auto root=ModuleOp::create(loc);b.setInsertionPointToEnd(root.getBody());
  auto c=b.create<CircuitOp>(loc,b.getStringAttr("GGNICLoopbackControlTest"));
  b.setInsertionPointToEnd(c.getBodyBlock());
  for(auto name:{"GGNICMMIOBank","GGNICMCRFile","GGNICMCRFile_1"})b.insert(named(production,name)->clone());
  auto bank=named(c,"GGNICMMIOBank");auto ctrl=named(c,"GGNICMCRFile").getPortType(2);
  SmallVector<PortInfo> ports{
    {b.getStringAttr("hostClock"),ClockType::get(b.getContext()),Direction::In},
    {b.getStringAttr("hostReset"),UIntType::get(b.getContext(),1,false),Direction::In}};
  for(unsigned i=0;i<2;++i) {
    ports.push_back({b.getStringAttr("done"+std::to_string(i)+"_input"),bank.getPortType(2),Direction::In});
    ports.push_back({b.getStringAttr("nicBridge_"+std::to_string(i)+"_ctrl"),ctrl,Direction::In});
  }
  auto top=b.create<FModuleOp>(loc,b.getStringAttr("GGNICLoopbackControlTest"),bank.getConventionAttr(),ports);
  b.setInsertionPointToStart(top.getBodyBlock());
  for(unsigned i=0;i<2;++i) {
    auto regs=b.create<InstanceOp>(loc,bank,"regs"+std::to_string(i));
    auto cr=b.create<InstanceOp>(loc,named(c,i?"GGNICMCRFile_1":"GGNICMCRFile"),"cr"+std::to_string(i));
    for(unsigned p=0;p<2;++p) {
      b.create<StrictConnectOp>(loc,regs.getResult(p),top.getArgument(p));
      b.create<StrictConnectOp>(loc,cr.getResult(p),top.getArgument(p));
    }
    b.create<StrictConnectOp>(loc,regs.getResult(2),top.getArgument(2+2*i));
    b.create<ConnectOp>(loc,cr.getResult(2),top.getArgument(3+2*i));
    b.create<ConnectOp>(loc,cr.getResult(3),regs.getResult(3));
  }
  require(succeeded(verify(root)),"standalone verify");if(!path.empty())dump(root,path);root.erase();
}
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();std::string error;
    auto root=argc==4?parseSourceFile<ModuleOp>(argv[1],&ctx):fixture(ctx,true,1,2,true);
    require(bool(root),"parse input");auto c=*root->getOps<CircuitOp>().begin();
    if(argc!=4) {
      require(succeeded(goldengate::addNICLoopbackTokenQueues(c,error)),error.c_str());
      // All malformed late bindings and ports must fail without partial edits.
      for(unsigned bad=0;bad<5;++bad) {
        OwningOpRef<ModuleOp> copy(cast<ModuleOp>(root->clone()));auto cc=*copy->getOps<CircuitOp>().begin();
        auto top=named(cc,cc.getName());OpBuilder b(&ctx);
        if(bad==0)cc.setName("GGNICCPUStreamResponseBufferWrapper");
        if(bad==1)top->removeAttr("goldengate.nicTokenBindings");
        if(bad==2)top->setAttr("goldengate.nicTokenBindings",b.getArrayAttr({}));
        if(bad==3) {SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names.back()=b.getStringAttr("missing_done");top.setPortNames(names);}
        if(bad==4) {SmallVector<bool> ds(top.getPortDirections().begin(),top.getPortDirections().end());ds.back()=!ds.back();top.setPortDirections(ds);}
        std::string before,after;{llvm::raw_string_ostream s(before);copy->print(s);}
        require(failed(goldengate::addNICLoopbackMMIOBank(cc,error)),"malformed loopback bank accepted");
        {llvm::raw_string_ostream s(after);copy->print(s);}require(before==after,"failed preflight mutated IR");
      }
      require(succeeded(goldengate::addNICLoopbackMMIOBank(c,error)),error.c_str());
      require(succeeded(goldengate::mapNICBridgeControl(c,32,12,error)),error.c_str());
    }
    require(succeeded(verify(*root)),"loopback control verify");
    auto wrapper=named(c,"GGNICMMIOWrapper");InstanceOp sim;
    for(auto i:wrapper.getOps<InstanceOp>())if(i.getName()=="sim")sim=i;
    require(bool(sim),"token path must remain live");
    for(unsigned widget=0;widget<2;++widget) {
      InstanceOp regs;for(auto i:wrapper.getOps<InstanceOp>())
        if(i.getName()=="nicRegisters_"+std::to_string(widget))regs=i;
      require(bool(regs),"independent status banks required");
      auto tokenTop=named(c,"GGNICLoopbackTokenQueuesWrapper");
      std::optional<unsigned> done;
      for(auto [i,p]:llvm::enumerate(tokenTop.getPorts()))
        if(p.name=="nic"+std::to_string(widget)+"_done")done=i;
      require(bool(done),"per-widget tFire status must exist");bool routed=false;
      for(auto op:wrapper.getOps<StrictConnectOp>())
        routed|=op.getDest()==regs.getResult(2)&&op.getSrc()==sim.getResult(*done);
      require(routed,"done register must sample its own live token predicate");
    }
    auto bank=named(c,"GGNICMMIOBank");require(bank.getNumPorts()==4,"one status plus MCR ABI");
    auto rows=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    require(rows&&rows.size()==1,"singleton registry");auto row=cast<DictionaryAttr>(rows[0]);
    require(row.getAs<StringAttr>("name")=="done"&&row.getAs<IntegerAttr>("offset").getInt()==0&&row.getAs<BoolAttr>("readable").getValue()&&row.getAs<BoolAttr>("writeable").getValue(),"SFC genROReg remains writable");
    require(std::distance(bank.getOps<RegOp>().begin(),bank.getOps<RegOp>().end())==1&&bank.getOps<RegResetOp>().empty(),"done must sample without reset");
    require((*bank.getOps<RegOp>().begin()).getName()=="done","done register identity");
    for(auto name:{"GGNICMCRFile","GGNICMCRFile_1"}) {
      auto m=named(c,name);require(std::distance(m.getOps<RegResetOp>().begin(),m.getOps<RegResetOp>().end())==4,"independent reset transaction flags");
      require(std::distance(m.getOps<RegOp>().begin(),m.getOps<RegOp>().end())==3,"singleton bank must not introduce index registers");
    }
    if(argc==4) {dump(*root,argv[2]);standalone(c,argv[3]);}
    else if(argc==3) {dump(*root,argv[1]);standalone(c,argv[2]);}
    else standalone(c,{});
    llvm::outs()<<"PASS two compile-time loopback singleton done banks, independent F2 AXI transports and atomic rejections\n";return 0;
  }catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}
}
