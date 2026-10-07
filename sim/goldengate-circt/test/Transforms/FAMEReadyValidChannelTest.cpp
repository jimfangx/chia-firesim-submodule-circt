// See LICENSE for license details.
#include "goldengate/FAMEReadyValidChannel.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLDialect.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include <deque>
#include <functional>
#include <map>
#include <tuple>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, llvm::StringRef message) {
  if (!condition) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}

// Recursive evaluation handles FIRRTL wires used to express the queue and
// completion dependencies. Missing drivers and combinational cycles are errors.
struct Interpreter {
  FModuleOp module;
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, uint64_t> registers, memo;
  llvm::DenseSet<Value> active;
  Interpreter(FModuleOp module) : module(module) {
    for (auto c : module.getOps<StrictConnectOp>())
      require(drivers.try_emplace(c.getDest(), c.getSrc()).second, "multiple drivers");
  }
  uint64_t eval(Value v) {
    if (auto i = memo.find(v); i != memo.end()) return i->second;
    require(active.insert(v).second, "combinational cycle");
    uint64_t result;
    auto *op = v.getDefiningOp();
    if (op && isa<RegOp, RegResetOp>(op)) result = registers.lookup(v);
    else if (auto i = drivers.find(v); i != drivers.end()) result = eval(i->second);
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) result = c.getValue().getZExtValue();
    else if (op && isa<NotPrimOp>(op)) result = !eval(op->getOperand(0));
    else if (op && isa<AndPrimOp>(op)) result = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (op && isa<OrPrimOp>(op)) result = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (op && isa<MuxPrimOp>(op)) result = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto cat = dyn_cast_or_null<CatPrimOp>(op))
      result = (eval(op->getOperand(0)) << cast<UIntType>(op->getOperand(1).getType()).getWidthOrSentinel()) |
               eval(op->getOperand(1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) {
      unsigned width = bits.getHi() - bits.getLo() + 1;
      result = (eval(op->getOperand(0)) >> bits.getLo()) & ((uint64_t(1) << width) - 1);
    } else if (op && isa<AsUIntPrimOp, AsSIntPrimOp>(op)) result = eval(op->getOperand(0));
    else throw std::runtime_error("unsupported operation or missing driver");
    active.erase(v); memo[v] = result; return result;
  }
  Value arg(unsigned index) { return module.getBodyBlock()->getArgument(index); }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (Operation &op : module.getBodyBlock()->getOperations()) {
      if (auto r = dyn_cast<RegResetOp>(op))
        next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.lookup(r.getResult()));
      else if (auto r = dyn_cast<RegOp>(op)) next[r.getResult()] = eval(drivers.lookup(r.getResult()));
    }
    registers = std::move(next);
  }
};

