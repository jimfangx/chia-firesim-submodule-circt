// See LICENSE for license details.
// Hierarchy cases from Scala WriteXDCFileSpec plus typed-reference failures.
#include "goldengate/XDCEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/AnnotationEmission.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
using goldengate::AnnotationClasses;
namespace {
void require(bool value,StringRef reason) {if(!value) throw std::runtime_error(reason.str());}
std::string dump(Operation *op) {std::string s;llvm::raw_string_ostream out(s);op->print(out,OpPrintingFlags().useLocalScope());return s;}
DictionaryAttr paths(OpBuilder &b) {
  return b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::XDCPaths)),
    b.getNamedAttr("postLinkPath",b.getStringAttr("post/link"))});
}
DictionaryAttr snippet(OpBuilder &b,bool implementation,StringRef format,ArrayRef<StringRef> arguments) {
  SmallVector<Attribute> refs;for(auto a:arguments) refs.push_back(b.getStringAttr(a));
  return b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::InternalXDC)),
    b.getNamedAttr("destinationFile",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(implementation?AnnotationClasses::XDCImplementation:AnnotationClasses::XDCSynthesis))})),
    b.getNamedAttr("formatString",b.getStringAttr(format)),b.getNamedAttr("argumentList",b.getArrayAttr(refs))});
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto m=parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Top" {
    firrtl.module @Top(in %clock: !firrtl.clock) {
      %a1:2 = firrtl.instance a1 @A(in clock: !firrtl.clock, in bus: !firrtl.bundle<lanes: vector<uint<1>, 2>>)
      %b = firrtl.instance b @B(in clock: !firrtl.clock)
    }
    firrtl.module private @B(in %clock: !firrtl.clock) {
      %a2:2 = firrtl.instance a2 @A(in clock: !firrtl.clock, in bus: !firrtl.bundle<lanes: vector<uint<1>, 2>>)
      %a3:2 = firrtl.instance a3 @A(in clock: !firrtl.clock, in bus: !firrtl.bundle<lanes: vector<uint<1>, 2>>)
    }
    firrtl.module private @A(in %clock: !firrtl.clock, in %bus: !firrtl.bundle<lanes: vector<uint<1>, 2>>) {
      %n = firrtl.wire : !firrtl.uint<1>
    }
    firrtl.module private @Unused(in %clock: !firrtl.clock) {}
  } })mlir",&ctx);
  require(bool(m),"fixture parse");OpBuilder b(&ctx);auto c=*m->getOps<CircuitOp>().begin();
  c->setAttr("rawAnnotations",b.getArrayAttr({paths(b),b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Preserved")),b.getNamedAttr("target",b.getStringAttr("~Top|A>clock"))})}));
  require(succeeded(verify(*m)),"fixture verify");return m;
}
void append(CircuitOp c,Attribute a) {
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> out(raw.begin(),raw.end());out.push_back(a);
  c->setAttr("rawAnnotations",ArrayAttr::get(c.getContext(),out));
}
std::map<std::string,std::string> bodies(CircuitOp c) {
  std::map<std::string,std::string> result;
  for(auto m:c.getOps<FModuleLike>()) result[m.getModuleName().str()]=dump(m.getOperation());
  return result;
}
std::map<std::string,std::string> run(CircuitOp c) {
  auto before=bodies(c);std::string error;require(succeeded(goldengate::prepareXDCOutput(c,error)),error);
  require(succeeded(verify(c))&&bodies(c)==before,"XDC altered modules");
  std::map<std::string,std::string> result;
  for(auto a:c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto d=cast<DictionaryAttr>(a);auto cls=d.getAs<StringAttr>("class").getValue();
    require(cls!=AnnotationClasses::InternalXDC&&cls!=AnnotationClasses::XDCPaths,"consumed XDC remains");
    if(cls==AnnotationClasses::XDCOutput) result[d.getAs<StringAttr>("suffix").getValue().str()]=d.getAs<StringAttr>("fileBody").getValue().str();
  }
  require(result.size()==2,"two XDC files required");return result;
}
std::set<std::string> lines(StringRef body) {
  SmallVector<StringRef> split;body.split(split,'\n',-1,false);std::set<std::string> result;
  for(auto line:split) if(!line.starts_with("#")) result.insert(line.str());return result;
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();OpBuilder b(&ctx);std::string error;
    auto make=[&](){return fixture(ctx);};
    auto root=make();auto c=*root->getOps<CircuitOp>().begin();
    append(c,snippet(b,false,"{}",{"~Top|A>clock"}));
    append(c,snippet(b,true,"{} {}",{"~Top|A>clock","~Top|A>n"}));
    append(c,snippet(b,true,"one:{}",{"~Top|Top/b:B/a2:A>clock"}));
    append(c,snippet(b,false,"lane:{}",{"~Top|A>bus.lanes[1]"}));
    append(c,snippet(b,true,"zero arguments",{}));
    append(c,snippet(b,true,"unused:{}",{"~Top|Unused>clock"}));
    auto output=run(c);
    require(lines(output[".synthesis.xdc"])==std::set<std::string>{"a1/clock","b/a2/clock","b/a3/clock","lane:a1/bus/lanes/1","lane:b/a2/bus/lanes/1","lane:b/a3/bus/lanes/1"},"duplicated synthesis paths");
    require(lines(output[".implementation.xdc"])==std::set<std::string>{"post/link/a1/clock post/link/a1/n","post/link/b/a2/clock post/link/b/a2/n","post/link/b/a3/clock post/link/b/a3/n","one:post/link/b/a2/clock","zero arguments"},"post-link/shared root/nonlocal/zero argument paths");
    auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==3&&cast<DictionaryAttr>(raw[2]).getAs<StringAttr>("class").getValue()=="test.Preserved","unrelated annotation retention");
    for(unsigned mode=0;mode<13;++mode) {
      auto r=make();auto tc=*r->getOps<CircuitOp>().begin();
      if(mode==0)tc->removeAttr("rawAnnotations");
      if(mode==1)tc->setAttr("rawAnnotations",b.getArrayAttr({}));
      if(mode==2)append(tc,paths(b));
      if(mode>=3&&mode<=7) {
        StringRef targets[]={"~Wrong|A>clock","~Top|A>missing","~Top|Top/b:Wrong>clock","~Top|A>bus","~Top|A>bus.lanes[2]"};
        append(tc,snippet(b,true,"{}",{targets[mode-3]}));
      }
      if(mode==8)append(tc,snippet(b,true,"{} {}",{"~Top|A>clock","~Top|B>clock"}));
      if(mode==9){NamedAttrList d(paths(b));d.set("postLinkPath",b.getBoolAttr(true));tc->setAttr("rawAnnotations",b.getArrayAttr({d.getDictionary(&ctx)}));}
      if(mode==10)append(tc,b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::XDCOutput))}));
      if(mode==11){NamedAttrList d(snippet(b,true,"{}",{"~Top|A>clock"}));d.set("destinationFile",b.getStringAttr("unknown"));append(tc,d.getDictionary(&ctx));}
      if(mode==12) tc.walk([&](InstanceOp instance) {
        if(instance.getName()=="b") instance->setAttr("goldengate.generatedClockConstraint",
          b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("clock")),b.getNamedAttr("mfmr",b.getI64IntegerAttr(0))}));
      });
      auto before=dump(*r);require(failed(goldengate::prepareXDCOutput(tc,error))&&!error.empty(),"invalid XDC accepted");require(dump(*r)==before,"XDC rejection mutated IR");
    }
    // Clock constraints are bound to a real FIRRTL instance output. MFMR 3
    // relaxes setup by 3 and hold by 2; the clock still divides host_clock by 1.
    auto r=make();auto tc=*r->getOps<CircuitOp>().begin();FModuleOp model;
    for(auto m:tc.getOps<FModuleOp>()) if(m.getName()=="A") model=m;
    b.setInsertionPointToEnd(tc.getBodyBlock());auto gate=b.create<FExtModuleOp>(tc.getLoc(),b.getStringAttr("AbstractClockGate"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{
      {b.getStringAttr("I"),ClockType::get(&ctx),Direction::In},{b.getStringAttr("CE"),UIntType::get(&ctx,1),Direction::In},{b.getStringAttr("O"),ClockType::get(&ctx),Direction::Out}},"AbstractClockGate");
    b.setInsertionPointToEnd(model.getBodyBlock());b.create<InstanceOp>(tc.getLoc(),gate,"target_clock_buffer");
    goldengate::RationalClockInfo clock{"target_clock",1,1,3};
    require(succeeded(goldengate::addFAMEClockConstraint(tc,model,"target_clock",clock,error)),error);
    auto before=dump(*r);require(failed(goldengate::addFAMEClockConstraint(tc,model,"target_clock",clock,error))&&dump(*r)==before,"duplicate clock annotation mutation");
    output=run(tc);
    require(output[".implementation.xdc"].find("post/link/b/a2/target_clock_buffer/O")!=std::string::npos&&
      output[".implementation.xdc"].find("set_multicycle_path 3 -setup")!=std::string::npos&&
      output[".implementation.xdc"].find("set_multicycle_path 2 -hold")!=std::string::npos,"clock constraints/typed instance output");
    llvm::outs()<<"XDC synthesis/post-link duplication, nonlocal/shared-root/aggregate/zero-argument references, MFMR clock constraints; 14 atomic rejections passed\n";
    if(argc==3) {
      auto real=parseSourceFile<ModuleOp>(argv[1],&ctx);require(bool(real),"real F1Shim parse");auto rc=*real->getOps<CircuitOp>().begin();
      auto original=bodies(rc);run(rc);require(bodies(rc)==original,"real retained module bodies");
      require(succeeded(goldengate::emitOutputFiles(rc,argv[2],"FireSim-generated",error)),error);
      llvm::SmallString<256> json(argv[2]);llvm::sys::path::append(json,"post-fame-xdc-all.json");
      require(succeeded(goldengate::emitAllAnnotations(rc,json,error)),error);
      llvm::outs()<<"Real F1Shim XDC emitted; "<<original.size()<<" module bodies unchanged\n";
    }
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
