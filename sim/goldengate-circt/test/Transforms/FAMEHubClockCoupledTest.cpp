// See LICENSE for license details.
// Couple the emitted rational producer to the emitted FAME hub controls.
// Evaluate FIRRTL connects, simultaneous host state and actual gate enables;
// compare the trace with FAMEHubClockCoupledOracle.scala.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEOutputChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEClockGate.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEFiredRegister.h"
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEOutputValid.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/SingleClockBridge.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
Value field(OpBuilder &b, Location loc, Value port, StringRef name) {
  return b.create<SubfieldOp>(loc, port, name);
}
// Canonical field identities bridge model arguments to their actual instance
// results, so ready feeds the producer through the constructed FIRRTL graph.
struct Interpreter {
  SmallVector<FModuleOp> scopes;
  SmallVector<FModuleOp> instanceCopies;
  llvm::DenseMap<Value, Value> aliases;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  std::set<std::string> visiting;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>())
      return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto i = v.getDefiningOp<SubindexOp>())
      return key(i.getInput()) + "[" + std::to_string(i.getIndex()) + "]";
    if (aliases.count(v)) return key(aliases.lookup(v));
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(CircuitOp circuit, FModuleOp top) {
    // Elaborate repeated definitions into private interpreter copies so each
    // queue instance owns independent registers. The exported compiler IR
    // retains the shared definition and the actual instance connections.
    std::function<void(FModuleOp)> visit = [&](FModuleOp scope) {
      require(!llvm::is_contained(scopes, scope), "multiply instantiated fixture module");
      scopes.push_back(scope);
      for (auto instance : scope.getOps<InstanceOp>()) {
        FModuleOp child;
        for (auto module : circuit.getOps<FModuleOp>())
          if (module.getName() == instance.getModuleName()) child = module;
        if (!child) continue; // AbstractClockGate extmodule: observe its CE.
        if (llvm::is_contained(scopes, child)) {
          child = cast<FModuleOp>(child->clone());
          instanceCopies.push_back(child);
        }
        for (unsigned i = 0; i < child.getNumPorts(); ++i)
          aliases[child.getArgument(i)] = instance.getResult(i);
        visit(child);
      }
    };
    visit(top);
    // Channelization emits aggregate connects. Project their actual fields,
    // including ready's reversed flow, without rebuilding the wiring by hand.
    OpBuilder builder(top.getContext());
    std::function<void(Value, Value, Location)> connect =
        [&](Value dest, Value src, Location loc) {
      if (auto bundle = dyn_cast<BundleType>(dest.getType())) {
        for (const auto &element : bundle.getElements()) {
          auto d = field(builder, loc, dest, element.name.getValue());
          auto s = field(builder, loc, src, element.name.getValue());
          connect(element.isFlip ? s : d, element.isFlip ? d : s, loc);
        }
      } else if (auto vector = dyn_cast<FVectorType>(dest.getType())) {
        for (unsigned i = 0; i < vector.getNumElements(); ++i)
          connect(builder.create<SubindexOp>(loc, dest, i),
                  builder.create<SubindexOp>(loc, src, i), loc);
      } else {
        require(drivers.emplace(key(dest), src).second, "multiple drivers");
      }
    };
    for (auto scope : scopes)
      for (auto &operation : llvm::make_early_inc_range(scope.getBodyBlock()->getOperations())) {
        builder.setInsertionPoint(&operation);
        if (auto c = dyn_cast<StrictConnectOp>(operation))
          connect(c.getDest(), c.getSrc(), c.getLoc());
        else if (auto c = dyn_cast<ConnectOp>(operation))
          connect(c.getDest(), c.getSrc(), c.getLoc());
      }
  }
  ~Interpreter() {
    for (auto copy : instanceCopies) copy->destroy();
  }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    require(visiting.insert(k).second, "combinational loop");
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AsUIntPrimOp, AsClockPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (auto p = dyn_cast_or_null<MuxPrimOp>(op)) n = eval(eval(p.getSel()) ? p.getHigh() : p.getLow());
    else if (auto p = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(p.getInput()) >> p.getLo();
    else throw std::runtime_error("unsupported operation or missing driver: " + k);
    if (auto type = dyn_cast<UIntType>(v.getType()); type && type.getWidth() && *type.getWidth() < 64)
      n &= (uint64_t(1) << *type.getWidth()) - 1;
    visiting.erase(k); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto scope : scopes) {
      for (auto r : scope.getOps<RegResetOp>())
        next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
      for (auto r : scope.getOps<RegOp>()) {
        // Target registers are clocked by the actual AbstractClockGate.O.
        auto clock = dyn_cast<OpResult>(r.getClockVal());
        auto gate = clock ? dyn_cast<InstanceOp>(clock.getOwner()) : InstanceOp();
        if (gate) {
          require(gate.getModuleName() == "AbstractClockGate" && clock.getResultNumber() == 2,
                  "target state lacks gated clock");
          next[r.getResult()] = eval(gate.getResult(1)) ? eval(drivers.at(key(r.getResult()))) : state.lookup(r.getResult());
        } else {
          // ClockBridge counter snapshot registers advance on hostClock.
          next[r.getResult()] = eval(drivers.at(key(r.getResult())));
        }
      }
    }
    state = std::move(next);
  }
};

