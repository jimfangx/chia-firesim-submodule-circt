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
  auto prepared = goldengate::PreparedFAMEAnalysis::create(circuit, error);
  require(bool(prepared), error);
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    auto reused = goldengate::analyzeFAMEChannelClockDomains(*prepared, model, domains, error);
    require(reused && reused->size() == assignments->size() && dump(*root) == before,
            "prepared clock query mutated IR or changed assignment count: " + error);
    for (unsigned i = 0; i < reused->size(); ++i) {
      const auto &a = (*reused)[i];
      const auto &b = (*assignments)[i];
      require(a.globalName == b.globalName && a.localName == b.localName &&
                  a.direction == b.direction && a.modelClockName == b.modelClockName &&
                  a.hasClockDomain == b.hasClockDomain,
              "prepared clock query changed annotation/domain order");
    }
  }
  prepared.reset(); // The following cases deliberately mutate annotations/IR.
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
void multipleModels(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Top" {
    firrtl.module @Top(
      in %h0_c0: !firrtl.clock, in %h0_c1: !firrtl.clock, in %h0_data: !firrtl.uint<8>,
      out %h0_out: !firrtl.uint<8>, out %h0_alias0: !firrtl.clock, out %h0_alias1: !firrtl.clock,
      in %h1_c0: !firrtl.clock, in %h1_c1: !firrtl.clock, in %h1_data: !firrtl.uint<8>,
      out %h1_out: !firrtl.uint<8>, out %h1_alias0: !firrtl.clock, out %h1_alias1: !firrtl.clock,
      in %w0_clock: !firrtl.clock, in %w0_data: !firrtl.uint<8>, out %w0_out: !firrtl.uint<8>, out %w0_alias: !firrtl.clock,
      in %w1_clock: !firrtl.clock, in %w1_data: !firrtl.uint<8>, out %w1_out: !firrtl.uint<8>, out %w1_alias: !firrtl.clock) {}
    firrtl.module private @Hub(in %c0: !firrtl.clock, in %c1: !firrtl.clock, in %data: !firrtl.uint<8>,
      out %out: !firrtl.uint<8>, out %alias0: !firrtl.clock, out %alias1: !firrtl.clock) {
      firrtl.strictconnect %out, %data : !firrtl.uint<8>
      firrtl.strictconnect %alias0, %c0 : !firrtl.clock
      firrtl.strictconnect %alias1, %c1 : !firrtl.clock
    }
    firrtl.module private @Worker(in %clock: !firrtl.clock, in %data: !firrtl.uint<8>,
      out %out: !firrtl.uint<8>, out %alias: !firrtl.clock) {
      firrtl.strictconnect %out, %data : !firrtl.uint<8>
      firrtl.strictconnect %alias, %clock : !firrtl.clock
    }
  } })mlir", &ctx);
  require(bool(root), "multi-model fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>();
  auto top = *modules.begin(), hub = *std::next(modules.begin()), worker = *std::next(modules.begin(), 2);
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto connectInstance = [&](FModuleOp model, StringRef name, unsigned base) {
    auto instance = b.create<InstanceOp>(top.getLoc(), model, name);
    for (unsigned i = 0; i < model.getNumPorts(); ++i) {
      bool input = model.getPortDirection(i) == Direction::In;
      b.create<StrictConnectOp>(top.getLoc(), input ? instance.getResult(i) : top.getArgument(base + i),
                               input ? top.getArgument(base + i) : instance.getResult(i));
    }
  };
  connectInstance(hub, "h0", 0); connectInstance(hub, "h1", 6);
  connectInstance(worker, "w0", 12); connectInstance(worker, "w1", 16);
  auto target = [&](StringRef module, StringRef port) {
    return b.getStringAttr("~Top|" + module.str() + ">" + port.str());
  };
  SmallVector<Attribute> annotations;
  auto group = [&](StringRef module, StringRef name, StringRef port, StringRef clock) {
    NamedAttrList attrs;
    attrs.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts));
    attrs.set("localName", b.getStringAttr(name));
    attrs.set("ports", b.getArrayAttr({target(module, port)}));
    if (!clock.empty()) attrs.set("clockPort", target(module, clock));
    annotations.push_back(attrs.getDictionary(&ctx));
  };
  group("Hub", "input", "data", "alias0"); group("Hub", "output", "out", "alias1");
  group("Worker", "input", "data", ""); group("Worker", "output", "out", "");
  auto channel = [&](StringRef name, StringRef port, StringRef clock, bool input) {
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr(name)),
      b.getNamedAttr("clock", target("Top", clock)),
      b.getNamedAttr(input ? "sinks" : "sources", b.getArrayAttr({target("Top", port)})),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
        b.getNamedAttr("latency", b.getI64IntegerAttr(0))}))}));
  };
  for (StringRef instance : {"h0", "h1"}) {
    channel(instance.str() + "_input", instance.str() + "_data", instance.str() + "_alias0", true);
    channel(instance.str() + "_output", instance.str() + "_out", instance.str() + "_alias1", false);
  }
  for (StringRef instance : {"w0", "w1"}) {
    channel(instance.str() + "_input", instance.str() + "_data", "h0_alias0", true);
    channel(instance.str() + "_output", instance.str() + "_out", "h1_alias1", false);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  SmallVector<goldengate::FAMEHubClockDomain> domains{
    {0, 0, "c0", "h0_c0", "_0", {"d0", 1, 2, 2}},
    {1, 1, "c1", "h0_c1", "_1", {"d1", 2, 3, 3}}};
  std::string error;
  auto before = dump(*root);
  auto hubAssignments = goldengate::analyzeFAMEChannelClockDomains(circuit, hub, domains, error);
  require(hubAssignments && hubAssignments->size() == 2 && dump(*root) == before, error);
  require((*hubAssignments)[0].modelClockName == "c0" && (*hubAssignments)[1].modelClockName == "c1",
          "shared-definition hub assigned another instance/domain");
  auto virtualAssignments = goldengate::analyzeFAMEChannelClockDomains(circuit, worker, {}, error);
  require(virtualAssignments && virtualAssignments->size() == 2 && dump(*root) == before, error);
  for (const auto &assignment : *virtualAssignments)
    require(!assignment.hasClockDomain && assignment.modelClockName.empty(),
            "remote hub clock was projected into a virtual-clock model");
  auto checkVirtualFSMs = [&](ArrayRef<goldengate::FAMEChannelClockDomain> assignments,
                              StringRef mode) {
    auto rewritten = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Worker" {
      firrtl.module @Worker(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {
        %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      }
    } })mlir", &ctx);
    require(bool(rewritten), "virtual transformed fixture parse");
    auto c = *rewritten->getOps<CircuitOp>().begin();
    auto m = *c.getOps<FModuleOp>().begin();
    OpBuilder builder(m.getBodyBlock(), m.getBodyBlock()->end());
    Value one = builder.create<ConstantOp>(m.getLoc(), UIntType::get(&ctx, 1), APInt(1, 1));
    SmallVector<goldengate::FAMEFiredChannel> channels;
    for (const auto &assignment : assignments)
      channels.push_back({assignment.localName, assignment.direction == Direction::In,
                          one, assignment.hasClockDomain});
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, channels, error)), error);
    require(succeeded(goldengate::rewriteFAMEFiredStates(m, channels, error)), error);
    goldengate::FAMEFiredRegisterIndex index;
    require(succeeded(index.collect(m, error)), error);
    for (const auto &assignment : assignments) {
      auto reg = index.lookup(assignment.localName).getDefiningOp<RegResetOp>();
      auto reset = reg.getResetValue().getDefiningOp<ConstantOp>().getValue().getZExtValue();
      require(reset == unsigned(assignment.direction == Direction::In && assignment.hasClockDomain),
              "virtual model reset disagrees with associated clock metadata");
      llvm::outs() << "VIRTUAL " << mode << ' ' << assignment.localName << ' '
                   << (assignment.hasClockDomain ? assignment.modelClockName : "none") << ' ' << reset << '\n';
    }
    require(succeeded(verify(*rewritten)), "virtual fired FSM IR invalid");
  };
  checkVirtualFSMs(*virtualAssignments, "unassociated");
  // Different instances of the same hub definition cannot exchange local
  // clocks. Each branch must own the clock named by its local annotation.
  auto good = annotations;
  for (StringRef badClock : {"h0_alias0", "h1_alias1"}) {
    Annotation bad(annotations[6]); // h1 input
    bad.setMember("clock", target("Top", badClock));
    annotations[6] = bad.getAttr();
    circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    before = dump(*root); error.clear();
    require(!goldengate::analyzeFAMEChannelClockDomains(circuit, hub, domains, error) &&
            !error.empty() && dump(*root) == before, "heterogeneous shared hub clocks accepted");
    annotations = good;
  }
  // A second input channel on one instance is not a distinct per-instance
  // binding; accepting it would schedule two producers on one local sink.
  Annotation duplicate(good[4]);
  duplicate.setMember("globalName", b.getStringAttr("duplicate_input"));
  annotations.push_back(duplicate.getAttr());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  before = dump(*root); error.clear();
  require(!goldengate::analyzeFAMEChannelClockDomains(circuit, hub, domains, error) &&
          !error.empty() && dump(*root) == before, "duplicate same-instance input accepted");
  annotations = good;
  // Explicit associated clocks on a virtual model still have a domain and
  // therefore use the input fired reset of one, unlike its unassociated ports.
  for (unsigned i : {2u, 3u}) {
    Annotation associated(annotations[i]);
    associated.setMember("clockPort", target("Worker", "alias"));
    annotations[i] = associated.getAttr();
  }
  for (unsigned i = 8; i < 12; ++i) {
    Annotation associated(annotations[i]);
    associated.setMember("clock", target("Top", i < 10 ? "w0_alias" : "w1_alias"));
    annotations[i] = associated.getAttr();
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  before = dump(*root); error.clear();
  virtualAssignments = goldengate::analyzeFAMEChannelClockDomains(circuit, worker, {}, error);
  require(virtualAssignments && virtualAssignments->size() == 2 && dump(*root) == before, error);
  for (const auto &assignment : *virtualAssignments)
    require(assignment.hasClockDomain && assignment.modelClockName == "clock",
            "virtual model lost its explicit local clock association");
  checkVirtualFSMs(*virtualAssignments, "associated");
  error.clear(); before = dump(*root);
  require(!goldengate::analyzeFAMEChannelClockDomains(circuit, hub, {}, error) &&
          !error.empty() && dump(*root) == before, "multiclock non-hub model accepted");
  require(succeeded(verify(*root)), "multi-model fixture IR invalid");
  llvm::outs() << "PASS two shared hub instances, two rational domains, two virtual model instances, local clock ownership\n";
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
    if (argc == 3) rocket(ctx, argv[1], argv[2]); else { fixture(ctx); multipleModels(ctx); }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
