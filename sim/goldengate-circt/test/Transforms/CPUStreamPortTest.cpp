// See LICENSE for license details.
#include "goldengate/CPUStreamPort.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  // Recorded pre-assembly scalar boundary, including AW address/len retained
  // by Queue_51 and the full 512-bit R payload. FPGATop provides 12 additional
  // external input fields whose payload has no consumers for zero sinkParams.
  auto root=parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "GGHostMemoryPlatformWrapper" {
    firrtl.module @GGHostMemoryPlatformWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %mem_0: !firrtl.uint<1>, out %cpu_stream_ar_ready: !firrtl.uint<1>, in %cpu_stream_ar_valid: !firrtl.uint<1>, in %cpu_stream_ar_bits_id: !firrtl.uint<16>, in %cpu_stream_ar_bits_addr: !firrtl.uint<64>, in %cpu_stream_ar_bits_len: !firrtl.uint<8>, in %cpu_stream_ar_bits_size: !firrtl.uint<3>, in %cpu_stream_r_ready: !firrtl.uint<1>, out %cpu_stream_r_valid: !firrtl.uint<1>, out %cpu_stream_r_bits_id: !firrtl.uint<16>, out %cpu_stream_r_bits_data: !firrtl.uint<512>, out %cpu_stream_r_bits_last: !firrtl.uint<1>, out %cpu_stream_r_bits_resp: !firrtl.uint<2>, out %cpu_stream_aw_ready: !firrtl.uint<1>, in %cpu_stream_aw_valid: !firrtl.uint<1>, in %cpu_stream_aw_bits_id: !firrtl.uint<16>, in %cpu_stream_aw_bits_size: !firrtl.uint<3>, out %cpu_stream_w_ready: !firrtl.uint<1>, in %cpu_stream_w_valid: !firrtl.uint<1>, in %cpu_stream_w_bits_strb: !firrtl.uint<64>, out %cpu_stream_b_valid: !firrtl.uint<1>, out %cpu_stream_b_bits_id: !firrtl.uint<16>, out %cpu_stream_b_bits_resp: !firrtl.uint<2>, in %cpu_stream_b_ready: !firrtl.uint<1>, in %cpu_stream_aw_bits_addr: !firrtl.uint<64>, in %cpu_stream_aw_bits_len: !firrtl.uint<8>) {}
    firrtl.module @GGCPUStreamRead() attributes {goldengate.streamAddressSpaceBits = 19 : i64} {}
    firrtl.module @GGEmptyCPUStreamWrite() attributes {goldengate.fromHostCPUStreamCount = 0 : i32} {}
    firrtl.module @GGHostMemoryPortAdapter() {}
  } })mlir",&ctx);require(bool(root),"fixture parse");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);auto m=top(c,"GGHostMemoryPlatformWrapper");
  SmallVector<Attribute> annotations;
  for(auto n:{"diagnostic","hostReset","mem_0","cpu_stream_ar_bits_id","cpu_stream_r_bits_data"})
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGHostMemoryPlatformWrapper|GGHostMemoryPlatformWrapper>"+std::string(n))}))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations));
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto child=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryReadResponseWrapper"),ConventionAttr::get(&ctx,Convention::Internal),m.getPorts());
  b.setInsertionPointToStart(m.getBodyBlock());auto sim=b.create<InstanceOp>(c.getLoc(),child,"sim");
  b.create<InstanceOp>(c.getLoc(),top(c,"GGHostMemoryPortAdapter"),"memory_port");
  for(auto [i,p]:llvm::enumerate(m.getPorts())) {
    Value a=m.getBodyBlock()->getArgument(i),v=sim.getResult(i);
    b.create<ConnectOp>(c.getLoc(),p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  return root;
}
std::string path(FModuleOp m,Value v) {
  if(auto f=v.getDefiningOp<SubfieldOp>())return path(m,f.getInput())+"."+f.getFieldName().str();
  if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str();
  auto i=v.getDefiningOp<InstanceOp>();require(bool(i),"unknown wrapper root");
  return i.getName().str()+"."+i.getPortName(cast<OpResult>(v).getResultNumber()).str();
}
void bindings(CircuitOp c) {
  auto m=top(c,"GGCPUStreamPlatformWrapper"),old=top(c,"GGHostMemoryPlatformWrapper");
  std::map<std::string,std::string> got;
  for(auto conn:m.getOps<ConnectOp>())require(got.emplace(path(m,conn.getDest()),path(m,conn.getSrc())).second,"duplicate connection");
  for(auto conn:m.getOps<StrictConnectOp>())require(got.emplace(path(m,conn.getDest()),path(m,conn.getSrc())).second,"duplicate leaf connection");
  unsigned copied=0,consumed=0;
  for(auto p:old.getPorts()) {
    auto n=p.name.getValue().str();bool input=p.direction==Direction::In;
    if(StringRef(n).starts_with("cpu_stream_")) {
      auto scalar="cpu_port.stream_"+n.substr(11);auto child="sim."+n;
      require(got.at(input?child:scalar)==(input?scalar:child),"CPU leaf binding");++consumed;
    } else {
      require(got.at(input?"sim."+n:n)==(input?n:"sim."+n),"copied binding");++copied;
      bool found=false;
      for(auto q:m.getPorts())if(q.name==p.name){found=true;require(q.type==p.type&&q.direction==p.direction&&q.annotations==p.annotations&&q.sym==p.sym,"copied port identity");}
      require(found,"missing copied port");
    }
  }
  require(consumed==25&&got.at("cpu_port.cpu_managed_axi4")=="cpu_managed_axi4"&&got.size()==copied+26,"aggregate/wrapper binding");
  require(m.getNumPorts()==copied+1,"top port count");
}
void behavior(CircuitOp c) {
  auto m=top(c,"GGCPUStreamPortAdapter");
  require(m.getOps<RegOp>().empty()&&m.getOps<RegResetOp>().empty()&&m.getOps<MemOp>().empty()&&m.getOps<InstanceOp>().empty(),"adapter must be combinational");
  std::map<std::string,std::string> drivers;std::map<std::string,unsigned> inputs;
  for(auto conn:m.getOps<StrictConnectOp>())require(drivers.emplace(path(m,conn.getDest()),path(m,conn.getSrc())).second,"duplicate helper driver");
  std::function<void(Type,std::string,bool)> flatten=[&](Type t,std::string n,bool input){
    if(auto bundle=dyn_cast<BundleType>(t)){for(auto e:bundle.getElements())flatten(e.type,n+"."+e.name.getValue().str(),input!=e.isFlip);return;}
    if(input)inputs[n]=*cast<UIntType>(t).getWidth();
  };
  for(auto p:m.getPorts())flatten(p.type,p.name.getValue().str(),p.direction==Direction::In);
  // A direct, width-preserving connection proves identity for every bit,
  // including the 512-bit response. Check the actual IR driver graph once.
  for(auto p:m.getPorts())if(p.name.getValue().starts_with("stream_")) {
      std::string n=p.name.getValue().str(),leaf=n.substr(7);auto split=StringRef(leaf).split('_');
      std::string ext="cpu_managed_axi4."+split.first.str()+".";
      auto rest=split.second;ext+=rest.starts_with("bits_")?"bits."+rest.drop_front(5).str():rest.str();
      bool input=p.direction==Direction::In;auto dest=input?ext:n,src=input?n:ext;
      require(drivers.at(dest)==src,"CPU leaf must be an unconditional identity connection");
  }
  require(drivers.size()==25,"all 25 CPU leaves required");
  unsigned unused=0;
  for(auto [n,w]:inputs)if(StringRef(n).starts_with("cpu_managed_axi4.")) {
    bool used=false;for(auto [d,s]:drivers)used|=s==n;
    if(!used){++unused;require(StringRef(n).contains(".burst")||StringRef(n).contains(".lock")||StringRef(n).contains(".cache")||StringRef(n).contains(".prot")||StringRef(n).contains(".qos")||n=="cpu_managed_axi4.w.bits.data"||n=="cpu_managed_axi4.w.bits.last","unexpected unused input");}
  }
  require(unused==12,"recorded unused metadata/data/last fields");
  llvm::outs()<<"25 unconditional identity leaf connections; 12 unused external fields, including 512-bit W data\n";
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();std::string error;
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    std::map<std::string,std::string> before;for(auto m:c.getOps<FModuleOp>())before[m.getName().str()]=dump(m);
    require(succeeded(goldengate::assembleCPUStreamPort(c,error)),error);require(succeeded(verify(*root)),"invalid CPU platform IR");bindings(c);behavior(c);
    for(auto [n,s]:before)require(dump(top(c,n))==s,"old body changed");
    auto ann=c->getAttrOfType<ArrayAttr>("rawAnnotations");for(auto [i,a]:llvm::enumerate(ann)) {
      auto target=cast<StringAttr>(cast<DictionaryAttr>(a).getAs<ArrayAttr>("targets")[0]).getValue();
      require(target.starts_with("~GGCPUStreamPlatformWrapper|"+std::string(i<3?"GGCPUStreamPlatformWrapper":"GGHostMemoryPlatformWrapper")+">"),"annotation transfer/retention");
    }
    for(unsigned mode=0;mode<10;++mode) {
      auto test=fixture(ctx);auto tc=*test->getOps<CircuitOp>().begin();auto t=top(tc,"GGHostMemoryPlatformWrapper");OpBuilder b(&ctx);
      if(mode==0)tc->removeAttr("rawAnnotations");
      else if(mode==1)tc.setName("Wrong");
      else if(mode==2)top(tc,"GGCPUStreamRead")->removeAttr("goldengate.streamAddressSpaceBits");
      else if(mode==3)top(tc,"GGEmptyCPUStreamWrite")->setAttr("goldengate.fromHostCPUStreamCount",b.getI32IntegerAttr(1));
      else if(mode==4||mode==5){b.setInsertionPointToEnd(tc.getBodyBlock());b.create<FModuleOp>(tc.getLoc(),b.getStringAttr(mode==4?"GGCPUStreamPlatformWrapper":"GGCPUStreamPortAdapter"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
      else if(mode==6){for(auto conn:t.getOps<ConnectOp>())if(path(t,conn.getDest())=="sim.cpu_stream_ar_valid"){conn.erase();break;}}
      else if(mode==7){for(auto conn:t.getOps<ConnectOp>())if(path(t,conn.getDest())=="sim.cpu_stream_ar_valid"){b.setInsertionPoint(conn);b.create<ConnectOp>(conn.getLoc(),conn.getDest(),conn.getSrc());break;}}
      else if(mode==8){b.setInsertionPointToStart(top(tc,"GGHostMemoryReadResponseWrapper").getBodyBlock());b.create<InstanceOp>(tc.getLoc(),t,"nested");}
      else {auto names=t.getPortNamesAttr();SmallVector<Attribute> ns(names.begin(),names.end());for(auto &n:ns)if(cast<StringAttr>(n).getValue()=="cpu_stream_ar_valid")n=b.getStringAttr("cpu_stream_unknown");t.setPortNamesAttr(b.getArrayAttr(ns));}
      auto s=dump(*test);require(failed(goldengate::assembleCPUStreamPort(tc,error))&&!error.empty(),"invalid boundary accepted");require(s==dump(*test),"rejection mutated IR");
    }
    auto s=dump(*root);require(failed(goldengate::assembleCPUStreamPort(c,error)),"repeat accepted");require(s==dump(*root),"repeat mutated IR");
    llvm::outs()<<"11 atomic rejections; old bodies, aggregate bindings and copied/consumed targets preserved\n";
    if(argc>=2){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"cannot write helper");os<<"module { firrtl.circuit \"GGCPUStreamPortAdapter\" {\n";top(c,"GGCPUStreamPortAdapter")->print(os);os<<"\n} }\n";}
    if(argc>=3) {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"real parse");auto rc=*real->getOps<CircuitOp>().begin();auto n=rc->getAttrOfType<ArrayAttr>("rawAnnotations").size();
      require(succeeded(goldengate::assembleCPUStreamPort(rc,error)),error);require(succeeded(verify(*real)),"real invalid");bindings(rc);
      require(rc->getAttrOfType<ArrayAttr>("rawAnnotations").size()==n,"real annotation count");
      llvm::outs()<<"Real Rocket boundary: "<<top(rc,"GGCPUStreamPlatformWrapper").getNumPorts()<<" ports, "<<n<<" annotations\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