// Independent FIFO model: flow queues expose the incoming token when empty;
// the reference queue advances only when all host transactions complete.
void update(std::deque<uint64_t> &q, bool valid, uint64_t bits, bool ready,
            bool flow, bool reset) {
  bool accept = valid && q.size() < 2;
  bool bypass = flow && q.empty() && ready;
  if (reset) { q.clear(); return; }
  if (ready && !q.empty()) q.pop_front();
  if (accept && !bypass) q.push_back(bits);
}
void behavior(MLIRContext &context, unsigned width) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &context);
  auto circuit = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addFAMEReadyValidChannel(circuit, width, error)), error);
  require(succeeded(verify(*root)), "ReadyValidChannel IR verification failed");
  Interpreter sim(named(circuit, "GGFAMEReadyValid" + std::to_string(width)));
  std::deque<uint64_t> fwd, rev, reference;
  bool enqFired = false, deqFired = false;
  std::mt19937_64 random(144 + width);
  auto mask = (uint64_t(1) << width) - 1;
  unsigned fires = 0, delayed = 0, resetTokens = 0, bypasses = 0;
  for (unsigned cycle = 0; cycle < 20000; ++cycle) {
    bool reset = cycle < 2 || cycle % 499 < 2;
    bool enqValid = random() & 1, fwdValid = random() & 1, revValid = random() & 1;
    bool targetReady = random() & 1, enqRevReady = random() & 1, deqFwdReady = random() & 1;
    bool resetValid = (random() % 5) != 0, resetBit = random() % 31 == 0;
    uint64_t bits = random() & mask;
    sim.memo.clear();
    for (auto [index, value] : std::initializer_list<std::pair<unsigned, uint64_t>>{
         {0, 0}, {1, reset}, {3, enqValid}, {4, bits}, {6, fwdValid},
         {7, enqRevReady}, {9, targetReady}, {12, deqFwdReady}, {15, revValid},
         {16, resetBit}, {17, resetValid}}) sim.memo[sim.arg(index)] = value;
    bool fv = !fwd.empty() || fwdValid, rv = !rev.empty() || revValid;
    uint64_t fd = fwd.empty() ? (uint64_t(enqValid) << width) | bits : fwd.front();
    uint64_t rd = rev.empty() ? targetReady : rev.front();
    bool done = (enqFired || enqRevReady) && (deqFired || deqFwdReady);
    bool fire = resetValid && fv && rv && done;
    bool fqReady = resetValid && rv && done, rqReady = resetValid && fv && done;
    require(sim.eval(sim.arg(2)) == (reference.size() < 2), "target enqueue readiness mismatch");
    require(sim.eval(sim.arg(5)) == (fwd.size() < 2), "forward flow queue readiness mismatch");
    require(sim.eval(sim.arg(14)) == (rev.size() < 2), "reverse flow queue readiness mismatch");
    require(sim.eval(sim.arg(8)) == !enqFired, "reverse completion state mismatch");
    require(sim.eval(sim.arg(13)) == !deqFired, "forward completion state mismatch");
    require(sim.eval(sim.arg(18)) == (fv && rv && done), "target reset readiness mismatch");
    require(sim.eval(sim.arg(10)) == !reference.empty(), "target dequeue valid mismatch");
    if (!reference.empty()) require(sim.eval(sim.arg(11)) == reference.front(), "payload order or stall mismatch");
    sim.edge();
    fires += fire; resetTokens += fire && resetBit;
    delayed += enqFired || deqFired;
    bypasses += fwd.empty() && fwdValid && fqReady;
    update(fwd, fwdValid, (uint64_t(enqValid) << width) | bits, fqReady, true, reset);
    update(rev, revValid, targetReady, rqReady, true, reset);
    update(reference, fire && (fd >> width), fd & mask, fire && rd, false,
           reset || (fire && resetBit));
    enqFired = reset ? false : !fire && (enqFired || enqRevReady);
    deqFired = reset ? false : !fire && (deqFired || deqFwdReady);
  }
  llvm::outs() << "width=" << width << " fires=" << fires << " delayed=" << delayed << " resetTokens=" << resetTokens << " bypasses=" << bypasses << '\n';
  require(fires > 1000 && resetTokens > 50 && delayed > 1000 && bypasses > 500,
          "insufficient coverage of token completion, reset or flow bypass");
}

OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned bad) {
  std::string text = R"mlir(
module { firrtl.circuit "Top" {
 firrtl.module @Top(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
 out %a: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<x: uint<3>, pad: uint<0>, y: uint<5>, valid: uint<1>>>,
 in %ar: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
 in %b: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<x: uint<3>, pad: uint<0>, y: uint<5>, valid: uint<1>>>,
 out %br: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>) {}
} }
)mlir";
  auto replace = [&](const std::string &from, const std::string &to) {
    for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size())
      text.replace(pos, from.size(), to);
  };
  if (bad == 5 || bad == 7 || bad == 11) replace("x: uint<3>", "x: sint<3>");
  if (bad == 6 || bad == 7) replace("y: uint<5>", "y: sint<5>");
  if (bad >= 5 && bad <= 7) replace("pad: uint<0>", "pad: sint<0>");
  if (bad == 8) replace("x: uint<3>", "x: clock");
  if (bad == 9) replace("x: uint<3>", "x flip: uint<3>");
  if (bad == 10) replace("y: uint<5>", "y: bundle<nested: uint<5>>");
  if (bad == 11) replace("y: uint<5>", "y: uint<0>");
  auto root = parseSourceString<ModuleOp>(text, &context);
  OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (unsigned i = 0; i < 2; ++i) {
    std::string name = i == 0 ? "send" : "receive", port = i == 0 ? "a" : "b", ready = i == 0 ? "ar" : "br";
    auto target = [&](llvm::StringRef suffix) { return b.getStringAttr("~Top|Top>" + port + ".bits." + suffix); };
    auto rt = b.getStringAttr("~Top|Top>" + ready + ".bits");
    auto endpoints = b.getArrayAttr({target("x"), target("pad"), target("y"), target("valid")});
    auto info = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DecoupledForwardChannel)),
      b.getNamedAttr(i == 0 ? "validSource" : "validSink", target("valid")),
      b.getNamedAttr(i == 0 ? "readySink" : "readySource", bad == 3 && i == 0 ? target("valid") : rt)});
    auto annotation = [&](llvm::StringRef suffix, Attribute kind, Attribute ep, bool source) {
      return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
       b.getNamedAttr("globalName", b.getStringAttr(name + suffix)), b.getNamedAttr("channelInfo", kind),
       b.getNamedAttr(source ? "sources" : "sinks", ep)});
    };
    auto forward = annotation("_fwd", info,
       bad == 2 && i == 0 ? b.getArrayAttr({target("valid")}) : endpoints, i == 0);
    if (bad == 4 && i == 0) {
      NamedAttrList attrs(forward);
      attrs.set("clock", b.getStringAttr("~Top|Top>differentClock"));
      forward = attrs.getDictionary(&context);
    }
    annotations.push_back(forward);
    auto revInfo = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DecoupledReverseChannel))});
    if (!(bad == 1 && i == 0)) annotations.push_back(annotation("_rev", revInfo, b.getArrayAttr({rt}), i != 0));
  }
  auto c = *root->getOps<CircuitOp>().begin(); c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return root;
}
// A single nonempty signed leaf must enter the UInt queue through a cast even
// when there is no CatPrimOp to coerce its type implicitly.
void signedScalar(MLIRContext &context) {
  auto root = fixture(context, 11);
  auto circuit = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(goldengate::addFAMEBoundaryReadyValidChannels(circuit, error)), error);
  require(succeeded(verify(*root)), "single signed payload failed verification");
  auto wrapper = named(circuit, "GGFAMEPipeWrapper");
  unsigned checked = 0;
  for (auto instance : wrapper.getOps<InstanceOp>()) {
    if (!instance.getName().starts_with("ReadyValidChannel_")) continue;
    require(instance.getModuleName() == "GGFAMEReadyValid3", "single signed leaf width");
    for (auto connect : wrapper.getOps<ConnectOp>()) {
      if (connect.getDest() != instance.getResult(4)) continue;
      auto cast = connect.getSrc().getDefiningOp<AsUIntPrimOp>();
      require(bool(cast) && cast.getInput().getType() == SIntType::get(&context, 3),
              "single signed leaf lacks unsigned queue input");
      Interpreter evaluation(wrapper);
      for (uint64_t bits = 0; bits < 8; ++bits) {
        evaluation.memo.clear(); evaluation.memo[cast.getInput()] = bits;
        require(evaluation.eval(cast.getResult()) == bits, "single signed leaf changed bits");
      }
      ++checked;
    }
  }
  require(checked == 2, "single signed leaf must cover both bridge orientations");
}
void wrapper(MLIRContext &context, const char *output) {
  for (unsigned bad = 0; bad < 11; ++bad) {
    auto root = fixture(context, bad); auto c = *root->getOps<CircuitOp>().begin();
    std::string error;
    require(succeeded(goldengate::addFAMEPipeWrapper(c, error)), error);
    auto result = goldengate::addFAMEBoundaryReadyValidChannels(c, error);
    if (bad && !(bad >= 5 && bad <= 7)) {
      require(failed(result), "invalid pair accepted");
      require(!llvm::any_of(c.getOps<FModuleOp>(), [](FModuleOp m) { return m.getName().starts_with("GGFAMEReadyValid"); }), "mutated rejected circuit");
      continue;
    }
    require(succeeded(result), error);
    require(succeeded(verify(*root)), "wrapper failed verification");
    auto w = named(c, "GGFAMEPipeWrapper");
    auto endpoint = [&](Value value) {
      std::function<std::string(Value)> describe = [&](Value v) -> std::string {
        if (auto f = v.getDefiningOp<SubfieldOp>())
          return describe(f.getInput()) + "." +
              cast<BundleType>(f.getInput().getType()).getElements()[f.getFieldIndex()].name.getValue().str();
        if (auto a = dyn_cast<BlockArgument>(v)) return "external." + w.getPortName(a.getArgNumber()).str();
        if (auto i = v.getDefiningOp<InstanceOp>()) return i.getName().str() + "." + i.getPortName(cast<OpResult>(v).getResultNumber()).str();
        return "expression";
      };
      return describe(value);
    };
    std::map<std::string, Value> drivers;
    for (auto c : w.getOps<ConnectOp>())
      require(drivers.emplace(endpoint(c.getDest()), c.getSrc()).second, "duplicate wrapper driver");
    unsigned instances = 0;
    for (auto i : w.getOps<InstanceOp>())
      if (i.getName().starts_with("ReadyValidChannel_")) {
        require(i.getModuleName() == "GGFAMEReadyValid8", "payload packing width mismatch");
        bool send = i.getName() == "ReadyValidChannel_send";
        std::string enq = send ? "target_FAMETop.a" : "external.b";
        std::string enqR = send ? "target_FAMETop.ar" : "external.br";
        std::string deq = send ? "external.a" : "target_FAMETop.b";
        std::string deqR = send ? "external.ar" : "target_FAMETop.br";
        auto io = [&](unsigned port) { return endpoint(i.getResult(port)); };
        for (auto [dest, src] : std::initializer_list<std::pair<std::string, std::string>>{
          {io(0), "external.hostClock"}, {io(1), "external.hostReset"},
          {enqR + ".bits", io(2)}, {io(3), enq + ".bits.valid"},
          {enq + ".ready", io(5)}, {io(6), enq + ".valid"},
          {io(7), enqR + ".ready"}, {enqR + ".valid", io(8)},
          {io(9), deqR + ".bits"}, {deq + ".bits.valid", io(10)},
          {io(12), deq + ".ready"}, {deq + ".valid", io(13)},
          {deqR + ".ready", io(14)}, {io(15), deqR + ".valid"}})
          require(endpoint(drivers.at(dest)) == src, "wrapper orientation or handshake mismatch");
        for (unsigned port : {16, 17}) {
          auto constant = drivers.at(io(port)).getDefiningOp<ConstantOp>();
          require(constant && constant.getValue().getZExtValue() == (port == 17), "wrapper reset token mismatch");
        }
        auto pack = drivers.at(io(4)).getDefiningOp<CatPrimOp>();
        auto uncast = [&](Value v, bool isSigned, bool packing) {
          if (isSigned) {
            auto *op = v.getDefiningOp();
            require(op && (packing ? isa<AsUIntPrimOp>(op) : isa<AsSIntPrimOp>(op)),
                    "signed payload lost its type conversion");
            return op->getOperand(0);
          }
          return v;
        };
        bool signedX = bad == 5 || bad == 7, signedY = bad == 6 || bad == 7;
        require(pack && endpoint(uncast(pack.getOperand(0), signedX, true)) == enq + ".bits.x" &&
                        endpoint(uncast(pack.getOperand(1), signedY, true)) == enq + ".bits.y", "payload packing lost field identity");
        for (auto [field, high, low] : std::initializer_list<std::tuple<const char *, unsigned, unsigned>>{
            {"x", 7, 5}, {"y", 4, 0}}) {
          auto value = drivers.at(deq + ".bits." + field);
          auto slice = uncast(value, std::string(field) == "x" ? signedX : signedY, false).getDefiningOp<BitsPrimOp>();
          require(slice && slice.getOperand() == i.getResult(11) && slice.getHi() == high && slice.getLo() == low,
                  "payload unpacking lost field identity");
        }
        require(endpoint(drivers.at(deq + ".bits.pad")) == enq + ".bits.pad",
                "zero-width payload identity changed");
        // Exhaust every 3/5-bit pattern, including signed minima and -1.
        // The queue transports bits; sign interpretation belongs to the leaves.
        Interpreter evaluation(w);
        for (uint64_t value = 0; value < 256; ++value) {
          evaluation.memo.clear();
          evaluation.memo[uncast(pack.getOperand(0), signedX, true)] = value >> 5;
          evaluation.memo[uncast(pack.getOperand(1), signedY, true)] = value & 31;
          evaluation.memo[i.getResult(11)] = value;
          require(evaluation.eval(pack.getResult()) == value, "payload pack sign-extended or reordered bits");
          require(evaluation.eval(drivers.at(deq + ".bits.x")) == value >> 5 &&
                  evaluation.eval(drivers.at(deq + ".bits.y")) == (value & 31),
                  "payload unpack changed signed bit pattern");
        }
        ++instances;
      }
    require(instances == 2, "both pair orientations must materialize");
    if (output && bad == 5) {
      std::error_code ec; llvm::raw_fd_ostream out(output, ec);
      require(!ec, "cannot write signed wrapper evidence");
      root->print(out);
    }
    require(succeeded(goldengate::activateFAMEPipeWrapper(c, error)), error);
    require(succeeded(verify(*root)), "active wrapper failed verification");
  }
}
// Secondary fanout removal shifts all following ReadyValid port ordinals.
// Match the retained target by identity when replacing its passthroughs.
void fanoutWrapper(MLIRContext &context) {
  auto root = fixture(context, 0);
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, "Top"); OpBuilder b(&context);
  auto bit = UIntType::get(&context, 1);
  auto type = BundleType::get(&context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bit}});
  SmallVector<std::pair<unsigned, PortInfo>> additions;
  for (StringRef name : {"forkPrimary", "forkSecondary"})
    additions.emplace_back(2, PortInfo(b.getStringAttr(name), type, Direction::In));
  top.insertPorts(additions);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  for (StringRef name : {"forkPrimary", "forkSecondary"})
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr(name)),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
        b.getNamedAttr("latency", b.getI64IntegerAttr(0))})),
      b.getNamedAttr("sinks", b.getArrayAttr({b.getStringAttr("~Top|Top>" + name + ".bits")}))}));
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelFanout)),
      b.getNamedAttr("channelNames", b.getArrayAttr({b.getStringAttr("forkPrimary"), b.getStringAttr("forkSecondary")}))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  std::string error;
  require(succeeded(goldengate::addFAMEBoundaryPipeChannels(circuit, error)), error);
  require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(goldengate::addFAMEBoundaryReadyValidChannels(circuit, error)), error);
  require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(verify(*root)), "fanout shifted ReadyValid wrapper failed verification");
  auto wrapper = named(circuit, "GGFAMEPipeWrapper");
  require(wrapper.getNumPorts() == top.getNumPorts()-1 && wrapper.getPortName(3) == "a",
          "secondary fanout port was not removed before ReadyValid mapping");
}

} // namespace
int main(int argc, char **argv) {
  MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    wrapper(context, argc > 1 ? argv[1] : nullptr);
    signedScalar(context);
    fanoutWrapper(context);
    for (unsigned width : {1, 8, 32}) behavior(context, width);
    if (argc > 3) {
      // Exercise an ingested production-Scala boundary, not a rebuilt fixture.
      auto root = parseSourceFile<ModuleOp>(argv[2], &context);
      require(bool(root), "cannot parse ingested boundary");
      auto circuit = *root->getOps<CircuitOp>().begin();
      std::string error;
      require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
      require(succeeded(goldengate::addFAMEBoundaryReadyValidChannels(circuit, error)), error);
      require(succeeded(verify(*root)), "ingested ReadyValid wrapper failed verification");
      std::error_code ec; llvm::raw_fd_ostream out(argv[3], ec);
      require(!ec, "cannot write ingested wrapper evidence");
      root->print(out);
    }
  }
  catch (const std::exception &e) { llvm::errs() << "ReadyValidChannel: " << e.what() << '\n'; return 1; }
  llvm::outs() << "ReadyValidChannel: both orientations, signed/unsigned exhaustive payloads, malformed pairs and 60000 randomized cycles passed\n";
}
