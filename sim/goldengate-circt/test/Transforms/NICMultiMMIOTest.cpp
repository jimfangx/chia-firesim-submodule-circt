// See LICENSE for license details.
// Exercise the actual indexed wrapper, including observable bank routing.
#include "goldengate/NICMMIOBank.h"
#include "goldengate/ControlAddressDecode.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok,StringRef why) {if(!ok)throw std::runtime_error(why.str());}
static FModuleOp named(CircuitOp c,StringRef n) {for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
static std::string dump(ModuleOp root) {std::string s;llvm::raw_string_ostream os(s);root.print(os);return s;}
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root=parseSourceString<ModuleOp>(R"(module {firrtl.circuit "GGNICCPUStreamResponseBufferWrapper" {
    firrtl.module @GGNICCPUStreamResponseBufferWrapper() {}
  }})",&ctx);require(bool(root),"fixture parse");
  auto c=*root->getOps<CircuitOp>().begin();auto stub=named(c,c.getName());OpBuilder b(&ctx);auto loc=c.getLoc();
  const StringRef signals[]{"macAddr","rlimit_inc","rlimit_period","rlimit_size","pauser_threshold","pauser_quanta","pauser_refresh"};
  const unsigned widths[]{48,8,8,8,16,16,16};
  auto bit=UIntType::get(&ctx,1);SmallVector<PortInfo> ports{
    {b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("hostReset"),bit,Direction::In}};
  for(unsigned i=0;i<2;++i) {
    auto prefix="nic"+std::to_string(i)+"_";
    for(unsigned j=0;j<7;++j)ports.push_back({b.getStringAttr(prefix+signals[j].str()),UIntType::get(&ctx,widths[j]),Direction::In});
    ports.push_back({b.getStringAttr(prefix+"done"),bit,Direction::Out});
    ports.push_back({b.getStringAttr("done"+std::to_string(i)+"_input"),bit,Direction::In});
    for(unsigned j=0;j<7;++j)ports.push_back({b.getStringAttr("observed"+std::to_string(i)+"_"+signals[j].str()),UIntType::get(&ctx,widths[j]),Direction::Out});
  }
  stub.erase();b.setInsertionPointToEnd(c.getBodyBlock());
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ports);
  b.setInsertionPointToStart(top.getBodyBlock());SmallVector<Attribute> annotations;
  for(unsigned i=0;i<2;++i) {
    unsigned first=2+i*16;
    for(unsigned j=0;j<7;++j)b.create<StrictConnectOp>(loc,top.getArgument(first+9+j),top.getArgument(first+j));
    b.create<StrictConnectOp>(loc,top.getArgument(first+7),top.getArgument(first+8));
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
      b.getNamedAttr("widgetClass",b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule")),
      b.getNamedAttr("target",b.getStringAttr("~"+c.getName().str()+"|"+c.getName().str()+">done"+std::to_string(i)+"_input"))}));
  }
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations));return root;
}
static void control(MLIRContext &ctx) {
  std::string error;
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICMMIOBank(c,error)),error);
    auto top = named(c,c.getName()); OpBuilder b(&ctx);
    unsigned port = 0;
    while (top.getPortName(port) != "nicBridge_1_mcr") ++port;
    if (bad == 1) { auto ns=llvm::to_vector(top.getPortNames()); ns[port]=b.getStringAttr("nicBridge_2_mcr"); top.setPortNames(ns); }
    if (bad == 2) { auto ns=llvm::to_vector(top.getPortNames()); ns[2]=b.getStringAttr("nicBridge_1_ctrl"); top.setPortNames(ns); }
    if (bad == 3) {
      auto rows=llvm::to_vector(top->getAttrOfType<ArrayAttr>("goldengate.nicMMIOBindings"));
      NamedAttrList row(cast<DictionaryAttr>(rows[1])); row.set("widget",b.getI32IntegerAttr(0)); rows[1]=row.getDictionary(&ctx);
      top->setAttr("goldengate.nicMMIOBindings",b.getArrayAttr(rows));
    }
    if (bad == 4) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGNICMCRFile_1"),top.getConventionAttr(),ArrayRef<PortInfo>{}); }
    if (bad == 5) top->removeAttr("goldengate.nicMMIOBindings");
    auto before=dump(*root); auto result=goldengate::mapNICBridgeControl(c,32,12,error);
    if (bad) { require(failed(result)&&dump(*root)==before,"late NIC control failure mutated circuit"); continue; }
    require(succeeded(result),error); require(succeeded(verify(*root)),"multi control verify");
    auto outer=named(c,"GGNICBridgeControlWrapper"), first=named(c,"GGNICBridgeControlWrapper_0");
    for (unsigned i=0;i<2;++i) {
      auto owner=i?outer:first;
      InstanceOp cr;
      for(auto inst:owner.getOps<InstanceOp>())if(inst.getName()=="crFile")cr=inst;
      require(cr&&cr.getModuleName()==("GGNICMCRFile"+(i?std::string("_1"):std::string())),"independent NIC MCR instance");
      bool found=false;
      for(auto p:outer.getPorts())found|=p.name==("nicBridge_"+std::to_string(i)+"_ctrl")&&p.direction==Direction::In;
      require(found,"missing indexed live NIC AXI");
    }
  }
}
int main(int argc,char **argv) {try {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();std::string error;
  for(unsigned mode=0;mode<4;++mode) {
    auto bad=fixture(ctx);auto c=*bad->getOps<CircuitOp>().begin();auto top=named(c,c.getName());OpBuilder b(&ctx);
    if(mode==0){auto names=llvm::to_vector(top.getPortNames());names[18]=b.getStringAttr("nic1_wrong");top.setPortNames(names);}
    if(mode==1){auto types=llvm::to_vector(top.getPortTypes());types[24]=TypeAttr::get(UIntType::get(&ctx,8));top->setAttr("portTypes",b.getArrayAttr(types));}
    if(mode==2){auto rows=llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations"));NamedAttrList last(cast<DictionaryAttr>(rows[1]));last.set("widgetConstructorKey",b.getStringAttr("unsupported"));rows[1]=last.getDictionary(&ctx);c->setAttr("rawAnnotations",b.getArrayAttr(rows));}
    if(mode==3){auto names=llvm::to_vector(top.getPortNames());names[26]=b.getStringAttr("nicBridge_1_mcr");top.setPortNames(names);}
    auto before=dump(*bad);require(failed(goldengate::addNICMMIOBank(c,error))&&dump(*bad)==before,"late widget rejection mutated circuit");
  }
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::addNICMMIOBank(c,error)),error);require(succeeded(verify(*root)),"invalid multi NIC MMIO");
  auto top=named(c,c.getName());auto binding=top->getAttrOfType<ArrayAttr>("goldengate.nicMMIOBindings");
  require(binding&&binding.size()==2,"missing MMIO widget bindings");unsigned banks=0;
  for(auto inst:top.getOps<InstanceOp>())if(inst.getModuleName()=="GGNICMMIOBank") {
    auto row=cast<DictionaryAttr>(binding[banks]);
    require(row.getAs<IntegerAttr>("widget").getInt()==banks&&row.getAs<StringAttr>("registers")==inst.getName()&&
      row.getAs<StringAttr>("mcr")=="nicBridge_"+std::to_string(banks)+"_mcr","MMIO bank identity");++banks;
  }
  require(banks==2,"MMIO instances share state");auto before=dump(*root);
  require(failed(goldengate::addNICMMIOBank(c,error))&&dump(*root)==before,"second insertion changed IR");
  control(ctx);
  if(argc==3){require(succeeded(goldengate::mapNICBridgeControl(c,25,12,error)),error);require(succeeded(verify(*root)),"emitted multi control verify");}
  if(argc>=2){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"output open");root->print(os);os<<'\n';}
  llvm::outs()<<"PASS two-widget NIC MMIO and independent AXI controls, routing, bindings and atomic rejections\n";return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
