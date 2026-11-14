// See LICENSE for license details.
#include "goldengate/FASEDTimingMonitor.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <array>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) {
  if (!ok)
    throw std::runtime_error(why.str());
}
std::string print(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}
struct Interpreter {
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, uint64_t> memo;
  Interpreter(FModuleOp module) {
    for (auto c : module.getOps<StrictConnectOp>())
      drivers[c.getDest()] = c.getSrc();
  }
  uint64_t eval(Value v) {
    if (memo.count(v))
      return memo.lookup(v);
    auto *op = v.getDefiningOp();
    uint64_t value;
    if (drivers.count(v))
      value = eval(drivers.lookup(v));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op))
      value = c.getValue().getZExtValue();
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      value = eval(bits.getInput()) >> bits.getLo();
    else if (isa_and_nonnull<MuxPrimOp>(op))
      value = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (isa_and_nonnull<PadPrimOp>(op))
      value = eval(op->getOperand(0));
    else if (isa_and_nonnull<NotPrimOp>(op))
      value = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<AndPrimOp>(op))
      value = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op))
      value = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op))
      value = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op))
      value = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<LEQPrimOp>(op))
      value = eval(op->getOperand(0)) <= eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op))
      value = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op))
      value = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else
      throw std::runtime_error("unexpected timing-monitor operation/leaf");
    unsigned width = *cast<UIntType>(v.getType()).getWidth();
    value &= (uint64_t(1) << width) - 1;
    memo[v] = value;
    return value;
  }
};

