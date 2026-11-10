// See LICENSE for license details.
#include "goldengate/FASEDRefreshUnit.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) {
  if (!ok) throw std::runtime_error(why.str());
}
std::string print(Operation *op) {
  std::string text; llvm::raw_string_ostream os(text); op->print(os); return text;
}
// Evaluate actual emitted operations; the reference below separately expresses
// the Scala eligibility sets and first-member selection, including no-match ABI.
struct Interpreter {
  FModuleOp module;
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, uint64_t> memo;
  Interpreter(FModuleOp module) : module(module) {
    for (auto c : module.getOps<StrictConnectOp>()) drivers[c.getDest()] = c.getSrc();
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t eval(Value v) {
    if (memo.count(v)) return memo.lookup(v);
    uint64_t n = 0; auto *op = v.getDefiningOp();
    if (drivers.count(v)) n = eval(drivers.lookup(v));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (auto b = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(b.getInput()) >> b.getLo();
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NEQPrimOp>(op)) n = eval(op->getOperand(0)) != eval(op->getOperand(1));
    else throw std::runtime_error("unsupported operation or undriven value");
    n &= (1u << *cast<UIntType>(v.getType()).getWidth()) - 1;
    memo[v] = n; return n;
  }
};
unsigned first(unsigned mask, unsigned count) {
  for (unsigned i = 0; i < count; ++i) if (mask & (1u << i)) return i;
  return count - 1;
}
void behavior(MLIRContext &ctx, llvm::StringRef output) {
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  circuit->setAttr("test.keep", StringAttr::get(&ctx, "identity"));
  auto topBefore = print(top.getOperation());
  FModuleOp helper; std::string error;
  require(succeeded(goldengate::materializeFASEDRefreshUnit(circuit, helper, error)), error);
  require(succeeded(verify(*root)), "invalid refresh helper IR");
  require(circuit.getName() == "Top" && topBefore == print(top.getOperation()), "changed existing top");
  require(circuit->getAttrOfType<StringAttr>("test.keep").getValue() == "identity", "changed metadata");
  require(helper.getPorts().size() == 13, "unexpected refresh interface");
  constexpr std::array<unsigned, 13> widths{4,4,4,8,8,8,8,4,1,2,1,2,3};
  for (auto [i, p] : llvm::enumerate(helper.getPorts())) {
    require(*cast<UIntType>(p.type).getWidth() == widths[i], "wrong port width");
    require(p.direction == (i < 8 ? Direction::In : Direction::Out), "wrong port direction");
  }
  require(helper.getOps<RegOp>().empty() && helper.getOps<RegResetOp>().empty(), "refresh must be combinational");
  Interpreter sim(helper);
  std::mt19937 rng(0x52454634);
  unsigned cases = 0, both = 0, masked = 0, noPre = 0, multiRank = 0, multiBank = 0;
  auto sample = [&](unsigned want, unsigned ref, unsigned pre, std::array<unsigned,4> banks, unsigned use) {
    sim.memo.clear();
    std::array<unsigned,8> inputs{want,ref,pre,banks[0],banks[1],banks[2],banks[3],use};
    for (unsigned i = 0; i < inputs.size(); ++i) sim.memo[sim.arg(i)] = inputs[i];
    unsigned readyRef = want & ref & use;
    unsigned readyPre = want & pre & use;
    for (unsigned i = 0; i < 4; ++i) if (!banks[i]) readyPre &= ~(1u << i);
    unsigned rank = first(readyPre,4);
    std::array<unsigned,5> expected{unsigned(readyRef != 0),first(readyRef,4),unsigned(readyPre != 0),rank,first(banks[rank],8)};
    for (unsigned i = 0; i < expected.size(); ++i)
      require(sim.eval(sim.arg(8+i)) == expected[i], "refresh arbitration differs from Scala contract");
    both += readyRef && readyPre;
    masked += bool((want & ref & ~use) || (want & pre & ~use));
    noPre += !readyPre;
    multiRank += bool(readyPre & (readyPre-1));
    multiBank += bool(banks[rank] & (banks[rank]-1));
    ++cases;
  };
  // Exhaust all four rank masks independently; bank masks vary in every case.
  for (unsigned want=0;want<16;++want) for (unsigned ref=0;ref<16;++ref)
    for (unsigned pre=0;pre<16;++pre) for (unsigned use=0;use<16;++use)
      sample(want,ref,pre,{unsigned(rng())&255,unsigned(rng())&255,unsigned(rng())&255,unsigned(rng())&255},use);
  // Exhaust every bank mask in each rank against all rank-use masks. Include
  // empty competing ranks, and force highest rank as the invalid mux default.
  for (unsigned rank=0;rank<4;++rank) for (unsigned bank=0;bank<256;++bank)
    for (unsigned use=0;use<16;++use) {
      std::array<unsigned,4> banks{}; banks[rank]=bank;
      sample(15,15,15,banks,use);
      sample(0,15,15,banks,use);
    }
  for (unsigned i=0;i<20000;++i)
    sample(unsigned(rng())&15,unsigned(rng())&15,unsigned(rng())&15,{unsigned(rng())&255,unsigned(rng())&255,unsigned(rng())&255,unsigned(rng())&255},unsigned(rng())&15);
  require(both && masked && noPre && multiRank && multiBank, "missing arbitration edge cases");
  auto unchanged = print(root->getOperation()); FModuleOp sentinel = top;
  require(failed(goldengate::materializeFASEDRefreshUnit(circuit,sentinel,error)), "duplicate helper accepted");
  require(sentinel == top && unchanged == print(root->getOperation()), "collision changed IR/result");
  auto external = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} firrtl.extmodule @GGFASEDRefreshUnit() } }", &ctx);
  require(bool(external), "invalid extmodule collision fixture");
  auto other = *external->getOps<CircuitOp>().begin();
  unchanged = print(external->getOperation());
  require(failed(goldengate::materializeFASEDRefreshUnit(other,sentinel,error)), "external symbol collision accepted");
  require(sentinel == top && unchanged == print(external->getOperation()), "external collision changed IR/result");
  if (!output.empty()) {
    circuit.setName(helper.getName());
    std::error_code ec; llvm::raw_fd_ostream stream(output,ec);
    require(!ec,"cannot open helper MLIR output"); root->print(stream); stream << "\n";
  }
  llvm::outs() << "FASED refresh unit: " << cases << " cases; " << both << " dual proposals; "
    << masked << " masked requests; " << noPre << " invalid PRE proposals; "
    << multiRank << " multi-rank and " << multiBank << " multi-bank priorities; atomic collisions\n";
}
}
int main(int argc,char **argv) {
  try {
    require(argc<=2,"usage: goldengate-fased-refresh-unit-test [helper.mlir]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    behavior(ctx,argc==2?argv[1]:""); return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
}
