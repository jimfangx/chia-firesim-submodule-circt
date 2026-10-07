// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/FAMEOutputChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/InferModelPorts.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/WrapTop.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
#include <tuple>

using namespace mlir;
using namespace circt::firrtl;
using goldengate::AnnotationClasses;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
// A pair of data directions and two unrelated printf channels. Connection
// order, local channel names, and physical port order deliberately differ.
void run(MLIRContext &context, unsigned rejection) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %hostClock: !firrtl.clock,
          in %data: !firrtl.uint<8>, in %ready: !firrtl.uint<1>,
          in %trigger: !firrtl.uint<1>, out %printfA: !firrtl.uint<8>,
          out %forwardData: !firrtl.uint<8>, out %forwardValid: !firrtl.uint<1>,
          out %reverseReady: !firrtl.uint<1>, out %printfB: !firrtl.uint<8>,
          in %inputValid: !firrtl.uint<1>, in %otherData: !firrtl.uint<8>,
          out %otherPrintf: !firrtl.uint<8>) {}
      firrtl.module private @Model(in %clock: !firrtl.clock,
          in %data: !firrtl.uint<8>, in %ready: !firrtl.uint<1>,
          in %trigger: !firrtl.uint<1>, out %printfA: !firrtl.uint<8>,
          out %forwardData: !firrtl.uint<8>, out %forwardValid: !firrtl.uint<1>,
          out %reverseReady: !firrtl.uint<1>, out %printfB: !firrtl.uint<8>,
          in %inputValid: !firrtl.uint<1>) {}
      firrtl.module private @Other(in %data: !firrtl.uint<8>,
                                  out %printf: !firrtl.uint<8>) {}
      firrtl.extmodule private @BlackBox(in input: !firrtl.uint<1>,
                                       out out: !firrtl.uint<8>)
    }
  })mlir", &context);
  require(bool(root), "output selection fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>();
  auto it = modules.begin();
  auto top = *it++;
  auto model = *it++;
  auto other = *it;
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  for (unsigned i = 0; i < model.getNumPorts(); ++i) {
    auto t = top.getBodyBlock()->getArgument(i);
    auto m = instance.getResult(i);
    bool input = model.getPortDirection(i) == Direction::In;
    b.create<StrictConnectOp>(top.getLoc(), input ? m : t, input ? t : m);
  }
  if (rejection >= 16) {
    PortInfo alias(b.getStringAttr("secondPrintf"), UIntType::get(&context, 8),
                   Direction::Out);
    alias.sym = circt::hw::InnerSymAttr::get(b.getStringAttr("alias_payload"));
    alias.annotations = AnnotationSet(b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("firrtl.transforms.DontTouchAnnotation"))})}));
    unsigned aliasPort = top.getNumPorts();
    top.insertPorts({{aliasPort, alias}});
    b.create<StrictConnectOp>(top.getLoc(),
        top.getBodyBlock()->getArgument(aliasPort), instance.getResult(8));
    if (rejection == 18)
      b.create<NodeOp>(top.getLoc(), instance.getResult(8), "extra_use");
    if (rejection == 19)
      top.setPortSymbolsAttr(8, circt::hw::InnerSymAttr::get(
          b.getStringAttr("other_payload")));
    if (rejection == 20)
      b.create<NodeOp>(top.getLoc(), top.getBodyBlock()->getArgument(aliasPort),
                       "extra_alias_use");
  }
  auto otherInstance = b.create<InstanceOp>(top.getLoc(), other, "other");
  b.create<StrictConnectOp>(top.getLoc(), otherInstance.getResult(0),
                           top.getBodyBlock()->getArgument(10));
  b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(11),
                           otherInstance.getResult(1));
  b.setInsertionPointToEnd(other.getBodyBlock());
  b.create<StrictConnectOp>(other.getLoc(), other.getBodyBlock()->getArgument(1),
                           other.getBodyBlock()->getArgument(0));
  b.setInsertionPointToEnd(model.getBodyBlock());
  auto arg = [&](unsigned i) { return model.getBodyBlock()->getArgument(i); };
  if (rejection == 7 || rejection == 9) {
    auto external = *circuit.getOps<FExtModuleOp>().begin();
    if (rejection == 9)
      external->setAttr("defname", b.getStringAttr("plusarg_reader"));
    auto blackbox = b.create<InstanceOp>(model.getLoc(), external, "blackbox");
    b.create<StrictConnectOp>(model.getLoc(), blackbox.getResult(0), arg(3));
    b.create<StrictConnectOp>(model.getLoc(), arg(4), blackbox.getResult(1));
  } else if (rejection != 6) {
    auto print = b.create<MuxPrimOp>(model.getLoc(), arg(3), arg(1), arg(1));
    b.create<StrictConnectOp>(model.getLoc(), arg(4), print.getResult());
  }
  auto forward = b.create<MuxPrimOp>(model.getLoc(), arg(2), arg(1), arg(1));
  b.create<StrictConnectOp>(model.getLoc(), arg(5), forward.getResult());
  b.create<StrictConnectOp>(model.getLoc(), arg(6), arg(3));
  b.create<StrictConnectOp>(model.getLoc(), arg(7), arg(2));
  // Native bundle construction and aggregate multibit selection must not make
  // printfB depend on the unused trigger field. The index shares rx_local with
  // data, so the selected channel must be deduplicated in the valid rule.
  auto recordType = BundleType::get(&context, {
      {b.getStringAttr("data"), false, UIntType::get(&context, 8)},
      {b.getStringAttr("unused"), false, UIntType::get(&context, 1)}});
  Value record = b.create<BundleCreateOp>(model.getLoc(), recordType,
                                        ValueRange{arg(1), arg(3)});
  Value alternative = b.create<BundleCreateOp>(model.getLoc(), recordType,
                                              ValueRange{arg(1), arg(3)});
  record = b.create<MultibitMuxOp>(model.getLoc(), arg(9),
                                   ValueRange{alternative, record});
  Value printData = b.create<SubfieldOp>(model.getLoc(), record, "data");
  b.create<StrictConnectOp>(model.getLoc(), arg(8), printData);

  auto strings = [&](ArrayRef<StringRef> ports, StringRef module) {
    SmallVector<Attribute> targets;
    for (auto port : ports)
      targets.push_back(b.getStringAttr(("~Top|" + module + ">" + port).str()));
    return b.getArrayAttr(targets);
  };
  SmallVector<Attribute> annotations;
  auto group = [&](StringRef name, ArrayRef<StringRef> ports,
                   StringRef module = "Model") {
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelPorts)),
        b.getNamedAttr("localName", b.getStringAttr(name)),
        b.getNamedAttr("ports", strings(ports, module))}));
  };
  group("print_local_a", {"printfA"});
  group(rejection == 4 ? "print_local_a" : "print_local_b", {"printfB"});
  group("tx_local", {"forwardData", "forwardValid"});
  group("tx_ready_local", {"ready"});
  group("rx_local", {"data", "inputValid"});
  group("rx_ready_local", {"reverseReady"});
  group(rejection == 10 ? "rx_local" : "trigger_local", {"trigger"});
  group("other_input", {"data"}, "Other");
  group("other_print", {"printf"}, "Other");
  if (rejection == 3) group("ambiguous_print", {"printfB"});
  if (rejection == 5) annotations.erase(annotations.begin());
  auto channel = [&](StringRef name, StringRef kind,
                     ArrayRef<StringRef> sources, ArrayRef<StringRef> sinks,
                     StringRef valid = "", StringRef ready = "", unsigned latency = 0) {
    SmallVector<NamedAttribute> info{
        b.getNamedAttr("class", b.getStringAttr(kind))};
    if (kind == AnnotationClasses::PipeChannel)
      info.push_back(b.getNamedAttr("latency", b.getI64IntegerAttr(latency)));
    if (!valid.empty()) {
      bool source = !sources.empty();
      info.push_back(b.getNamedAttr(source ? "validSource" : "validSink",
                                   b.getStringAttr(("~Top|Top>" + valid).str())));
      info.push_back(b.getNamedAttr(source ? "readySink" : "readySource",
                                   b.getStringAttr(("~Top|Top>" + ready).str())));
    }
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("globalName", b.getStringAttr(name)),
        b.getNamedAttr("channelInfo", b.getDictionaryAttr(info)),
        b.getNamedAttr("sources", strings(sources, "Top")),
        b.getNamedAttr("sinks", strings(sinks, "Top"))}));
  };
  channel("arbitrary_print_second", AnnotationClasses::PipeChannel,
          rejection == 2 ? ArrayRef<StringRef>{"printfB", "printfB"}
                         : ArrayRef<StringRef>{"printfB"}, {});
  channel("tx_global", AnnotationClasses::DecoupledForwardChannel,
          {"forwardValid", "forwardData"}, {}, "forwardValid", "ready");
  channel("other_print_global", AnnotationClasses::PipeChannel, {"otherPrintf"}, {});
  channel("rx_ready_global", AnnotationClasses::DecoupledReverseChannel,
          {"reverseReady"}, {});
  channel(rejection == 1 ? "arbitrary_print_second" : "arbitrary_print_first",
          AnnotationClasses::PipeChannel, {"printfA"}, {});
  channel("tx_ready_global", AnnotationClasses::DecoupledReverseChannel, {}, {"ready"});
  channel("rx_global", AnnotationClasses::DecoupledForwardChannel, {},
          {"inputValid", "data"}, "inputValid", "reverseReady");
  channel("trigger_global", AnnotationClasses::PipeChannel, {}, {"trigger"});
  channel("other_input_global", AnnotationClasses::PipeChannel, {}, {"otherData"});
  if (rejection == 8 || (rejection >= 16 && rejection != 17))
    channel("second_claim_on_print_b", AnnotationClasses::PipeChannel,
            rejection >= 16 ? ArrayRef<StringRef>{"secondPrintf"}
                            : ArrayRef<StringRef>{"printfB"}, {}, "", "", 1);
  if (rejection == 11 || rejection == 12)
    channel("tx_alias", AnnotationClasses::DecoupledForwardChannel,
            rejection == 12 ? ArrayRef<StringRef>{"forwardData", "forwardValid"}
                            : ArrayRef<StringRef>{"forwardValid", "forwardData"},
            {}, "forwardValid", "ready");
  if (rejection == 13)
    channel("changed_kind", AnnotationClasses::PipeChannel,
            {"forwardValid", "forwardData"}, {});
  if (rejection == 14)
    channel("partial_payload", AnnotationClasses::PipeChannel, {"forwardData"}, {});
  if (rejection == 15)
    channel("input_alias", AnnotationClasses::DecoupledReverseChannel, {}, {"ready"});
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  require(succeeded(verify(*root)), "output selection fixture invalid");
  auto before = dump(*root);
  std::string error;
  auto dataSelection = goldengate::analyzeFAMEDataSelection(circuit, model, error);
  auto selected = dataSelection
      ? std::optional<SmallVector<goldengate::FAMEOutputSelection>>(dataSelection->outputs)
      : std::nullopt;
  require(dump(*root) == before, "output selection mutated IR/annotations");
  if (rejection && rejection != 8 && rejection != 11 && rejection < 16) {
    require(!selected && !error.empty(), "unsafe output selection accepted: " +
                                          std::to_string(rejection));
    return;
  }
  require(selected && selected->size() == 4, "expected four selected outputs: " + error);
  require(dataSelection->inputs.size() == 3 &&
              dataSelection->inputs[0].globalName == "tx_ready_global" &&
              dataSelection->inputs[0].localName == "tx_ready_local" &&
              dataSelection->inputs[0].kind == goldengate::ChannelKind::DecoupledReverse &&
              dataSelection->inputs[0].fieldCount == 1 &&
              dataSelection->inputs[1].globalName == "rx_global" &&
              dataSelection->inputs[1].localName == "rx_local" &&
              dataSelection->inputs[1].kind == goldengate::ChannelKind::DecoupledForward &&
              dataSelection->inputs[1].fieldCount == 2 &&
              dataSelection->inputs[2].globalName == "trigger_global" &&
              dataSelection->inputs[2].localName == "trigger_local" &&
              dataSelection->inputs[2].kind == goldengate::ChannelKind::Pipe &&
              dataSelection->inputs[2].fieldCount == 1,
          "input selection lost annotation order, model isolation or payload shape");
  const char *global[] = {"arbitrary_print_second", "tx_global", "rx_ready_global",
                          "arbitrary_print_first"};
  const char *local[] = {"print_local_b", "tx_local", "rx_ready_local", "print_local_a"};
  const goldengate::ChannelKind kinds[] = {goldengate::ChannelKind::Pipe,
      goldengate::ChannelKind::DecoupledForward, goldengate::ChannelKind::DecoupledReverse,
      goldengate::ChannelKind::Pipe};
  const std::vector<std::string> dependencies[] = {{"rx_local"},
      {"tx_ready_local", "rx_local", "trigger_local"}, {"tx_ready_local"},
      {"trigger_local", "rx_local"}};
  for (unsigned i = 0; i < 4; ++i) {
    const auto &output = (*selected)[i];
    require(output.globalName == global[i] && output.localName == local[i] &&
                output.kind == kinds[i] && output.fieldCount == (i == 1 ? 2u : 1u),
            "output order/name/kind/field count differs from annotations");
    require(output.dependency.outputChannel == local[i] &&
                output.dependency.inputChannels == dependencies[i] &&
                output.dependency.unresolvedPorts.empty() &&
                output.dependency.unresolvedCauses.empty(),
            "output lost data/ready/trigger dependencies");
  }
  require((*selected)[0].globalAliases ==
              ((rejection == 8 || (rejection >= 16 && rejection != 17)) ? std::vector<std::string>{"second_claim_on_print_b"}
                              : std::vector<std::string>{}),
          "shared output branch did not retain one producer and its dependencies");
  require((*selected)[1].globalAliases ==
              (rejection == 11 ? std::vector<std::string>{"tx_alias"}
                               : std::vector<std::string>{}),
          "multiport shared producer lost ordered branch identity");
  if (rejection == 8) {
    llvm::outs() << "PRODUCER printfB branches 2\n";
  }
  if (rejection == 11) return;
  error.clear();
  auto otherSelection = goldengate::analyzeFAMEOutputSelection(circuit, other, error);
  require(otherSelection && otherSelection->size() == 1 &&
              otherSelection->front().globalName == "other_print_global" &&
              otherSelection->front().dependency.inputChannels ==
                  std::vector<std::string>{"other_input"},
          "selection leaked channels across models: " + error);

  auto hierarchy = goldengate::analyzeTopHierarchy(circuit, error);
  SmallVector<goldengate::ModelPortGroup> groups;
  SmallVector<goldengate::GGChannelConnection, 0> channels;
  for (auto attr : annotations) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::ChannelPorts)) {
      auto value = goldengate::analyzeModelPortGroup(circuit, annotation, error);
      require(bool(value), error);
      groups.push_back(std::move(*value));
    } else {
      auto value = goldengate::analyzeChannelConnection(circuit, annotation, error);
      require(bool(value), error);
      channels.push_back(std::move(*value));
    }
  }
  require(goldengate::validateDecoupledChannelPairs(channels, error), error);
  SmallVector<goldengate::ModelChannelBinding> bindings;
  for (const auto &connection : channels) {
    auto value = goldengate::bindChannelToModels(connection, *hierarchy, groups, error);
    require(bool(value), error);
    bindings.append(value->begin(), value->end());
  }
  auto plan = goldengate::analyzeFAMEPorts(*hierarchy, bindings, channels, {model}, error);
  require(plan && plan->sources.size() == 4, "selected ports cannot be planned: " + error);
  auto payload = cast<BundleType>(plan->sources[1].type.getElementType(2));
  require(payload.getElements()[0].name.getValue() == "forwardValid" &&
              payload.getElements()[1].name.getValue() == "forwardData",
          "source annotation payload order replaced by physical port order");
  // Exercise the selected arbitrary Print channel through the actual FAME IR
  // rewrite: its data driver must move under token.bits, with a typed host
  // handshake on both the wrapper and model source ports.
  if (rejection >= 17) {
    auto beforeRewrite = dump(*root);
    require(failed(goldengate::rewriteFAMEOutputChannel(
                *hierarchy, plan->sources.front(), error)) && !error.empty(),
            "unsafe output alias rewrite accepted: " + std::to_string(rejection));
    require(beforeRewrite == dump(*root), "rejected output alias mutated IR");
    return;
  }
  require(succeeded(goldengate::rewriteFAMEOutputChannel(
              *hierarchy, plan->sources.front(), error)), error);
  require(succeeded(verify(*root)), "selected Print output rewrite invalid");
  require(top.getPortName(8) == "model_print_local_b_source" &&
              model.getPortName(8) == "print_local_b_source" &&
              top.getPorts()[8].type == plan->sources.front().type &&
              model.getPorts()[8].type == plan->sources.front().type,
          "selected Print output did not become a matching token interface");
  if (rejection == 16) {
    require(top.getNumPorts() == 12 &&
                llvm::range_size(top.getPorts()[8].sym) == 1 &&
                (*top.getPorts()[8].sym.begin()).getName().getValue() ==
                    "alias_payload" &&
                (*top.getPorts()[8].sym.begin()).getFieldID() ==
                    plan->sources.front().type.getFieldID(2),
            "physical alias collapse lost payload identity or retained an old port");
    require(!cast<ArrayAttr>(top.getPortAnnotationsAttr()[8]).empty(),
            "physical alias collapse lost DontTouch metadata");
    llvm::outs() << "PRODUCER printfB physical aliases 2 one token port\n";
  }
  bool payloadDriven = false;
  for (auto connect : model.getOps<StrictConnectOp>())
    if (auto field = connect.getDest().getDefiningOp<SubfieldOp>())
      if (field.getInput() == model.getBodyBlock()->getArgument(8) &&
          field.getFieldName() == "bits") {
        require(connect.getSrc() == printData,
                "selected Print payload data driver changed");
        payloadDriven = true;
      }
  require(payloadDriven && other.getPortName(1) == "printf" &&
              model.getPortName(4) == "printfA",
          "selected Print rewrite lost payload or changed another channel");
}
// PrintSynthesis introduces aggregates after the initial target lowering.
// Reproduce that boundary: WrapTop creates scalar external leaves, while the
// model still has an aggregate port. The second lowering must expose every
// leaf to InferModelPorts and preserve its original combinational dependency.
void newlySynthesizedPrintBundle(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "PrintTarget" {
      firrtl.module @PrintTarget(in %clock: !firrtl.clock,
          in %data: !firrtl.sint<5>,
          out %record: !firrtl.bundle<enable: uint<1>, arg: sint<5>>) {
        %configuration = firrtl.instance reader @PlusArg(out out: !firrtl.uint<1>)
        %enable = firrtl.subfield %record[enable] : !firrtl.bundle<enable: uint<1>, arg: sint<5>>
        %arg = firrtl.subfield %record[arg] : !firrtl.bundle<enable: uint<1>, arg: sint<5>>
        firrtl.strictconnect %enable, %configuration : !firrtl.uint<1>
        firrtl.strictconnect %arg, %data : !firrtl.sint<5>
      }
      firrtl.extmodule private @PlusArg(out out: !firrtl.uint<1>)
    }
  })mlir", &context);
  require(bool(root), "new Print bundle parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  auto reader = *circuit.getOps<FExtModuleOp>().begin();
  reader->setAttr("defname", b.getStringAttr("plusarg_reader"));
  SmallVector<Attribute> annotations;
  for (auto [name, endpoint, input] : {
           std::tuple<StringRef, StringRef, bool>{"input_data", "data", true},
           {"print_enable", "record.enable", false},
           {"print_argument", "record.arg", false}}) {
    auto target = b.getArrayAttr({b.getStringAttr(
        ("~PrintTarget|PrintTarget>" + endpoint).str())});
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("globalName", b.getStringAttr(name)),
        b.getNamedAttr("channelInfo", b.getDictionaryAttr({
            b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::PipeChannel)),
            b.getNamedAttr("latency", b.getI64IntegerAttr(0))})),
        b.getNamedAttr(input ? "sinks" : "sources", target)}));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  std::string error;
  if (failed(goldengate::wrapTop(circuit, error)))
    throw std::runtime_error("Print WrapTop: " + error);
  if (failed(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)))
    throw std::runtime_error("Print LowerTypes: " + error);
  // FAMEDefaults selects the model after WrapTop changes the circuit identity.
  annotations.assign(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").begin(),
                     circuit->getAttrOfType<ArrayAttr>("rawAnnotations").end());
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::FAMETransform)),
      b.getNamedAttr("target", b.getStringAttr("~FAMETop|PrintTarget"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (failed(goldengate::inferModelPorts(circuit, error)))
    throw std::runtime_error("Print InferModelPorts: " + error);
  auto selected = goldengate::analyzeFAMEOutputSelection(circuit, model, error);
  require(bool(selected) && selected->size() == 2,
          "new Print bundle fields were omitted from FAME output selection: " + error);
  require((*selected)[0].globalName == "print_enable" &&
              (*selected)[0].dependency.inputChannels.empty(),
          "configuration-only Print enable dependency changed");
  require((*selected)[1].globalName == "print_argument" &&
              (*selected)[1].dependency.inputChannels ==
                  std::vector<std::string>{"data"},
          "signed Print argument lost its data-channel dependency");
  require(succeeded(verify(*root)), "new Print bundle lowering produced invalid IR");
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    if (argc == 4 && StringRef(argv[1]) == "--data-selection") {
      auto root = parseSourceFile<ModuleOp>(argv[2], &context);
      require(bool(root), "data-selection boundary parse failed");
      auto circuit = *root->getOps<CircuitOp>().begin();
      FModuleOp model;
      for (auto candidate : circuit.getOps<FModuleOp>())
        if (candidate.getName() == argv[3]) model = candidate;
      require(bool(model), "data-selection boundary model missing");
      auto before = dump(*root);
      std::string error;
      auto selection = goldengate::analyzeFAMEDataSelection(circuit, model, error);
      require(bool(selection), error);
      require(before == dump(*root), "data selection mutated boundary");
      for (const auto &input : selection->inputs)
        llvm::outs() << "INPUT " << input.globalName << " " << input.localName
                     << " " << input.fieldCount << '\n';
      for (const auto &output : selection->outputs)
        llvm::outs() << "OUTPUT " << output.globalName << " " << output.localName
                     << " " << output.fieldCount << '\n';
      for (const auto &output : selection->outputs) {
        llvm::outs() << "BRANCH " << output.globalName << " " << output.localName << '\n';
        for (const auto &alias : output.globalAliases)
          llvm::outs() << "BRANCH " << alias << " " << output.localName << '\n';
      }
      return 0;
    }
    require(argc == 1, "usage: test [--data-selection boundary.mlir model]");
    for (unsigned rejection = 0; rejection <= 20; ++rejection) run(context, rejection);
    newlySynthesizedPrintBundle(context);
    llvm::outs() << "Annotation-selected Print/forward/reverse outputs, payload order, "
                    "dependencies, scalar/multiport shared producers, all data inputs and model isolation passed; 13 unsafe selections "
                    "rejected without mutation; scalar physical aliases collapsed and four unsafe rewrites rejected atomically\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
