// See LICENSE for license details.
#include "goldengate/FASEDFRFCFSTimingModel.h"
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

static void require(bool ok, StringRef text) {
  if (!ok)
    throw std::runtime_error(text.str());
}
static std::string print(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}
static void checkWiringAndAssertions(FModuleOp model) {
  llvm::DenseMap<Value, Value> sources;
  llvm::DenseMap<Value, uint64_t> values;
  auto arg = [&](StringRef name) -> Value {
    for (unsigned i = 0; i < model.getNumPorts(); ++i)
      if (model.getPortName(i) == name)
        return model.getBodyBlock()->getArgument(i);
    llvm_unreachable("unknown model port");
  };
  for (auto con : model.getOps<StrictConnectOp>())
    sources[con.getDest()] = con.getSrc();
  unsigned children = 0;
  InstanceOp llc, dram, monitor, releaser;
  for (auto i : model.getOps<InstanceOp>()) {
    ++children;
    if (i.getName() == "llc")
      llc = i;
    if (i.getName() == "dram")
      dram = i;
    if (i.getName() == "monitor")
      monitor = i;
    if (i.getName() == "release")
      releaser = i;
  }
  require(children == 4 && llc && dram && monitor && releaser,
          "timing model must own LLC, controller, monitor and releaser");
  auto pin = [&](InstanceOp i, StringRef name) -> Value {
    for (unsigned p = 0; p < i.getNumResults(); ++p)
      if (i.getPortName(p) == name)
        return i.getResult(p);
    llvm_unreachable("unknown child port");
  };
  for (auto n : {"clock", "reset", "targetFire"})
    for (auto child : {llc, dram, monitor, releaser})
      require(sources.lookup(pin(child, n)) == arg(n),
              "clock/reset/fire wiring differs");
  for (auto n : {"pendingAWReq", "pendingWReq"})
    require(sources.lookup(pin(dram, n)) == pin(monitor, n),
            "controller must use native target pending counts");
  require(sources.lookup(pin(llc, "memRValid")) ==
              pin(dram, "completedReadValid"),
          "LLC must consume native backend completion");
  require(sources.lookup(pin(dram, "completedReadReady")) ==
              pin(llc, "memRReady"),
          "completion backpressure must return from native LLC");
  require(sources.lookup(pin(releaser, "nextReadValid")) ==
              pin(llc, "rRespValid"),
          "target responses must be released from native LLC");
  require(sources.lookup(pin(llc, "rRespReady")) ==
              pin(releaser, "nextReadReady"),
          "LLC must observe native response occupancy");
  require(sources.lookup(arg("targetCycle")) == pin(dram, "targetCycle"),
          "model must share one native target-cycle counter");

  std::function<uint64_t(Value)> eval = [&](Value v) -> uint64_t {
    if (values.count(v))
      return values.lookup(v);
    if (sources.count(v))
      return eval(sources.lookup(v));
    Operation *op = v.getDefiningOp();
    if (auto c = dyn_cast_or_null<ConstantOp>(op))
      return c.getValue().getZExtValue();
    if (isa_and_nonnull<AndPrimOp>(op))
      return eval(op->getOperand(0)) & eval(op->getOperand(1));
    if (isa_and_nonnull<OrPrimOp>(op))
      return eval(op->getOperand(0)) | eval(op->getOperand(1));
    if (isa_and_nonnull<NotPrimOp>(op))
      return !eval(op->getOperand(0));
    if (isa_and_nonnull<EQPrimOp>(op))
      return eval(op->getOperand(0)) == eval(op->getOperand(1));
    if (isa_and_nonnull<LTPrimOp>(op))
      return eval(op->getOperand(0)) < eval(op->getOperand(1));
    throw std::runtime_error("unsupported protocol predicate operation");
  };
  std::mt19937_64 rng(0x50524f544f);
  for (unsigned j = 0; j < 5000; ++j) {
    values.clear();
    bool fire = rng() & 1, reset = rng() & 1;
    values[arg("targetFire")] = fire;
    values[arg("reset")] = reset;
    unsigned a = 0;
    for (auto ch : {"ar", "aw"}) {
      std::string stem(ch);
      bool valid = rng() & 1, ready = rng() & 1;
      uint64_t length = rng() % 256, burst = rng() % 4;
      if (j % 5 == 0)
        length = 0;
      values[arg(stem + "Valid")] = valid;
      values[arg(stem + "Ready")] = ready;
      values[arg(stem + "Len")] = length;
      values[arg(stem + "Burst")] = burst;
      SmallVector<bool> expected{!valid || burst == 1 ||
                                     (burst == 0 && length == 0),
                                 !(valid && ready) || length < 8};
      unsigned k = 0;
      for (auto assertion : model.getOps<AssertOp>()) {
        if (k >= a && k < a + 2) {
          require(eval(assertion.getEnable()) == (fire && !reset),
                  "protocol assertion gating changed");
          require(bool(eval(assertion.getPredicate())) == expected[k - a],
                  "protocol assertion predicate changed");
        }
        ++k;
      }
      require(k == 4, "missing burst protocol/length diagnostics");
      a += 2;
    }
  }
}
int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 3, "usage: test [10|20 output.mlir]");
    unsigned selected = argc == 3 ? std::stoul(argv[1]) : 0;
    require(!selected || selected == 10 || selected == 20,
            "unsupported selected capacity");
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned capacity : {10, 20}) {
      if (selected && capacity != selected)
        continue;
      auto root = parseSourceString<ModuleOp>(
          "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",
          &ctx);
      auto circuit = *root->getOps<CircuitOp>().begin();
      auto top = *circuit.getOps<FModuleOp>().begin();
      auto original = print(top);
      FModuleOp helper;
      std::string error;
      require(succeeded(goldengate::materializeFASEDFRFCFSTimingModel(
                  circuit, capacity, helper, error)),
              error);
      require(succeeded(verify(*root)), "invalid composed timing model IR");
      require(print(top) == original && circuit.getName() == "Top",
              "original top changed");
      checkWiringAndAssertions(helper);
      auto saved = print(root->getOperation());
      auto sentinel = helper;
      require(failed(goldengate::materializeFASEDFRFCFSTimingModel(
                  circuit, capacity, sentinel, error)),
              "duplicate accepted");
      require(saved == print(root->getOperation()) && sentinel == helper,
              "duplicate changed circuit/result");
      require(failed(goldengate::materializeFASEDFRFCFSTimingModel(
                  circuit, 11, sentinel, error)),
              "unsupported capacity accepted");
      require(saved == print(root->getOperation()),
              "invalid capacity changed circuit");
      for (auto child : circuit.getOps<FModuleOp>()) {
        if (child == top)
          continue;
        for (bool external : {false, true}) {
          std::string text =
              "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} ";
          text += external ? "firrtl.extmodule @" : "firrtl.module @";
          text += child.getName().str();
          text += external ? "() } }" : "() {} } }";
          auto collision = parseSourceString<ModuleOp>(text, &ctx);
          auto c = *collision->getOps<CircuitOp>().begin();
          auto before = print(collision->getOperation());
          require(failed(goldengate::materializeFASEDFRFCFSTimingModel(
                      c, capacity, sentinel, error)),
                  "dependency collision accepted");
          require(before == print(collision->getOperation()) &&
                      sentinel == helper,
                  "dependency failure partially mutated circuit/result");
        }
      }
      if (argc == 3) {
        circuit.setName(helper.getName());
        std::error_code ec;
        llvm::raw_fd_ostream os(argv[2], ec);
        require(!ec, "output failed");
        root->print(os);
        os << "\n";
      }
    }
    llvm::outs()
        << "FRFCFS timing model: both capacities, native feedback composition, "
           "10000 protocol vectors and atomic dependencies\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << "\n";
    return 1;
  }
}
