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
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <limits>
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
    {
      // Real annotation payloads include nested constructor data, escaped
      // source text and large inline blackboxes. JSON borrowing must retain
      // their values, preserve filtering, and never reuse a previous boundary.
      llvm::SmallString<256> directory;
      require(!llvm::sys::fs::createUniqueDirectory("goldengate-annotations", directory),
              "annotation temporary directory");
      auto cleanup = llvm::make_scope_exit([&] { llvm::sys::fs::remove_directories(directory); });
      auto root = fixture(ctx);auto c = *root->getOps<CircuitOp>().begin();
      std::string body = "\"quoted\"\\path\n\t\r";
      body.append(1024 * 1024, 'x');body.push_back('\0');body += u8"λ";
      auto data = b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::BridgeIO)),
          b.getNamedAttr("body", b.getStringAttr(body)),
          b.getNamedAttr("empty", b.getArrayAttr({b.getArrayAttr({}), b.getDictionaryAttr({})})),
          b.getNamedAttr("limits", b.getArrayAttr({
              b.getI64IntegerAttr(std::numeric_limits<int64_t>::min()),
              b.getI64IntegerAttr(std::numeric_limits<int64_t>::max()),
              b.getF64FloatAttr(-0.0)})),
          b.getNamedAttr("nested", b.getArrayAttr({b.getUnitAttr(), b.getBoolAttr(true),
              b.getI64IntegerAttr(-42), b.getF64FloatAttr(0.125),
              b.getDictionaryAttr({b.getNamedAttr(u8"κ\\\"", b.getStringAttr(u8"λ\n"))})}))});
      c->setAttr("rawAnnotations", b.getArrayAttr({data,
          b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Preserved"))})}));
      auto raw = c->getAttr("rawAnnotations");
      auto read = [&](StringRef path) {
        auto bytes = llvm::MemoryBuffer::getFile(path);require(bool(bytes), "annotation output read");
        auto value = llvm::json::parse((*bytes)->getBuffer());
        require(bool(value), "annotation output JSON syntax");return std::move(*value);
      };
      auto allPath = (directory + "/all.json").str();
      require(succeeded(goldengate::emitAllAnnotations(c, allPath, error)), error);
      auto all = read(allPath);auto array = all.getAsArray();
      require(array && array->size() == 2, "all annotation membership");
      auto first = (*array)[0].getAsObject();
      auto expected = llvm::json::parse(u8R"json([null,true,-42,0.125,{"κ\\\"":"λ\n"}])json");
      require(first && first->getString("body") == StringRef(body) && expected &&
              *first->get("nested") == *expected, "nested or escaped annotation payload changed");
      auto emptyExpected = llvm::json::parse("[[],{}]");
      auto limitsExpected = llvm::json::parse("[-9223372036854775808,9223372036854775807,-0.0]");
      require(emptyExpected && limitsExpected && *first->get("empty") == *emptyExpected &&
              *first->get("limits") == *limitsExpected, "empty containers or numeric limits changed");
      auto famePath = (directory + "/fame.json").str();
      require(succeeded(goldengate::emitFAMEAnnotations(c, famePath, error)), error);
      auto fame = read(famePath);
      require(fame.getAsArray() && fame.getAsArray()->size() == 1 &&
              fame.getAsArray()->front() == array->front() && c->getAttr("rawAnnotations") == raw,
              "FAME filtering or source preservation");
      c->setAttr("rawAnnotations", b.getArrayAttr({}));
      require(succeeded(goldengate::emitAllAnnotations(c, allPath, error)), error);
      auto empty = read(allPath);
      require(empty.getAsArray() && empty.getAsArray()->empty(), "stale annotation serialization reused");

      // The streaming writer must validate the complete selected payload
      // before opening an existing output, even after many valid values.
      for (unsigned mode = 0; mode < 8; ++mode) {
        Attribute bad;
        StringRef expectedError;
        if (mode < 2) {
          bad = mode == 0 ? Attribute(b.getStringAttr("not a dictionary"))
                          : Attribute(b.getDictionaryAttr({}));
          expectedError = mode == 0 ? "FAME annotation is not a dictionary"
                                    : "annotation has no class name";
        } else {
          Attribute value;
          if (mode == 2 || mode == 3) {
            value = b.getF64FloatAttr(mode == 2 ? std::numeric_limits<double>::infinity()
                                               : std::numeric_limits<double>::quiet_NaN());
            expectedError = "annotation float is not a finite JSON number";
          } else if (mode == 4) {
            value = b.getIntegerAttr(IntegerType::get(&ctx, 65), llvm::APInt(65, 1));
            expectedError = "annotation integer exceeds JSON's 64-bit range";
          } else {
            value = TypeAttr::get(b.getI32Type());
            expectedError = "unsupported attribute in FAME annotation";
            if (mode == 6)
              value = b.getArrayAttr({b.getI64IntegerAttr(1), value});
            if (mode == 7)
              value = b.getDictionaryAttr({b.getNamedAttr("inner", b.getArrayAttr({value}))});
          }
          bad = b.getDictionaryAttr({
              b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::BridgeIO)),
              b.getNamedAttr("bad", value)});
        }
        c->setAttr("rawAnnotations", b.getArrayAttr({data, bad}));
        auto before = dump(*root);
        { std::error_code ec;llvm::raw_fd_ostream sentinel(allPath, ec);
          require(!ec, "sentinel write");sentinel << "existing output\n"; }
        require(failed(goldengate::emitAllAnnotations(c, allPath, error)) &&
                error == expectedError && dump(*root) == before,
                "invalid annotation accepted or altered source");
        auto sentinel = llvm::MemoryBuffer::getFile(allPath);
        require(sentinel && (*sentinel)->getBuffer() == "existing output\n",
                "invalid annotation truncated existing output");
      }

      auto ignored = b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr("test.Ignored")),
          b.getNamedAttr("unsupported", TypeAttr::get(b.getI32Type()))});
      c->setAttr("rawAnnotations", b.getArrayAttr({ignored, data}));
      require(succeeded(goldengate::emitFAMEAnnotations(c, famePath, error)) &&
              read(famePath).getAsArray()->size() == 1,
              "FAME writer validated a filtered payload");
      require(failed(goldengate::emitAllAnnotations(c, allPath, error)),
              "unfiltered unsupported payload accepted");
      c->setAttr("rawAnnotations", b.getArrayAttr({data}));
      require(failed(goldengate::emitAllAnnotations(c, directory, error)) &&
              StringRef(error).starts_with("cannot write FAME annotations:"),
              "unwritable annotation path accepted");
      if (llvm::sys::fs::exists("/dev/full"))
        require(failed(goldengate::emitAllAnnotations(c, "/dev/full", error)) &&
                StringRef(error).starts_with("cannot finish FAME annotations:"),
                "annotation write failure reported success");
#ifdef NDEBUG
      // LLVM's release fallback replaces invalid UTF-8 keys. Preserve the
      // original object's ordering and collision semantics in that rare path.
      auto invalidKeys = b.getDictionaryAttr({
          b.getNamedAttr(StringRef("\x80", 1), b.getI64IntegerAttr(1)),
          b.getNamedAttr(StringRef("\x81", 1), b.getI64IntegerAttr(2))});
      c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::BridgeIO)),
          b.getNamedAttr("payload", invalidKeys)})}));
      require(succeeded(goldengate::emitAllAnnotations(c, allPath, error)), error);
      auto invalid = read(allPath);
      auto payload = invalid.getAsArray()->front().getAsObject()->getObject("payload");
      require(payload && payload->size() == 1 && payload->begin()->second.getAsInteger() == 2,
              "invalid UTF-8 key collision behavior changed");
      llvm::outs() << "Release invalid-UTF-8 key normalization/collision check passed\n";
#endif
      llvm::outs() << "Annotation JSON: nested values, empty containers, numeric limits, Unicode/NUL/large strings, filtering, changed boundaries, eight validation rejections and I/O failures passed\n";
    }
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
