// See LICENSE for license details.
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlReadArbiter.h"
#include "goldengate/ControlReadDispatch.h"
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlRegionGeometry.h"
#include "goldengate/ControlWriteArbiter.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteTracker.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) {
  if (!ok) throw std::runtime_error(why.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
// Execute the actual emitted helper ops with arbitrary-width values. In
// particular a one-hot bit above 63 must survive decode, queuing and dispatch.
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> args, drivers;
  std::map<std::string, APInt> memo, state;
  std::vector<APInt> memory;
  MemOp ram;
  static unsigned width(Value v) { return cast<UIntType>(v.getType()).getWidth().value(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto r = v.getDefiningOp<RegOp>()) return r.getName().str();
    if (auto r = v.getDefiningOp<RegResetOp>()) return r.getName().str();
    if (auto a = dyn_cast<BlockArgument>(v)) return module.getPortName(a.getArgNumber()).str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto [i,p] : llvm::enumerate(m.getPorts())) args[p.name.getValue().str()] = m.getBodyBlock()->getArgument(i);
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()),c.getSrc()).second,"duplicate driver");
    for (auto r : m.getOps<RegResetOp>()) state.emplace(key(r.getResult()), r.getResetValue().getDefiningOp<ConstantOp>().getValue());
    for (auto r : m.getOps<RegOp>()) state.emplace(key(r.getResult()), APInt(width(r.getResult()),0));
    for (auto m : module.getOps<MemOp>()) {
      require(!ram,"expected one route queue memory"); ram=m;
      memory.assign(ram.getDepth(),APInt(cast<UIntType>(ram.getDataType()).getWidth().value(),0));
    }
  }
  APInt eval(Value v) {
    auto name=key(v); if (memo.count(name)) return memo.at(name);
    unsigned w=width(v); auto *op=v.getDefiningOp(); APInt n(w,0);
    if (state.count(name)) n=state.at(name);
    else if (drivers.count(name)) n=eval(drivers.at(name));
    else if (auto c=dyn_cast_or_null<ConstantOp>(op)) n=c.getValue();
    else if (auto f=dyn_cast_or_null<SubfieldOp>(op)) {
      require(ram && f.getInput()==ram.getResult(0) && f.getFieldName()=="data","unsupported field");
      n=memory.at(get(key(ram.getResult(0))+".addr").getZExtValue());
    } else if (isa_and_nonnull<MuxPrimOp>(op)) n=eval(op->getOperand(eval(op->getOperand(0)).isZero()?2:1));
    else if (auto b=dyn_cast_or_null<BitsPrimOp>(op)) n=eval(b.getInput()).extractBits(b.getHi()-b.getLo()+1,b.getLo());
    else if (isa_and_nonnull<PadPrimOp>(op)) n=eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<NotPrimOp>(op)) n=~eval(op->getOperand(0));
    else if (op && op->getNumOperands()==2) {
      auto a=eval(op->getOperand(0)), b=eval(op->getOperand(1));
      if (isa<CatPrimOp>(op)) n=a.concat(b);
      else {
        unsigned size=std::max({a.getBitWidth(),b.getBitWidth(),w}); a=a.zextOrTrunc(size); b=b.zextOrTrunc(size);
        if (isa<AndPrimOp>(op)) n=a&b;
        else if (isa<OrPrimOp>(op)) n=a|b;
        else if (isa<XorPrimOp>(op)) n=a^b;
        else if (isa<AddPrimOp>(op)) n=a+b;
        else if (isa<EQPrimOp>(op)) n=APInt(1,a==b);
        else if (isa<LTPrimOp>(op)) n=APInt(1,a.ult(b));
        else if (isa<GTPrimOp>(op)) n=APInt(1,a.ugt(b));
        else throw std::runtime_error("unsupported binary op");
      }
    } else throw std::runtime_error("missing input " + name);
    n=n.zextOrTrunc(w); memo.insert_or_assign(name,n); return n;
  }
  APInt get(const std::string &name) { return eval(drivers.at(name)); }
  void set(const std::string &name, const APInt &value) {
    memo.insert_or_assign(name,value);
  }
  void set(const std::string &name, uint64_t value) {
    set(name,APInt(args.count(name)?width(args.at(name)):state.at(name).getBitWidth(),value));
  }
  void edge() {
    std::map<std::string,APInt> next;
    for (auto r : module.getOps<RegOp>()) next.emplace(key(r.getResult()),get(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>()) next.emplace(key(r.getResult()),eval(r.getResetSignal()).isZero()?get(key(r.getResult())):eval(r.getResetValue()));
    if (ram) {
      auto prefix=key(ram.getResult(1))+".";
      if (!get(prefix+"en").isZero()) memory.at(get(prefix+"addr").getZExtValue())=get(prefix+"data");
    }
    state=std::move(next);
  }
};
void exercise(CircuitOp c, unsigned count) {
  std::mt19937_64 random(0x1968+count);
  Interpreter decoder(named(c,"GGControlAddressDecode"));
  Interpreter queue(named(c,"GGControlWriteRoute"));
  Interpreter dispatch(named(c,"GGControlWriteDispatch"));
  unsigned encoded=goldengate::controlRouteWidth(count);
  require(Interpreter::width(decoder.args.at("aw_route"))==count && Interpreter::width(decoder.args.at("aw_target"))==encoded,"decoder geometry");
  for (auto helperName : {"GGControlReadTracker","GGControlWriteTracker"}) {
    auto helper=named(c,helperName);
    require(helper->getAttrOfType<IntegerAttr>("goldengate.trackerSlots").getInt()==64,"transaction slots changed with regions");
    require(helper->getAttrOfType<IntegerAttr>("goldengate.trackerRouteWidth").getInt()==encoded,"tracker route width");
    require(helper->getAttrOfType<IntegerAttr>("goldengate.trackerDequeuePorts").getInt()==count+1,"missing retirement endpoint");
  }
  // Every normal and error address; high bits must survive both AW and AR.
  for (unsigned i=0;i<=count;++i) {
    decoder.memo.clear(); decoder.set("aw_addr",i*4); decoder.set("ar_addr",i*4);
    APInt onehot(count,0); if(i<count)onehot.setBit(i);
    for(auto ch:{"aw","ar"}) {
      require(decoder.get(std::string(ch)+"_route")==onehot,"one-hot route truncated");
      require(decoder.get(std::string(ch)+"_target").getZExtValue()==i,"error/normal route truncated");
    }
  }
  Interpreter reads(named(c,"GGControlReadDispatch"));
  for(unsigned selected=0;selected<=count;++selected)for(bool capacity:{false,true})for(bool ready:{false,true}) {
    reads.memo.clear();APInt onehot(count,0);if(selected<count)onehot.setBit(selected);
    reads.set("route",onehot);reads.set("target",selected);reads.set("tracker_ready",capacity);
    reads.set("master_ar.valid",APInt(1,1));
    reads.set("master_ar.bits.id",APInt(12,selected));
    reads.set("master_ar.bits.addr",APInt(32,selected*4));
    for(unsigned i=0;i<=count;++i)reads.set((i==count?"err_slave_ar":"slave_"+std::to_string(i)+"_ar")+".ready",APInt(1,ready));
    require(reads.get("master_ar.ready").getZExtValue()==(capacity&&ready),"AR capacity/ready at high index");
    require(reads.get("track_valid").getZExtValue()==ready,"AR tracker acceptance");
    require(reads.get("track_target").getZExtValue()==selected,"AR target truncation");
    for(unsigned i=0;i<=count;++i)require(reads.get((i==count?"err_slave_ar":"slave_"+std::to_string(i)+"_ar")+".valid").getZExtValue()==(capacity&&i==selected),"AR high-index validity");
  }
  // Fill, stall, drain, wrap, and reset a queue containing high one-hot bits.
  // Include zero/error and malformed multi-hot routes: the queue transports
  // every bit; dispatch preserves Scala's highest-index ready priority.
  std::deque<APInt> expected;
  for(unsigned cycle=0;cycle<8*count+40;++cycle) {
    bool reset=cycle==0 || cycle==4*count+3;
    bool aw=cycle<2*count || (random()&1), w=cycle>=count && (random()%4!=0);
    bool last=random()%4!=0, tracker=random()%5!=0, aws=random()%5!=0, ws=random()%5!=0;
    APInt route(count,0); if(cycle%(count+1)<count)route.setBit(cycle%(count+1));
    if(cycle%7==0)route.setBit(count-1);
    queue.memo.clear(); queue.set("reset",reset);queue.set("aw_route",route);
    queue.set("aw_valid",aw);queue.set("aw_tracker_ready",tracker);queue.set("aw_slave_ready",aws);
    queue.set("w_valid",w);queue.set("w_last",last);queue.set("w_slave_ready",ws);
    bool ready=expected.size()<count,valid=!expected.empty();
    require(queue.get("aw_ready").getZExtValue()==(ready&&tracker&&aws),"queue readiness");
    require(queue.get("w_ready").getZExtValue()==(valid&&ws),"queue valid");
    APInt output=queue.get("w_route");
    if(valid)require(output==expected.front(),"queued high route bit lost");
    dispatch.memo.clear();dispatch.set("aw_route",route);dispatch.set("w_route",output);
    dispatch.set("aw_valid",aw&&ready&&tracker);dispatch.set("w_valid",w&&valid);
    for(auto [name,value]:std::map<std::string,uint64_t>{{"master_aw_bits_addr",cycle*4},{"master_aw_bits_len",7},{"master_aw_bits_id",41},{"master_w_bits_data",0xabcdef},{"master_w_bits_last",last}})dispatch.set(name,value);
    for(unsigned i=0;i<=count;++i)for(auto ch:{"aw","w"})dispatch.set((i==count?"err_slave":"slave_"+std::to_string(i))+"_"+ch+"_ready",(cycle+i)%3!=0);
    for(auto ch:{"aw","w"}) {
      auto bits=StringRef(ch)=="aw"?route:output;
      bool inputValid=StringRef(ch)=="aw"?(aw&&ready&&tracker):(w&&valid);
      unsigned chosen=bits.isZero()?count:bits.getActiveBits()-1;
      require(dispatch.get(std::string(ch)+"_ready").getZExtValue()==((cycle+chosen)%3!=0),"dispatch high-index priority");
      for(unsigned i=0;i<=count;++i)require(dispatch.get((i==count?"err_slave":"slave_"+std::to_string(i))+"_"+ch+"_valid").getZExtValue()==(inputValid&&(i==count?bits.isZero():bits[i])),"dispatch high-index validity");
    }
    bool push=ready&&aw&&tracker&&aws,pop=valid&&w&&ws&&last;
    queue.edge();
    if(pop)expected.pop_front();if(push)expected.push_back(route);if(reset)expected.clear();
  }
  // Exercise all arbitration sources, including high indices and the error
  // endpoint, under a full set of simultaneous requests and backpressure.
  for(bool read:{false,true}) {
    Interpreter arb(named(c,read?"GGControlReadArbiter":"GGControlWriteArbiter"));
    for(unsigned cursor=0;cursor<=count;++cursor)for(bool ready:{false,true}) {
      arb.memo.clear(); arb.set(read?"lockIdx":"lastGrant",cursor);
      if(read){arb.set("locked",0);arb.set("reset",0);}arb.set("out_ready",ready);
      for(unsigned i=0;i<=count;++i) {
        auto prefix="in_"+std::to_string(i)+"_";
        arb.set(prefix+"valid",1);arb.set(prefix+"bits_resp",0);arb.set(prefix+"bits_id",i);arb.set(prefix+"bits_user",0);
        if(read){arb.set(prefix+"bits_data",i);arb.set(prefix+"bits_last",1);}
      }
      unsigned chosen=(cursor+1)%(count+1);
      require(arb.get("out_bits_id").getZExtValue()==chosen,"high-index RR selection");
      require(arb.get("out_valid").getZExtValue()==1,"RR valid");
      for(unsigned i=0;i<=count;++i)require(arb.get("in_"+std::to_string(i)+"_ready").getZExtValue()==(ready&&chosen==i),"high-index RR ready");
    }
  }
}
void test(MLIRContext &ctx, unsigned count, StringRef output = {}) {
  OpBuilder b(&ctx);auto loc=b.getUnknownLoc();OwningOpRef<ModuleOp> root(ModuleOp::create(loc));
  b.setInsertionPointToStart(root->getBody());auto c=b.create<CircuitOp>(loc,b.getStringAttr("Base"));
  auto u=[&](unsigned w){return UIntType::get(&ctx,w,false);};
  b.setInsertionPointToStart(c.getBodyBlock());
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},{b.getStringAttr("hostReset"),u(1),Direction::In}};
  b.create<FModuleOp>(loc,b.getStringAttr("Base"),ConventionAttr::get(&ctx,Convention::Internal),ports);
  c->setAttr("rawAnnotations",b.getArrayAttr({}));std::string error;
  auto stage=[&](LogicalResult result){require(succeeded(result),error);require(succeeded(verify(*root)),"invalid composed IR");};
  stage(goldengate::addControlErrorSlave(c,32,12,"Base",error));
  std::vector<std::string> names;names.reserve(count);
  for(unsigned i=0;i<count;++i)names.push_back("Widget_"+std::to_string(i));
  SmallVector<goldengate::ControlMMIOWidget> widgets;
  for(auto &name:names)widgets.push_back({name,1});
  SmallVector<goldengate::ControlMMIORegion> regions;
  stage(goldengate::allocateControlMMIORegions(32,widgets,regions,error));
  stage(goldengate::addControlAddressDecode(c,32,regions,error));
  stage(goldengate::addControlWriteRoute(c,error));stage(goldengate::addControlWriteDispatch(c,error));
  // Widget-bank binding is independently tested. Leave normal slaves exposed
  // here to test the complete generic routing path without a design fixture.
  auto inner=named(c,c.getName());b.setInsertionPointToEnd(c.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr("GGControlWidgetWriteWrapper"),inner.getConventionAttr(),inner.getPorts());
  b.setInsertionPointToStart(wrapper.getBodyBlock());auto sim=b.create<InstanceOp>(loc,inner,"sim");
  for(auto [i,p]:llvm::enumerate(inner.getPorts())) {
    Value ext=wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):ext,p.direction==Direction::In?ext:sim.getResult(i));
  }
  wrapper->setAttr("goldengate.controlWriteBindings",b.getArrayAttr({}));c.setNameAttr(wrapper.getNameAttr());
  stage(goldengate::addControlReadDispatch(c,error));stage(goldengate::addControlReadTracker(c,error));
  stage(goldengate::addControlReadArbiter(c,error));stage(goldengate::addControlWriteArbiter(c,error));
  stage(goldengate::addControlWriteTracker(c,error));exercise(c,count);
  // Increasing count must still respect actual address capacity atomically.
  SmallVector<goldengate::ControlMMIORegion> sentinel{{"sentinel",9,4}};
  if(count>1){require(failed(goldengate::allocateControlMMIORegions(2,widgets,sentinel,error)),"address overflow accepted");require(sentinel.size()==1&&sentinel[0].name=="sentinel","failed allocation changed output");}
  if(!output.empty()) {
    std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot write fixture");root->print(out);out<<'\n';
  }
  llvm::outs()<<"Composed control routing passed for "<<count<<" regions, encoded width "<<goldengate::controlRouteWidth(count)<<"\n";
}
}
int main(int argc,char **argv){try{
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  require(!goldengate::validControlRegionCount(0)&&!goldengate::validControlRegionCount(UINT32_MAX),"unrepresentable geometry accepted");
  if(argc!=1) {
    unsigned count=0;
    require(argc==4 && StringRef(argv[1])=="--dump" && !StringRef(argv[2]).getAsInteger(10,count) && goldengate::validControlRegionCount(count),"usage: --dump region-count output.mlir");
    test(ctx,count,argv[3]);
  } else for(unsigned count:{1u,63u,64u,65u,127u,128u,129u})test(ctx,count);
  return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
