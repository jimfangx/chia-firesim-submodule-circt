// See LICENSE for license details.
#include "goldengate/FASEDRankStateTracker.h"
#include "goldengate/FASEDBankStateTracker.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <functional>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok, llvm::StringRef why) {
  if (!ok) throw std::runtime_error(why.str());
}
static std::string print(Operation *op) {
  std::string text; llvm::raw_string_ostream os(text); op->print(os); return text;
}
// Evaluate the actual retained assertion expressions against independently
// chosen old counter/bank states. The RTL stress test disables illegal-command
// assertions, so validate their enable and legality contracts separately here.
static void checkAssertions(FModuleOp helper) {
  llvm::DenseMap<Value, uint64_t> values;
  std::function<uint64_t(Value)> eval = [&](Value v) -> uint64_t {
    if (values.count(v)) return values.lookup(v);
    auto *op = v.getDefiningOp();
    if (auto c = dyn_cast_or_null<ConstantOp>(op)) return c.getValue().getZExtValue();
    if (isa_and_nonnull<AndPrimOp>(op)) return eval(op->getOperand(0)) & eval(op->getOperand(1));
    if (isa_and_nonnull<OrPrimOp>(op)) return eval(op->getOperand(0)) | eval(op->getOperand(1));
    if (isa_and_nonnull<NotPrimOp>(op)) return !eval(op->getOperand(0));
    if (isa_and_nonnull<EQPrimOp>(op)) return eval(op->getOperand(0)) == eval(op->getOperand(1));
    throw std::runtime_error("unexpected assertion operation/leaf");
  };
  std::mt19937 rng(0x52414e4b);
  unsigned checked = 0;
  for (unsigned i = 0; i < 5000; ++i) {
    values.clear();
    bool fire = rng() % 2, reset = rng() % 2, uses = rng() % 2;
    unsigned cmd = rng() % 8, pre = rng() % 2, act = rng() % 2;
    unsigned casr = rng() % 2, casw = rng() % 2, count = rng() % 5;
    auto arg = [&](unsigned n, uint64_t value) { values[helper.getBodyBlock()->getArgument(n)] = value; };
    arg(1, reset); arg(2, fire); arg(3, cmd); arg(4, uses);
    for (auto r : helper.getOps<RegResetOp>()) {
      if (r.getName() == "nextLegalPRE") values[r.getResult()] = pre;
      if (r.getName() == "nextLegalACT") values[r.getResult()] = act;
      if (r.getName() == "nextLegalCASR") values[r.getResult()] = casr;
      if (r.getName() == "nextLegalCASW") values[r.getResult()] = casw;
      if (r.getName() == "tFAWCount") values[r.getResult()] = count;
    }
    bool canREF = true;
    for (auto instance : helper.getOps<InstanceOp>()) {
      bool ready = rng() % 2;
      values[instance.getResult(11)] = ready;
      canREF &= ready;
    }
    std::array<bool, 5> enabled{uses && cmd == 1, cmd == 4, cmd == 3, uses && cmd == 2, uses && cmd == 5};
    std::array<bool, 5> legal{act == 0 && count != 4, !uses || casr == 0, !uses || casw == 0, pre == 0, canREF};
    unsigned n = 0;
    for (auto a : helper.getOps<AssertOp>()) {
      require(eval(a.getEnable()) == (fire && !reset && enabled[n]), "assertion command/reset/fire gating changed");
      require(eval(a.getPredicate()) == legal[n], "assertion legality predicate changed");
      ++n; ++checked;
    }
  }
  require(checked == 25000, "missing assertion checks");
}
int main(int argc, char **argv) {
  try {
    require(argc <= 2, "usage: goldengate-fased-rank-state-tracker-test [helper.mlir]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto top = *circuit.getOps<FModuleOp>().begin();
    auto before = print(top.getOperation());
    FModuleOp helper; std::string error;
    require(succeeded(goldengate::materializeFASEDRankStateTracker(circuit, helper, error)), error);
    require(succeeded(verify(*root)), "invalid rank helper IR");
    require(before == print(top.getOperation()) && circuit.getName() == "Top", "changed existing top");
    unsigned banks = 0, assertions = 0;
    for (auto inst : helper.getOps<InstanceOp>()) {
      require(inst.getModuleName() == "GGFASEDBankStateTracker", "unexpected child module");
      ++banks;
    }
    for (auto a : helper.getOps<AssertOp>()) { (void)a; ++assertions; }
    require(banks == 8 && assertions == 5, "missing bank instances or command assertions");
    checkAssertions(helper);
    auto ports = helper.getPorts();
    require(ports.size() == 18, "rank interface shape changed");
    require(*cast<UIntType>(ports[7].type).getWidth() == 26, "selected row width");
    require(*cast<UIntType>(ports[9].type).getWidth() == 7, "tCycle wrap width");
    auto vector = cast<FVectorType>(ports.back().type);
    require(vector.getNumElements() == 8, "selected bank count");
    auto unchanged = print(root->getOperation());
    FModuleOp sentinel = top;
    require(failed(goldengate::materializeFASEDRankStateTracker(circuit, sentinel, error)), "duplicate accepted");
    require(sentinel == top && unchanged == print(root->getOperation()), "duplicate rejection changed IR/result");
    // A preexisting bank helper must also reject before adding a rank module.
    auto other = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
    auto otherCircuit = *other->getOps<CircuitOp>().begin(); FModuleOp bank;
    require(succeeded(goldengate::materializeFASEDBankStateTracker(otherCircuit, bank, error)), error);
    unchanged = print(other->getOperation());
    require(failed(goldengate::materializeFASEDRankStateTracker(otherCircuit, sentinel, error)), "existing bank accepted");
    require(sentinel == top && unchanged == print(other->getOperation()), "bank collision changed IR/result");
    auto rankOnly = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} firrtl.module @GGFASEDRankStateTracker() {} } }", &ctx);
    auto rankOnlyCircuit = *rankOnly->getOps<CircuitOp>().begin();
    unchanged = print(rankOnly->getOperation());
    require(failed(goldengate::materializeFASEDRankStateTracker(rankOnlyCircuit, sentinel, error)), "existing rank accepted");
    require(sentinel == top && unchanged == print(rankOnly->getOperation()), "rank collision added bank or changed result");
    if (argc == 2) {
      circuit.setName(helper.getName());
      std::error_code ec; llvm::raw_fd_ostream stream(argv[1], ec);
      require(!ec, "cannot open helper output"); root->print(stream); stream << "\n";
    }
    llvm::outs() << "FASED rank tracker: verified IR, eight banks, 25000 assertion checks, interface and atomic collisions\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
}