void clockRecordRejections(MLIRContext &ctx) {
  for (StringRef payload : {"uint<1>", "bundle<>",
                           "bundle<_0: clock, data: uint<1>>",
                           "bundle<_0 flip: clock>",
                           "bundle<nested: bundle<_0: clock>>",
                           "vector<clock, 2>",
                           "bundle<_0: clock, _1: clock>"}) {
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Model\" { firrtl.module @Model("
        "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
        "in %in0_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>, "
        "in %tick_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: " +
        payload.str() + ">) { %done = firrtl.wire : !firrtl.uint<1> } } }", &ctx);
    require(bool(root), "clock rejection fixture parse");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = *circuit.getOps<FModuleOp>().begin();
    for (auto w : model.getOps<WireOp>()) w.setName("targetCycleFinishing");
    auto before = dump(root->getOperation());
    std::string error;
    // The final case is a valid ClockRecord wrongly listed as a data input.
    bool isRecord = payload == "bundle<_0: clock, _1: clock>";
    SmallVector<std::string> inputs = isRecord ?
        SmallVector<std::string>{"tick", "in0"} : SmallVector<std::string>{"in0"};
    require(failed(goldengate::rewriteFAMEFinishing(model, inputs, {},
                    isRecord ? "" : "tick", error)) &&
                error.find(isRecord ? "explicit target clock" : "Clock/ClockRecord") != std::string::npos &&
                before == dump(root->getOperation()),
            "clock payload was not rejected before mutation: " + payload.str());
  }
  llvm::errs() << "Passed seven atomic ClockRecord completion rejections\n";
}

void outputValidRejections(MLIRContext &ctx) {
  // HasModelPort constructs passive valid fields; a flipped field is driven
  // by the peer. Reject it before even a preceding valid channel is rewritten.
  for (bool flippedInput : {false, true}) {
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Model\" { firrtl.module @Model("
        "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
        "in %in0_sink: !firrtl.bundle<ready flip: uint<1>, valid" +
        std::string(flippedInput ? " flip" : "") + ": uint<1>, bits: uint<16>>, "
        "out %out0_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>, "
        "out %out1_source: !firrtl.bundle<ready flip: uint<1>, valid" +
        std::string(flippedInput ? "" : " flip") + ": uint<1>, bits: uint<16>>) { "
        "%done = firrtl.wire : !firrtl.uint<1> } } }", &ctx);
    require(bool(root), "output valid rejection fixture parse");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = *circuit.getOps<FModuleOp>().begin();
    for (auto w : model.getOps<WireOp>()) w.setName("targetCycleFinishing");
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(model,
            {{"out0", false, {}}, {"out1", false, {}}}, error)), error);
    auto before = dump(root->getOperation());
    require(failed(goldengate::rewriteFAMEOutputValids(model,
            {{"out0", {}, {}, {}}, {"out1", {"in0"}, {}, {}}}, error)) &&
            error.find("valid port") != std::string::npos &&
            before == dump(root->getOperation()),
            "flipped valid field accepted or partially rewritten");
  }
  llvm::errs() << "Passed two atomic flipped-valid rejections\n";
}

