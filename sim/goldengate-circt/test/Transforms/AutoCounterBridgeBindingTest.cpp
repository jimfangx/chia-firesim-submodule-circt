// See LICENSE for license details.
#include "goldengate/AutoCounterBridge.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include <functional>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool b,const std::string &s){if(!b)throw std::runtime_error(s);}
std::string dump(Operation *op){std::string s;llvm::raw_string_ostream out(s);op->print(out);return s;}
struct Fixture {
  OwningOpRef<ModuleOp> root; CircuitOp circuit; SmallVector<FModuleOp> hosts;
  Fixture(MLIRContext &ctx,StringRef path,unsigned width=32) {
    llvm::SourceMgr manager;
    for(auto file:{path.str()+".fir",path.str()+".anno.json"}) {
      auto buffer=llvm::MemoryBuffer::getFile(file);require(bool(buffer),"missing fixture "+file);
      manager.AddNewSourceBuffer(std::move(*buffer),llvm::SMLoc());
    }
    DefaultTimingManager timer;auto scope=timer.getRootScope();FIRParserOptions options;options.numAnnotationFiles=1;
    root=importFIRFile(manager,&ctx,scope,options);require(bool(root),"fixture import");
    circuit=*root->getOps<CircuitOp>().begin();std::string error;
    require(succeeded(goldengate::materializeAutoCounterBridgeHosts(circuit,width,12,hosts,error)),error);
    require(hosts.size()==2,"fixture constructor count");
  }
};
void rewrite(Fixture &f,unsigned index,StringRef field,Attribute value) {
  auto raw=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> attrs(raw.begin(),raw.end());
  NamedAttrList d(cast<DictionaryAttr>(attrs[index]));d.set(field,value);attrs[index]=d.getDictionary(f.circuit.getContext());
  f.circuit->setAttr("rawAnnotations",ArrayAttr::get(f.circuit.getContext(),attrs));
}
void positive(MLIRContext &ctx,StringRef path,unsigned width,const char *output) {
  Fixture f(ctx,path,width);std::string error;auto oldTop=*f.circuit.getOps<FModuleOp>().begin();auto arg=oldTop.getArgument(0);
  require(succeeded(goldengate::bindAutoCounterBridgeHosts(f.circuit,f.hosts,error)),error);
  require(succeeded(verify(*f.root)),"bound circuit verification");
  require(f.circuit.getName()=="GGAutoCounterBridgeHostWrapper"&&oldTop.getArgument(0)==arg,"top transition or SSA preservation");
  FModuleOp wrapper;for(auto m:f.circuit.getOps<FModuleOp>())if(m.getName()==f.circuit.getName())wrapper=m;
  require(bool(wrapper),"wrapper missing");auto rows=wrapper->getAttrOfType<ArrayAttr>("goldengate.autocounterHostBindings");
  require(rows&&rows.size()==2&&wrapper.getPortName(wrapper.getNumPorts()-2)=="autocounter_0_ctrl"&&wrapper.getPortName(wrapper.getNumPorts()-1)=="autocounter_1_ctrl","indexed control order");
  for(unsigned i=0;i<2;++i){auto row=cast<DictionaryAttr>(rows[i]);auto clock=row.getAs<DictionaryAttr>("clockInfo");require(clock.getAs<IntegerAttr>("divisor").getInt()==(i?3:1),"clock ownership lost");}
  unsigned instances=0;for(auto i:wrapper.getOps<InstanceOp>())++instances;require(instances==3,"wrapper must instantiate target + both hosts");
  unsigned bridges=0,channels=0;
  for(auto a:f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto d=cast<DictionaryAttr>(a);auto c=d.getAs<StringAttr>("class");
    if(c.getValue()=="firesim.lib.bridgeutils.BridgeIOAnnotation") {++bridges;require(d.getAs<StringAttr>("target").getValue().contains(">hBits"),"bridge target not host-bound");}
    else if(c.getValue()=="midas.passes.fame.FAMEChannelConnectionAnnotation") {++channels;auto sink=d.getAs<ArrayAttr>("sinks");require(sink&&sink.size()==1&&cast<StringAttr>(sink[0]).getValue().contains(">hBits."),"channel sink missing");}
  }
  require(bridges==2&&channels==8,"retained annotation inventory");
  SmallVector<FModuleOp> discovered;
  require(succeeded(goldengate::discoverBoundAutoCounterBridgeHosts(f.circuit, discovered, error)) &&
      discovered == f.hosts, "production discovery changed constructor order");
  auto registryBefore = wrapper->getAttr("goldengate.autocounterHostBindings");
  Builder registryBuilder(&ctx);
  wrapper->setAttr("goldengate.autocounterHostBindings", registryBuilder.getArrayAttr({rows[1], rows[0]}));
  auto discoveryBefore = dump(*f.root);
  require(failed(goldengate::discoverBoundAutoCounterBridgeHosts(f.circuit, discovered, error)) &&
      discovered == f.hosts && dump(*f.root) == discoveryBefore,
      "corrupt production discovery accepted or changed output/IR");
  wrapper->setAttr("goldengate.autocounterHostBindings", registryBefore);
  SmallVector<goldengate::ControlMMIOWidget> widgets;
  require(succeeded(goldengate::deriveAutoCounterControlMMIOCatalog(f.circuit,f.hosts,widgets,error)),error);
  require(widgets.size()==2&&widgets[0].name=="AutoCounterBridgeModule_0"&&widgets[1].name=="AutoCounterBridgeModule_1"&&widgets[0].registerCount==13&&widgets[1].registerCount==13,"CSV materialized MMIO catalog");
  SmallVector<goldengate::ControlMMIORegion> regions;
  require(succeeded(goldengate::allocateControlMMIORegions(width,widgets,regions,error)),error);
  std::string header="unchanged";
  require(succeeded(goldengate::prepareAutoCounterBridgeHeader(f.circuit,f.hosts,regions,header,error)),error);
  require(StringRef(header).contains(".event_addr_hi = 4U, .event_addr_lo = 0U")&&StringRef(header).contains(".event_addr_hi = 68U, .event_addr_lo = 64U")&&StringRef(header).contains("ClockInfo{\"domain1\", 1U, 3U}"),"CSV header addresses/clocks");
  auto headerBefore=header;auto irBefore=dump(*f.root);
  auto badRegions=regions;badRegions[1].start=0;
  require(failed(goldengate::prepareAutoCounterBridgeHeader(f.circuit,f.hosts,badRegions,header,error))&&header==headerBefore&&dump(*f.root)==irBefore,"overlapping regions not rejected atomically");
  auto reversed=f.hosts;std::swap(reversed[0],reversed[1]);
  require(failed(goldengate::deriveAutoCounterControlMMIOCatalog(f.circuit,reversed,widgets,error))&&widgets.size()==2&&widgets[0].name=="AutoCounterBridgeModule_0","catalog host order not atomic");
  badRegions=regions;badRegions[0].size*=2;
  require(failed(goldengate::prepareAutoCounterBridgeHeader(f.circuit,f.hosts,badRegions,header,error))&&header==headerBefore,"wrong bank size rejected");
  auto bankName=f.hosts[0]->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost").getAs<StringAttr>("bankModule");FModuleOp bank;
  for(auto m:f.circuit.getOps<FModuleOp>())if(m.getName()==bankName)bank=m;
  auto originalRows=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");Builder b(&ctx);
  for(bool badOffset:{false,true}) {
    SmallVector<Attribute> mutatedRows(originalRows.begin(),originalRows.end());
    NamedAttrList changed(cast<DictionaryAttr>(mutatedRows[0]));
    changed.set(badOffset?"offset":"readable",badOffset?Attribute(b.getI64IntegerAttr(4)):Attribute(b.getBoolAttr(false)));
    mutatedRows[0]=changed.getDictionary(&ctx);bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(mutatedRows));
    auto mutatedIR=dump(*f.root);
    require(failed(goldengate::prepareAutoCounterBridgeHeader(f.circuit,f.hosts,regions,header,error))&&header==headerBefore&&mutatedIR==dump(*f.root),"register offset/permission corruption not rejected atomically");
    bank->setAttr("goldengate.mmioRegisters",originalRows);
  }
  if(output){std::error_code ec;llvm::raw_fd_ostream out(std::string(output)+".h",ec);require(!ec,"header output open");out<<header;}
  auto before=dump(*f.root);require(failed(goldengate::bindAutoCounterBridgeHosts(f.circuit,f.hosts,error))&&before==dump(*f.root),"repeat bind not atomic");
  if(output){std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"output open");f.root->print(out);}
}
void negative(MLIRContext &ctx,StringRef path) {
  Builder b(&ctx);unsigned cases=0;
  auto reject=[&](const std::function<void(Fixture&)> &mutate){Fixture f(ctx,path);mutate(f);auto before=dump(*f.root);auto oldHost=f.hosts[0].getArgument(0);std::string error;
    require(failed(goldengate::bindAutoCounterBridgeHosts(f.circuit,f.hosts,error))&&!error.empty()&&before==dump(*f.root)&&f.hosts[0].getArgument(0)==oldHost,"invalid binding changed input");++cases;};
  reject([&](Fixture &f){std::swap(f.hosts[0],f.hosts[1]);});
  reject([&](Fixture &f){f.hosts.pop_back();});
  reject([&](Fixture &f){rewrite(f,0,"channelMapping",b.getDictionaryAttr({}));});
  reject([&](Fixture &f){rewrite(f,1,"channelInfo",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.passes.fame.PipeChannel")),b.getNamedAttr("latency",b.getI64IntegerAttr(1))}));});
  reject([&](Fixture &f){rewrite(f,1,"sinks",b.getArrayAttr({b.getStringAttr("~Top|Top>bad")}));});
  reject([&](Fixture &f){rewrite(f,1,"sources",b.getArrayAttr({b.getStringAttr("~Top|Top>hostReset")}));});
  reject([&](Fixture &f){rewrite(f,2,"clock",b.getStringAttr("~Top|Top>domain1"));});
  reject([&](Fixture &f){rewrite(f,0,"clockInfo",b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("bad")),b.getNamedAttr("multiplier",b.getI64IntegerAttr(1)),b.getNamedAttr("divisor",b.getI64IntegerAttr(0))}));});
  reject([&](Fixture &f){auto raw=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> attrs(raw.begin(),raw.end());attrs.push_back(attrs[1]);f.circuit->setAttr("rawAnnotations",b.getArrayAttr(attrs));});
  reject([&](Fixture &f){auto raw=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");auto first=cast<DictionaryAttr>(raw[1]);rewrite(f,2,"globalName",first.get("globalName"));});
  reject([&](Fixture &f){auto info=f.hosts[1]->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");NamedAttrList altered(info);altered.set("addressBits",b.getI64IntegerAttr(25));f.hosts[1]->setAttr("goldengate.autocounterHost",altered.getDictionary(&ctx));});
  llvm::outs()<<"PASS "<<cases<<" malformed constructor/order/channel/clock/control bindings rejected atomically\n";
}
}
int main(int argc,char **argv) {
 try {require(argc==2||argc==3,"usage fixture-basename [output.mlir]");MLIRContext ctx;ctx.getOrLoadDialect<FIRRTLDialect>();ctx.getOrLoadDialect<circt::hw::HWDialect>();positive(ctx,argv[1],25,nullptr);positive(ctx,argv[1],32,argc==3?argv[2]:nullptr);negative(ctx,argv[1]);llvm::outs()<<"PASS constructor-ordered two-widget CSV host-token binding, independent rational clocks, F1/F2 AXI and retained metadata\n";}
 catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;
}