void behavior(MLIRContext &ctx, unsigned capacity, llvm::StringRef output) {
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto topBefore = print(top.getOperation());
  FModuleOp helper;
  std::string error;
  require(succeeded(goldengate::materializeFASEDTimingMonitor(
              circuit, capacity, helper, error)),
          error);
  require(succeeded(verify(*root)), "invalid timing-monitor IR");
  require(topBefore == print(top.getOperation()) && circuit.getName() == "Top",
          "changed existing top");
  auto ports = helper.getPorts();
  require(ports.size() == 24, "unexpected monitor interface");
  for (unsigned i = 14; i < 17; ++i)
    require(*cast<UIntType>(ports[i].type).getWidth() ==
                llvm::Log2_64_Ceil(capacity + 1),
            "pending width does not match constructor");
  require(cast<FVectorType>(ports[21].type).getNumElements() == 5 &&
              cast<FVectorType>(ports[22].type).getNumElements() == 5,
          "histogram size changed");
  require(cast<FVectorType>(ports[23].type).getNumElements() == 4,
          "selected rank count changed");
  Interpreter sim(helper);
  std::map<std::string, uint64_t> state;
  SmallVector<RegResetOp> regs;
  for (auto r : helper.getOps<RegResetOp>()) {
    regs.push_back(r);
    state[r.getName().str()] = 0;
  }
  require(regs.size() == 33 && helper.getOps<RegOp>().empty(),
          "all monitor state must have synchronous reset");
  std::mt19937 rng(0x504f5745 + capacity);
  unsigned resets = 0, stalledReset = 0, simultaneous = 0, satFull = 0,
           satEmpty = 0, overHistogram = 0, wrapped = 0;
  constexpr std::array<unsigned, 4> bounds{0, 2, 4, 8};
  for (unsigned cycle = 0; cycle < 24000; ++cycle) {
    // Seed only wrapping counters at boundaries, preserving the tied-zero last
    // histogram register. Occupancies evolve through legal widths and bounds.
    if (cycle % 257 == 0)
      for (auto &[name, value] : state)
        if (name.find("pending") != 0 && name.back() != '4')
          value = UINT32_MAX - (rng() % 3);
    bool fire = cycle < 300 || (rng() % 5 != 0);
    bool reset = cycle == 0 || (cycle > 300 && rng() % 113 == 0);
    std::array<unsigned, 7> events;
    for (auto &e : events)
      e = rng() & 1;
    if (cycle < 100)
      events = {1, 1, 1, 1, 0, 0, 0};
    else if (cycle < 200)
      events = {0, 0, 0, 0, 1, 1, 1};
    else if (cycle < 300)
      events = {1, 1, 1, 1, 1, 1, 1};
    unsigned cmd = rng() % 8, rank = rng() % 4, rankStates = rng() % 16;
    std::array<unsigned, 4> banks;
    for (auto &mask : banks)
      mask = rng() % 3 == 0 ? 255 : rng() % 256;
    sim.memo.clear();
    auto arg = [&](unsigned i) {
      return helper.getBodyBlock()->getArgument(i);
    };
    sim.memo[arg(1)] = reset;
    sim.memo[arg(2)] = fire;
    for (unsigned i = 0; i < events.size(); ++i)
      sim.memo[arg(3 + i)] = events[i];
    sim.memo[arg(10)] = cmd;
    sim.memo[arg(11)] = rank;
    sim.memo[arg(12)] = rankStates;
    for (auto sub : helper.getOps<SubindexOp>())
      if (sub.getInput() == arg(13))
        sim.memo[sub.getResult()] = banks[sub.getIndex()];
    for (auto reg : regs)
      sim.memo[reg.getResult()] = state.at(reg.getName().str());
    auto expected = state;
    if (fire) {
      auto pending = [&](const char *name, bool inc, bool dec) {
        auto old = state.at(name);
        if (inc && !dec && old < capacity)
          ++expected[name];
        if (!inc && dec && old > 0)
          --expected[name];
        simultaneous += inc && dec;
        satFull += inc && !dec && old == capacity;
        satEmpty += !inc && dec && old == 0;
      };
      pending("pendingReads", events[0], events[4] && events[5]);
      pending("pendingAWReq", events[1], events[6]);
      pending("pendingWReq", events[2] && events[3], events[6]);
      auto count = [&](const std::string &name, bool inc) {
        if (inc) {
          expected[name] = uint32_t(state.at(name) + 1);
          wrapped += expected[name] == 0;
        }
      };
      count("totalReads", events[0]);
      count("totalWrites", events[1]);
      count("totalReadBeats", events[4]);
      count("totalWriteBeats", events[2]);
      for (auto [prefix, pendingName] :
           {std::pair<const char *, const char *>{"readOutstandingHistogram",
                                                  "pendingReads"},
            {"writeOutstandingHistogram", "pendingAWReq"}}) {
        uint64_t occupancy = state.at(pendingName);
        overHistogram += occupancy > 8;
        for (unsigned i = 0; i < bounds.size(); ++i)
          if (occupancy <= bounds[i]) {
            count(std::string(prefix) + "_" + std::to_string(i), true);
            break;
          }
      }
      for (unsigned i = 0; i < 4; ++i) {
        std::string prefix = "rank" + std::to_string(i) + "_";
        count(prefix + "allPreCycles",
              !(rankStates & (1 << i)) && banks[i] == 255);
        count(prefix + "numACT", rank == i && cmd == 1);
        count(prefix + "numCASR", rank == i && cmd == 4);
        count(prefix + "numCASW", rank == i && cmd == 3);
      }
    }
    if (fire && reset) {
      for (auto &[name, value] : expected)
        value = 0;
      ++resets;
    }
    stalledReset += reset && !fire;
    // Evaluate emitted feedback and reset separately, as a synchronous register
    // edge would. No expected operation is used to produce the actual next
    // state.
    for (auto reg : regs) {
      uint64_t actual = sim.eval(reg.getResetSignal())
                            ? sim.eval(reg.getResetValue())
                            : sim.eval(sim.drivers.lookup(reg.getResult()));
      auto name = reg.getName().str();
      require(actual == expected.at(name),
              "counter/histogram/power transition differs: " + name);
    }
    unsigned outputLeaves = 0;
    for (auto c : helper.getOps<StrictConnectOp>()) {
      Value dest = c.getDest();
      if (dest.getDefiningOp<RegResetOp>())
        continue;
      std::string expectedName;
      if (auto field = dest.getDefiningOp<SubfieldOp>()) {
        auto index = field.getInput().getDefiningOp<SubindexOp>();
        require(index && index.getInput() == arg(23),
                "unexpected power output path");
        expectedName = "rank" + std::to_string(index.getIndex()) + "_" +
                       field.getFieldName().str();
      } else if (auto index = dest.getDefiningOp<SubindexOp>()) {
        require(index.getInput() == arg(21) || index.getInput() == arg(22),
                "unexpected histogram output path");
        expectedName = std::string(index.getInput() == arg(21)
                                       ? "readOutstandingHistogram_"
                                       : "writeOutstandingHistogram_") +
                       std::to_string(index.getIndex());
      } else {
        auto block = dyn_cast<BlockArgument>(dest);
        require(block && block.getArgNumber() >= 14 &&
                    block.getArgNumber() <= 20,
                "unexpected scalar output path");
        expectedName = ports[block.getArgNumber()].name.getValue().str();
      }
      require(sim.eval(dest) == state.at(expectedName),
              "output does not expose matching old register: " + expectedName);
      ++outputLeaves;
    }
    require(outputLeaves == 33, "missing monitor output leaves");
    state = expected;
  }
  require(resets && stalledReset && simultaneous && satFull && satEmpty &&
              overHistogram && wrapped,
          "missing reset/stall/saturation/wrap/histogram coverage");
  require(state.at("readOutstandingHistogram_4") == 0 &&
              state.at("writeOutstandingHistogram_4") == 0,
          "Scala zip bug must retain zero fifth histogram bin");
  auto unchanged = print(root->getOperation());
  FModuleOp sentinel = top;
  require(failed(goldengate::materializeFASEDTimingMonitor(
              circuit, capacity, sentinel, error)),
          "duplicate accepted");
  require(sentinel == top && unchanged == print(root->getOperation()),
          "symbol collision changed circuit/result");
  for (unsigned invalid : {0u, 33u, 64u}) {
    require(failed(goldengate::materializeFASEDTimingMonitor(
                circuit, invalid, sentinel, error)),
            "unsupported capacity accepted");
    require(sentinel == top && unchanged == print(root->getOperation()),
            "capacity failure changed circuit/result");
  }
  std::string fixture = "module { firrtl.circuit \"Top\" { firrtl.module "
                        "@Top() {} firrtl.extmodule @GGFASEDTimingMonitor" +
                        std::to_string(capacity) + "() } }";
  auto other = parseSourceString<ModuleOp>(fixture, &ctx);
  require(bool(other), "invalid external collision fixture");
  auto otherCircuit = *other->getOps<CircuitOp>().begin();
  unchanged = print(other->getOperation());
  require(failed(goldengate::materializeFASEDTimingMonitor(
              otherCircuit, capacity, sentinel, error)),
          "external collision accepted");
  require(sentinel == top && unchanged == print(other->getOperation()),
          "external symbol failure changed circuit/result");
  if (!output.empty()) {
    circuit.setName(helper.getName());
    std::error_code ec;
    llvm::raw_fd_ostream stream(output, ec);
    require(!ec, "cannot open monitor output");
    root->print(stream);
    stream << "\n";
  }
  llvm::outs() << "FASED timing monitor " << capacity << ": 24000 transitions; "
               << simultaneous << " simultaneous occupancy events; " << satFull
               << " full saturations; " << satEmpty << " empty saturations; "
               << wrapped << " wraps; " << overHistogram
               << " above-bin samples; " << stalledReset << " stalled resets\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(
        argc <= 3,
        "usage: goldengate-fased-timing-monitor-test [helper.mlir] [10|20]");
    unsigned emitCapacity = argc == 3 ? std::stoul(argv[2]) : 20;
    require(emitCapacity == 10 || emitCapacity == 20,
            "output capacity must be 10 or 20");
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned capacity : {10u, 16u, 20u, 32u})
      behavior(
          ctx, capacity, argc >= 2 && capacity == emitCapacity ? argv[1] : "");
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << "\n";
    return 1;
  }
}
