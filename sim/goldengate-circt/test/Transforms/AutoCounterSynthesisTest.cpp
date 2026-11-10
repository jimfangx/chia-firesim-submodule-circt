// See LICENSE for license details.
#include "goldengate/AutoCounterSynthesis.h"
#include "goldengate/AnnotationClasses.h"
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
#include <set>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,const std::string &why){if(!ok)throw std::runtime_error(why);}
std::string text(Operation *op){std::string s;llvm::raw_string_ostream o(s);op->print(o);return s;}
FModuleOp named(CircuitOp c,StringRef name){for(auto m:c.getOps<FModuleOp>())if(m.getName()==name)return m;throw std::runtime_error("missing module "+name.str());}
struct Fixture {
  OwningOpRef<ModuleOp> root;CircuitOp circuit;
  Fixture(MLIRContext &ctx,StringRef base){llvm::SourceMgr manager;for(auto path:{base.str()+".fir",base.str()+".anno.json"}){auto f=llvm::MemoryBuffer::getFile(path);require(bool(f),"missing "+path);manager.AddNewSourceBuffer(std::move(*f),llvm::SMLoc());}
    DefaultTimingManager timer;auto scope=timer.getRootScope();FIRParserOptions options;options.numAnnotationFiles=1;root=importFIRFile(manager,&ctx,scope,options);require(bool(root),"fixture parse");circuit=*root->getOps<CircuitOp>().begin();}
};
void edit(Fixture &f,unsigned i,StringRef field,Attribute value){auto raw=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> next(raw.begin(),raw.end());NamedAttrList dict(cast<DictionaryAttr>(next[i]));dict.set(field,value);next[i]=dict.getDictionary(f.circuit.getContext());f.circuit->setAttr("rawAnnotations",ArrayAttr::get(f.circuit.getContext(),next));}
void positive(MLIRContext &ctx,StringRef path,const char *output){Fixture f(ctx,path);unsigned domains=999;std::string error;
 require(succeeded(goldengate::synthesizeEnabledAutoCounterCSV(f.circuit,{},domains,error)),error);require(domains==2&&succeeded(verify(*f.root)),"domain count or verification");
 auto top=named(f.circuit,"Top"),mid=named(f.circuit,"Mid"),leaf=named(f.circuit,"Leaf");require(top.getNumPorts()==25&&mid.getNumPorts()==8&&leaf.getNumPorts()==8,"hierarchy signatures");
 unsigned bridges=0,channels=0,triggers=0,resets=0,events=0;std::set<std::string> labels,globals;
 for(auto a:f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {auto d=cast<DictionaryAttr>(a);auto cls=d.getAs<StringAttr>("class");require(bool(cls),"annotation class");
  require(cls!=goldengate::AnnotationClasses::InternalAutoCounter&&cls!=goldengate::AnnotationClasses::AutoCounterCoverModule&&cls!=goldengate::AnnotationClasses::BridgeTopWiringOutput,"selection not consumed");
  if(cls==goldengate::AnnotationClasses::BridgeIO){++bridges;auto key=d.getAs<DictionaryAttr>("widgetConstructorKey");auto metadata=key.getAs<ArrayAttr>("eventMetadata");require(metadata.size()==(bridges==1?4:3),"clock-domain grouping");
   SmallVector<StringRef> order;for(auto m:metadata)order.push_back(cast<DictionaryAttr>(m).getAs<StringAttr>("portName").getValue());
   SmallVector<StringRef> expected=bridges==1?SmallVector<StringRef>{"autocounter_left_leaf_coverCount","autocounter_left_leaf_leafValue","autocounter_left_leaf_leafCount_0","autocounter_topCount_0"}:SmallVector<StringRef>{"autocounter_right_leaf_coverCount","autocounter_right_leaf_leafValue","autocounter_right_leaf_leafCount_0"};
   require(order==expected,"SFC positional constructor event order");
   for(auto m:metadata){auto e=cast<DictionaryAttr>(m);labels.insert(e.getAs<StringAttr>("label").getValue().str());auto op=e.getAs<DictionaryAttr>("opType").getAs<StringAttr>("class");require(op==goldengate::AnnotationClasses::AutoCounterAccumulate||op==goldengate::AnnotationClasses::AutoCounterIdentity,"operation singleton lost");++events;}}
  if(cls==goldengate::AnnotationClasses::ChannelConnection){++channels;require(globals.insert(d.getAs<StringAttr>("globalName").getValue().str()).second,"channel identity collision");}
  triggers+=cls==goldengate::AnnotationClasses::InternalTriggerSink;resets+=cls==goldengate::AnnotationClasses::GlobalResetSink;
 }
 require(bridges==2&&events==7&&channels==12&&triggers==2&&resets==2&&labels.size()==7,"generated annotation inventory");
 require(labels.count("Top_left_leaf_leafCount")&&labels.count("Top_right_leaf_leafValue")&&labels.count("Top_topCount"),"absolute path labels");
 if(output){std::error_code ec;llvm::raw_fd_ostream stream(output,ec);require(!ec,"output open");f.root->print(stream);}
}
void negatives(MLIRContext &ctx,StringRef path){Builder b(&ctx);unsigned cases=0;
 auto reject=[&](const std::function<void(Fixture&)> &change){Fixture f(ctx,path);change(f);auto original=text(*f.root);auto arg=named(f.circuit,"Top").getArgument(0);unsigned count=999;std::string error;
  require(failed(goldengate::synthesizeEnabledAutoCounterCSV(f.circuit,{},count,error))&&!error.empty()&&count==999&&text(*f.root)==original&&named(f.circuit,"Top").getArgument(0)==arg,"failed synthesis mutated IR/results/SSA");++cases;};
 reject([&](Fixture &f){edit(f,2,"opType",b.getDictionaryAttr({}));});
 reject([&](Fixture &f){edit(f,3,"description",b.getI64IntegerAttr(0));});
 reject([&](Fixture &f){edit(f,3,"reset",b.getStringAttr("~Top|Leaf>clock"));});
 reject([&](Fixture &f){edit(f,3,"target",b.getStringAttr("~Top|Leaf>missing"));});
 reject([&](Fixture &f){edit(f,4,"coverGenerated",b.getStringAttr("true"));});
 reject([&](Fixture &f){edit(f,6,"coverGenerated",b.getBoolAttr(false));});
 reject([&](Fixture &f){auto m=named(f.circuit,"Leaf");OpBuilder op(&ctx);op.setInsertionPointToEnd(m.getBodyBlock());for(auto w:m.getOps<WireOp>())if(w.getName()=="localClock")op.create<StrictConnectOp>(m.getLoc(),w.getResult(),w.getResult());});
 reject([&](Fixture &f){auto raw=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> next(raw.begin(),raw.end());next.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeTopWiring)),b.getNamedAttr("target",b.getStringAttr("~Top|Leaf>event"))}));f.circuit->setAttr("rawAnnotations",b.getArrayAttr(next));});
 // A late global-name collision happens after staged hierarchy edits; the
 // original circuit still must keep every pre-existing operation/SSA identity.
 reject([&](Fixture &f){auto raw=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> next(raw.begin(),raw.end());auto d=cast<DictionaryAttr>(next[0]);NamedAttrList duplicate(d);duplicate.set("globalName",b.getStringAttr("autocounter_triggerEnable_0"));next.push_back(duplicate.getDictionary(&ctx));f.circuit->setAttr("rawAnnotations",b.getArrayAttr(next));});
 llvm::outs()<<"PASS "<<cases<<" malformed operation/type/selection/clock/wiring/collision cases rejected atomically\n";
}
}
int main(int argc,char **argv){try{require(argc==2||argc==3,"usage fixture-basename [output.mlir]");MLIRContext ctx;ctx.getOrLoadDialect<FIRRTLDialect>();ctx.getOrLoadDialect<circt::hw::HWDialect>();positive(ctx,argv[1],argc==3?argv[2]:nullptr);negatives(ctx,argv[1]);llvm::outs()<<"PASS enabled CSV target selection/reset gating, repeated hierarchy exports, two-domain constructors and cleanup\n";}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
