// See LICENSE for license details.
#include "goldengate/FASEDTimingCycle.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <functional>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDResponseReleaserWrapper\" { firrtl.module @GGFASEDResponseReleaserWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto loc=c.getLoc();
  auto u=[&](unsigned w){return UIntType::get(&ctx,w,false);};
  const llvm::StringRef names[]{"hostClock","fased_model_reset","fased_tfire"};
  SmallVector<PortInfo> ps{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("fased_model_reset"),u(1),Direction::Out},{b.getStringAttr("fased_tfire"),u(1),Direction::Out},
    {b.getStringAttr("other"),u(8),Direction::In}};
  if(mode>=1&&mode<=3)ps[mode-1].name=b.getStringAttr("missing");
  if(mode==4)ps[1].type=u(2);if(mode==5)ps[2].direction=Direction::In;
  const llvm::StringRef extras[]{"fased_timing_cycle","fased_read_latency","fased_write_latency","fased_read_release_cycle","fased_write_release_cycle"};
  if(mode>=14&&mode<=18)ps[3].name=b.getStringAttr(extras[mode-14]);
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  if(mode!=6){auto engine=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    if(mode!=13)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",b.getDictionaryAttr({
      b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(mode==12?32:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))}))}));}
  if(mode==8||mode==9)b.create<FModuleOp>(loc,b.getStringAttr(mode==8?"GGFASEDTimingCycle":"GGFASEDTimingCycleWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==10){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  if(mode==11)c.setName("wrong");
  if(mode!=7)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDResponseReleaserWrapper"),
      b.getStringAttr("~GGFASEDResponseReleaserWrapper|GGFASEDResponseReleaserWrapper>other"),
      b.getStringAttr("~GGFASEDResponseReleaserWrapper|GGFASEDTokenEngine>state")}))})}));
  return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    auto link = [&](Value d, Value z) {
      if (!isa<BundleType>(d.getType())) { require(drivers.emplace(key(d), z).second, "multiple drivers"); return; }
      std::function<void(std::string,std::string,Type,bool)> expand = [&](std::string dest, std::string src, Type t, bool flip) {
        if (auto bundle = dyn_cast<BundleType>(t)) {
          for (auto e : bundle.getElements()) expand(dest + "." + e.name.str(), src + "." + e.name.str(), e.type, flip != e.isFlip);
        } else require(links.emplace(flip ? src : dest, flip ? dest : src).second, "multiple aggregate drivers");
      };
      expand(key(d), key(z), d.getType(), false);
    };
    for (auto c : m.getOps<StrictConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto c : m.getOps<ConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) { memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n); }
  APInt read(std::string k, unsigned w) {
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    if (drivers.count(k)) return eval(drivers.at(k)).zextOrTrunc(w);
    if (links.count(k)) return read(links.at(k), w);
    auto prefix = k;
    while (prefix.find('.') != std::string::npos) {
      prefix.resize(prefix.rfind('.'));
      if (drivers.count(prefix)) return read(key(drivers.at(prefix)) + k.substr(prefix.size()), w);
    }
    throw std::runtime_error("missing aggregate driver: " + k);
  }
  APInt output(Value v, llvm::StringRef path, unsigned w = 64) {
    return read(key(v) + (path.empty() ? "" : "." + path.str()), w);
  }
  APInt eval(Value v) {
    auto k = key(v); unsigned w = width(v);
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ult(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else if (isa_and_nonnull<SubfieldOp>(op)) n = read(k, w);
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDTimingCycle(c,error)),error);require(succeeded(verify(*root)),"invalid timing cycle IR");
  Interpreter sim(named(c,"GGFASEDTimingCycle"));auto reg=*sim.module.getOps<RegResetOp>().begin();
  require(std::distance(sim.module.getOps<RegResetOp>().begin(),sim.module.getOps<RegResetOp>().end())==1,"unexpected timing state");
  std::mt19937_64 random(220);unsigned cases=0,stalls=0,stalledResets=0,wraps=0;
  auto sample=[&](uint64_t cycle,uint32_t read,uint32_t write,unsigned flags) {
    bool reset=flags&1,fire=flags&2;sim.memo.clear();sim.state[reg.getResult()]=APInt(64,cycle);
    sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);sim.put(sim.arg(4),"",read);sim.put(sim.arg(5),"",write);
    require(sim.output(sim.arg(3),"").getZExtValue()==cycle,"counter observation differs");
    require(sim.output(sim.arg(6),"").getZExtValue()==cycle+uint64_t(read)-1,"read deadline differs");
    require(sim.output(sim.arg(7),"").getZExtValue()==cycle+uint64_t(write)-1,"write deadline differs");
    uint64_t next=fire?(reset?0:cycle+1):cycle;sim.edge();
    require(sim.state.lookup(reg.getResult()).getZExtValue()==next,"enabled counter/reset differs");
    ++cases;stalls+=!fire;stalledResets+=reset&&!fire;wraps+=fire&&!reset&&cycle==UINT64_MAX;return next;
  };
  const uint64_t cycles[]{0,1,29,30,UINT32_MAX,uint64_t(UINT32_MAX)+1,UINT64_MAX-1,UINT64_MAX};
  const uint32_t latencies[]{0,1,2,30,UINT32_MAX-1,UINT32_MAX};
  for(auto cycle:cycles)for(auto r:latencies)for(auto w:latencies)for(unsigned flags=0;flags<4;++flags)sample(cycle,r,w,flags);
  for(unsigned i=0;i<10000;++i)sample(random(),random(),random(),random()&3);
  uint64_t cycle=UINT64_MAX-10;
  for(unsigned i=0;i<10000;++i)cycle=sample(cycle,random(),random(),random()&3);
  require(stalls&&stalledResets&&wraps,"timing coverage missing");
  llvm::outs()<<cases<<" deadline/counter transitions passed; stalls="<<stalls<<", stalled resets="<<stalledResets<<", wraps="<<wraps<<"\n";
}
void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDTimingCycle(c,error)),error);
  auto top=named(c,"GGFASEDTimingCycleWrapper"),inner=named(c,"GGFASEDResponseReleaserWrapper");
  require(top.getNumPorts()==9,"wrapper port count differs");
  for(unsigned i=0;i<4;++i)require(top.getPorts()[i].name==inner.getPorts()[i].name&&top.getPorts()[i].type==inner.getPorts()[i].type&&top.getPorts()[i].direction==inner.getPorts()[i].direction,"copied port differs");
  auto it=top.getOps<InstanceOp>().begin();auto sim=*it++;auto timer=*it;Interpreter w(top);
  for(unsigned flags=0;flags<4;++flags){w.memo.clear();w.put(sim.getResult(1),"",flags&1);w.put(sim.getResult(2),"",flags>>1);
    require(w.output(timer.getResult(1),"",1).getZExtValue()==(flags&1)&&w.output(timer.getResult(2),"",1).getZExtValue()==(flags>>1),"reset/fire binding differs");
    for(uint64_t value:{0ULL,1ULL,0xffffffffULL,0xffffffffffffffffULL}){
      w.put(timer.getResult(3),"",value);w.put(timer.getResult(6),"",value);w.put(timer.getResult(7),"",value);
      require(w.output(w.arg(4),"").getZExtValue()==value&&w.output(w.arg(7),"").getZExtValue()==value&&w.output(w.arg(8),"").getZExtValue()==value,"cycle/deadline binding differs");
      w.put(w.arg(5),"",uint32_t(value));w.put(w.arg(6),"",uint32_t(value));
      require(w.output(timer.getResult(4),"",32).getZExtValue()==uint32_t(value)&&w.output(timer.getResult(5),"",32).getZExtValue()==uint32_t(value),"runtime latency binding differs");
    }
  }
  auto targets=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
  const llvm::StringRef expected[]{"~GGFASEDTimingCycleWrapper","~GGFASEDTimingCycleWrapper|GGFASEDTimingCycleWrapper>other","~GGFASEDTimingCycleWrapper|GGFASEDTokenEngine>state"};
  for(unsigned i=0;i<3;++i)require(cast<StringAttr>(targets[i]).getValue()==expected[i],"target transfer differs");
  llvm::outs()<<"16 wrapper mapping cases and three target transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=18;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDTimingCycle(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDTimingCycle(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDTimingCycle(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"19 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
