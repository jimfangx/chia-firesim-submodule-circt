// See LICENSE for license details.
#include "goldengate/HostMemoryOutputBuffer.h"
#include "goldengate/HostMemoryPort.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx){
  auto token=[](StringRef fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs.str()+">>";};
  auto address=token("id: uint<5>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>");
  std::string s="module { firrtl.circuit \"GGHostMemoryReadResponseWrapper\" { firrtl.module @GGHostMemoryReadResponseWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %host_mem: !firrtl.bundle<aw: "+address+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<5>, resp: uint<2>")+", ar: "+address+", r flip: "+token("id: uint<5>, last: uint<1>, data: uint<64>, resp: uint<2>")+">) {} firrtl.module @GGFASEDTokenEngine() {} } }";
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  SmallVector<Attribute> as;
  for(auto n:{"diagnostic","hostReset","host_mem.aw.bits.addr","host_mem.r.bits.data","host_mem"})
    as.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGHostMemoryReadResponseWrapper|GGHostMemoryReadResponseWrapper>"+std::string(n))}))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(as));
  auto k=b.getDictionaryAttr({b.getNamedAttr("memoryRegionName",b.getStringAttr("MainMemory_0")),
    b.getNamedAttr("axi4Widths",b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),
      b.getNamedAttr("dataBits",b.getI64IntegerAttr(64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))})),
    b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxReadTransfer",b.getI64IntegerAttr(8)), b.getNamedAttr("idReuse",b.getI64IntegerAttr(1)), b.getNamedAttr("maxFlight",b.getI64IntegerAttr(10))}))});
  top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k);
  auto wrapper=top(c,"GGHostMemoryReadResponseWrapper");
  auto master=cast<BundleType>(wrapper.getPorts()[3].type);
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto inner=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryReadWrapper"),
    ConventionAttr::get(&ctx,Convention::Internal),wrapper.getPorts());
  auto readType=master.getElements()[4].type;
  SmallVector<PortInfo> routerPorts{{b.getStringAttr("host"),readType,Direction::In},
    {b.getStringAttr("unused1"),UIntType::get(&ctx,1,false),Direction::Out},
    {b.getStringAttr("unused2"),UIntType::get(&ctx,1,false),Direction::Out}};
  auto router=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryReadResponseRouter"),
    ConventionAttr::get(&ctx,Convention::Internal),routerPorts);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(c.getLoc(),inner,"sim"),r=b.create<InstanceOp>(c.getLoc(),router,"read_responses");
  auto arg=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(c.getLoc(),v,n);};
  for(auto ch:master.getElements()) {
    Value host=f(arg(3),ch.name),child=ch.name=="r"?r.getResult(0):f(sim.getResult(3),ch.name);
    b.create<ConnectOp>(c.getLoc(),ch.isFlip?child:host,ch.isFlip?host:child);
  }
  for(unsigned i:{0u,1u})b.create<ConnectOp>(c.getLoc(),sim.getResult(i),arg(i));
  b.create<ConnectOp>(c.getLoc(),arg(2),sim.getResult(2));
  std::string error;require(succeeded(goldengate::addHostMemoryOutputBuffer(c,error)),error);
  return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string,Value> drivers;
  std::map<std::string,APInt> inputs,memo;
  std::string path(Value v) {
    if(auto f=v.getDefiningOp<SubfieldOp>())return path(f.getInput())+"."+f.getFieldName().str();
    if(auto a=dyn_cast<BlockArgument>(v))return module.getPortName(a.getArgNumber()).str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m):module(m) {
    for(auto c:m.getOps<StrictConnectOp>())require(drivers.emplace(path(c.getDest()),c.getSrc()).second,"duplicate adapter driver");
  }
  APInt eval(Value v) {
    auto n=path(v);if(memo.count(n))return memo.at(n);
    unsigned w=*cast<UIntType>(v.getType()).getWidth();APInt result(w,0);
    if(drivers.count(n))result=eval(drivers.at(n));
    else if(auto bits=v.getDefiningOp<BitsPrimOp>())result=eval(bits.getInput()).lshr(bits.getLo()).trunc(w);
    else if(auto pad=v.getDefiningOp<PadPrimOp>())result=eval(pad.getInput()).zextOrTrunc(w);
    else result=inputs.at(n);
    result=result.zextOrTrunc(w);memo.insert_or_assign(n,result);return result;
  }
  uint64_t output(StringRef n){return eval(drivers.at(n.str())).getZExtValue();}
};
void behavior(CircuitOp c) {
  auto helper=top(c,"GGHostMemoryPortAdapter");Interpreter sim(helper);std::mt19937_64 rng(247);
  require(helper.getOps<RegOp>().empty()&&helper.getOps<RegResetOp>().empty()&&helper.getOps<MemOp>().empty()&&helper.getOps<InstanceOp>().empty(),"adapter must be combinational");
  auto internal=cast<BundleType>(helper.getPorts()[0].type);
  unsigned cases=0;uint64_t checks=0;
  for(unsigned id=0;id<65536;++id)for(unsigned handshake=0;handshake<4;++handshake) {
    sim.inputs.clear();sim.memo.clear();std::map<std::string,uint64_t> expected;
    for(auto ch:internal.getElements()) {
      std::string input=std::string(ch.isFlip?"out.":"in.")+ch.name.getValue().str();
      std::string output=std::string(ch.isFlip?"in.":"out.")+ch.name.getValue().str();
      sim.inputs.emplace(input+".valid",APInt(1,handshake&1));expected[output+".valid"]=handshake&1;
      sim.inputs.emplace(output+".ready",APInt(1,handshake>>1));expected[input+".ready"]=handshake>>1;
      auto payload=cast<BundleType>(cast<BundleType>(ch.type).getElements()[2].type);
      for(auto e:payload.getElements()) {
        auto name=e.name.getValue().str();unsigned w=*cast<UIntType>(e.type).getWidth();
        unsigned iw=name=="id"&&ch.isFlip?16:w;
        uint64_t v=name=="id"?(ch.isFlip?id:id&31):rng();v=APInt(iw,v).getZExtValue();
        sim.inputs.emplace(input+".bits."+name,APInt(iw,v));expected[output+".bits."+name]=name=="id"&&ch.isFlip?v&31:v;
      }
    }
    require(sim.drivers.size()==expected.size(),"missing or additional adapter leaf");
    for(auto [n,v]:expected){require(sim.output(n)==v,"platform memory ID/handshake/payload mismatch");++checks;}
    ++cases;
  }
  llvm::outs()<<cases<<" cases covering all 65536 response IDs, all 32 request IDs and four handshake combinations; "<<checks<<" leaf checks\n";
}
void bindings(CircuitOp c) {
  auto m=top(c,"GGHostMemoryPlatformWrapper"),old=top(c,"GGHostMemoryReadResponseWrapper");
  InstanceOp sim,adapter;for(auto i:m.getOps<InstanceOp>())if(i.getName()=="sim")sim=i;else if(i.getName()=="memory_port")adapter=i;
  require(sim&&adapter&&std::distance(m.getOps<InstanceOp>().begin(),m.getOps<InstanceOp>().end())==2,"wrapper instances");
  unsigned count=0,mi=0;
  for(auto [i,p]:llvm::enumerate(m.getPorts())) {
    if(p.name=="mem_0"){mi=i;continue;}
    auto op=old.getPorts()[i];require(p.name==op.name&&p.type==op.type&&p.direction==op.direction&&p.annotations==op.annotations&&p.sym==op.sym,"copied port identity changed");
    Value a=m.getBodyBlock()->getArgument(i),v=sim.getResult(i);
    for(auto conn:m.getOps<ConnectOp>())count+=conn.getDest()==(p.direction==Direction::In?v:a)&&conn.getSrc()==(p.direction==Direction::In?a:v);
  }
  require(mi==m.getNumPorts()-1&&m.getNumPorts()==old.getNumPorts(),"platform port placement");
  for(auto conn:m.getOps<ConnectOp>()) {
    count+=conn.getDest()==adapter.getResult(0)&&conn.getSrc()==sim.getResult(old.getNumPorts()-1);
    count+=conn.getDest()==m.getBodyBlock()->getArgument(mi)&&conn.getSrc()==adapter.getResult(1);
  }
  require(count==m.getNumPorts()+1&&count==std::distance(m.getOps<ConnectOp>().begin(),m.getOps<ConnectOp>().end()),"wrapper bindings mismatch");
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();std::string error;
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    std::map<std::string,std::string> before;for(auto m:c.getOps<FModuleOp>())before[m.getName().str()]=dump(m);
    require(succeeded(goldengate::specializeHostMemoryPort(c,error)),error);require(succeeded(verify(*root)),"invalid platform IR");
    for(auto [n,s]:before)require(dump(top(c,n))==s,"existing body changed");bindings(c);behavior(c);
    auto ann=c->getAttrOfType<ArrayAttr>("rawAnnotations");
    for(auto [i,a]:llvm::enumerate(ann)) {
      auto target=cast<StringAttr>(cast<DictionaryAttr>(a).getAs<ArrayAttr>("targets")[0]).getValue();
      std::string prefix="~GGHostMemoryPlatformWrapper|"+std::string(i<2?"GGHostMemoryPlatformWrapper":"GGHostMemoryReadResponseWrapper")+">";
      require(target.starts_with(prefix),"copied/consumed target identity mismatch");
    }
    for(unsigned bad=0;bad<11;++bad) {
      auto test=fixture(ctx);auto tc=*test->getOps<CircuitOp>().begin();auto t=top(tc,"GGHostMemoryReadResponseWrapper");OpBuilder b(&ctx);
      if(bad==0)tc->removeAttr("rawAnnotations");
      else if(bad==1)tc.setName("Wrong");
      else if(bad==2)top(tc,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
      else if(bad==3||bad==4){b.setInsertionPointToEnd(tc.getBodyBlock());b.create<FModuleOp>(tc.getLoc(),b.getStringAttr(bad==3?"GGHostMemoryPlatformWrapper":"GGHostMemoryPortAdapter"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
      else if(bad==5){for(auto x:t.getOps<ConnectOp>())if(x.getDest()==t.getBodyBlock()->getArgument(3)){x.erase();break;}}
      else if(bad==6){for(auto x:t.getOps<StrictConnectOp>())if(x.getSrc()==t.getBodyBlock()->getArgument(0)){x.erase();break;}}
      else if(bad==7){b.setInsertionPointToStart(top(tc,"GGHostMemoryReadWrapper").getBodyBlock());b.create<InstanceOp>(tc.getLoc(),t,"nested");}
      else if(bad==8){b.setInsertionPointToStart(t.getBodyBlock());b.create<SubfieldOp>(tc.getLoc(),t.getBodyBlock()->getArgument(3),"aw");}
      else if(bad==9){auto e=top(tc,"GGFASEDTokenEngine");auto key=e->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");NamedAttrList ws(key.getAs<DictionaryAttr>("axi4Widths")),k(key);ws.set("idBits",b.getI64IntegerAttr(99));k.set("axi4Widths",ws.getDictionary(&ctx));e->setAttr("goldengate.bridgeConstructor",k.getDictionary(&ctx));}
      else {for(auto i:t.getOps<InstanceOp>())if(i.getName()=="host_memory_buffer"){i.setNameAttr(b.getStringAttr("Wrong"));break;}}
      auto s=dump(*test);require(failed(goldengate::specializeHostMemoryPort(tc,error))&&!error.empty(),"invalid boundary accepted");require(s==dump(*test),"rejection mutated IR");
    }
    auto s=dump(*root);require(failed(goldengate::specializeHostMemoryPort(c,error)),"repeat accepted");require(s==dump(*root),"repeat mutated IR");
    llvm::outs()<<"12 atomic rejections; existing bodies, wrapper bindings and copied/consumed annotation targets preserved\n";
    if(argc>=2){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"cannot write helper");os<<"module { firrtl.circuit \"GGHostMemoryPortAdapter\" {\n";top(c,"GGHostMemoryPortAdapter")->print(os);os<<"\n} }\n";}
    if(argc>=3) {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"real parse failed");auto rc=*real->getOps<CircuitOp>().begin();
      auto n=rc->getAttrOfType<ArrayAttr>("rawAnnotations").size();
      require(succeeded(goldengate::specializeHostMemoryPort(rc,error)),error);require(succeeded(verify(*real)),"real invalid");bindings(rc);
      require(rc->getAttrOfType<ArrayAttr>("rawAnnotations").size()==n,"real annotation count changed");
      llvm::outs()<<"Real Rocket boundary: "<<top(rc,"GGHostMemoryPlatformWrapper").getNumPorts()<<" ports, "<<n<<" annotations\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
