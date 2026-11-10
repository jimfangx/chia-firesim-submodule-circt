// See LICENSE for license details.
#include "goldengate/FASEDFRFCFSMMIO.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <fstream>
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
  std::string s;
  llvm::raw_string_ostream os(s);
  op->print(os);
  return s;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> values;
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      drivers[path(c.getDest())] = c.getSrc();
  }
  std::string path(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>())
      return path(f.getInput()) + "_" + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>())
      return path(f.getInput()) + "_" + std::to_string(f.getIndex());
    if (auto a = dyn_cast<BlockArgument>(v))
      return module.getPortName(a.getArgNumber()).str();
    if (auto r = v.getDefiningOp<RegResetOp>())
      return "reg_" + r.getName().str();
    return "value_" +
           std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  uint64_t eval(Value v) {
    auto p = path(v);
    if (values.count(p))
      return values.at(p);
    uint64_t n;
    auto *op = v.getDefiningOp();
    if (drivers.count(p))
      n = eval(drivers.at(p));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op))
      n = c.getValue().getZExtValue();
    else if (auto b = dyn_cast_or_null<BitsPrimOp>(op))
      n = eval(b.getInput()) >> b.getLo();
    else if (isa_and_nonnull<MuxPrimOp>(op))
      n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (isa_and_nonnull<AndPrimOp>(op))
      n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op))
      n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<NEQPrimOp>(op))
      n = eval(op->getOperand(0)) != eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op))
      n = eval(op->getOperand(0));
    else
      throw std::runtime_error("unknown MMIO value: " + p);
    unsigned width = *cast<UIntType>(v.getType()).getWidth();
    n &= (uint64_t(1) << width) - 1;
    return n;
  }
  uint64_t named(const std::string &p) {
    require(drivers.count(p), "missing output " + p);
    return eval(drivers.at(p));
  }
};
std::string boundary(const goldengate::FASEDFRFCFSMMIORegister &r) {
  std::string p = r.port;
  if (r.index >= 0)
    p += "_" + std::to_string(r.index);
  if (!r.field.empty())
    p += "_" + r.field;
  return p;
}
void exercise(MLIRContext &ctx, const std::string &output) {
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto before = print(top);
  FModuleOp bank;
  std::string error;
  require(
      succeeded(goldengate::materializeFASEDFRFCFSMMIO(circuit, bank, error)),
      error);
  require(succeeded(verify(*root)), "invalid MMIO IR");
  require(print(top) == before && circuit.getName() == "Top", "changed top");
  auto rows = goldengate::getFASEDFRFCFSMMIORegisters();
  require(rows.size() == 68, "catalogue size");
  auto registry = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  unsigned rw = 0;
  for (auto [i, r] : llvm::enumerate(rows)) {
    auto d = cast<DictionaryAttr>(registry[i]);
    require(d.getAs<StringAttr>("name").getValue() == r.name &&
                d.getAs<IntegerAttr>("offset").getInt() == 4 * i &&
                d.getAs<BoolAttr>("readable").getValue() &&
                d.getAs<BoolAttr>("writeable").getValue() == r.writeable,
            "catalogue registry mismatch");
    rw += r.writeable;
  }
  require(rw == 32, "RW count");
  require(std::distance(bank.getOps<PrintFOp>().begin(),
                        bank.getOps<PrintFOp>().end()) == 36,
          "RO diagnostic count");
  std::map<std::string, uint32_t> state;
  SmallVector<RegResetOp> regs;
  for (auto r : bank.getOps<RegResetOp>()) {
    regs.push_back(r);
    state["reg_" + r.getName().str()] = 0;
  }
  require(regs.size() == 34, "register count");
  Interpreter sim(bank);
  std::mt19937 rng(0xFA5ED68);
  std::array<uint32_t, 68> expected{};
  uint32_t errors[2]{};
  unsigned writes = 0, captures = 0, resets = 0;
  for (unsigned cycle = 0; cycle < 2048; ++cycle) {
    bool reset = cycle == 0 || cycle % 127 == 0;
    resets += reset;
    bool rv = rng() % 2, rr = rng() % 2, bv = rng() % 2, br = rng() % 2;
    unsigned rp = rng() % 4, bp = rng() % 4;
    sim.values.clear();
    for (auto [p, v] : state)
      sim.values[p] = v;
    sim.values["reset"] = reset;
    sim.values["rValid"] = rv;
    sim.values["rReady"] = rr;
    sim.values["bValid"] = bv;
    sim.values["bReady"] = br;
    sim.values["rResp"] = rp;
    sim.values["bResp"] = bp;
    sim.values["mcr_wstrb"] = rng() % 16;
    std::array<bool, 68> valid;
    std::array<uint32_t, 68> data;
    for (auto [i, r] : llvm::enumerate(rows)) {
      valid[i] = cycle < 68 ? i == cycle : rng() % 4 == 0;
      data[i] = rng();
      sim.values["mcr_write_" + std::to_string(i) + "_valid"] = valid[i];
      sim.values["mcr_write_" + std::to_string(i) + "_bits"] = data[i];
      sim.values["mcr_read_" + std::to_string(i) + "_ready"] = rng() % 2;
      if (!r.writeable && i < 66)
        sim.values[boundary(r)] = rng() & ((uint64_t(1) << r.width) - 1);
      uint32_t want = r.writeable ? expected[i]
                      : i >= 66   ? errors[i - 66]
                                  : sim.values[boundary(r)];
      require(sim.named("mcr_read_" + std::to_string(i) + "_bits") == want,
              "readback mismatch");
      require(sim.named("mcr_read_" + std::to_string(i) + "_valid") == 1 &&
                  sim.named("mcr_write_" + std::to_string(i) + "_ready") == 1,
              "handshake mismatch");
      if (r.writeable)
        require(sim.named(boundary(r)) ==
                    (uint64_t(expected[i]) & ((uint64_t(1) << r.width) - 1)),
                "model truncation mismatch");
    }
    std::map<std::string, uint32_t> next;
    for (auto r : regs)
      next["reg_" + r.getName().str()] =
          reset ? sim.eval(r.getResetValue())
                : sim.eval(sim.drivers.at(sim.path(r.getResult())));
    for (auto [i, r] : llvm::enumerate(rows))
      if (r.writeable) {
        if (reset)
          expected[i] = r.initial;
        else if (valid[i]) {
          expected[i] = data[i];
          ++writes;
        }
      }
    if (reset)
      errors[0] = errors[1] = 0;
    else {
      if (rv && rr && rp) {
        errors[0] = rp;
        ++captures;
      }
      if (bv && br && bp) {
        errors[1] = rp;
        ++captures;
      }
    }
    state = next;
  }
  require(writes > 10000 && captures > 500 && resets > 10,
          "insufficient behavior coverage");
  // Symbol rejection preserves both circuit and output handle.
  auto all = print(circuit);
  auto unchanged = bank;
  require(failed(goldengate::materializeFASEDFRFCFSMMIO(circuit, bank, error)),
          "duplicate accepted");
  require(bank == unchanged && print(circuit) == all,
          "rejection mutated circuit/output");
  if (!output.empty()) {
    circuit.setName(bank.getName());
    top.erase();
    std::error_code ec;
    llvm::raw_fd_ostream out(output, ec);
    require(!ec, "fixture open failed");
    root->print(out);
  }
  llvm::outs() << "FRFCFS MMIO: 68 words, 32 RW, 2048 host cycles, " << writes
               << " writes, " << captures << " error captures, " << resets
               << " resets pass\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 3 && StringRef(argv[1]) == "--catalogue") {
      llvm::json::Array a;
      for (auto [i, r] :
           llvm::enumerate(goldengate::getFASEDFRFCFSMMIORegisters()))
        a.push_back(llvm::json::Object{{"name", r.name},
                                       {"index", int64_t(i)},
                                       {"width", int64_t(r.width)},
                                       {"initial", int64_t(r.initial)},
                                       {"writeable", r.writeable},
                                       {"native_port", boundary(r)}});
      std::error_code ec;
      llvm::raw_fd_ostream os(argv[2], ec);
      require(!ec, "catalogue open failed");
      os << llvm::formatv("{0:2}", llvm::json::Value(std::move(a))) << "\n";
      return 0;
    }
    std::string output;
    if (argc == 3 && StringRef(argv[1]) == "--fixture")
      output = argv[2];
    else
      require(argc == 1,
              "usage: [--fixture output.mlir | --catalogue output.json]");
    MLIRContext ctx;
    ctx.getOrLoadDialect<FIRRTLDialect>();
    ctx.getOrLoadDialect<circt::hw::HWDialect>();
    exercise(ctx, output);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << "\n";
    return 1;
  }
}
