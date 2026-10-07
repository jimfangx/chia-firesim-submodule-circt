// See LICENSE for license details.
#include "goldengate/RationalClockTokenGenerator.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
struct Case { const char *name; std::vector<std::pair<unsigned, unsigned>> ratios; };
// The Scala oracle independently elaborates these same constructor values.
const std::vector<Case> cases{
  {"single", {{2,2}}}, {"base-half", {{1,1},{1,2}}},
  {"two-three", {{1,2},{1,3}}}, {"three-two", {{1,3},{1,2}}},
  {"two-three-four", {{1,2},{1,3},{1,4}}},
  {"unreduced", {{2,4},{3,9},{4,16}}},
  {"equal", {{2,2},{3,3},{2147483647,2147483647}}},
  {"cross-product", {{2147483647,2147483646},{2147483647,1073741823}}},
  {"limit", {{1,1},{1,65535}}}};
// Interpret the emitted FIRRTL SSA graph. Register drivers are evaluated from
// old state, all updates commit together, and synchronous reset has precedence.
struct Interpreter {
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, uint64_t> memo, state;
  FModuleOp module;
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>()) drivers[c.getDest()] = c.getSrc();
  }
  uint64_t eval(Value v) {
    if (memo.count(v)) return memo.lookup(v);
    Operation *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(v)) n = eval(drivers.lookup(v));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi()-bits.getLo()+1))-1);
    else throw std::runtime_error("unsupported scheduler operation");
    memo[v] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value,uint64_t> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.lookup(r.getResult()));
    state = std::move(next);
  }
};
void behavior(MLIRContext &context, bool observations, llvm::StringRef directory) {
  for (auto &test : cases) {
    std::vector<goldengate::RationalClockInfo> clocks;
    for (auto [n,d] : test.ratios) clocks.push_back({"",n,d,1});
    std::string error;
    auto schedule = goldengate::analyzeRationalClockSchedule(clocks,error);
    require(bool(schedule),error);
    OpBuilder b(&context); auto loc = b.getUnknownLoc();
    auto root = ModuleOp::create(loc); OwningOpRef<ModuleOp> owned(root);
    b.setInsertionPointToStart(root.getBody());
    auto circuit = b.create<CircuitOp>(loc,b.getStringAttr("Tokens"));
    b.setInsertionPointToStart(circuit.getBodyBlock());
    auto bit = UIntType::get(&context,1,false);
    SmallVector<PortInfo> ports{
      {b.getStringAttr("clock"),ClockType::get(&context),Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In},
      {b.getStringAttr("ready"),bit,Direction::In},
      {b.getStringAttr("valid"),bit,Direction::Out}};
    for (unsigned i=0;i<clocks.size();++i)
      ports.push_back({b.getStringAttr("bit"+std::to_string(i)),bit,Direction::Out});
    auto module = b.create<FModuleOp>(loc,b.getStringAttr("Tokens"),
      ConventionAttr::get(&context,Convention::Internal),ports);
    b.setInsertionPointToStart(module.getBodyBlock());
    auto args=module.getBodyBlock()->getArguments();
    auto tokens=goldengate::buildRationalClockTokens(b,loc,args[0],args[1],args[2],*schedule);
    Value valid=b.create<ConstantOp>(loc,bit,APInt(1,1));
    b.create<StrictConnectOp>(loc,args[3],valid);
    for (auto [i,token] : llvm::enumerate(tokens)) b.create<StrictConnectOp>(loc,args[i+4],token);
    require(succeeded(verify(root)),"scheduler verification failed");
    if (observations) {
      llvm::outs()<<"PERIOD "<<test.name<<" "<<schedule->counterWidth;
      for (auto p:schedule->periods) llvm::outs()<<" "<<p;
      llvm::outs()<<"\n";
    }
    if (!directory.empty()) {
      std::error_code ec;
      llvm::raw_fd_ostream file(directory.str()+"/"+test.name+".circt.mlir",ec);
      require(!ec,"cannot write candidate IR"); root.print(file);
    }
    Interpreter sim(module);
    uint64_t previous=0; bool stalled=false;
    unsigned cycles=std::string(test.name)=="limit"?200000:4096;
    for (unsigned cycle=0;cycle<cycles;++cycle) {
      bool reset=cycle<2 || cycle==cycles/2;
      bool ready=cycle%97>=13 && cycle%7!=0;
      sim.memo.clear();sim.memo[args[0]]=0;sim.memo[args[1]]=reset;sim.memo[args[2]]=ready;
      uint64_t mask=0;
      for (auto [i,token]:llvm::enumerate(tokens)) mask|=sim.eval(token)<<i;
      require(sim.eval(args[3])==1 && mask!=0,"empty or invalid clock token");
      if (stalled) require(mask==previous,"clock token changed under backpressure");
      previous=mask;stalled=!ready && !reset;
      sim.edge();
      if (observations) llvm::outs()<<"TOKEN "<<test.name<<" "<<cycle<<" 1 "<<mask<<"\n";
    }
  }
}
void rejection() {
  std::string error;
  for (auto ratios:std::vector<std::vector<goldengate::RationalClockInfo>>{
      {},{{"",0,1,1}},{{"",1,0,1}},{{"",2147483648ULL,1,1}},
      {{"",1,2147483648ULL,1}},{{"",1,1,1},{"",1,65536,1}},
      {{"",65536,1,1},{"",1,1,1}},
      {{"",65521,1,1},{"",65519,1,1},{"",65513,1,1}}})
    require(!goldengate::analyzeRationalClockSchedule(ratios,error) && !error.empty(),"bad schedule accepted");
  // Many coprime Int32 equal ratios would overflow a UInt64 product. Scala's
  // BigInt product/GCD and the bounded relative-fraction calculation both give 1.
  std::vector<goldengate::RationalClockInfo> equal;
  for(unsigned n:{2147483647U,2147483629U,2147483587U,2147483579U}) equal.push_back({"",n,n,1});
  auto normalized=goldengate::analyzeRationalClockSchedule(equal,error);
  require(normalized && normalized->counterWidth==1 &&
    llvm::all_of(normalized->periods,[](unsigned p){return p==1;}),"normalization overflow");
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    behavior(context,argc>1 && llvm::StringRef(argv[1])=="--observations",argc>2?argv[2]:"");rejection();
    if(argc==1) llvm::outs()<<"PASS rational clock scheduling: stalls, reset, lane order, exact periods, 16-bit bounds\n";
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
}
