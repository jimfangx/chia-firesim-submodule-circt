// See LICENSE for license details.
#include "goldengate/ControlErrorSlave.h"
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
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = 0;
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = 0;
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
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<GTPrimOp>(op)) n = eval(op->getOperand(0)) > eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<CatPrimOp>(op)) n = (eval(op->getOperand(0)) << cast<UIntType>(op->getOperand(1).getType()).getWidthOrSentinel()) | eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
    unsigned width = cast<UIntType>(v.getType()).getWidthOrSentinel();
    if (width < 64) n &= (uint64_t(1) << width) - 1;
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
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGLoadMemControlWrapper" {
    firrtl.module @GGLoadMemControlWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })", &context);
  require(bool(root), "fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (auto name : {"hostClock", "hostReset", "other"}) annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target", b.getStringAttr("~GGLoadMemControlWrapper|GGLoadMemControlWrapper>" + std::string(name)))}));
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Internal")),
      b.getNamedAttr("targets", b.getArrayAttr({b.getStringAttr("~GGLoadMemControlWrapper"),
          b.getStringAttr("~GGLoadMemControlWrapper|Model>state")}))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlErrorSlave(c, 25, 12, error)), error);
  require(succeeded(verify(*root)), "error endpoint invalid");
  Interpreter sim(named(c, "GGControlErrorSlave")); std::mt19937_64 random(175);
  // Independent transaction state: queued request identity, response startup,
  // remaining read beats, and one queued write identity while W is drained.
  bool readQueued = false, writeQueued = false, responding = false, draining = false;
  unsigned remaining = 0, queuedLen = 0, readID = 0, writeID = 0;
  unsigned cycles = 0, reads = 0, writes = 0, blockedAW = 0, blockedAR = 0,
      blockedW = 0, heldR = 0, heldB = 0, resetCaptures = 0, startup = 0;
  auto cycle = [&](bool reset, bool aw, bool w, bool last, bool br, bool ar, unsigned len, bool rr) {
    ++cycles; sim.memo.clear(); unsigned awID = random() & 4095, arID = random() & 4095;
    auto input = [&](unsigned i, uint64_t value) { sim.memo[sim.key(sim.arg(i))] = value; };
    input(1, reset); input(3, aw); input(4, random() & ((1U << 25) - 1)); input(5, awID);
    input(7, w); input(8, last); input(9, br); input(13, ar); input(14, random() & ((1U << 25) - 1));
    input(15, len); input(16, arID); input(17, rr);
    bool canAW = !writeQueued && !draining, canAR = !readQueued;
    bool bv = writeQueued && !draining, rv = readQueued && responding;
    require(sim.eval(sim.arg(2)) == canAW && sim.eval(sim.arg(6)) == draining &&
        sim.eval(sim.arg(12)) == canAR, "request acceptance differs");
    require(sim.eval(sim.arg(10)) == bv && sim.eval(sim.arg(18)) == rv &&
        sim.eval(sim.arg(19)) == (remaining == 0), "response valid/last differs");
    require(sim.eval(sim.arg(11)) == writeID && sim.eval(sim.arg(20)) == readID, "queued IDs differ");
    require(sim.eval(sim.arg(21)) == 0 && sim.eval(sim.arg(22)) == 3 &&
        sim.eval(sim.arg(23)) == 0 && sim.eval(sim.arg(24)) == 3 && sim.eval(sim.arg(25)) == 0,
        "DECERR data/sidebands differ");
    bool takeAW = canAW && aw, takeAR = canAR && ar, takeB = bv && br, takeR = rv && rr;
    bool endRead = takeR && remaining == 0, endWrite = w && draining && last;
    bool beginRead = !responding && readQueued;
    unsigned prints = 0;
    for (auto print : sim.module.getOps<PrintFOp>()) {
      bool expected = print.getFormatString() == "Invalid read address %x\n" ? takeAR : takeAW;
      require(sim.eval(print.getCond()) == (expected && !reset), "invalid-address print condition differs");
      ++prints;
    }
    require(prints == 2, "invalid-address diagnostics missing");
    reads += takeR; writes += takeB; blockedAW += aw && !canAW; blockedAR += ar && !canAR;
    blockedW += w && !draining; heldR += rv && !rr; heldB += bv && !br; startup += beginRead;
    resetCaptures += reset && (takeAW || takeAR);
    if (takeAR) { queuedLen = len; readID = arID; }
    if (takeAW) writeID = awID;
    if (takeAR != endRead) readQueued = takeAR;
    if (takeAW != takeB) writeQueued = takeAW;
    if (beginRead) responding = true;
    if (endRead) responding = false;
    if (takeR && !endRead) --remaining;
    else if (beginRead) remaining = queuedLen;
    if (takeAW) draining = true;
    if (endWrite) draining = false;
    if (reset) { readQueued = writeQueued = responding = draining = false; remaining = 0; }
    sim.edge(); sim.memo.clear();
    for (auto reg : sim.module.getOps<RegResetOp>()) {
      unsigned expected = reg.getName() == "r_full" ? readQueued : reg.getName() == "b_full" ? writeQueued :
          reg.getName() == "responding" ? responding : reg.getName() == "draining" ? draining : remaining;
      require(sim.state.lookup(reg.getResult()) == expected, "next control state differs");
    }
    for (auto reg : sim.module.getOps<RegOp>()) {
      unsigned expected = reg.getName() == "r_len" ? queuedLen : reg.getName() == "r_id" ? readID : writeID;
      require(sim.state.lookup(reg.getResult()) == expected, "reset-time payload capture/retention differs");
    }
  };
  // Exercise every AR len, including the full 256-beat burst, with concurrent
  // blocked reads, early W, and stalls on the terminal beat.
  for (unsigned len = 0; len < 256; ++len) {
    cycle(true, false, true, true, true, false, 0, true);
    cycle(false, false, true, true, true, true, len, true);
    cycle(false, false, true, true, true, true, len, true);
    for (unsigned beat = 0; beat <= len; ++beat) {
      cycle(false, false, true, true, true, true, len, false);
      cycle(false, false, true, true, true, true, len, true);
    }
    require(!readQueued && !responding, "read burst did not terminate");
  }
  for (unsigned length = 1; length <= 16; ++length) {
    cycle(true, false, false, false, false, false, 0, false);
    cycle(false, true, true, true, true, false, 0, false);
    for (unsigned beat = 0; beat < length; ++beat)
      cycle(false, true, true, beat + 1 == length, true, false, 0, false);
    cycle(false, true, true, true, false, false, 0, false);
    cycle(false, true, true, true, true, false, 0, false);
    require(!writeQueued && !draining, "write did not terminate");
  }
  for (unsigned i = 0; i < 20000; ++i)
    cycle(i % 97 == 0, random() & 1, random() & 1, random() & 1, random() & 1,
        random() & 1, random() & 255, random() & 1);
  require(reads && writes && blockedAW && blockedAR && blockedW && heldR && heldB && resetCaptures && startup,
      "missing error transport coverage");
  llvm::outs() << "Control error endpoint: " << cycles << " cycles; all 256 burst lengths, " << reads
      << " R beats, " << writes << " B responses, " << heldR << " R stalls, " << heldB
      << " B stalls, " << resetCaptures << " reset captures matched\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlErrorSlave(c, 25, 12, error)), error);
  auto wrapper = named(c, "GGControlErrorWrapper"), helper = named(c, "GGControlErrorSlave");
  require(wrapper.getNumPorts() == 27 && helper.getNumPorts() == 26, "wrong endpoint boundary");
  Interpreter keys(helper); std::map<std::string, Value> wires; InstanceOp endpoint, sim;
  for (auto instance : wrapper.getOps<InstanceOp>()) {
    if (instance.getName() == "controlError") endpoint = instance;
    if (instance.getName() == "sim") sim = instance;
  }
  for (auto connect : wrapper.getOps<StrictConnectOp>()) wires.emplace(keys.key(connect.getDest()), connect.getSrc());
  for (unsigned i = 0; i < helper.getNumPorts(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(i < 2 ? i : i + 1);
    require(wires.at(keys.key(helper.getPortDirection(i) == Direction::In ? endpoint.getResult(i) : external)) ==
        (helper.getPortDirection(i) == Direction::In ? external : endpoint.getResult(i)), "endpoint binding differs");
    if (i >= 2) require(wrapper.getPortName(i + 1) == "ctrl_error_" + helper.getPortName(i).str(), "endpoint field name differs");
  }
  unsigned copied = 0;
  for (auto connect : wrapper.getOps<ConnectOp>()) {
    unsigned i = copied++;
    require(connect.getDest() == (i < 2 ? sim.getResult(i) : wrapper.getBodyBlock()->getArgument(i)) &&
        connect.getSrc() == (i < 2 ? wrapper.getBodyBlock()->getArgument(i) : sim.getResult(i)), "copied port binding differs");
  }
  require(copied == 3, "copied port missing");
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(raw.size() == 4, "annotations lost");
  for (unsigned i = 0; i < 3; ++i)
    require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue() ==
        "~GGControlErrorWrapper|GGControlErrorWrapper>" + wrapper.getPortName(i).str(), "copied target transfer differs");
  auto targets = cast<DictionaryAttr>(raw[3]).getAs<ArrayAttr>("targets");
  require(cast<StringAttr>(targets[0]).getValue() == "~GGControlErrorWrapper" &&
      cast<StringAttr>(targets[1]).getValue() == "~GGControlErrorWrapper|Model>state", "internal identity transfer differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 12; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGLoadMemControlWrapper");
    OpBuilder b(&context); unsigned address = 25, id = 12;
    if (bad == 0) c.setName("WrongTop"); if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 3 || bad == 4) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 0 : bad == 3 ? 1 : 2] = b.getStringAttr(bad == 4 ? "ctrl_error_aw_valid" : "wrong"); top.setPortNames(names);
    }
    if (bad == 5) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 6 || bad == 7) {
      b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr(
          bad == 6 ? "GGControlErrorSlave" : "GGControlErrorWrapper"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 8) { SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[1] = TypeAttr::get(UIntType::get(&context, 2, false)); top.setPortTypes(types); }
    if (bad == 9) address = 0; if (bad == 10) id = 0; if (bad == 11) id = 65;
    std::string before, after, error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addControlErrorSlave(c, address, id, error)), "bad error endpoint accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mapping(context); rejection(context);
    llvm::outs() << "Endpoint wiring, diagnostics, target transfers and 12 atomic rejections passed\n";
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
