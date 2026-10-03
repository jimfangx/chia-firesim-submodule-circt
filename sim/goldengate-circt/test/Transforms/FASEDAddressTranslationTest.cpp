// See LICENSE for license details.
#include "goldengate/FASEDAddressTranslation.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <functional>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0){
  auto token=[](StringRef fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs.str()+">>";};
  auto address=token("id: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>");
  std::string s="module { firrtl.circuit \"GGFASEDHostMemoryWrapper\" { firrtl.module @GGFASEDHostMemoryWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %fased_host_mem: !firrtl.bundle<aw: "+address+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<4>, resp: uint<2>")+", ar: "+address+", r flip: "+token("id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>")+">) {} firrtl.module @GGFASEDTokenEngine() {} } }";
  if(bad==1)s.replace(s.find("addr: uint<35>"),std::string("addr: uint<35>").size(),"addr: uint<34>");
  if(bad==2)s.replace(s.find("out %fased_host_mem"),3,"in");
  if(bad==3)s.replace(s.find("!firrtl.clock"),13,"!firrtl.uint<1>");
  if(bad==4)s.replace(s.find("b flip:"),7,"b:");
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  SmallVector<Attribute> as;
  for(auto n:{"diagnostic","hostReset","fased_host_mem.aw.bits.addr","fased_host_mem.r.bits.data","fased_host_mem"})
    as.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDHostMemoryWrapper|GGFASEDHostMemoryWrapper>"+std::string(n))}))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(as));
  SmallVector<Attribute> sets;
  const uint64_t base[]{0x80000000ULL,0x100000000ULL,0x200000000ULL,0x400000000ULL};
  const uint64_t mask[]{0x7fffffffULL,0xffffffffULL,0x1ffffffffULL,0x7fffffffULL};
  for(unsigned j=0;j<4;++j)sets.push_back(b.getDictionaryAttr({
    b.getNamedAttr("base",b.getI64IntegerAttr(base[j])),
    b.getNamedAttr("mask",b.getI64IntegerAttr(mask[j]+(bad==5&&j==3)))}));
  auto k=b.getDictionaryAttr({b.getNamedAttr("memoryRegionName",b.getStringAttr(bad==6?"Other":"MainMemory_0")),
    b.getNamedAttr("axi4Widths",b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),
      b.getNamedAttr("dataBits",b.getI64IntegerAttr(bad==7?128:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))})),
    b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("address",b.getArrayAttr(sets))}))});
  top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k);
  if(bad==8)c->removeAttr("rawAnnotations");
  if(bad==9){auto m=top(c,"GGFASEDHostMemoryWrapper");b.setInsertionPointToStart(m.getBodyBlock());b.create<InstanceOp>(c.getLoc(),m,"used");}
  if(bad==10)c.setName("Wrong");
  if(bad==11 || bad==12 || bad==13){b.setInsertionPointToEnd(c.getBodyBlock());auto m=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==11?"GGFASEDAddressTranslation":bad==12?"GGFASEDAddressTranslationWrapper":"OtherEngine"),top(c,"GGFASEDHostMemoryWrapper").getConventionAttr(),ArrayRef<PortInfo>{});if(bad==13)m->setAttr("goldengate.bridgeConstructor",k);}
  if(bad==14)top(c,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
  return root;
}
std::string leaf(Value v,FModuleOp m){
  if(auto f=v.getDefiningOp<SubfieldOp>())return leaf(f.getInput(),m)+"."+f.getFieldName().str();
  if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str();
  return "computed";
}
// Evaluate generated SSA for vectors spanning both virtual bounds, address
// truncation, disabled requests and reset. Expectations come from the golden
// AXI4AddressTranslation boundary, independently of the pass construction.
uint64_t evaluate(Value v,FModuleOp m,const std::map<std::string,uint64_t> &inputs){
  auto l=leaf(v,m);if(l!="computed")return inputs.at(l);
  auto op=v.getDefiningOp();auto x=[&](unsigned i){return evaluate(op->getOperand(i),m,inputs);};
  uint64_t result;
  if(auto c=dyn_cast<ConstantOp>(op))result=c.getValue().getZExtValue();
  else if(isa<AddPrimOp>(op))result=x(0)+x(1);
  else if(auto bits=dyn_cast<BitsPrimOp>(op))result=x(0)>>bits.getLo();
  else if(isa<NotPrimOp>(op))result=~x(0);
  else if(isa<OrPrimOp>(op))result=x(0)|x(1);
  else if(isa<LEQPrimOp>(op))result=x(0)<=x(1);
  else if(isa<GEQPrimOp>(op))result=x(0)>=x(1);
  else throw std::runtime_error("unsupported expression");
  unsigned width=*cast<UIntType>(v.getType()).getWidth();
  return result&((uint64_t(1)<<width)-1);
}
}
int main(int argc,char **argv){
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
    auto inner=top(c,"GGFASEDHostMemoryWrapper");auto before=dump(inner);
    require(succeeded(goldengate::addFASEDAddressTranslation(c,error)),error);
    require(succeeded(verify(*root)),"invalid translation IR");
    auto h=top(c,"GGFASEDAddressTranslation");auto wrapper=top(c,"GGFASEDAddressTranslationWrapper");
    require(wrapper.getNumPorts()==4 && before==dump(inner),"inner state/ports changed");
    SmallVector<StrictConnectOp> translated;unsigned direct=0;
    for(auto connect:h.getOps<StrictConnectOp>()){
      auto d=leaf(connect.getDest(),h),s=leaf(connect.getSrc(),h);
      if(s=="computed")translated.push_back(connect);
      else{require(d.substr(d.find('.'))==s.substr(s.find('.')) && d.substr(0,d.find('.'))!=s.substr(0,s.find('.')),"AXI leaf permutation");++direct;}
    }
    require(direct==35 && translated.size()==2,"AXI pass-through leaves missing");
    SmallVector<AssertOp> checks;for(auto a:h.getOps<AssertOp>())checks.push_back(a);
    require(checks.size()==4,"four address checks missing");
    unsigned vectors=0;
    for(uint64_t aw:{0ULL,0x7fffffffULL,0x80000000ULL,0xffffffffULL,0x400000000ULL,0x47fffffffULL,0x480000000ULL,0x7ffffffffULL})
      for(uint64_t ar:{0x7fffffffULL,0x80000000ULL,0x47fffffffULL,0x480000000ULL})
        for(unsigned valid=0;valid<4;++valid)for(unsigned reset=0;reset<2;++reset)for(unsigned ready=0;ready<4;++ready){
          std::map<std::string,uint64_t> inputs{{"clock",1},{"reset",reset},
            {"in.aw.bits.addr",aw},{"in.ar.bits.addr",ar},{"in.aw.valid",valid&1},{"in.ar.valid",(valid>>1)&1},
            {"out.aw.ready",ready&1},{"out.ar.ready",ready>>1}};
          for(auto t:translated){auto ch=leaf(t.getDest(),h)=="out.aw.bits.addr"?aw:ar;
            require(evaluate(t.getSrc(),h,inputs)==((ch-0x80000000ULL)&0x3ffffffffULL),"translated address differs");}
          for(unsigned j=0;j<4;++j){auto addr=j%2?ar:aw;bool active=valid&(j%2?2:1);
            bool expected=!active || (j<2?addr<=0x47fffffffULL:addr>=0x80000000ULL);
            require(leaf(checks[j]->getOperand(0),h)=="clock","assertion clock changed");
            require(evaluate(checks[j]->getOperand(1),h,inputs)==expected &&
                    evaluate(checks[j]->getOperand(2),h,inputs)==!reset,"assertion bound/reset/backpressure differs");}
          ++vectors;
        }
    auto as=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(as.size()==5,"annotation loss");
    for(auto [i,a]:llvm::enumerate(as)){
      auto s=cast<StringAttr>(cast<DictionaryAttr>(a).getAs<ArrayAttr>("targets")[0]).getValue();
      require(s.starts_with("~GGFASEDAddressTranslationWrapper|"),"circuit target not updated");
      require(s.contains(i<2?"|GGFASEDAddressTranslationWrapper>":"|GGFASEDHostMemoryWrapper>"),"pre-translation identity lost");
    }
    auto good=dump(*root);require(failed(goldengate::addFASEDAddressTranslation(c,error)) && good==dump(*root),"repeat mutated IR");
    for(unsigned bad=1;bad<=14;++bad){auto r=fixture(ctx,bad);auto c=*r->getOps<CircuitOp>().begin();auto original=dump(*r);require(failed(goldengate::addFASEDAddressTranslation(c,error)),"bad boundary accepted");require(original==dump(*r),"rejection mutated IR");}
    if(argc==2){std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"cannot write helper");out<<"module { firrtl.circuit \"GGFASEDAddressTranslation\" {\n";h->print(out);out<<"\n} }\n";}
    llvm::outs()<<"FASED translation: 35 pass-through leaves, two addresses, four assertions, "<<vectors<<" semantic vectors and 15 atomic rejections passed\n";
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
