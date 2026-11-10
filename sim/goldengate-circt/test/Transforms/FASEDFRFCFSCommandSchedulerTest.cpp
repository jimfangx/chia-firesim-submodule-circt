// See LICENSE for license details.
#include "goldengate/FASEDFRFCFSCommandScheduler.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
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
static void checkLatency(FModuleOp helper) {
  llvm::DenseMap<Value, uint64_t> values;
  std::function<uint64_t(Value)> eval = [&](Value v) -> uint64_t {
    if (values.count(v)) return values.lookup(v);
    auto *op = v.getDefiningOp();
    if (auto c = dyn_cast_or_null<ConstantOp>(op)) return c.getValue().getZExtValue();
    if (isa_and_nonnull<AddPrimOp>(op)) return eval(op->getOperand(0)) + eval(op->getOperand(1));
    if (isa_and_nonnull<PadPrimOp>(op)) return eval(op->getOperand(0));
    if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      return (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi()-bits.getLo()+1))-1);
    throw std::runtime_error("unexpected latency expression");
  };
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  Value readLatency;
  for (auto c : helper.getOps<StrictConnectOp>())
    if (c.getDest() == arg(19)) readLatency = c.getSrc();
  require(bool(readLatency), "missing read latency output");
  SmallVector<Value> casFields, alFields;
  for (auto f : helper.getOps<SubfieldOp>()) {
    if (f.getInput() != arg(8)) continue;
    if (f.getFieldName() == "tCAS") casFields.push_back(f.getResult());
    if (f.getFieldName() == "tAL") alFields.push_back(f.getResult());
  }
  std::mt19937 rng(0x46524643);
  for (unsigned i=0; i<10000; ++i) {
    values.clear();
    unsigned cas=rng()%128, al=rng()%128, backend=rng()%4096;
    values[arg(12)] = backend;
    for (Value v : casFields) values[v] = cas;
    for (Value v : alFields) values[v] = al;
    require(eval(readLatency) == ((((cas+al)&127)+backend)&4095), "latency intermediate/final wrap changed");
  }
}
int main(int argc, char **argv) {
  try {
    require(argc <= 2, "usage: goldengate-fased-frfcfs-command-scheduler-test [helper.mlir]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto top = *circuit.getOps<FModuleOp>().begin();
    auto before = print(top.getOperation());
    FModuleOp helper; std::string error;
    require(succeeded(goldengate::materializeFASEDFRFCFSCommandScheduler(circuit, helper, error)), error);
    require(succeeded(verify(*root)), "invalid composed scheduler IR");
    require(before == print(top.getOperation()) && circuit.getName() == "Top", "changed existing top");
    unsigned ranks=0, refresh=0, windows=0;
    for (auto inst : helper.getOps<InstanceOp>()) {
      ranks += inst.getModuleName() == "GGFASEDRankStateTracker";
      refresh += inst.getModuleName() == "GGFASEDRefreshUnit";
      windows += inst.getModuleName() == "GGFASEDCollapsingBuffer";
    }
    require(ranks == 4 && refresh == 1 && windows == 1, "missing composed dependencies");
    checkLatency(helper);
    auto snapshot = print(root->getOperation());
    FModuleOp sentinel = top;
    require(failed(goldengate::materializeFASEDFRFCFSCommandScheduler(circuit, sentinel, error)), "duplicate accepted");
    require(sentinel == top && snapshot == print(root->getOperation()), "duplicate changed IR/result");
    for (auto name : {"GGFASEDFRFCFSCommandScheduler", "GGFASEDRankStateTracker", "GGFASEDBankStateTracker", "GGFASEDRefreshUnit", "GGFASEDCollapsingBuffer"}) {
      for (bool external : {false, true}) {
        std::string source = "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} ";
        source += external ? "firrtl.extmodule @" : "firrtl.module @";
        source += name;
        source += external ? "() } }" : "() {} } }";
        auto collision = parseSourceString<ModuleOp>(source, &ctx);
        require(bool(collision), "collision fixture parse failed");
        auto c = *collision->getOps<CircuitOp>().begin();
        auto old = print(collision->getOperation());
        require(failed(goldengate::materializeFASEDFRFCFSCommandScheduler(c, sentinel, error)), "dependency collision accepted");
        require(sentinel == top && old == print(collision->getOperation()), "collision partially mutated dependencies");
      }
    }
    if (argc == 2) {
      circuit.setName(helper.getName());
      std::error_code ec; llvm::raw_fd_ostream stream(argv[1], ec);
      require(!ec, "cannot open helper output"); root->print(stream); stream << "\n";
    }
    llvm::outs() << "FRFCFS command scheduler: verified composed IR, four ranks, refresh/window, 10000 latency checks, atomic symbol failures\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
}
