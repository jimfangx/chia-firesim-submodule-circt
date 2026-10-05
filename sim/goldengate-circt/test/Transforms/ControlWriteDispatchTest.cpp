// See LICENSE for license details.
#include "goldengate/ControlWriteDispatch.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,llvm::StringRef s){if(!ok)throw std::runtime_error(s.str());}
FModuleOp named(CircuitOp c,llvm::StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned slaveCount=11) {
  OpBuilder b(&ctx);auto loc=b.getUnknownLoc();auto root=ModuleOp::create(loc);b.setInsertionPointToStart(root.getBody());
  auto c=b.create<CircuitOp>(loc,b.getStringAttr("GGControlWriteRouteWrapper"));b.setInsertionPointToStart(c.getBodyBlock());
  SmallVector<PortInfo> ports;auto add=[&](llvm::StringRef n,unsigned w,Direction d){ports.push_back({b.getStringAttr(n),UIntType::get(&ctx,w,false),d});};
  ports.push_back({b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In});add("hostReset",1,Direction::In);
  for(auto n:{"ctrl_decode_aw_route","ctrl_write_route_w_route"})add(n,slaveCount,Direction::Out);
  add("ctrl_decode_aw_addr",25,Direction::In);
  for(auto n:{"ctrl_write_route_aw_slave_valid","ctrl_write_route_w_slave_valid","ctrl_error_aw_ready","ctrl_error_w_ready"})add(n,1,Direction::Out);
  for(auto n:{"ctrl_write_route_w_last","ctrl_write_route_aw_slave_ready","ctrl_write_route_w_slave_ready","ctrl_error_aw_valid","ctrl_error_w_valid","ctrl_error_w_bits_last"})add(n,1,Direction::In);
  add("ctrl_error_aw_bits_addr",25,Direction::In);add("ctrl_error_aw_bits_id",12,Direction::In);add("other",8,Direction::Out);
  b.create<FModuleOp>(loc,b.getStringAttr("GGControlWriteRouteWrapper"),ConventionAttr::get(&ctx,Convention::Internal),ports);
  auto decoder=b.create<FModuleOp>(loc,b.getStringAttr("GGControlAddressDecode"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
  SmallVector<Attribute> rows(slaveCount,b.getDictionaryAttr({}));decoder->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
  SmallVector<Attribute> annos;
  for(auto n:{"other","ctrl_error_aw_valid","ctrl_write_route_aw_slave_ready","ctrl_decode_aw_addr"})annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~GGControlWriteRouteWrapper|GGControlWriteRouteWrapper>"+std::string(n)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));return root;
}
struct Interpreter {
  FModuleOp m;std::map<std::string,Value>args;llvm::DenseMap<Value,Value>drivers;llvm::DenseMap<Value,uint64_t>memo;
  Interpreter(FModuleOp module):m(module) {
    for(auto [i,p]:llvm::enumerate(m.getPorts()))args[p.name.getValue().str()]=m.getBodyBlock()->getArgument(i);
    for(auto conn:m.getOps<StrictConnectOp>())require(drivers.try_emplace(conn.getDest(),conn.getSrc()).second,"duplicate driver");
  }
  uint64_t eval(Value v) {
    if(memo.count(v))return memo.lookup(v);auto *op=v.getDefiningOp();uint64_t n;
    if(drivers.count(v))n=eval(drivers.lookup(v));
    else if(auto c=dyn_cast_or_null<ConstantOp>(op))n=c.getValue().getZExtValue();
    else if(isa_and_nonnull<AndPrimOp>(op))n=eval(op->getOperand(0))&eval(op->getOperand(1));
    else if(isa_and_nonnull<EQPrimOp>(op))n=eval(op->getOperand(0))==eval(op->getOperand(1));
    else if(isa_and_nonnull<MuxPrimOp>(op))n=eval(op->getOperand(eval(op->getOperand(0))?1:2));
    else if(auto bit=dyn_cast_or_null<BitsPrimOp>(op))n=(eval(bit.getInput())>>bit.getLo())&((1ULL<<(bit.getHi()-bit.getLo()+1))-1);
    else throw std::runtime_error("missing driver/unsupported operation");
    memo[v]=n;return n;
  }
};
void behavior(MLIRContext &ctx, unsigned slaveCount) {
  auto root=fixture(ctx,slaveCount);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addControlWriteDispatch(c,error)),error);require(succeeded(verify(*root)),"dispatch IR invalid");
  Interpreter sim(named(c,"GGControlWriteDispatch"));std::mt19937_64 rng(178);unsigned cases=0;
  auto sample=[&](uint64_t ar,uint64_t wr,bool av,bool wv,uint64_t awr,uint64_t wwr){
    sim.memo.clear();std::map<std::string,uint64_t>input{{"aw_route",ar},{"w_route",wr},{"aw_valid",av},{"w_valid",wv}};
    for(auto [n,width]:std::map<std::string,unsigned>{{"master_aw_bits_addr",25},{"master_aw_bits_len",8},{"master_aw_bits_id",12},{"master_w_bits_data",32},{"master_w_bits_last",1}})input[n]=rng()&((1ULL<<width)-1);
    for(unsigned i=0;i<=slaveCount;++i) {
      std::string p=(i==slaveCount?"err_slave":"slave_"+std::to_string(i))+std::string("_");input[p+"aw_ready"]=(awr>>i)&1;input[p+"w_ready"]=(wwr>>i)&1;
    }
    for(auto [n,v]:input)sim.memo[sim.args.at(n)]=v;
    for(auto ch:{"aw","w"}) {
      uint64_t route=std::string(ch)=="aw"?ar:wr;bool valid=std::string(ch)=="aw"?av:wv;uint64_t ready=std::string(ch)=="aw"?awr:wwr;
      // Independent highest-index rule from Scala's ordered when assignments.
      unsigned selected=slaveCount;if(route) {selected=0;for(uint64_t r=route;r>>=1;)++selected;}
      require(sim.eval(sim.args.at(std::string(ch)+"_ready"))==((ready>>selected)&1),"selected readiness differs");
      for(unsigned i=0;i<=slaveCount;++i) {
        std::string p=(i==slaveCount?"err_slave":"slave_"+std::to_string(i))+std::string("_")+ch;
        require(sim.eval(sim.args.at(p+"_valid"))==unsigned(valid&&(i==slaveCount?route==0:((route>>i)&1))),"valid broadcast differs");
        for(auto field:{"addr","len","id","data","last"})if((std::string(ch)=="aw")== (std::string(field)!="data"&&std::string(field)!="last"))
          require(sim.eval(sim.args.at(p+"_bits_"+field))==input.at(std::string("master_")+ch+"_bits_"+field),"payload broadcast differs");
      }
    }
    ++cases;
  };
  const uint64_t routeMask=(uint64_t(1)<<slaveCount)-1;
  const uint64_t readyMask=slaveCount==63?~uint64_t(0):(uint64_t(1)<<(slaveCount+1))-1;
  auto route=[&](unsigned i){return i==slaveCount?uint64_t(0):uint64_t(1)<<i;};
  // Independently select every AW/W endpoint, including the error lane.
  for(unsigned ar=0;ar<=slaveCount;++ar)for(unsigned wr=0;wr<=slaveCount;++wr)
    for(unsigned flags=0;flags<4;++flags)for(unsigned r=0;r<4;++r) {
      uint64_t ready=r==0?0:r==1?readyMask:r==2?0xaaaaaaaaaaaaaaaaULL&readyMask:0x5555555555555555ULL&readyMask;
      sample(route(ar),route(wr),flags&1,flags&2,ready,readyMask^ready);
    }
  // Retain exhaustive baseline readiness coverage for all eleven slaves + error.
  if(slaveCount==11)
    for(unsigned i=0;i<=slaveCount;++i)for(unsigned flags=0;flags<4;++flags)for(unsigned r=0;r<4096;++r)
      sample(route(i),route(i),flags&1,flags&2,r,readyMask^r);
  for(unsigned i=0;i<10000;++i)
    sample(rng()&routeMask,rng()&routeMask,rng()&1,rng()&1,rng()&readyMask,rng()&readyMask);
  sample(routeMask,routeMask,true,true,readyMask,readyMask);
  llvm::outs()<<slaveCount<<" slaves: "<<cases<<" payload/readiness/valid cases passed, including independent AW/W routes and multi-bit priority\n";
}
void mapping(MLIRContext &ctx, unsigned slaveCount) {
  auto root=fixture(ctx,slaveCount);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  auto old=named(c,"GGControlWriteRouteWrapper");require(succeeded(goldengate::addControlWriteDispatch(c,error)),error);
  auto top=named(c,"GGControlWriteDispatchWrapper"),helper=named(c,"GGControlWriteDispatch");
  require(old.getNumPorts()==18&&top.getNumPorts()==14+9*slaveCount&&helper.getNumPorts()==20+9*slaveCount,"wrong port count");
  std::map<std::string,Value>oldArgs,newArgs,ports;
  for(auto [i,p]:llvm::enumerate(top.getPorts()))newArgs[p.name.getValue().str()]=top.getBodyBlock()->getArgument(i);
  InstanceOp sim,dispatch;for(auto i:top.getOps<InstanceOp>()){if(i.getName()=="sim")sim=i;else if(i.getName()=="controlWriteDispatch")dispatch=i;}
  require(sim&&dispatch,"instances lost");
  for(auto [i,p]:llvm::enumerate(old.getPorts()))oldArgs[p.name.getValue().str()]=sim.getResult(i);
  for(auto [i,p]:llvm::enumerate(helper.getPorts()))ports[p.name.getValue().str()]=dispatch.getResult(i);
  llvm::DenseMap<Value,Value>drivers;
  for(auto conn:top.getOps<StrictConnectOp>())require(drivers.try_emplace(conn.getDest(),conn.getSrc()).second,"duplicate wrapper driver");
  auto wire=[&](Value d,Value s){require(drivers.lookup(d)==s,"dispatch binding differs");};
  for(auto [h,n]:std::map<std::string,std::string>{{"aw_route","ctrl_decode_aw_route"},{"w_route","ctrl_write_route_w_route"},{"aw_valid","ctrl_write_route_aw_slave_valid"},{"w_valid","ctrl_write_route_w_slave_valid"},{"err_slave_aw_ready","ctrl_error_aw_ready"},{"err_slave_w_ready","ctrl_error_w_ready"}})wire(ports.at(h),oldArgs.at(n));
  for(auto [h,n]:std::map<std::string,std::string>{{"aw_ready","ctrl_write_route_aw_slave_ready"},{"w_ready","ctrl_write_route_w_slave_ready"},{"err_slave_aw_valid","ctrl_error_aw_valid"},{"err_slave_aw_bits_addr","ctrl_error_aw_bits_addr"},{"err_slave_aw_bits_id","ctrl_error_aw_bits_id"},{"err_slave_w_valid","ctrl_error_w_valid"},{"err_slave_w_bits_last","ctrl_error_w_bits_last"}}) {
    require(!newArgs.count(n),"consumed input still exposed");wire(oldArgs.at(n),ports.at(h));
  }
  wire(ports.at("master_aw_bits_addr"),newArgs.at("ctrl_decode_aw_addr"));wire(ports.at("master_w_bits_last"),newArgs.at("ctrl_write_route_w_last"));
  for(auto p:helper.getPorts()) {
    auto n=p.name.getValue().str();auto found=newArgs.find("ctrl_write_dispatch_"+n);
    if(found!=newArgs.end()) {if(p.direction==Direction::In)wire(ports.at(n),found->second);else wire(found->second,ports.at(n));}
  }
  auto a=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(a.size()==4,"annotations lost");
  for(auto [i,x]:llvm::enumerate(a))require(cast<DictionaryAttr>(x).getAs<StringAttr>("target").getValue().starts_with(i==1||i==2?"~GGControlWriteDispatchWrapper|GGControlWriteRouteWrapper>":"~GGControlWriteDispatchWrapper|GGControlWriteDispatchWrapper>"),"consumed or copied identity differs");
}
void rejection(MLIRContext &ctx) {
  for(unsigned bad=0;bad<14;++bad) {
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGControlWriteRouteWrapper");OpBuilder b(&ctx);
    if(bad==0)c.setName("WrongTop");if(bad==1)c->removeAttr("rawAnnotations");
    if(bad==2||bad==3){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[bad==2?16:17]=b.getStringAttr(bad==2?"wrongID":"ctrl_write_dispatch_slave_0_aw_ready");top.setPortNames(names);}
    if(bad==4){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad==5||bad==6){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==5?"GGControlWriteDispatch":"GGControlWriteDispatchWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==7)named(c,"GGControlAddressDecode")->removeAttr("goldengate.controlRegions");
    if(bad==8)named(c,"GGControlAddressDecode")->setAttr("goldengate.controlRegions",b.getArrayAttr({}));
    if(bad==9||bad==10){SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end());types[bad==9?4:16]=TypeAttr::get(UIntType::get(&ctx,24,false));top.setPortTypes(types);}
    if(bad==11)named(c,"GGControlAddressDecode")->setAttr("goldengate.controlRegions",b.getArrayAttr(SmallVector<Attribute>(64,b.getDictionaryAttr({}))));
    if(bad==12||bad==13){SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end());types[bad==12?2:3]=TypeAttr::get(UIntType::get(&ctx,13,false));top.setPortTypes(types);}
    std::string before,after,error;{llvm::raw_string_ostream o(before);root->print(o);}require(failed(goldengate::addControlWriteDispatch(c,error)),"invalid boundary accepted");{llvm::raw_string_ostream o(after);root->print(o);}require(before==after,"rejection mutated IR");
  }
  llvm::outs()<<"Mapping and fourteen atomic rejection cases passed\n";
}
}
int main(){try{MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();for(unsigned n:{1,2,11,13,63}){behavior(ctx,n);mapping(ctx,n);}rejection(ctx);return 0;}catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}}
