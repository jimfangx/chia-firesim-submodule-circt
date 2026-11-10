// See LICENSE for license details.
#include "goldengate/AutoCounterControl.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
std::string dump(Operation *op){std::string s;llvm::raw_string_ostream out(s);op->print(out);return s;}
struct Fixture {
  OwningOpRef<ModuleOp> root;CircuitOp circuit;SmallVector<FModuleOp> hosts;
  SmallVector<goldengate::ControlMMIORegion> regions;
  Fixture(MLIRContext &ctx,StringRef path,unsigned width=32) {
    llvm::SourceMgr source;
    for(auto suffix:{".fir",".anno.json"}){auto file=llvm::MemoryBuffer::getFile(path.str()+suffix);require(bool(file),"missing fixture");source.AddNewSourceBuffer(std::move(*file),llvm::SMLoc());}
    DefaultTimingManager timing;auto scope=timing.getRootScope();FIRParserOptions options;options.numAnnotationFiles=1;
    root=importFIRFile(source,&ctx,scope,options);require(bool(root),"import fixture");
    circuit=*root->getOps<CircuitOp>().begin();std::string error;
    require(succeeded(goldengate::materializeAutoCounterBridgeHosts(circuit,width,12,hosts,error)),error);
    require(succeeded(goldengate::bindAutoCounterBridgeHosts(circuit,hosts,error)),error);
    SmallVector<goldengate::ControlMMIOWidget> widgets;
    require(succeeded(goldengate::deriveAutoCounterControlMMIOCatalog(circuit,hosts,widgets,error)),error);
    require(succeeded(goldengate::allocateControlMMIORegions(width,widgets,regions,error)),error);
  }
};
void positive(MLIRContext &ctx,StringRef path,unsigned width,bool relocate) {
  Fixture f(ctx,path,width);std::string error,header="sentinel";
  if(relocate){f.regions[0].start=uint64_t(1)<<(width-1);f.regions[1].start=f.regions[0].start+64;std::swap(f.regions[0],f.regions[1]);}
  auto rawBefore=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::assembleAutoCounterControl(f.circuit,f.hosts,f.regions,header,error)),error);
  require(succeeded(verify(*f.root)),"complete control verify");
  require(f.circuit.getName()=="GGControlMasterWrapper","control top identity");
  FModuleOp top,decoder;
  for(auto m:f.circuit.getOps<FModuleOp>()){if(m.getName()==f.circuit.getName())top=m;if(m.getName()=="GGControlAddressDecode")decoder=m;}
  unsigned controls=0;for(auto p:top.getPorts()){auto n=p.name.getValue();controls+=n=="ctrl";require(n=="ctrl"||(!n.starts_with("ctrl_")&&!n.starts_with("autocounter_")),"unbound route or local control remains");}
  require(controls==1&&top->hasAttr("goldengate.autocounterGlobalControl"),"one complete master");
  auto rows=decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");require(rows&&rows.size()==2,"complete allocation");
  for(auto [i,a]:llvm::enumerate(rows)){auto row=cast<DictionaryAttr>(a);require(row.getAs<StringAttr>("name")==f.regions[i].name&&uint64_t(row.getAs<IntegerAttr>("start").getInt())==f.regions[i].start,"allocation used by hardware changed");}
  auto rawAfter=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");require(rawAfter.size()==rawBefore.size(),"annotation inventory changed");
  require(StringRef(header).contains("new autocounter_t"),"missing driver constructor");
  if(relocate)require(StringRef(header).contains(std::to_string(uint64_t(1)<<(width-1))+"U"),"global header address truncated");
  auto before=dump(*f.root),headerBefore=header;
  require(failed(goldengate::assembleAutoCounterControl(f.circuit,{},f.regions,header,error))&&before==dump(*f.root)&&header==headerBefore,"repeat assembly not atomic");
}
void negative(MLIRContext &ctx,StringRef path) {
  unsigned count=0;Builder builder(&ctx);
  auto reject=[&](const std::function<void(Fixture&)> &mutate){Fixture f(ctx,path);mutate(f);std::string error,header="sentinel";auto before=dump(*f.root);auto arg=f.hosts[0].getArgument(0);
    require(failed(goldengate::assembleAutoCounterControl(f.circuit,f.hosts,f.regions,header,error))&&!error.empty()&&before==dump(*f.root)&&header=="sentinel"&&arg==f.hosts[0].getArgument(0),"invalid assembly changed IR/header/SSA");++count;};
  reject([](Fixture &f){f.regions.pop_back();});
  reject([](Fixture &f){f.regions.push_back({"UnimplementedWidget_0",128,64});});
  reject([](Fixture &f){f.regions[0].size=128;});
  reject([](Fixture &f){f.regions[1].start=0;});
  reject([](Fixture &f){std::swap(f.hosts[0],f.hosts[1]);});
  reject([&](Fixture &f){auto info=f.hosts[1]->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");NamedAttrList changed(info);changed.set("idBits",builder.getI64IntegerAttr(8));f.hosts[1]->setAttr("goldengate.autocounterHost",changed.getDictionary(&ctx));});
  // A late response-stage failure must roll back already generated decode,
  // queues, request dispatch and annotation retargeting, including header.
  reject([&](Fixture &f){OpBuilder b(&ctx);b.setInsertionPointToEnd(f.circuit.getBodyBlock());b.create<FModuleOp>(f.circuit.getLoc(),b.getStringAttr("GGControlReadArbiter"),f.hosts[0].getConventionAttr(),ArrayRef<PortInfo>{});});
  llvm::outs()<<"PASS "<<count<<" complete CSV control invalid catalog/width/late-stage cases preserve IR/header/SSA\n";
}
}
int main(int argc,char **argv){try{require(argc==2,"usage fixture-basename");MLIRContext ctx;ctx.getOrLoadDialect<FIRRTLDialect>();ctx.getOrLoadDialect<circt::hw::HWDialect>();for(unsigned width:{25u,32u})for(bool relocate:{false,true})positive(ctx,argv[1],width,relocate);negative(ctx,argv[1]);llvm::outs()<<"PASS complete two-widget CSV global AXI control and allocation-bound header F1/F2\n";}catch(const std::exception&e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
