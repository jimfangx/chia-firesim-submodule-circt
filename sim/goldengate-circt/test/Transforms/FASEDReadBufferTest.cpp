// See LICENSE for license details.
#include "goldengate/FASEDReadBuffer.h"
#include "circt/Dialect/HW/HWDialect.h"
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
#include <random>
#include <stdexcept>
#include <vector>
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
  MemOp ram;
  std::vector<APInt> memory;
  APInt readOutput = APInt(65,0);
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  unsigned width(Value v) { return cast<UIntType>(v.getType()).getWidthOrSentinel(); }
  Interpreter(FModuleOp m, unsigned entries) : module(m), memory(16 * entries) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 16 * entries && ram.getDataType() == UIntType::get(m.getContext(), 65, false) &&
        ram.getReadLatency() == 1 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    for (auto &v : memory) v = APInt(65,0);
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  APInt drive(Value root, llvm::StringRef field) { return eval(drivers.at(key(root) + "." + field.str())); }
  APInt eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); APInt n(width(v), 0);
    if ((isa_and_nonnull<RegResetOp>(op) || isa_and_nonnull<RegOp>(op))) {
      if (state.count(v)) n = state.lookup(v);
    } else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zext(width(v)) + eval(op->getOperand(1)).zext(width(v));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(!eval(op->getOperand(0)).isZero() ? 1 : 2));
    else if (auto cat = dyn_cast_or_null<CatPrimOp>(op)) {
      auto a = eval(op->getOperand(0)), b = eval(op->getOperand(1));
      n = (a.zext(width(v)) << b.getBitWidth()) | b.zext(width(v));
    } else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = eval(bits.getInput()).lshr(bits.getLo()).trunc(width(v));
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput() == ram.getResult(0) && f.getFieldName() == "data", "unsupported memory access");
      require(drive(ram.getResult(0), "en").getZExtValue() == 1, "queue read disabled");
      n = readOutput;
    } else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(width(v)); memo.insert_or_assign(k, n); return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = !eval(r.getResetSignal()).isZero() ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    readOutput = memory.at(drive(ram.getResult(0), "addr").getZExtValue());
    Value writer = ram.getResult(1);
    if (!drive(writer, "en").isZero() && !drive(writer, "mask").isZero())
      memory.at(drive(writer, "addr").getZExtValue()) = drive(writer, "data");
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned variant = 0, bool selected = false, unsigned reuse = 1, unsigned flight = 10) {
  std::string text=R"(module {
    firrtl.circuit "GGFASEDIngressIssueWrapper" {
      firrtl.module @GGFASEDTokenEngine() attributes {goldengate.bridgeConstructor = {
        axi4Edge = {maxReadTransfer = 8 : i64, idReuse = 1 : i64, maxFlight = 10 : i64},
        axi4Widths = {addrBits = 35 : i64, dataBits = 64 : i64, idBits = 4 : i64}}} {}
      firrtl.module @GGFASEDIngressIssueWrapper(in %hostClock: !firrtl.clock,
        out %fased_egress_reset: !firrtl.uint<1>,
        in %fased_host_responses: !firrtl.bundle<rReady: uint<1>, rValid: uint<1>, rLast: uint<1>, bReady: uint<1>, bValid: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })";
  auto replace=[&](llvm::StringRef a,llvm::StringRef z) {
    auto p=text.find(a.str()); require(p!=std::string::npos,"missing replacement");text.replace(p,a.size(),z.str());
  };
  if (variant==1) replace("maxReadTransfer = 8","maxReadTransfer = 9");
  if (variant==2) replace("idReuse = 1","idReuse = 3");
  if (variant==3) replace("maxFlight = 10","maxFlight = 0");
  if (variant==4) replace("addrBits = 35","addrBits = 34");
  if (variant==5) replace("dataBits = 64","dataBits = 128");
  if (variant==6) replace("idBits = 4","idBits = 5");
  if (variant==7) replace("in %hostClock: !firrtl.clock","out %hostClock: !firrtl.clock");
  if (variant==8) replace("!firrtl.clock","!firrtl.uint<1>");
  if (variant==9) replace("out %fased_egress_reset","in %fased_egress_reset");
  if (variant==10) replace("rLast: uint<1>","rLast: uint<2>");
  if (variant==11) replace("in %fased_host_responses","out %fased_host_responses");
  if (variant==12) replace("%other:","%fased_read_buffer_deq:");
  if (variant==13) replace("@GGFASEDTokenEngine","@missingEngine");
  if (variant==14) replace("maxReadTransfer","missingMaxReadTransfer");
  if (variant==18) replace("idReuse = 1 : i64", "idReuse = -1 : i64");
  if (variant==19) replace("idReuse", "missingReuse");
  if (variant==20) replace("idReuse = 1 : i64", "idReuse = 1 : i128");
  if (variant==21) replace("idReuse = 1 : i64", "idReuse = \"1\"");
  if (variant==22) replace("maxFlight", "missingFlight");
  if (variant==23) replace("maxFlight = 10 : i64", "maxFlight = -1 : i64");
  if (variant==24) replace("maxFlight = 10 : i64", "maxFlight = 10 : i128");
  if (variant==25) replace("maxFlight = 10 : i64", "maxFlight = \"10\"");
  if (variant != 2 && !(variant >= 18 && variant <= 21))
    replace("idReuse = 1", "idReuse = " + std::to_string(reuse));
  if (variant != 3 && !(variant >= 22 && variant <= 25))
    replace("maxFlight = 10", "maxFlight = " + std::to_string(flight));
  if (selected) {
    const std::string old = "GGFASEDIngressIssueWrapper", updated = "GGSelectedPrintFASEDIngressIssueWrapper";
    for (size_t at = 0; (at = text.find(old, at)) != std::string::npos; at += updated.size())
      text.replace(at, old.size(), updated);
  }
  auto root=parseSourceString<ModuleOp>(text,&context);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto n:{"other","fased_host_responses","fased_host_responses.rReady","fased_host_responses.rValid",
               "fased_host_responses.rLast","fased_host_responses.bReady","fased_host_responses.bValid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target",b.getStringAttr("~"+c.getName().str()+"|"+c.getName().str()+">"+std::string(n)))}));
  if (variant!=15) c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if (variant==16) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDReadBuffer16x" + std::to_string(8*reuse)),ConventionAttr::get(&context,Convention::Internal),ArrayRef<PortInfo>{});
  }
  if (variant==17) {
    b.setInsertionPointToEnd(named(c,"GGFASEDTokenEngine").getBodyBlock());
    b.create<InstanceOp>(c.getLoc(),named(c,c.getName()),"unexpectedUse");
  }
  return root;
}
void behavior(MLIRContext &context, bool selected, unsigned reuse, unsigned flight) {
  unsigned entries = 8 * reuse;
  auto root=fixture(context,0,selected,reuse,flight);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDReadBuffer(c,error)),error);
  require(succeeded(verify(*root)),"read buffer IR verification failed");
  const std::string outer = selected ? "GGSelectedPrintFASEDReadBufferWrapper" : "GGFASEDReadBufferWrapper";
  const std::string inner = selected ? "GGSelectedPrintFASEDIngressIssueWrapper" : "GGFASEDIngressIssueWrapper";
  auto wrapper=named(c,outer);require(wrapper.getNumPorts()==7,"wrong outer boundary");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef expected[]{"GGFASEDReadBufferWrapper>other","GGFASEDIngressIssueWrapper>fased_host_responses",
    "GGFASEDReadBufferWrapper>fased_host_read_response.ready","GGFASEDReadBufferWrapper>fased_host_read_response.valid",
    "GGFASEDReadBufferWrapper>fased_host_read_response.bits.last","GGFASEDReadBufferWrapper>fased_host_write_responses.bReady",
    "GGFASEDReadBufferWrapper>fased_host_write_responses.bValid"};
  for (auto [i,a]:llvm::enumerate(annos)) {
    std::string target = expected[i].str();
    if (selected) target.insert(2, "SelectedPrint");
    require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue()==
        "~"+outer+"|"+target,"annotation transfer differs");
  }
  Interpreter sim(named(c,"GGFASEDReadBuffer16x" + std::to_string(entries)),entries);std::mt19937_64 rng(216);
  unsigned same=0,advance=0,resetWrites=0,full=0,wrap=0,collisions=0;
  // Independent known-state samples include all IDs and arbitrary pointer/full
  // states, previous-address switches, stalls, wraparound and reset edges.
  for (unsigned cycle=0;cycle<40000;++cycle) {
    std::array<unsigned,16> ep,dp,mf;
    for (unsigned i=0;i<16;++i) { ep[i]=rng()%entries;dp[i]=rng()%entries;mf[i]=rng()%2; }
    unsigned ea=rng()%16,da=rng()%16,previous=rng()%16;
    if (cycle%3==0) da=previous;
    if (cycle%5==0) ea=previous;
    bool valid=rng()%2,ready=rng()%2,dv=rng()%2,reset=cycle%17==0;
    uint64_t data=rng();bool last=rng()%2;
    sim.memo.clear();sim.state.clear();
    for (auto r:sim.module.getOps<RegResetOp>()) {
      auto n=r.getName();unsigned v=0;
      if (n.consume_front("enqPtrs_")) v=ep[std::stoul(n.str())];
      else if (n.consume_front("deqPtrs_")) v=dp[std::stoul(n.str())];
      else if (n.consume_front("maybe_full_")) v=mf[std::stoul(n.str())];
      else { require(n=="deqValid","unexpected register");v=dv; }
      sim.state[r.getResult()]=APInt(sim.width(r.getResult()),v);
    }
    auto id=*sim.module.getOps<RegOp>().begin();sim.state[id.getResult()]=APInt(4,previous);
    auto input=[&](Value v,llvm::StringRef name,unsigned w,uint64_t value) {
      sim.memo.insert_or_assign(sim.key(v)+(name.empty()?"":"."+name.str()),APInt(w,value));
    };
    input(sim.arg(1),"",1,reset);input(sim.arg(2),"valid",1,valid);
    input(sim.arg(2),"bits.data",64,data);input(sim.arg(2),"bits.last",1,last);
    input(sim.arg(3),"ready",1,ready);input(sim.arg(4),"",4,ea);input(sim.arg(5),"",4,da);
    bool enqReady=!(ep[ea]==dp[ea]&&mf[ea]),push=valid&&enqReady,pop=ready&&dv;
    bool prefetch=pop&&previous==da;
    bool empty=prefetch ? ((dp[da]+1)%entries)==ep[da] : dp[da]==ep[da]&&!mf[da];
    unsigned address=da*entries+(dp[da]+unsigned(prefetch))%entries;
    require(sim.drive(sim.arg(2),"ready").getBoolValue()==enqReady,"enqueue full differs");
    require(sim.drive(sim.arg(3),"valid").getBoolValue()==dv,"synchronous dequeue valid differs");
    require(sim.eval(sim.arg(6)).getBoolValue()==empty,"prefetch empty differs");
    require(sim.drive(sim.ram.getResult(0),"addr").getZExtValue()==address,"prefetch address differs");
    require(sim.drive(sim.arg(3),"bits.data")==sim.readOutput.lshr(1).trunc(64)&&
      sim.drive(sim.arg(3),"bits.last")==sim.readOutput.trunc(1),"stored beat unpack differs");
    auto expectedMemory=sim.memory;APInt nextRead=expectedMemory[address];
    if (push) expectedMemory[ea*entries+ep[ea]]=(APInt(65,data)<<1)|APInt(65,last);
    same+=ea==previous;advance+=prefetch;resetWrites+=reset&&push;full+=!enqReady;
    wrap+=pop&&dp[previous]==entries-1;collisions+=push&&ea*entries+ep[ea]==address;
    sim.edge();require(sim.memory==expectedMemory&&sim.readOutput==nextRead,"RAM write/read/reset differs");
    for (auto r:sim.module.getOps<RegResetOp>()) {
      auto n=r.getName();unsigned v=0;
      if (n.consume_front("enqPtrs_")) {unsigned i=std::stoul(n.str());v=(ep[i]+unsigned(push&&ea==i))%entries;}
      else if (n.consume_front("deqPtrs_")) {unsigned i=std::stoul(n.str());v=(dp[i]+unsigned(pop&&previous==i))%entries;}
      else if (n.consume_front("maybe_full_")) {
        unsigned i=std::stoul(n.str());v=mf[i];
        if (ea==previous) {if (ea==i&&push!=pop) v=push;}
        else {if (push&&ea==i) v=1;if (pop&&previous==i) v=0;}
      } else v=!empty;
      require(sim.state.lookup(r.getResult()).getZExtValue()==(reset?0:v),"next per-ID state differs");
    }
    require(sim.state.lookup(id.getResult()).getZExtValue()==da,"read ID was reset or held");
  }
  require(same&&advance&&resetWrites&&full&&wrap&&collisions,"missing coverage");
  llvm::outs()<<"entries="<<entries<<" maxFlight="<<flight<<" 40000 read buffer state/edge samples: same ID "<<same<<", prefetch "<<advance
    <<", reset writes "<<resetWrites<<", full "<<full<<", dequeue wraps "<<wrap<<", RUW collisions "<<collisions<<"\n";
}
void rejection(MLIRContext &context, bool selected, unsigned reuse) {
  for (unsigned v=1;v<=25;++v) {
    auto root=fixture(context,v,selected,reuse);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;
    {llvm::raw_string_ostream s(before);root->print(s);}
    require(failed(goldengate::addFASEDReadBuffer(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream s(after);root->print(s);}
    require(before==after,"rejection mutated IR");
  }
  auto root=fixture(context,0,selected,reuse);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;
  require(succeeded(goldengate::addFASEDReadBuffer(c,error)),error);
  {llvm::raw_string_ostream s(before);root->print(s);}
  require(failed(goldengate::addFASEDReadBuffer(c,error)),"repeated pass accepted");
  {llvm::raw_string_ostream s(after);root->print(s);} require(before==after,"repeat mutated IR");
  llvm::outs()<<"26 atomic rejection cases\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;context.getOrLoadDialect<FIRRTLDialect>();context.getOrLoadDialect<circt::hw::HWDialect>();
    if (argc == 3) {
      auto root = parseSourceFile<ModuleOp>(argv[1], &context);
      require(bool(root) && succeeded(verify(*root)), "production read buffer boundary invalid");
      auto circuit = *root->getOps<CircuitOp>().begin();
      require(circuit.getName() == "GGFASEDReadBufferWrapper" ||
              circuit.getName() == "GGSelectedPrintFASEDReadBufferWrapper",
              "expected FASED read buffer boundary");
      auto isolated = ModuleOp::create(circuit.getLoc()); OpBuilder b(&context);
      b.setInsertionPointToEnd(isolated.getBody());
      FModuleOp production;
      for (auto m : circuit.getOps<FModuleOp>())
        if (m.getName() == "GGFASEDReadBuffer16x8" || m.getName() == "GGFASEDReadBuffer16x16") {
          require(!production, "ambiguous production read buffer"); production = m;
        }
      require(bool(production), "missing production read buffer");
      auto exported = b.create<CircuitOp>(circuit.getLoc(), b.getStringAttr(production.getName()));
      b.setInsertionPointToEnd(exported.getBodyBlock());
      // Clone actual production FIRRTL, including its memory and all control
      // expressions. No queue state or payload logic is rebuilt for this test.
      auto helper = cast<FModuleOp>(b.insert(production->clone()));
      helper.setPublic();
      require(succeeded(verify(isolated)), "exported read buffer invalid");
      std::error_code ec; llvm::raw_fd_ostream out(argv[2], ec);
      require(!ec, "cannot export read buffer"); isolated.print(out); out << '\n';
      out.close(); isolated.erase(); return 0;
    }
    require(argc == 1, "usage: goldengate-fased-read-buffer-test [read-buffer.mlir helper.mlir]");
    for (bool selected : {false,true}) for (unsigned reuse : {1u,2u}) {
      for (unsigned flight : {1u,10u,20u}) behavior(context,selected,reuse,flight);
      rejection(context,selected,reuse);
    } return 0;
  } catch (const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
