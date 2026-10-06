// See LICENSE for license details.
#include "goldengate/FASEDHistograms.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = 0xA5 & ((uint64_t(1) << cast<UIntType>(r.getResult().getType()).getWidthOrSentinel()) - 1); }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 0 && reset == 8, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = eval(op->getOperand(0)) <= eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NEQPrimOp>(op)) n = eval(op->getOperand(0)) != eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
std::string histogramName(unsigned i) {
  return std::string(i<5?"writeOutstandingHistogram_":"readOutstandingHistogram_")+std::to_string(i%5);
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root=parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGFASEDStatisticsWrapper" {
      firrtl.module @GGFASEDTokenEngine() {}
      firrtl.module @GGFASEDStatisticsWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %fased_model_reset: !firrtl.uint<1>, out %fased_tfire: !firrtl.uint<1>,
        out %fased_pending_reads: !firrtl.bundle<value: uint<4>, full: uint<1>>,
        out %fased_pending_writes: !firrtl.bundle<awValue: uint<4>, wValue: uint<4>, awFull: uint<1>, wFull: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })",&context);
  require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
  named(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({
    b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(10))}))}));
  SmallVector<Attribute> annos;
  for(auto name:{"fased_pending_writes.awValue","hostReset","other"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target",b.getStringAttr("~GGFASEDStatisticsWrapper|GGFASEDStatisticsWrapper>"+std::string(name)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));return root;
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
void phases(MLIRContext &context) {
  auto early = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGControlErrorWrapper" {
    firrtl.module @GGControlErrorWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })", &context);
  require(bool(early), "early fixture parse failed");
  auto c = *early->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Target")),
      b.getNamedAttr("target", b.getStringAttr("~GGControlErrorWrapper|GGControlErrorWrapper>other"))})}));
  auto top = named(c, "GGControlErrorWrapper"); auto topBefore = dump(top);
  auto annotations = c->getAttr("rawAnnotations"); FModuleOp bank; std::string error;
  require(succeeded(goldengate::materializeFASEDHistograms(c, bank, error)), error);
  require(c.getName() == "GGControlErrorWrapper" && dump(top) == topBefore &&
      c->getAttr("rawAnnotations") == annotations && succeeded(verify(*early)),
      "early materialization changed top identity, ports or annotations");
  unsigned uses = 0; c.walk([&](InstanceOp i) { uses += i.getModuleName() == bank.getName(); });
  require(uses == 0, "materialization prematurely attached the bank");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 10, "early bank lost histogram lanes");
  for (unsigned i = 0; i < 10; ++i) {
    auto reg = cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name").getValue() == histogramName(i) &&
        reg.getAs<IntegerAttr>("offset").getInt() == 16 + 4*i &&
        reg.getAs<BoolAttr>("readable").getValue() && !reg.getAs<BoolAttr>("writeable").getValue(),
        "early bank changed global words 4-13 or read-only permissions");
  }
  auto before = dump(*early); FModuleOp unchanged = top;
  require(failed(goldengate::materializeFASEDHistograms(c, unchanged, error)) &&
      unchanged == top && dump(*early) == before, "duplicate materialization changed result or IR");
  require(failed(goldengate::attachFASEDHistograms(c, bank, error)) && dump(*early) == before,
      "premature attachment changed IR");
  // The split API must preserve all hardware equations and target transfers.
  auto split = fixture(context), combined = fixture(context);
  auto splitCircuit = *split->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::materializeFASEDHistograms(splitCircuit, bank, error)) &&
      succeeded(goldengate::attachFASEDHistograms(splitCircuit, bank, error)), error);
  require(succeeded(goldengate::addFASEDHistograms(*combined->getOps<CircuitOp>().begin(), error)), error);
  require(dump(*split) == dump(*combined) && succeeded(verify(*split)),
      "split bank construction changed hardware or annotations");

  // Null/foreign/wrong banks, each port's name/type/direction, arity and uses.
  for (unsigned bad = 0; bad < 26; ++bad) {
    auto root = fixture(context), foreign = fixture(context);
    auto circuit = *root->getOps<CircuitOp>().begin(); FModuleOp candidate;
    require(succeeded(goldengate::materializeFASEDHistograms(circuit, candidate, error)), error);
    if (bad == 0) candidate = {};
    if (bad == 1) require(succeeded(goldengate::materializeFASEDHistograms(
        *foreign->getOps<CircuitOp>().begin(), candidate, error)), error);
    if (bad == 2) candidate = named(circuit, "GGFASEDStatisticsWrapper");
    if (bad >= 3 && bad <= 24) {
      SmallVector<PortInfo> malformed(candidate.getPorts());
      if (bad == 24) malformed.pop_back();
      else {
        unsigned port = (bad - 3) / 3, mutation = (bad - 3) % 3;
        if (mutation == 0) malformed[port].name = b.getStringAttr("wrong");
        if (mutation == 1) malformed[port].type = UIntType::get(&context, 3);
        if (mutation == 2) malformed[port].direction = malformed[port].direction == Direction::In ? Direction::Out : Direction::In;
      }
      candidate.erase(); b.setInsertionPointToEnd(circuit.getBodyBlock());
      candidate = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("GGFASEDHistograms"),
          ConventionAttr::get(&context, Convention::Internal), malformed);
    }
    if (bad == 25) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      auto user = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("User"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
      b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), candidate, "used");
    }
    auto before = dump(*root);
    require(failed(goldengate::attachFASEDHistograms(circuit, candidate, error)) &&
        !error.empty() && dump(*root) == before, "invalid standalone bank accepted or mutated IR");
  }
  llvm::outs() << "FASED histograms MMIO phases: early global words 4-13, unchanged top/annotations, split/combined equivalence and 28 atomic rejections passed\n";
}
void behavior(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDHistograms(c,error)),error);
  require(succeeded(verify(*root)),"histogram IR verification failed");
  Interpreter sim(named(c,"GGFASEDHistograms"));
  std::array<uint32_t,10> expected{};std::mt19937 random(233);
  unsigned wraps=0,stalledResets=0,overflowSamples=0,assertionFailures=0;
  for(unsigned cycle=0;cycle<32768;++cycle) {
    unsigned controls=cycle<16384?cycle%8:random()&7;
    bool hostReset=controls&1,modelReset=(controls>>1)&1,fire=(controls>>2)&1;
    unsigned reads=cycle<16384?(cycle/8)%16:random()%16;
    unsigned aw=cycle<16384?(cycle/128)%16:random()%16;
    if(cycle<16384 || cycle%97==0) {
      const uint32_t edges[]{0,1,2,0x7fffffffU,0x80000000U,0xfffffffdU,0xfffffffeU,0xffffffffU};
      for(unsigned i=0;i<10;++i)expected[i]=i%5==4?0:edges[(cycle/2048+i)%8];
      for(auto reg:sim.module.getOps<RegResetOp>())for(unsigned i=0;i<10;++i)
        if(reg.getName()==histogramName(i))sim.state[reg.getResult()]=expected[i];
    }
    sim.memo.clear();for(unsigned i=1;i<4;++i)sim.memo[sim.key(sim.arg(i))]=(controls>>(i-1))&1;
    sim.memo[sim.key(sim.arg(4))]=reads;sim.memo[sim.key(sim.arg(5))]=aw;
    for(unsigned i=0;i<10;++i) {
      sim.memo[sim.key(sim.arg(6))+".write["+std::to_string(i)+"].valid"]=random()&1;
      sim.memo[sim.key(sim.arg(6))+".write["+std::to_string(i)+"].bits"]=random();
      sim.memo[sim.key(sim.arg(6))+".read["+std::to_string(i)+"].ready"]=random()&1;
    }
    auto check=[&](){for(unsigned i=0;i<10;++i) {
      std::string lane="["+std::to_string(i)+"]";
      require(sim.output(6,"read"+lane+".bits")==expected[i],"histogram readback mismatch");
      require(sim.output(6,"read"+lane+".valid")==1 && sim.output(6,"write"+lane+".ready")==1,"MCR handshake mismatch");
    }};check();
    unsigned assertions=0;for(auto a:sim.module.getOps<AssertOp>()) {
      bool fail=sim.eval(a->getOperand(2)) && !sim.eval(a->getOperand(1));
      bool write=sim.memo.at(sim.key(sim.arg(6))+".write["+std::to_string(assertions)+"].valid");
      require(fail==(!hostReset && write),"assertion confused target/host reset");assertionFailures+=fail;++assertions;
    }
    require(assertions==10,"missing read-only assertion");
    if(fire) {
      if(modelReset)expected.fill(0);
      else for(unsigned group=0;group<2;++group) {
        unsigned count=group?reads:aw;const unsigned bounds[]{0,2,4,8};
        unsigned bin=0;while(bin<4 && count>bounds[bin])++bin;
        if(bin<4) {wraps+=expected[group*5+bin]==UINT32_MAX;++expected[group*5+bin];}
        else ++overflowSamples; // Fifth allocated bin remains constant zero.
      }
    }
    stalledResets+=modelReset&&!fire;sim.edge();sim.memo.clear();check();
  }
  require(wraps && stalledResets && overflowSamples && assertionFailures,"missing edge coverage");
}
void mapping(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDHistograms(c,error)),error);
  auto bank=named(c,"GGFASEDHistograms"),top=named(c,"GGFASEDHistogramsWrapper");
  require(top.getNumPorts()==8 && top.getPortName(7)=="fased_histograms_mcr","wrong top interface");
  auto rows=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");require(rows.size()==10,"wrong MMIO map");
  for(unsigned i=0;i<10;++i) {
    auto row=cast<DictionaryAttr>(rows[i]);require(row.getAs<StringAttr>("name").getValue()==histogramName(i) &&
      row.getAs<IntegerAttr>("offset").getInt()==16+4*i && row.getAs<BoolAttr>("readable").getValue() &&
      !row.getAs<BoolAttr>("writeable").getValue(),"wrong histogram map row");
  }
  auto bounds=bank->getAttrOfType<ArrayAttr>("goldengate.histogramUpperBounds");
  require(bounds && bounds.size()==4,"missing bin boundaries");
  const int64_t expectedBounds[]{0,2,4,8};
  for(unsigned i=0;i<4;++i)require(cast<IntegerAttr>(bounds[i]).getInt()==expectedBounds[i],"wrong bin boundary");
  InstanceOp sim,helper;for(auto i:top.getOps<InstanceOp>()) {
    if(i.getModuleName()=="GGFASEDStatisticsWrapper")sim=i;
    if(i.getModuleName()=="GGFASEDHistograms")helper=i;
  }
  require(sim && helper,"missing wrapper instances");
  llvm::DenseMap<Value,Value> drivers;for(auto op:top.getOps<StrictConnectOp>())drivers[op.getDest()]=op.getSrc();
  require(drivers.lookup(helper.getResult(0))==top.getArgument(0) &&
      drivers.lookup(helper.getResult(1))==top.getArgument(1) &&
      drivers.lookup(helper.getResult(2))==sim.getResult(2) &&
      drivers.lookup(helper.getResult(3))==sim.getResult(3),"wrong clock/reset/fire bindings");
  for(unsigned i=4;i<6;++i) {
    auto f=drivers.lookup(helper.getResult(i)).getDefiningOp<SubfieldOp>();
    require(f && f.getInput()==sim.getResult(i) && f.getFieldName()==(i==4?"value":"awValue"),
        "histograms must observe pending read and AW state, not W or full flags");
  }
  unsigned copies=0;for(auto op:top.getOps<ConnectOp>()) {
    if(op.getDest()==top.getArgument(7))require(op.getSrc()==helper.getResult(6),"wrong MCR binding");
    else {bool found=false;for(unsigned i=0;i<7;++i) {
      bool input=i<2;
      found|=op.getDest()==(input?sim.getResult(i):top.getArgument(i)) &&
          op.getSrc()==(input?top.getArgument(i):sim.getResult(i));
    }require(found,"copied port changed");++copies;}
  }require(copies==7,"missing copied port");
  for(auto a:c->getAttrOfType<ArrayAttr>("rawAnnotations"))require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().starts_with(
    "~GGFASEDHistogramsWrapper|GGFASEDHistogramsWrapper>"),"target transfer failed");
}
void rejections(MLIRContext &context) {
  for(unsigned mode=0;mode<22;++mode) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
    auto top=named(c,"GGFASEDStatisticsWrapper"),engine=named(c,"GGFASEDTokenEngine");
    if(mode==0)c.setName("WrongTop");if(mode==1)c->removeAttr("rawAnnotations");
    if(mode==2)engine->removeAttr("goldengate.bridgeConstructor");
    if(mode==3)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(9))}))}));
    if(mode>=4 && mode<10) {auto names=llvm::to_vector(top.getPortNames());names[mode-4]=b.getStringAttr("missing");top.setPortNames(names);}
    if(mode>=10 && mode<16) {SmallVector<bool> dirs;for(auto p:top.getPorts())dirs.push_back(p.direction==Direction::Out);dirs[mode-10]=!dirs[mode-10];top.setPortDirections(dirs);}
    if(mode==16) {b.setInsertionPointToEnd(engine.getBodyBlock());b.create<InstanceOp>(top.getLoc(),top,"usedTop");}
    if(mode==17) {auto names=llvm::to_vector(top.getPortNames());names[6]=b.getStringAttr("fased_histograms_mcr");top.setPortNames(names);}
    if(mode==18 || mode==19) {unsigned i=mode-14;auto t=UIntType::get(&context,32,false);top.getArgument(i).setType(t);
      auto types=llvm::to_vector(top.getPortTypes());types[i]=TypeAttr::get(t);top.setPortTypes(types);}
    if(mode==20 || mode==21) {b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(top.getLoc(),
        b.getStringAttr(mode==20?"GGFASEDHistograms":"GGFASEDHistogramsWrapper"),
        ConventionAttr::get(&context,Convention::Internal),ArrayRef<PortInfo>{});}
    std::string before;llvm::raw_string_ostream out(before);root->print(out);out.flush();std::string error;
    require(failed(goldengate::addFASEDHistograms(c,error)) && !error.empty(),"invalid boundary accepted");
    std::string after;llvm::raw_string_ostream next(after);root->print(next);next.flush();require(before==after,"rejection mutated circuit");
  }
}
}
int main() {
  try {
    MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();phases(context);behavior(context);mapping(context);rejections(context);
    llvm::outs()<<"FASED histograms: 32768 cycles, all four-bit occupancies, constant fifth bins, target-clock/reset bindings, targets and 22 atomic rejections passed\n";
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
}
