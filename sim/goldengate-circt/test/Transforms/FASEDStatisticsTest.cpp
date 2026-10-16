// See LICENSE for license details.
#include "goldengate/FASEDStatistics.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/IRMapping.h"
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
bool selected = false;
int64_t maxFlight = 10;
std::string profile(llvm::StringRef input) {
  std::string result = input.str();
  if (selected) {
    size_t pos = 0;
    while ((pos = result.find("GGFASED", pos)) != std::string::npos) {
      auto end = result.find("Wrapper", pos);
      auto stop = result.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", pos);
      if (end != std::string::npos && (stop == std::string::npos || end < stop)) {
        result.replace(pos, 7, "GGSelectedPrintFASED"); pos += 19;
      } else pos += 7;
    }
  }
  return result;
}
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == profile(name)) return m;
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
    require(regular == 0 && reset == 4, "wrong bank reset policy");
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
const llvm::StringRef hierarchy[]{"GGFASEDResponseErrorsWrapper", "GGFASEDFunctionalModelRegisterWrapper",
    "GGFASEDLatencyRegistersWrapper", "GGFASEDRequestLimitsWrapper", "GGFASEDReadAdmissionWrapper",
    "GGFASEDWriteAdmissionWrapper", "GGFASEDWriteRetirementWrapper", "GGFASEDWritePairingWrapper",
    "GGFASEDTimingAWQueueWrapper", "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
    "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root=parseSourceString<ModuleOp>(profile(R"(module {
    firrtl.circuit "GGFASEDResponseErrorsWrapper" {
      firrtl.module @GGFASEDTokenEngine() {}
      firrtl.module @GGFASEDResponseReleaser(out %r: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>>>) {}
      firrtl.module @GGFASEDResponseErrorsWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %fased_model_reset: !firrtl.uint<1>, out %fased_tfire: !firrtl.uint<1>,
        out %fased_timing_requests: !firrtl.bundle<aw: bundle<ready: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, region: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>>>, w: bundle<ready: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, strb: uint<8>, id: uint<4>, last: uint<1>, data: uint<64>>>, ar: bundle<ready: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, region: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>>>>,
        out %other: !firrtl.uint<8>) {}
    } })").c_str(),&context);
  require(bool(root),"fixture parse failed"); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  named(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({
    b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI64IntegerAttr(maxFlight))}))}));
  SmallVector<FModuleOp> modules{named(c,hierarchy[0])};
  for(unsigned i=1;i<std::size(hierarchy);++i) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    modules.push_back(b.create<FModuleOp>(c.getLoc(),b.getStringAttr(profile(hierarchy[i])),
        ConventionAttr::get(&context,Convention::Internal),ArrayRef<PortInfo>{}));
    b.setInsertionPointToEnd(modules[i-1].getBodyBlock());
    auto instance=b.create<InstanceOp>(c.getLoc(),modules[i],"sim");
    instance->setAttr("goldengate.test",b.getStringAttr("retained metadata"));
  }
  b.setInsertionPointToEnd(modules.back().getBodyBlock());
  b.create<InstanceOp>(c.getLoc(),named(c,"GGFASEDResponseReleaser"),"releaser");
  SmallVector<Attribute> annos;
  for(auto name:{"fased_timing_requests.w.bits.last","hostReset","other"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target",b.getStringAttr(profile("~GGFASEDResponseErrorsWrapper|GGFASEDResponseErrorsWrapper>"+std::string(name))))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos)); return root;
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
  require(succeeded(goldengate::materializeFASEDStatistics(c, bank, error)), error);
  require(c.getName() == "GGControlErrorWrapper" && dump(top) == topBefore &&
      c->getAttr("rawAnnotations") == annotations && succeeded(verify(*early)),
      "early materialization changed top identity, ports or annotations");
  unsigned uses = 0; c.walk([&](InstanceOp i) { uses += i.getModuleName() == bank.getName(); });
  require(uses == 0, "materialization prematurely attached the bank");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 4, "early bank lost statistics counters");
  const llvm::StringRef names[]{"totalWriteBeats", "totalReadBeats", "totalWrites", "totalReads"};
  for (unsigned i = 0; i < 4; ++i) {
    auto reg = cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name") == names[i] &&
        reg.getAs<IntegerAttr>("offset").getInt() == 56 + 4*i &&
        reg.getAs<BoolAttr>("readable").getValue() && !reg.getAs<BoolAttr>("writeable").getValue(),
        "early bank changed global words 14-17 or read-only permissions");
  }
  auto before = dump(*early); FModuleOp unchanged = top;
  require(failed(goldengate::materializeFASEDStatistics(c, unchanged, error)) &&
      unchanged == top && dump(*early) == before, "duplicate materialization changed result or IR");
  require(failed(goldengate::attachFASEDStatistics(c, bank, error)) && dump(*early) == before,
      "premature attachment changed IR");
  // The split API must preserve all hardware equations and target transfers.
  auto split = fixture(context), combined = fixture(context);
  auto splitCircuit = *split->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::materializeFASEDStatistics(splitCircuit, bank, error)) &&
      succeeded(goldengate::attachFASEDStatistics(splitCircuit, bank, error)), error);
  require(succeeded(goldengate::addFASEDStatistics(*combined->getOps<CircuitOp>().begin(), error)), error);
  require(dump(*split) == dump(*combined) && succeeded(verify(*split)),
      "split bank construction changed hardware or annotations");

  // Null/foreign/wrong banks, each port's name/type/direction, arity and uses.
  for (unsigned bad = 0; bad < 32; ++bad) {
    auto root = fixture(context), foreign = fixture(context);
    auto circuit = *root->getOps<CircuitOp>().begin(); FModuleOp candidate;
    require(succeeded(goldengate::materializeFASEDStatistics(circuit, candidate, error)), error);
    if (bad == 0) candidate = {};
    if (bad == 1) require(succeeded(goldengate::materializeFASEDStatistics(
        *foreign->getOps<CircuitOp>().begin(), candidate, error)), error);
    if (bad == 2) candidate = named(circuit, "GGFASEDResponseErrorsWrapper");
    if (bad >= 3 && bad <= 30) {
      SmallVector<PortInfo> malformed(candidate.getPorts());
      if (bad == 30) malformed.pop_back();
      else {
        unsigned port = (bad - 3) / 3, mutation = (bad - 3) % 3;
        if (mutation == 0) malformed[port].name = b.getStringAttr("wrong");
        if (mutation == 1) malformed[port].type = UIntType::get(&context, 3);
        if (mutation == 2) malformed[port].direction = malformed[port].direction == Direction::In ? Direction::Out : Direction::In;
      }
      candidate.erase(); b.setInsertionPointToEnd(circuit.getBodyBlock());
      candidate = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("GGFASEDStatistics"),
          ConventionAttr::get(&context, Convention::Internal), malformed);
    }
    if (bad == 31) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      auto user = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("User"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
      b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), candidate, "used");
    }
    auto before = dump(*root);
    require(failed(goldengate::attachFASEDStatistics(circuit, candidate, error)) &&
        !error.empty() && dump(*root) == before, "invalid standalone bank accepted or mutated IR");
  }
  llvm::outs() << "FASED statistics MMIO phases: early global words 14-17, unchanged top/annotations, split/combined equivalence and 34 atomic rejections passed\n";
}
void behavior(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDStatistics(c,error)),error);
  require(succeeded(verify(*root)),"statistics IR verification failed");
  Interpreter sim(named(c,"GGFASEDStatistics"));
  const llvm::StringRef names[]{"totalWriteBeats","totalReadBeats","totalWrites","totalReads"};
  const unsigned events[]{6,7,4,5};
  std::array<uint32_t,4> expected{1,1,1,1};std::mt19937 random(232);
  unsigned wraps=0,stalledResets=0,hostOnlyResets=0,assertionFailures=0;
  for(unsigned cycle=0;cycle<32768;++cycle) {
    unsigned bits=cycle<1024?cycle%128:random()&127;
    bool hostReset=bits&1,modelReset=(bits>>1)&1,fire=(bits>>2)&1;
    if(cycle<1024 || cycle%97==0) {
      const uint32_t values[]{0,1,2,0x7fffffffU,0x80000000U,0xfffffffdU,0xfffffffeU,0xffffffffU};
      for(unsigned i=0;i<4;++i) expected[i]=values[(cycle/128+i)%8];
      for(auto reg:sim.module.getOps<RegResetOp>())
        for(unsigned i=0;i<4;++i)if(reg.getName()==names[i])sim.state[reg.getResult()]=expected[i];
    }
    sim.memo.clear();for(unsigned i=1;i<8;++i)sim.memo[sim.key(sim.arg(i))]=(bits>>(i-1))&1;
    for(unsigned i=0;i<4;++i) {
      sim.memo[sim.key(sim.arg(8))+".write["+std::to_string(i)+"].valid"]=random()&1;
      sim.memo[sim.key(sim.arg(8))+".write["+std::to_string(i)+"].bits"]=random();
      sim.memo[sim.key(sim.arg(8))+".read["+std::to_string(i)+"].ready"]=random()&1;
    }
    auto check=[&]() { for(unsigned i=0;i<4;++i) {
      std::string lane="["+std::to_string(i)+"]";
      require(sim.output(8,"read"+lane+".bits")==expected[i],"counter readback mismatch");
      require(sim.output(8,"read"+lane+".valid")==1 && sim.output(8,"write"+lane+".ready")==1,"MCR handshake mismatch");
    }};
    check();unsigned assertions=0;
    for(auto a:sim.module.getOps<PrintFOp>()) {
      bool fail=sim.eval(a.getCond());
      bool write=sim.memo.at(sim.key(sim.arg(8))+".write["+std::to_string(assertions)+"].valid");
      require(fail==(!hostReset && write),"diagnostic confused model/host reset");
      require(a.getClock()==sim.arg(0) && a.getSubstitutions().empty() &&
          a.getFormatString()=="Assertion failed: Register "+names[assertions].str()+" is read only\n",
          "wrong diagnostic clock/message");assertionFailures+=fail;++assertions;
    }
    require(assertions==4 && sim.module.getOps<AssertOp>().empty() && sim.module.getOps<StopOp>().empty(),
        "read-only diagnostics must continue simulation");
    for(unsigned i=0;i<4;++i) {
      bool event=(bits>>(events[i]-1))&1;
      wraps+=fire && !modelReset && event && expected[i]==UINT32_MAX;
      if(fire)expected[i]=modelReset?0:event?expected[i]+1:expected[i];
    }
    stalledResets+=modelReset && !fire;hostOnlyResets+=hostReset && !modelReset;
    sim.edge();sim.memo.clear();check();
  }
  require(wraps && stalledResets && hostOnlyResets && assertionFailures,"coverage missing");
}
void mapping(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  auto constructor = named(c,"GGFASEDTokenEngine")->getAttr("goldengate.bridgeConstructor");
  require(succeeded(goldengate::addFASEDStatistics(c,error)),error);
  require(named(c,"GGFASEDTokenEngine")->getAttr("goldengate.bridgeConstructor") == constructor,
      "retained constructor changed");
  auto top=named(c,"GGFASEDStatisticsWrapper"),bank=named(c,"GGFASEDStatistics");
  require(top.getNumPorts()==7 && top.getPortName(6)=="fased_statistics_mcr","observation leaked through top");
  auto regs=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");require(regs.size()==4,"missing register map");
  for(unsigned i=0;i<4;++i) {auto d=cast<DictionaryAttr>(regs[i]);require(d.getAs<IntegerAttr>("offset").getInt()==56+4*i &&
      d.getAs<BoolAttr>("readable").getValue() && !d.getAs<BoolAttr>("writeable").getValue(),"wrong offsets/permissions");}
  InstanceOp sim,counter;for(auto i:top.getOps<InstanceOp>()){if(i.getName()=="sim")sim=i;else counter=i;}
  unsigned bindings=0;
  for(auto conn:top.getOps<StrictConnectOp>()) {
    for(unsigned i=0;i<4;++i)if(conn.getDest()==counter.getResult(i)) {
      require(conn.getSrc()==(i<2?top.getArgument(i):sim.getResult(i)),"host/model control binding mismatch");++bindings;
    }
    for(unsigned i=4;i<7;++i)if(conn.getDest()==counter.getResult(i)) {
      auto both=conn.getSrc().getDefiningOp<AndPrimOp>();require(bool(both),"handshake must include ready/valid");
      auto ready=both->getOperand(0).getDefiningOp<SubfieldOp>(),valid=both->getOperand(1).getDefiningOp<SubfieldOp>();
      require(ready && valid && ready.getFieldName()=="ready" && valid.getFieldName()=="valid" &&
          ready.getInput()==valid.getInput(),"request handshake mismatch");
      auto channel=ready.getInput().getDefiningOp<SubfieldOp>();
      require(channel && channel.getFieldName()==(i==4?"aw":i==5?"ar":"w") &&
          channel.getInput()==sim.getResult(4),"wrong request channel");++bindings;
    }
    if(conn.getDest()==counter.getResult(7)) {require(conn.getSrc()==sim.getResults().back(),"read handshake tap mismatch");++bindings;}
  }
  require(bindings==8,"missing control/event binding");
  for(unsigned j=0;j<std::size(hierarchy);++j) {
    auto m=named(c,hierarchy[j]);require(m.getPortName(m.getNumPorts()-1)=="fased_accepted_r_fire","missing response observation");
    unsigned observed=0;
    for(auto conn:m.getOps<StrictConnectOp>())if(conn.getDest()==m.getArguments().back()) {
      if(j+1<std::size(hierarchy)) {
        auto inst=conn.getSrc().getDefiningOp<InstanceOp>();require(inst && inst.getModuleName()==profile(hierarchy[j+1]) &&
            inst->getAttrOfType<StringAttr>("goldengate.test").getValue()=="retained metadata","response chain metadata lost");
      } else {
        auto both=conn.getSrc().getDefiningOp<AndPrimOp>();require(bool(both),"read event must include ready/valid");
        for(unsigned k=0;k<2;++k) {auto f=both->getOperand(k).getDefiningOp<SubfieldOp>();
          auto inst=f?f.getInput().getDefiningOp<InstanceOp>():InstanceOp();
          require(f && f.getFieldName()==(k?"valid":"ready") && inst && inst.getName()=="releaser" &&
              f.getInput()==inst.getResult(0),"read beats must count all accepted R, not just last");}
      }
      ++observed;
    }
    require(observed==1,"missing or duplicate R observation driver");
  }
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(annos.size()==3,"annotations lost");
  for(auto a:annos)require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().starts_with(
      profile("~GGFASEDStatisticsWrapper|GGFASEDStatisticsWrapper>")),"targets not retargeted");
}
void rejections(MLIRContext &context) {
  for(unsigned mode=0;mode<30;++mode) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
    auto top=named(c,hierarchy[0]),engine=named(c,"GGFASEDTokenEngine");
    if(mode==0)c.setName("WrongTop");if(mode==1)c->removeAttr("rawAnnotations");
    if(mode==2)engine->removeAttr("goldengate.bridgeConstructor");
    if(mode==3)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(0))}))}));
    if(mode>=4 && mode<9) {auto names=llvm::to_vector(top.getPortNames());names[mode-4]=b.getStringAttr("missing");top.setPortNames(names);}
    if(mode>=9 && mode<14) {SmallVector<bool> dirs;for(auto p:top.getPorts())dirs.push_back(p.direction==Direction::Out);dirs[mode-9]=!dirs[mode-9];top.setPortDirections(dirs);}
    if(mode==14) {b.setInsertionPointToEnd(engine.getBodyBlock());b.create<InstanceOp>(top.getLoc(),top,"usedTop");}
    if(mode==15) {auto names=llvm::to_vector(top.getPortNames());names[5]=b.getStringAttr("fased_statistics_mcr");top.setPortNames(names);}
    if(mode==16) {top.getArgument(4).setType(UIntType::get(&context,32,false));auto types=llvm::to_vector(top.getPortTypes());
      types[4]=TypeAttr::get(UIntType::get(&context,32,false));top.setPortTypes(types);}
    if(mode==17 || mode==18) {b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(top.getLoc(),
        b.getStringAttr(profile(mode==17?"GGFASEDStatistics":"GGFASEDStatisticsWrapper")),
        ConventionAttr::get(&context,Convention::Internal),ArrayRef<PortInfo>{});}
    if(mode==19)named(c,hierarchy[6]).setName("missingHierarchy");
    if(mode==20) {auto m=named(c,hierarchy[6]);b.setInsertionPointToEnd(engine.getBodyBlock());b.create<InstanceOp>(m.getLoc(),m,"duplicate");}
    if(mode==21) {auto m=named(c,hierarchy[6]);auto i=*m.getOps<InstanceOp>().begin();i.setName("wrongInstance");}
    if(mode==22) {auto m=named(c,hierarchy[5]);m.insertPorts({{m.getNumPorts(),PortInfo(b.getStringAttr("fased_accepted_r_fire"),UIntType::get(&context,1,false),Direction::Out)}});}
    if(mode==23) {auto m=named(c,"GGFASEDResponseReleaser");auto names=llvm::to_vector(m.getPortNames());names[0]=b.getStringAttr("wrongResponse");m.setPortNames(names);
      auto inst=*named(c,hierarchy[12]).getOps<InstanceOp>().begin();inst.erase();b.setInsertionPointToEnd(named(c,hierarchy[12]).getBodyBlock());b.create<InstanceOp>(m.getLoc(),m,"releaser");}
    if(mode==24) {auto m=named(c,hierarchy[12]);(*m.getOps<InstanceOp>().begin()).setName("missingReleaser");}
    if (mode == 25) engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
      b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight",
        b.getIntegerAttr(IntegerType::get(&context, 128), APInt(128, 10)))}))}));
    if (mode >= 26) {
      SmallVector<NamedAttribute> fields;
      if (mode == 26) fields.push_back(b.getNamedAttr("maxFlight", b.getI64IntegerAttr(-1)));
      if (mode == 27) fields.push_back(b.getNamedAttr("maxFlight", b.getStringAttr("10")));
      engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr(mode == 29
          ? ArrayRef<NamedAttribute>{}
          : ArrayRef<NamedAttribute>{b.getNamedAttr("axi4Edge", b.getDictionaryAttr(fields))}));
    }
    std::string before;llvm::raw_string_ostream out(before);root->print(out);out.flush();std::string error;
    require(failed(goldengate::addFASEDStatistics(c,error)) && !error.empty(),"invalid boundary accepted");
    std::string after;llvm::raw_string_ostream next(after);root->print(next);next.flush();require(before==after,"rejection mutated circuit");
  }
}
}
// Export the actual production bank and its observed wrapper bindings; do not
// reconstruct counter equations in the RTL differential test.
unsigned port(FModuleOp m, llvm::StringRef name) {
  for(unsigned i=0;i<m.getNumPorts();++i)if(m.getPortName(i)==name)return i;
  throw std::runtime_error("missing production port: "+name.str());
}
Value driver(FModuleOp m, Value dest) {
  Value result;
  for(auto &op:*m.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&op.getOperand(0)==dest) {
    require(!result,"multiple production drivers");result=op.getOperand(1);
  }
  require(bool(result),"missing production driver");return result;
}
void exportProduction(MLIRContext &ctx,const char *input,const char *output) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production statistics boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  require(c.getName()=="GGFASEDStatisticsWrapper","wrong production statistics root");
  auto top=named(c,c.getName()),inner=named(c,hierarchy[0]),bank=named(c,"GGFASEDStatistics");
  InstanceOp sim,original;
  for(auto i:top.getOps<InstanceOp>()) {
    if(i.getName()=="sim")sim=i;
    if(i.getName()=="statistics")original=i;
  }
  require(sim&&sim.getModuleName()==inner.getName()&&original&&original.getModuleName()==bank.getName(),
      "wrong production statistics instances");
  require(bank.getNumPorts()==9,"wrong production statistics bank arity");
  SmallVector<Value> sources;
  for(unsigned i=0;i<8;++i) {
    auto source=driver(top,original.getResult(i));sources.push_back(source);
    if(i<2)require(source==top.getArgument(port(top,i?"hostReset":"hostClock")),"wrong host clock/reset");
    else if(i<4)require(source==sim.getResult(port(inner,i==2?"fased_model_reset":"fased_tfire")),
        "wrong target reset/fire");
    else if(i<7) {
      auto both=source.getDefiningOp<AndPrimOp>();require(bool(both),"missing request handshake");
      for(unsigned j=0;j<2;++j) {
        auto f=both->getOperand(j).getDefiningOp<SubfieldOp>();
        auto channel=f?f.getInput().getDefiningOp<SubfieldOp>():SubfieldOp();
        require(f&&f.getFieldName()==(j?"valid":"ready")&&channel&&
            channel.getFieldName()==(i==4?"aw":i==5?"ar":"w")&&
            channel.getInput()==sim.getResult(port(inner,"fased_timing_requests")),"wrong request handshake");
      }
    } else require(source==sim.getResult(port(inner,"fased_accepted_r_fire")),"wrong accepted-R observation");
  }
  auto mcr=top.getArgument(port(top,"fased_statistics_mcr"));
  require(driver(top,mcr)==original.getResult(8),"wrong MCR binding");
  auto isolated=ModuleOp::create(c.getLoc());OpBuilder b(&ctx);
  b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDStatisticsProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());b.insert(bank->clone());
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDStatisticsProbe"),
      bank.getConventionAttr(),bank.getPorts());probe.setPublic();
  b.setInsertionPointToEnd(probe.getBodyBlock());
  auto instance=b.create<InstanceOp>(c.getLoc(),bank,"statistics");IRMapping map;
  for(unsigned i=0;i<9;++i)map.map(original.getResult(i),instance.getResult(i));
  for(unsigned i=0;i<8;++i)map.map(sources[i],probe.getArgument(i));
  map.map(mcr,probe.getArgument(8));unsigned cloned=0;
  for(auto &op:*top.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&
      map.contains(op.getOperand(0))&&map.contains(op.getOperand(1))) {b.clone(op,map);++cloned;}
  require(cloned==9&&succeeded(verify(isolated)),"invalid production statistics probe bindings");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export statistics probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned production statistics bank and nine validated control/event/MCR bindings\n";
}
int main(int argc,char **argv) {
  try {
    MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    require(argc==1||argc==3,"usage: goldengate-fased-statistics-test [boundary.mlir probe.mlir]");
    if(argc==3){exportProduction(context,argv[1],argv[2]);return 0;}
    for (bool profileSelected : {false, true}) {
      selected = profileSelected;
      for (int64_t flight : {int64_t(1), int64_t(9), int64_t(10), int64_t(16),
                            int64_t(20), INT64_MAX}) {
        maxFlight = flight; phases(context);behavior(context);mapping(context);rejections(context);
      }
    }
    llvm::outs()<<"FASED statistics: 393216 cycles across legacy/selected profiles and six positive flight capacities, wraps, target-clock/reset bindings, 13 response hierarchy modules, retained constructors, targets and 360 atomic boundary rejections passed\n";
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
}
