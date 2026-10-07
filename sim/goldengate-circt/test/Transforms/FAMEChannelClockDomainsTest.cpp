// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEFiredRegister.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEInputChannel.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const std::string &why) {
  if (!condition) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
unsigned eval(Value v, const llvm::DenseMap<Value, unsigned> &values) {
  if (auto found = values.find(v); found != values.end()) return found->second;
  if (auto c = v.getDefiningOp<ConstantOp>()) return c.getValue().getZExtValue();
  if (auto p = v.getDefiningOp<AsUIntPrimOp>()) return eval(p.getInput(), values);
  if (auto p = v.getDefiningOp<NotPrimOp>()) return !eval(p.getInput(), values);
  if (auto p = v.getDefiningOp<AndPrimOp>()) return eval(p.getLhs(), values) & eval(p.getRhs(), values);
  if (auto p = v.getDefiningOp<OrPrimOp>()) return eval(p.getLhs(), values) | eval(p.getRhs(), values);
  if (auto p = v.getDefiningOp<MuxPrimOp>())
    return eval(p.getSel(), values) ? eval(p.getHigh(), values) : eval(p.getLow(), values);
  throw std::runtime_error("unexpected fired-state expression");
}
void fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Top" {
    firrtl.module @Top(in %clock1: !firrtl.clock, in %clock0: !firrtl.clock,
      in %in0: !firrtl.uint<1>, in %in1: !firrtl.uint<1>,
      out %out0: !firrtl.uint<1>, out %out1: !firrtl.uint<1>,
      out %alias0: !firrtl.clock, out %alias1: !firrtl.clock) {}
    firrtl.module private @Model(in %clock1: !firrtl.clock, in %clock0: !firrtl.clock,
      in %in0: !firrtl.uint<1>, in %in1: !firrtl.uint<1>,
      out %out0: !firrtl.uint<1>, out %out1: !firrtl.uint<1>,
      out %alias0: !firrtl.clock, out %alias1: !firrtl.clock) {
      %wire = firrtl.wire : !firrtl.clock
      firrtl.strictconnect %wire, %clock0 : !firrtl.clock
      %node = firrtl.node %wire : !firrtl.clock
      firrtl.strictconnect %alias0, %node : !firrtl.clock
      firrtl.strictconnect %alias1, %clock1 : !firrtl.clock
      firrtl.strictconnect %out0, %in0 : !firrtl.uint<1>
      firrtl.strictconnect %out1, %in1 : !firrtl.uint<1>
    }
  } })mlir", &ctx);
  require(bool(root), "clock-domain fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto model = *std::next(circuit.getOps<FModuleOp>().begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto inst = b.create<InstanceOp>(top.getLoc(), model, "model");
  for (unsigned i = 0; i < 8; ++i) {
    bool input = model.getPortDirection(i) == Direction::In;
    b.create<StrictConnectOp>(top.getLoc(), input ? inst.getResult(i) : top.getArgument(i),
                             input ? top.getArgument(i) : inst.getResult(i));
  }
  SmallVector<Attribute> annos;
  auto targets = [&](StringRef mod, StringRef port) {
    return b.getArrayAttr({b.getStringAttr("~Top|" + mod.str() + ">" + port.str())});
  };
  // Reorder annotation directions and clocks independently of physical ports.
  for (StringRef name : {"out1", "in0", "out0", "in1"}) {
    auto clock = name.ends_with("0") ? "alias0" : "alias1";
    annos.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts)),
      b.getNamedAttr("localName", b.getStringAttr(name)),
      b.getNamedAttr("clockPort", b.getStringAttr("~Top|Model>" + std::string(clock))),
      b.getNamedAttr("ports", targets("Model", name))}));
    annos.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr(name)),
      b.getNamedAttr("clock", b.getStringAttr("~Top|Top>" + std::string(clock))),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
        b.getNamedAttr("latency", b.getI64IntegerAttr(0))})),
      b.getNamedAttr(name.starts_with("in") ? "sinks" : "sources", targets("Top", name))}));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<goldengate::FAMEHubClockDomain> domains{
      {1, 1, "clock0", "clock0", "_0", {"d0", 1, 2, 2}},
      {0, 0, "clock1", "clock1", "_1", {"d1", 1, 3, 3}}};
  std::string error;
  auto before = dump(*root);
  auto assignments = goldengate::analyzeFAMEChannelClockDomains(circuit, model, domains, error);
  require(assignments && assignments->size() == 4 && dump(*root) == before, error);
  for (const auto &a : *assignments)
    require(a.modelClockName == (StringRef(a.localName).ends_with("0") ? "clock0" : "clock1"),
            "clock identity followed port or annotation order");
  // A second global output on this same clock must reuse the producer's
  // assignment. Every branch still has to agree with its local clock.
  Annotation alias(annos[1]);
  alias.setMember("globalName", b.getStringAttr("out1_alias"));
  annos.push_back(alias.getAttr());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  before = dump(*root);
  error.clear();
  auto shared = goldengate::analyzeFAMEChannelClockDomains(circuit, model, domains, error);
  require(shared && shared->size() == 4 && dump(*root) == before,
          "shared producer created another clock-domain FSM: " + error);
  alias.setMember("clock", b.getStringAttr("~Top|Top>alias0"));
  annos.back() = alias.getAttr();
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  before = dump(*root); error.clear();
  require(!goldengate::analyzeFAMEChannelClockDomains(circuit, model, domains, error) &&
              !error.empty() && dump(*root) == before,
          "shared producer's different clock accepted");
  annos.pop_back();
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  // All rejections must leave the boundary unchanged.
  auto aliasConnect = *std::next(model.getOps<StrictConnectOp>().begin());
  Value original = aliasConnect.getSrc();
  for (unsigned bad = 0; bad < 5; ++bad) {
    b.setInsertionPoint(aliasConnect);
    Operation *temporary = nullptr;
    if (bad == 0) { // Same clock in both mux arms still depends on selector.
      auto mux = b.create<MuxPrimOp>(model.getLoc(), model.getArgument(2), original, original);
      temporary = mux; aliasConnect->setOperand(1, mux.getResult());
    } else if (bad == 1) aliasConnect->setOperand(1, model.getArgument(7));
    else if (bad == 2) { auto wire = b.create<WireOp>(model.getLoc(), ClockType::get(&ctx), "undriven");
      temporary = wire; aliasConnect->setOperand(1, wire.getResult());
    } else if (bad == 3) domains.pop_back();
    else domains.front().modelClockName = "stale";
    before = dump(*root); error.clear();
    require(!goldengate::analyzeFAMEChannelClockDomains(circuit, model, domains, error) &&
                !error.empty() && dump(*root) == before, "invalid channel clock accepted or mutated");
    aliasConnect->setOperand(1, original);
    if (temporary) temporary->erase();
    if (bad == 3) domains.push_back({0, 0, "clock1", "clock1", "_1", {"d1", 1, 3, 3}});
    if (bad == 4) domains.front().modelClockName = "clock0";
  }
  error.clear();
  require(!goldengate::analyzeLocalChannelClockSource(model, 1, error), "input clock alias accepted");
  // Consume captured identities after scalar clock/alias ports are gone.
  auto transformed = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Model" {
    firrtl.module @Model(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      in %tick_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<_1: clock, _0: clock>>,
      in %in0_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
      in %in1_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
      out %out0_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
      out %out1_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>) {
      %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
    }
  } })mlir", &ctx);
  require(bool(transformed), "transformed fixture parse");
  auto transformedCircuit = *transformed->getOps<CircuitOp>().begin();
  auto m = *transformedCircuit.getOps<FModuleOp>().begin();
  b.setInsertionPointToStart(m.getBodyBlock());
  Value bits = b.create<SubfieldOp>(m.getLoc(), m.getArgument(2), "bits");
  std::map<std::string, Value> raw;
  for (const auto &d : domains) {
    Value token = b.create<SubfieldOp>(m.getLoc(), bits, d.payloadField);
    raw[d.modelClockName] = b.create<AsUIntPrimOp>(m.getLoc(), token);
  }
  for (const auto &d : domains) {
    error.clear();
    require(succeeded(goldengate::addFAMEClockEnable(m, d.modelClockName, raw.at(d.modelClockName), error)), error);
  }
  SmallVector<goldengate::FAMEFiredChannel> channels;
  for (const auto &a : *assignments) {
    bool input = a.direction == Direction::In;
    Value enable = input ? raw.at(a.modelClockName) : goldengate::lookupFAMEClockEnable(m, a.modelClockName, error);
    require(bool(enable), error);
    channels.push_back({a.localName, input, enable});
  }
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, channels, error)), error);
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, channels, error)), error);
  require(succeeded(verify(*transformed)), "fired-state IR invalid");
  goldengate::FAMEFiredRegisterIndex index;
  require(succeeded(index.collect(m, error)), error);
  auto finishing = *m.getOps<WireOp>().begin();
  for (unsigned mask = 0; mask < 256; ++mask) {
    llvm::outs() << "FIRED " << mask;
    for (StringRef name : {"in0", "in1", "out0", "out1"}) {
      unsigned domain = name.ends_with("0") ? 0 : 1;
      bool input = name.starts_with("in");
      Value fired = index.lookup(name.str(), false);
      auto reg = fired.getDefiningOp<RegResetOp>();
      require(eval(reg.getResetValue(), llvm::DenseMap<Value, unsigned>()) == unsigned(input), "fired reset differs");
      Value transition;
      for (auto c : m.getOps<StrictConnectOp>()) if (c.getDest() == fired) transition = c.getSrc();
      unsigned port = input ? 3 + domain : 5 + domain;
      llvm::DenseMap<Value, unsigned> values{{finishing.getResult(), (mask >> 4) & 1}, {fired, (mask >> 5) & 1}};
      for (auto field : m.getOps<SubfieldOp>())
        if (field.getInput() == m.getArgument(port))
          values[field.getResult()] = (mask >> (field.getFieldName() == "ready" ? 6 : 7)) & 1;
      for (unsigned d = 0; d < 2; ++d) {
        auto key = "clock" + std::to_string(d);
        values[raw.at(key)] = (mask >> d) & 1;
        values[goldengate::lookupFAMEClockEnable(m, key, error)] = (mask >> (d + 2)) & 1;
      }
      unsigned actual = eval(transition, values);
      unsigned expected = ((mask >> 4) & 1) ? !((mask >> (domain + (input ? 0 : 2))) & 1)
          : (((mask >> 5) & 1) | (((mask >> 6) & 1) & ((mask >> 7) & 1)));
      require(actual == expected, "channel used another domain or wrong token phase");
      llvm::outs() << ' ' << actual;
    }
    llvm::outs() << '\n';
  }
}
void rocket(MLIRContext &ctx, const char *boundary, const char *golden) {
  auto root = parseSourceFile<ModuleOp>(boundary, &ctx);
  require(bool(root), "Rocket boundary parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp model;
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == "FireSim") model = m;
  require(bool(model), "Rocket model missing");
  unsigned clock = 0;
  while (clock < model.getNumPorts() && model.getPortName(clock) != "clockBridge_clocks_0") ++clock;
  SmallVector<goldengate::FAMEHubClockDomain> domains{
      {clock, 0, "clockBridge_clocks_0", "clockBridge_clocks_0", "", {"reference", 1, 1, 1}}};
  std::string error;
  auto assignments = goldengate::analyzeFAMEChannelClockDomains(circuit, model, domains, error);
  require(bool(assignments), error);
  auto file = llvm::MemoryBuffer::getFile(golden);
  require(bool(file), "immutable golden SV missing");
  StringRef sv = (*file)->getBuffer();
  auto start = sv.find("module FireSim(");
  require(start != StringRef::npos, "golden target model missing");
  sv = sv.drop_front(start);
  sv = sv.take_front(sv.find("endmodule"));
  unsigned inputs = 0, outputs = 0;
  for (const auto &a : *assignments) {
    bool input = a.direction == Direction::In;
    require(a.modelClockName == "clockBridge_clocks_0", "Rocket channel mapped to another clock");
    auto fired = a.localName + "_fired_0";
    auto complement = input ? "~clockBridge_clocks_0_sink_bits" : "~clockBridge_clocks_0_enabled";
    size_t pos = sv.find(fired + " <=");
    require(pos != StringRef::npos, "golden fired state missing: " + fired);
    // Input reset is inside a combined hostReset/finishing expression; output
    // reset is a separate branch. Compare each full next-state assignment.
    bool matched = false;
    bool resetMatched = false;
    while (pos != StringRef::npos) {
      auto rest = sv.drop_front(pos);
      auto statement = rest.take_front(rest.find(';'));
      if (input) {
        auto expression = statement.split("<=").second.trim();
        require(expression.consume_front("hostReset |"), "golden input reset differs: " + fired);
        auto signal = expression.trim();
        size_t definition = sv.find(signal.str() + " =");
        require(definition != StringRef::npos, "golden input transition wire missing");
        auto expressionText = sv.drop_front(definition);
        expressionText = expressionText.take_front(expressionText.find(';'));
        matched |= expressionText.contains(complement) &&
                   expressionText.contains("targetCycleFinishing ?") &&
                   expressionText.contains(fired);
        resetMatched = true;
      } else {
        matched |= statement.contains(complement);
        resetMatched |= statement.split("<=").second.trim() == "1'h0";
      }
      pos = sv.find(fired + " <=", pos + 1);
    }
    require(matched && resetMatched, "golden domain/phase/reset differs: " + fired);
    if (input) ++inputs; else ++outputs;
  }
  require(inputs == 17 && outputs == 25, "Rocket channel count differs from SFC RTL");
  llvm::outs() << "Rocket: 17 raw-token input and 25 buffered-enable output domain assignments match immutable SFC RTL\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    if (argc == 3) rocket(ctx, argv[1], argv[2]); else fixture(ctx);
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