void run(MLIRContext &ctx, bool reversed, bool queued, const char *output) {
  std::string common = R"mlir(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
    in %in0: !firrtl.uint<16>, in %in1: !firrtl.uint<16>,
    out %out0: !firrtl.uint<16>, out %out1: !firrtl.uint<16>,
    in %bridge_clocks_0: !firrtl.clock, in %bridge_clocks_1: !firrtl.clock,
    out %alias0: !firrtl.clock, out %alias1: !firrtl.clock)mlir";
  auto root = parseSourceString<ModuleOp>(
    "module { firrtl.circuit \"Top\" { firrtl.module @Top(" + common + ") {} "
    "firrtl.module private @Model(" + common + ") {\n" +
    R"mlir(%targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      firrtl.strictconnect %alias0, %bridge_clocks_0 : !firrtl.clock
      firrtl.strictconnect %alias1, %bridge_clocks_1 : !firrtl.clock
      %state0 = firrtl.reg %bridge_clocks_0 : !firrtl.clock, !firrtl.uint<16>
      %state1 = firrtl.reg %bridge_clocks_1 : !firrtl.clock, !firrtl.uint<16>
      %sum0 = firrtl.add %state0, %in0 : (!firrtl.uint<16>, !firrtl.uint<16>) -> !firrtl.uint<17>
      %sum1 = firrtl.add %state1, %in1 : (!firrtl.uint<16>, !firrtl.uint<16>) -> !firrtl.uint<17>
      %next0 = firrtl.bits %sum0 15 to 0 : (!firrtl.uint<17>) -> !firrtl.uint<16>
      %next1 = firrtl.bits %sum1 15 to 0 : (!firrtl.uint<17>) -> !firrtl.uint<16>
      firrtl.strictconnect %state0, %next0 : !firrtl.uint<16>
      firrtl.strictconnect %state1, %next1 : !firrtl.uint<16>
      firrtl.strictconnect %out0, %next0 : !firrtl.uint<16>
      firrtl.strictconnect %out1, %next1 : !firrtl.uint<16>
    } } })mlir", &ctx);
  require(bool(root), "coupled fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto model = *std::next(circuit.getOps<FModuleOp>().begin());
  auto loc = model.getLoc(); OpBuilder b(&ctx); std::string error;
  b.setInsertionPointToEnd(top.getBodyBlock());
  auto instance = b.create<InstanceOp>(loc, model, "model");
  for (unsigned i = 0; i < top.getNumPorts(); ++i) {
    bool input = model.getPortDirection(i) == Direction::In;
    b.create<StrictConnectOp>(loc, input ? instance.getResult(i) : top.getArgument(i),
                             input ? top.getArgument(i) : instance.getResult(i));
  }
  // Use the real pre-FAME annotation boundary to discover dependencies and
  // build both model and wrapper interfaces. Port indices and instances are
  // invalidated by each rewrite; re-analyze only the unconsumed annotations.
  SmallVector<Attribute> annotations, clockTopTargets, clockModelTargets, clockInfo;
  for (unsigned lane = 0; lane < 2; ++lane) {
    auto physical = reversed ? 1 - lane : lane;
    auto name = "bridge_clocks_" + std::to_string(physical);
    clockTopTargets.push_back(b.getStringAttr("~Top|Top>" + name));
    clockModelTargets.push_back(b.getStringAttr("~Top|Model>" + name));
    clockInfo.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr("domain" + std::to_string(lane))),
        b.getNamedAttr("multiplier", b.getI64IntegerAttr(1)),
        b.getNamedAttr("divisor", b.getI64IntegerAttr(lane + 2))}));
  }
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts)),
      b.getNamedAttr("localName", b.getStringAttr("bridge_clocks")),
      b.getNamedAttr("ports", b.getArrayAttr(clockModelTargets))}));
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr("bridge_clocks")),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::TargetClockChannel)),
          b.getNamedAttr("clockInfo", b.getArrayAttr(clockInfo)),
          b.getNamedAttr("perClockMFMR", b.getArrayAttr({b.getI64IntegerAttr(1), b.getI64IntegerAttr(2)}))})),
      b.getNamedAttr("sinks", b.getArrayAttr(clockTopTargets))}));
  for (StringRef name : {"in0", "in1", "out0", "out1"}) {
    bool input = name.starts_with("in");
    auto target = [&](StringRef module) {
      return b.getStringAttr(("~Top|" + module + ">" + name).str());
    };
    // The SFC handoff associates channels with output Clock aliases. Both
    // directions must recover their scalar source through the target graph.
    auto clock = "alias" + name.take_back(1).str();
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts)),
        b.getNamedAttr("localName", b.getStringAttr(name)),
        b.getNamedAttr("clockPort", b.getStringAttr("~Top|Model>" + clock)),
        b.getNamedAttr("ports", b.getArrayAttr({target("Model")}))}));
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("globalName", b.getStringAttr(name)),
        b.getNamedAttr("clock", b.getStringAttr("~Top|Top>" + clock)),
        b.getNamedAttr("channelInfo", b.getDictionaryAttr({
            b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
            b.getNamedAttr("latency", b.getI64IntegerAttr(0))})),
        b.getNamedAttr("sources", b.getArrayAttr(input ? ArrayRef<Attribute>{} : ArrayRef<Attribute>{target("Top")})),
        b.getNamedAttr("sinks", b.getArrayAttr(input ? ArrayRef<Attribute>{target("Top")} : ArrayRef<Attribute>{}))}));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto selection = goldengate::analyzeFAMEDataSelection(circuit, model, error);
  require(selection && selection->inputs.size() == 2 && selection->outputs.size() == 2,
          "coupled data selection: " + error);
  struct SelectedChannel { std::string globalName, localName; bool input; };
  SmallVector<SelectedChannel> selected;
  SmallVector<std::string> inputNames, outputNames;
  SmallVector<goldengate::LocalChannelDependency> dependencies;
  for (unsigned i = 0; i < selection->inputs.size(); ++i) {
    const auto &entry = selection->inputs[i];
    require(entry.localName == "in" + std::to_string(i) && entry.fieldCount == 1 &&
            entry.kind == goldengate::ChannelKind::Pipe, "pre-FAME input selection differs");
    selected.push_back({entry.globalName, entry.localName, true});
    inputNames.push_back(entry.localName);
  }
  for (unsigned i = 0; i < selection->outputs.size(); ++i) {
    const auto &entry = selection->outputs[i];
    require(entry.localName == "out" + std::to_string(i) && entry.fieldCount == 1 &&
            entry.dependency.inputChannels == std::vector<std::string>{"in" + std::to_string(i)},
            "pre-FAME combinational channel dependencies differ");
    dependencies.push_back(entry.dependency);
    selected.push_back({entry.globalName, entry.localName, false});
    outputNames.push_back(entry.localName);
  }
  auto hierarchy = goldengate::analyzeTopHierarchy(circuit, error);
  require(bool(hierarchy), error);
  auto clockGroup = goldengate::analyzeModelPortGroup(circuit, Annotation(annotations[0]), error);
  auto clockConnection = goldengate::analyzeChannelConnection(circuit, Annotation(annotations[1]), error);
  require(clockGroup && clockConnection, error);
  SmallVector<goldengate::ModelPortGroup> clockGroups{*clockGroup};
  auto clockBinding = goldengate::bindChannelToModels(*clockConnection, *hierarchy, clockGroups, error);
  require(clockBinding && clockBinding->size() == 1, error);
  auto domains = goldengate::analyzeFAMEHubClockDomains(*clockConnection, *hierarchy, clockBinding->front(), error);
  require(domains && domains->size() == 2, error);
  auto channelClocks = goldengate::analyzeFAMEChannelClockDomains(circuit, model, *domains, error);
  require(channelClocks && channelClocks->size() == 4, error);
  for (const auto &channel : *channelClocks)
    require(channel.modelClockName == "bridge_clocks_" + channel.localName.substr(channel.localName.size() - 1),
            "associated channel alias resolved to the wrong clock domain");
  auto clockPlan = goldengate::analyzeFAMEPorts(*hierarchy, *clockBinding, {*clockConnection}, {model}, error);
  require(clockPlan && clockPlan->sinks.size() == 1, error);
  require(succeeded(goldengate::rewriteFAMEHubClockChannel(*hierarchy, clockPlan->sinks.front(), *domains, false, error)), error);
  // Keep all data clock references transferred by the clock rewrite, consume
  // only the clock's own annotation pair before reanalyzing the data boundary.
  SmallVector<Attribute> dataAnnotations;
  for (auto attribute : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    Annotation annotation(attribute);
    auto global = annotation.getMember<StringAttr>("globalName");
    auto local = annotation.getMember<StringAttr>("localName");
    if ((!global || global.getValue() != "bridge_clocks") &&
        (!local || local.getValue() != "bridge_clocks"))
      dataAnnotations.push_back(attribute);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(dataAnnotations));
  for (const auto &next : selected) {
    auto hierarchy = goldengate::analyzeTopHierarchy(circuit, error);
    require(bool(hierarchy), error);
    SmallVector<goldengate::ModelPortGroup> groups;
    SmallVector<goldengate::GGChannelConnection, 0> connections;
    for (auto attribute : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      Annotation annotation(attribute);
      if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts)) {
        auto group = goldengate::analyzeModelPortGroup(circuit, annotation, error);
        require(bool(group), error);
        groups.push_back(std::move(*group));
      } else if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
        auto connection = goldengate::analyzeChannelConnection(circuit, annotation, error);
        require(bool(connection), error);
        connections.push_back(std::move(*connection));
      }
    }
    SmallVector<goldengate::ModelChannelBinding> bindings;
    for (const auto &connection : connections) {
      auto bound = goldengate::bindChannelToModels(connection, *hierarchy, groups, error);
      require(bool(bound), error);
      bindings.append(bound->begin(), bound->end());
    }
    auto plan = goldengate::analyzeFAMEPorts(*hierarchy, bindings, connections, {model}, error);
    require(bool(plan), error);
    const auto &planned = next.input ? plan->sinks : plan->sources;
    auto channel = llvm::find_if(planned, [&](const auto &entry) {
      return entry.binding->globalName == next.globalName;
    });
    require(channel != planned.end(), "selected data channel has no native port plan");
    require(succeeded(next.input ?
        goldengate::rewriteFAMEInputChannel(*hierarchy, *channel, error) :
        goldengate::rewriteFAMEOutputChannel(*hierarchy, *channel, error)), error);
    SmallVector<Attribute> remaining;
    for (auto attribute : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      Annotation annotation(attribute);
      auto global = annotation.getMember<StringAttr>("globalName");
      auto local = annotation.getMember<StringAttr>("localName");
      if ((!global || global.getValue() != next.globalName) &&
          (!local || local.getValue() != next.localName))
        remaining.push_back(attribute);
    }
    circuit->setAttr("rawAnnotations", b.getArrayAttr(remaining));
  }
  require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").empty(),
          "data channel annotations were not consumed");
  instance = *top.getOps<InstanceOp>().begin();
  for (unsigned i = 0; i < 4; ++i) {
    auto name = std::string(i < 2 ? "in" : "out") + std::to_string(i % 2) +
                (i < 2 ? "_sink" : "_source");
    require(model.getPortName(2 + i) == name &&
            top.getPortName(2 + i) == "model_" + name &&
            model.getPorts()[2 + i].type == top.getPorts()[2 + i].type &&
            isa<BundleType>(model.getPorts()[2 + i].type),
            "native channelization produced an unexpected port identity/type");
  }
  // Shape/order failures must leave both clock domains unmodified. A partial
  // first-domain enable/gate would conceal a bad second lane in larger hubs.
  for (unsigned bad = 0; bad < 5; ++bad) {
    auto invalid = *domains;
    if (bad == 0) std::reverse(invalid.begin(), invalid.end());
    if (bad == 1) invalid[1].payloadField = "missing";
    if (bad == 2) invalid[1].modelClockName = invalid[0].modelClockName;
    if (bad == 3) invalid[1].modelClockName.clear();
    if (bad == 4) invalid.pop_back();
    auto before = dump(root->getOperation());
    error.clear();
    require(!goldengate::constructFAMEHubClockControls(circuit, model, "bridge_clocks", invalid, error) &&
                !error.empty() && before == dump(root->getOperation()),
            "bad ordered clock controls mutated the model");
  }
  error.clear();
  auto controls = goldengate::constructFAMEHubClockControls(circuit, model, "bridge_clocks", *domains, error);
  require(controls && controls->size() == domains->size(), error);
  SmallVector<Value> enabled(2), targetState;
  for (const auto &control : *controls) {
    auto physical = control.modelClockName == "bridge_clocks_0" ? 0u : 1u;
    enabled[physical] = control.outputEnable;
  }
  SmallVector<goldengate::FAMEFiredChannel> channels;
  auto channelEnable = [&](StringRef name, Direction direction) {
    auto channel = llvm::find_if(*channelClocks, [&](const auto &entry) {
      return entry.localName == name && entry.direction == direction;
    });
    require(channel != channelClocks->end(), "missing snapshotted channel clock");
    auto control = llvm::find_if(*controls, [&](const auto &entry) {
      return entry.modelClockName == channel->modelClockName;
    });
    require(control != controls->end(), "missing generated channel clock controls");
    return direction == Direction::In ? control->inputEnable : control->outputEnable;
  };
  for (const auto &name : inputNames)
    channels.push_back({name, true, channelEnable(name, Direction::In)});
  for (const auto &name : outputNames)
    channels.push_back({name, false, channelEnable(name, Direction::Out)});
  require(succeeded(goldengate::internalizeFAMEOutputClocks(top, model, "model", error)), error);
  instance = *top.getOps<InstanceOp>().begin();
  goldengate::FAMEClockGateIndex aliasGates;
  require(succeeded(aliasGates.collect(model, error)), error);
  for (unsigned physical = 0; physical < 2; ++physical) {
    auto aliasName = "alias" + std::to_string(physical);
    auto clockName = "bridge_clocks_" + std::to_string(physical);
    Value alias;
    for (auto wire : model.getOps<WireOp>())
      if (wire.getName() == aliasName) alias = wire.getResult();
    require(bool(alias), "output clock alias was not internalized");
    unsigned matches = 0;
    for (auto connection : model.getOps<StrictConnectOp>())
      if (connection.getDest() == alias) {
        require(connection.getSrc() == aliasGates.lookup(clockName, false).getResult(2),
                "output clock alias does not read its gated domain");
        ++matches;
      }
    require(matches == 1, "internalized clock alias lost its unique driver");
    for (auto module : {top, model})
      for (unsigned port = 0; port < module.getNumPorts(); ++port)
        require(module.getPortName(port) != aliasName,
                "output clock alias remains in transformed interface");
  }
  require(succeeded(goldengate::ensureFAMEFiredRegisters(model, channels, error)), error);
  require(succeeded(goldengate::rewriteFAMEFiredStates(model, channels, error)), error);
  require(succeeded(goldengate::rewriteFAMEOutputValids(model,
              dependencies, error)), error);
  require(succeeded(goldengate::rewriteFAMEInputReadies(model, inputNames, error)), error);
  require(succeeded(goldengate::rewriteFAMEFinishing(model,
              inputNames, outputNames, "bridge_clocks", error)), error);
  b.setInsertionPointToEnd(model.getBodyBlock());
  for (auto r : model.getOps<RegOp>()) targetState.push_back(r.getResult());
  require(targetState.size() == 2, "target state missing");
  std::optional<unsigned> targetClockPort;
  for (unsigned i = 0; i < top.getNumPorts(); ++i)
    if (top.getPortName(i) == "model_bridge_clocks_sink") targetClockPort = i;
  require(bool(targetClockPort), "channelized target clock port missing");
  // Recreate the post-FAME boundary annotation with the retained ordered
  // ClockRecord leaves. SimulationMapping then converts that actual interface
  // to Vec[Bool] and attaches the actual ClockBridge producer instance.
  SmallVector<Attribute> clockSinks;
  for (const auto &domain : *domains)
    clockSinks.push_back(b.getStringAttr("~Top|Top>model_bridge_clocks_sink.bits." +
                                        domain.payloadField));
  NamedAttrList boundaryClock(cast<DictionaryAttr>(annotations[1]));
  boundaryClock.set("sinks", b.getArrayAttr(clockSinks));
  auto bridge = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top>clockBridge")),
      b.getNamedAttr("widgetClass", b.getStringAttr("midas.widgets.ClockBridgeModule")),
      b.getNamedAttr("widgetConstructorKey", b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridges.ClockParameters")),
          b.getNamedAttr("clocks", b.getArrayAttr(clockInfo))})),
      b.getNamedAttr("channelMapping", b.getDictionaryAttr({
          b.getNamedAttr("clocks", b.getStringAttr("bridge_clocks"))}))});
  SmallVector<Attribute> boundaryAnnotations{boundaryClock.getDictionary(&ctx), bridge};
  if (queued) {
    for (const auto &channel : channels) {
      auto endpoint = "~Top|Top>model_" + channel.name +
          (channel.isInput ? "_sink.bits" : "_source.bits");
      boundaryAnnotations.push_back(b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
          b.getNamedAttr("globalName", b.getStringAttr(channel.name)),
          b.getNamedAttr("channelInfo", b.getDictionaryAttr({
              b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
              b.getNamedAttr("latency", b.getI64IntegerAttr(0))})),
          b.getNamedAttr(channel.isInput ? "sinks" : "sources", b.getArrayAttr({b.getStringAttr(endpoint)}))}));
    }
    circuit->setAttr("rawAnnotations", b.getArrayAttr(boundaryAnnotations));
    require(succeeded(goldengate::addFAMEBoundaryPipeChannels(circuit, error)), error);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(boundaryAnnotations));
  require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(goldengate::addFAMEClockChannel(circuit, error)), error);
  require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(goldengate::addClockBridge(circuit, error)), error);
  FModuleOp producer;
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module.getName() == circuit.getName()) top = module;
    if (module.getName() == "GGSingleClockBridge") producer = module;
  }
  require(top.getName() == "GGClockBridgeWrapper" && bool(producer),
          "production clock bridge hierarchy missing");
  b.setInsertionPointToEnd(producer.getBodyBlock());
  Value tokenBits = field(b, loc, producer.getArgument(2), "bits");
  SmallVector<Value> tokens;
  for (unsigned lane = 0; lane < domains->size(); ++lane)
    tokens.push_back(b.create<SubindexOp>(loc, tokenBits, lane));
  require(succeeded(verify(*root)), "coupled FIRRTL verification");
  if (output) {
    // Retain the mapped identities separately. The active compiler consumes
    // raw annotations before the CIRCT backend, so lower an equivalent clone
    // with that compiler-owned transport attribute removed.
    auto write = [&](StringRef path, ModuleOp artifact) {
      std::error_code ec; llvm::raw_fd_ostream out(path, ec);
      require(!ec, ec.message()); artifact->print(out); out << '\n';
    };
    write(std::string(output) + ".mapped.mlir", *root);
    OwningOpRef<ModuleOp> lowered(cast<ModuleOp>(root->clone()));
    for (auto cloneCircuit : lowered->getOps<CircuitOp>())
      cloneCircuit->removeAttr("rawAnnotations");
    require(succeeded(verify(*lowered)), "lowering clone FIRRTL verification");
    write(output, *lowered);
  }

  Interpreter sim(circuit, top);
  goldengate::FAMEClockGateIndex gates;
  goldengate::FAMEFiredRegisterIndex fired;
  require(succeeded(gates.collect(model, error)) && succeeded(fired.collect(model, error)), error);
  Value finishing;
  for (auto w : model.getOps<WireOp>())
    if (w.getName() == "targetCycleFinishing") finishing = w.getResult();
  bool pending[2] = {false, false};
  uint64_t inputBits[2] = {0, 0}, expectedState[2] = {0, 0};
  bool blocked[2] = {false, false};
  uint64_t blockedBits[2] = {0, 0};
  uint64_t edges[2] = {0, 0}, outputCount[2] = {0, 0}, inputCount[2] = {0, 0};
  unsigned stalls = 0, unequal = 0, completed = 0;
  for (unsigned cycle = 0; cycle < 8192; ++cycle) {
    bool reset = cycle < 3 || cycle == 4096 || cycle == 4097;
    if (reset) {
      pending[0] = pending[1] = false;
      inputBits[0] = inputBits[1] = 0;
    }
    else {
      if (!pending[0] && (cycle % 11 == 3 || cycle % 43 > 37)) {
        pending[0] = true; inputBits[0] = (cycle * 73 + 19) & 65535;
      }
      if (!pending[1] && (cycle % 17 == 5 || cycle % 61 > 53)) {
        pending[1] = true; inputBits[1] = (cycle * 151 + 41) & 65535;
      }
    }
    sim.memo.clear();
    sim.memo[sim.key(top.getArgument(0))] = 0;
    sim.memo[sim.key(top.getArgument(1))] = reset;
    auto mcrKey = sim.key(top.getArgument(top.getNumPorts() - 1));
    sim.memo[mcrKey + ".wstrb"] = 0;
    for (unsigned slot = 0; slot < 6; ++slot) {
      sim.memo[mcrKey + ".read[" + std::to_string(slot) + "].ready"] = 0;
      sim.memo[mcrKey + ".write[" + std::to_string(slot) + "].valid"] = 0;
      sim.memo[mcrKey + ".write[" + std::to_string(slot) + "].bits"] = 0;
    }
    for (unsigned i = 0; i < 2; ++i) {
      sim.memo[sim.key(top.getArgument(2 + i)) + ".valid"] = pending[i];
      sim.memo[sim.key(top.getArgument(2 + i)) + ".bits"] = inputBits[i];
      sim.memo[sim.key(top.getArgument(4 + i)) + ".ready"] = i == 0 ?
          cycle % 97 >= 29 && cycle % 7 != 0 : cycle % 83 >= 37 && cycle % 11 != 0;
    }
    auto evalPortField = [&](FModuleOp scope, unsigned port, StringRef member) {
      auto k = sim.key(scope.getArgument(port)) + "." + member.str();
      return sim.memo.count(k) ? sim.memo.at(k) : sim.eval(sim.drivers.at(k));
    };
    auto evalField = [&](unsigned port, StringRef member) {
      return evalPortField(top, port, member);
    };
    auto hubField = [&](unsigned port, StringRef member) {
      return evalPortField(model, port, member);
    };
    unsigned mask = sim.eval(tokens[0]) | sim.eval(tokens[1]) << 1;
    unsigned done = sim.eval(finishing), enableMask = 0, firedMask = 0, inReady = 0, outValid = 0, ceMask = 0;
    for (unsigned i = 0; i < 2; ++i) {
      enableMask |= sim.eval(enabled[i]) << i;
      inReady |= hubField(2 + i, "ready") << i;
      outValid |= hubField(4 + i, "valid") << i;
      ceMask |= sim.eval(gates.lookup("bridge_clocks_" + std::to_string(i), false).getResult(1)) << i;
    }
    for (unsigned i = 0; i < channels.size(); ++i)
      firedMask |= sim.eval(fired.lookup(channels[i].name)) << i;
    llvm::outs() << "TRACE " << cycle << ' ' << mask << ' ' << done << ' ' << enableMask << ' '
      << firedMask << ' ' << inReady << ' ' << outValid << ' ' << ceMask << ' '
      << sim.eval(targetState[0]) << ' ' << sim.eval(targetState[1]) << ' '
      << hubField(4, "bits") << ' ' << hubField(5, "bits");
    unsigned externalReady = evalField(2, "ready") | evalField(3, "ready") << 1;
    unsigned externalValid = evalField(4, "valid") | evalField(5, "valid") << 1;
    if (queued)
      llvm::outs() << ' ' << externalReady << ' ' << externalValid << ' '
        << ((externalValid & 1) ? evalField(4, "bits") : 0) << ' '
        << ((externalValid & 2) ? evalField(5, "bits") : 0) << ' '
        << (hubField(2, "valid") | hubField(3, "valid") << 1) << ' '
        << hubField(2, "bits") << ' ' << hubField(3, "bits");
    llvm::outs() << '\n';
    require(sim.eval(targetState[0]) == expectedState[0] &&
            sim.eval(targetState[1]) == expectedState[1], "target-cycle state trajectory differs");
    require(!reset || !ceMask, "host reset advanced target state");
    if (!done && !reset) ++stalls;
    if (enableMask == 1 || enableMask == 2) ++unequal;
    if (done && !reset) ++completed;
    auto heldMask = mask;
    // Observe each actual output handshake, including early independent firing.
    for (unsigned i = 0; i < 2; ++i) {
      auto data = evalField(4 + i, "bits");
      auto hubInput = hubField(2 + i, "bits");
      require(hubField(4 + i, "bits") == ((expectedState[i] + hubInput) & 65535),
              "output payload does not reflect target state and current input");
      if (!reset && blocked[i])
        require((externalValid & (1 << i)) && data == blockedBits[i],
                "valid output token changed while backpressured");
      blocked[i] = !reset && (externalValid & (1 << i)) && !evalField(4 + i, "ready");
      blockedBits[i] = data;
      if (pending[i] && (externalReady & (1 << i))) { pending[i] = false; ++inputCount[i]; }
      if (evalField(4 + i, "ready") && (externalValid & (1 << i)) && !reset) ++outputCount[i];
      edges[i] += (ceMask >> i) & 1;
      if ((ceMask >> i) & 1) expectedState[i] = (expectedState[i] + hubInput) & 65535;
    }
    sim.edge();
    // Check the connected producer holds its next edge token while blocked.
    if (!done && !reset) {
      sim.memo.clear();
      require((sim.eval(tokens[0]) | sim.eval(tokens[1]) << 1) == heldMask,
              "producer token changed under hub backpressure");
    }
  }
  require(stalls > 2000 && unequal > 1000 && completed > 500 &&
          edges[0] > 200 && edges[1] > 200 && inputCount[0] > 200 && inputCount[1] > 200 &&
          outputCount[0] > 200 && outputCount[1] > 200, "coupled schedule lacks stall/domain coverage");
  llvm::errs() << "Coupled hub: " << completed << " completions, " << stalls << " stalled host cycles, "
               << edges[0] << '/' << edges[1] << " target edges\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc <= 3 && (argc == 1 || StringRef(argv[1]) == "--normal" ||
                         StringRef(argv[1]) == "--reversed" || StringRef(argv[1]) == "--queued" ||
                         StringRef(argv[1]) == "--queued-reversed"),
            "usage: FAMEHubClockCoupledTest [--normal|--reversed|--queued|--queued-reversed [output.mlir]]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    clockRecordRejections(ctx);
    outputValidRejections(ctx);
    bool reversed = argc > 1 && (StringRef(argv[1]) == "--reversed" ||
                                StringRef(argv[1]) == "--queued-reversed");
    bool queued = argc > 1 && StringRef(argv[1]).starts_with("--queued");
    run(ctx, reversed, queued, argc > 2 ? argv[2] : nullptr);
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
