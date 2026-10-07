// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/TargetUtils.h"
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
#include "llvm/Support/Path.h"
#include <deque>
#include <random>
#include <stdexcept>

using namespace circt::firrtl;
using namespace mlir;

namespace {
void require(bool condition, llvm::StringRef message) {
  if (!condition)
    throw std::runtime_error(message.str());
}

CircuitOp circuitOf(ModuleOp root) {
  return *root.getOps<CircuitOp>().begin();
}

FModuleOp moduleNamed(CircuitOp circuit, llvm::StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == name)
      return module;
  throw std::runtime_error("missing module " + name.str());
}

DictionaryAttr pipeAnnotation(OpBuilder &builder, llvm::StringRef name,
                              llvm::StringRef member, llvm::StringRef port,
                              unsigned latency) {
  auto info = builder.getDictionaryAttr({
      builder.getNamedAttr("class", builder.getStringAttr(
          goldengate::AnnotationClasses::PipeChannel)),
      builder.getNamedAttr("latency", builder.getI64IntegerAttr(latency))});
  auto endpoint = builder.getStringAttr("~FAMETop|FAMETop>" + port + ".bits");
  return builder.getDictionaryAttr({
      builder.getNamedAttr("class", builder.getStringAttr(
          goldengate::AnnotationClasses::ChannelConnection)),
      builder.getNamedAttr("globalName", builder.getStringAttr(name)),
      builder.getNamedAttr("channelInfo", info),
      builder.getNamedAttr(member, builder.getArrayAttr({endpoint}))});
}

OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  // Port names deliberately differ from channel names and contain no Rocket
  // or trace naming convention. The last port has no PipeChannel annotation.
  auto root = parseSourceString<ModuleOp>(R"mlir(
module {
  firrtl.circuit "FAMETop" {
    firrtl.module @FAMETop(in %hostClock: !firrtl.clock,
                          in %hostReset: !firrtl.uint<1>,
                          out %sample: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>,
                          out %echo: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>,
                          in %reset_tokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
                          in %bridge_info: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>,
                          out %flag: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
                          out %direct: !firrtl.uint<32>) {
    }
  }
}
)mlir", &context);
  require(bool(root), "fixture parse failed");
  OpBuilder builder(&context);
  circuitOf(*root)->setAttr("rawAnnotations", builder.getArrayAttr({
      pipeAnnotation(builder, "retired", "sources", "sample", 1),
      pipeAnnotation(builder, "mirror", "sources", "echo", 1),
      pipeAnnotation(builder, "reset", "sinks", "reset_tokens", 0),
      pipeAnnotation(builder, "info", "sinks", "bridge_info", 1),
      pipeAnnotation(builder, "flag", "sources", "flag", 1)}));
  return root;
}

void checkWrapper(MLIRContext &context) {
  auto root = fixture(context);
  auto circuit = circuitOf(*root);
  std::string error;
  require(succeeded(goldengate::addFAMEBoundaryPipeChannels(circuit, error)), error);
  unsigned definitions = 0;
  for (auto module : circuit.getOps<FModuleOp>())
    definitions += module.getName().starts_with("GGFAMEPipe");
  require(definitions == 3, "modules must be shared by width and latency");
  require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(verify(*root)), "wrapper failed MLIR verification");
  require(circuit.getName() == "GGFAMEPipeWrapper", "wrong active circuit");
  auto wrapper = moduleNamed(circuit, "GGFAMEPipeWrapper");
  InstanceOp child;
  unsigned queues = 0;
  for (auto instance : wrapper.getOps<InstanceOp>()) {
    if (instance.getName() == "target_FAMETop")
      child = instance;
    else
      ++queues;
  }
  require(queues == 5 && bool(child), "wrapper needs one queue per annotation");
  auto field = [](Value value, llvm::StringRef name) {
    auto bundle = cast<BundleType>(value.getType());
    unsigned index = *bundle.getElementIndex(name);
    for (auto &use : value.getUses())
      if (auto subfield = dyn_cast<SubfieldOp>(use.getOwner());
          subfield && subfield.getFieldIndex() == index)
        return subfield.getResult();
    throw std::runtime_error("missing subfield");
  };
  auto connected = [&](Value dest, Value src) {
    for (auto connect : wrapper.getOps<ConnectOp>())
      if (connect.getDest() == dest && connect.getSrc() == src)
        return true;
    return false;
  };
  for (auto instance : wrapper.getOps<InstanceOp>()) {
    if (instance == child)
      continue;
    unsigned port = instance.getName() == "PipeChannel_retired" ? 2 :
                    instance.getName() == "PipeChannel_mirror" ? 3 :
                    instance.getName() == "PipeChannel_reset" ? 4 :
                    instance.getName() == "PipeChannel_info" ? 5 : 6;
    bool output = wrapper.getPortDirection(port) == Direction::Out;
    Value internal = child.getResult(port);
    Value external = wrapper.getBodyBlock()->getArgument(port);
    Value source = output ? internal : external;
    Value sink = output ? external : internal;
    require(connected(instance.getResult(0), wrapper.getBodyBlock()->getArgument(0)) &&
            connected(instance.getResult(1), wrapper.getBodyBlock()->getArgument(1)) &&
            connected(field(source, "ready"), instance.getResult(2)) &&
            connected(instance.getResult(3), field(source, "valid")) &&
            connected(instance.getResult(4), field(source, "bits")) &&
            connected(instance.getResult(5), field(sink, "ready")) &&
            connected(field(sink, "valid"), instance.getResult(6)) &&
            connected(field(sink, "bits"), instance.getResult(7)),
            "queue connection direction mismatch");
  }
  require(connected(wrapper.getBodyBlock()->getArgument(7), child.getResult(7)),
          "unannotated port must pass through");
  for (Attribute attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto annotation = cast<DictionaryAttr>(attr);
    auto endpoints = annotation.getAs<ArrayAttr>("sources");
    if (!endpoints)
      endpoints = annotation.getAs<ArrayAttr>("sinks");
    auto target = goldengate::resolveAnnotationTarget(
        circuit, cast<StringAttr>(endpoints[0]).getValue(), error);
    require(target && target->module == wrapper, "active endpoint target is stale");
  }
}

void checkRejectedAnnotations(MLIRContext &context) {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    auto root = fixture(context);
    auto circuit = circuitOf(*root);
    OpBuilder builder(&context);
    SmallVector<Attribute> annotations;
    auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    annotations.append(raw.begin(), raw.end());
    NamedAttrList first(cast<DictionaryAttr>(annotations[0]));
    if (scenario == 0) {
      annotations[0] = pipeAnnotation(builder, "bad", "sources", "sample", 2);
    } else if (scenario == 1) {
      annotations[0] = pipeAnnotation(builder, "bad", "sinks", "sample", 1);
    } else if (scenario == 2) {
      first.set("sources", builder.getArrayAttr({
          builder.getStringAttr("~FAMETop|FAMETop>sample.valid")}));
      annotations[0] = first.getDictionary(&context);
    } else if (scenario == 3) {
      first.set("sinks", builder.getArrayAttr({
          builder.getStringAttr("~FAMETop|FAMETop>reset_tokens.bits")}));
      annotations[0] = first.getDictionary(&context);
    } else if (scenario == 4) {
      annotations.push_back(pipeAnnotation(builder, "fork", "sources", "sample", 1));
    } else {
      annotations.push_back(pipeAnnotation(builder, "retired", "sources", "flag", 1));
    }
    circuit->setAttr("rawAnnotations", builder.getArrayAttr(annotations));
    std::string error;
    require(failed(goldengate::addFAMEBoundaryPipeChannels(circuit, error)) &&
            !error.empty(), "unsupported annotation must report an error");
    require(std::distance(circuit.getOps<FModuleOp>().begin(),
                          circuit.getOps<FModuleOp>().end()) == 1,
            "invalid annotations must fail before creating modules");
  }
}

// Each endpoint still names one bits field, even when that field contains a
// nested aggregate. These are payload types, not multi-endpoint annotations.
OwningOpRef<ModuleOp> typedFixture(MLIRContext &context, llvm::StringRef spelling) {
  std::string input = "module { firrtl.circuit \"FAMETop\" { "
      "firrtl.module @FAMETop(in %hostClock: !firrtl.clock, "
      "in %hostReset: !firrtl.uint<1>, "
      "out %tokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: " +
      spelling.str() + ">) {} }}";
  auto root = parseSourceString<ModuleOp>(input, &context);
  require(bool(root), "typed fixture parse failed");
  OpBuilder b(&context);
  circuitOf(*root)->setAttr("rawAnnotations", b.getArrayAttr({
      pipeAnnotation(b, "typed", "sources", "tokens", 1)}));
  return root;
}

FModuleOp queueModule(CircuitOp circuit) {
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getPortName(0) == "clock")
      return module;
  throw std::runtime_error("missing queue module");
}

void checkTypedBoundary(MLIRContext &context, llvm::StringRef spelling) {
  auto root = typedFixture(context, spelling);
  auto circuit = circuitOf(*root);
  auto payload = cast<BundleType>(moduleNamed(circuit, "FAMETop").getPortType(2))
                     .getElement("bits")->type;
  std::string error;
  require(succeeded(goldengate::addFAMEBoundaryPipeChannels(circuit, error)), error);
  auto pipe = queueModule(circuit);
  require(pipe.getPortType(4) == payload && pipe.getPortType(7) == payload,
          "queue must preserve payload type in both directions");
  require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(verify(*root)), "typed wrapper verification failed");
  auto target = goldengate::resolveAnnotationTarget(circuit,
      "~GGFAMEPipeWrapper|GGFAMEPipeWrapper>tokens.bits", error);
  require(target && target->port == 2 &&
          target->fieldID ==
              cast<BundleType>(target->module.getPortType(2)).getFieldID(2),
          "typed endpoint retarget mismatch");
}

void checkRejectedTypes(MLIRContext &context) {
  for (llvm::StringRef spelling : {"uint", "sint", "clock", "analog<8>",
      "bundle<x flip: uint<8>>", "bundle<x: uint<8>, y: clock>",
      "vector<uint, 2>"}) {
    auto root = typedFixture(context, spelling);
    auto circuit = circuitOf(*root);
    auto payload = cast<BundleType>(moduleNamed(circuit, "FAMETop").getPortType(2))
                       .getElement("bits")->type;
    std::string error;
    require(failed(goldengate::addFAMEBoundaryPipeChannels(circuit, error)),
            "unsupported boundary payload must fail");
    require(failed(goldengate::addFAMEPipeChannel(circuit, payload, 1, error)),
            "unsupported direct payload must fail");
    require(std::distance(circuit.getOps<FModuleOp>().begin(),
                          circuit.getOps<FModuleOp>().end()) == 1,
            "unsupported types must fail before mutation");
  }
  // Equal packed widths do not imply equal payload types or field names.
  auto root = fixture(context);
  auto circuit = circuitOf(*root);
  auto bit = UIntType::get(&context, 1);
  auto uint = UIntType::get(&context, 13);
  auto sint = SIntType::get(&context, 13);
  auto recordA = BundleType::get(&context, {
      {StringAttr::get(&context, "a"), false, uint}});
  auto recordB = BundleType::get(&context, {
      {StringAttr::get(&context, "b"), false, uint}});
  auto vector = FVectorType::get(bit, 13);
  std::string error;
  for (FIRRTLBaseType type : {FIRRTLBaseType(uint), FIRRTLBaseType(sint),
      FIRRTLBaseType(recordA), FIRRTLBaseType(recordB), FIRRTLBaseType(vector)})
    require(succeeded(goldengate::addFAMEPipeChannel(circuit, type, 1, error)), error);
  require(std::distance(circuit.getOps<FModuleOp>().begin(),
                        circuit.getOps<FModuleOp>().end()) == 6,
          "different payload types need distinct definitions");
  require(succeeded(verify(*root)), "distinct payload definitions failed verification");
}

// Evaluate the generated FIRRTL operations, then compare each edge with a
// token FIFO reference. This exercises the emitted queue rather than a copy
// of its Boolean recurrence. The reference enqueues only when not full, just
// as Scala ShiftQueue(2) does even when a simultaneous dequeue frees space.
void checkQueueBehavior(MLIRContext &context, llvm::StringRef spelling,
                        unsigned latency, llvm::StringRef shape = "",
                        llvm::StringRef outputDirectory = "") {
  auto root = typedFixture(context, spelling);
  auto circuit = circuitOf(*root);
  std::string error;
  auto payload = cast<BundleType>(moduleNamed(circuit, "FAMETop").getPortType(2))
                     .getElement("bits")->type;
  unsigned width = *getBitWidth(payload);
  require(succeeded(goldengate::addFAMEPipeChannel(circuit, payload, latency, error)), error);
  auto pipe = queueModule(circuit);
  require(succeeded(verify(*root)), "queue failed MLIR verification");
  llvm::DenseMap<Value, uint64_t> state;
  std::deque<uint64_t> tokens;
  std::mt19937_64 random(0x143 + width + latency);
  uint64_t mask = width == 64 ? ~uint64_t(0) : (uint64_t(1) << width) - 1;
  bool initializing = false;
  for (unsigned cycle = 0; cycle < (shape.empty() ? 10000 : 512); ++cycle) {
    bool reset = cycle < 3 || cycle % 97 < 2;
    bool inputValid = shape.empty() ? random() & 1 : cycle % 7 != 0;
    bool outputReady = shape.empty() ? random() & 1 : cycle % 11 >= 4;
    uint64_t inputBits = (shape.empty()
        ? random()
        : (cycle * 0x143ull + 0x1234) ^ (cycle % 2 ? 0xa5a50000ull : 0)) & mask;
    llvm::DenseMap<Value, uint64_t> values, next;
    auto arg = [&](unsigned i) { return pipe.getBodyBlock()->getArgument(i); };
    values[arg(0)] = 0;
    values[arg(1)] = reset;
    values[arg(3)] = inputValid;
    values[arg(4)] = inputBits;
    values[arg(5)] = outputReady;
    for (Operation &op : pipe.getBodyBlock()->getOperations()) {
      if (auto constant = dyn_cast<ConstantOp>(&op))
        values[constant.getResult()] = constant.getValue().getZExtValue();
      else if (isa<RegOp, RegResetOp>(&op))
        values[op.getResult(0)] = state[op.getResult(0)];
      else if (auto connect = dyn_cast<StrictConnectOp>(&op)) {
        if (connect.getDest().getDefiningOp() &&
            isa<RegOp, RegResetOp>(connect.getDest().getDefiningOp()))
          next[connect.getDest()] = values.lookup(connect.getSrc());
        else
          values[connect.getDest()] = values.lookup(connect.getSrc());
      } else if (isa<BitCastOp>(&op))
        values[op.getResult(0)] = values.lookup(op.getOperand(0));
      else if (isa<NotPrimOp>(&op))
        values[op.getResult(0)] = !values.lookup(op.getOperand(0));
      else if (isa<AndPrimOp>(&op))
        values[op.getResult(0)] = values.lookup(op.getOperand(0)) & values.lookup(op.getOperand(1));
      else if (isa<OrPrimOp>(&op))
        values[op.getResult(0)] = values.lookup(op.getOperand(0)) | values.lookup(op.getOperand(1));
      else if (isa<MuxPrimOp>(&op))
        values[op.getResult(0)] = values.lookup(op.getOperand(values.lookup(op.getOperand(0)) ? 1 : 2));
      else
        throw std::runtime_error("queue contains an unsupported operation");
    }
    bool queueReady = tokens.size() < 2;
    require(values.lookup(arg(2)) == (queueReady && !initializing), "input ready mismatch");
    require(values.lookup(arg(6)) == !tokens.empty(), "output valid mismatch");
    if (!tokens.empty())
      require(values.lookup(arg(7)) == tokens.front(), "output token mismatch under stalls/reset");
    if (!shape.empty())
      llvm::outs() << "TRACE " << shape << " " << latency << " " << cycle << " "
                   << values.lookup(arg(2)) << " " << values.lookup(arg(6)) << " "
                   << (tokens.empty() ? 0 : values.lookup(arg(7))) << "\n";
    for (auto reg : pipe.getOps<RegResetOp>())
      if (values.lookup(reg.getResetSignal()))
        next[reg.getResult()] = values.lookup(reg.getResetValue());
    state = std::move(next);
    if (reset)
      tokens.clear();
    else {
      if (outputReady && !tokens.empty())
        tokens.pop_front();
      if (queueReady && (initializing || inputValid))
        tokens.push_back(initializing ? 0 : inputBits);
    }
    initializing = latency == 1 && reset;
  }
  if (!outputDirectory.empty()) {
    moduleNamed(circuit, "FAMETop").erase();
    circuit->removeAttr("rawAnnotations");
    circuit.setName(pipe.getName());
    require(succeeded(verify(*root)), "exported queue verification failed");
    llvm::SmallString<256> path(outputDirectory);
    llvm::sys::path::append(path, shape.str() + "-" + std::to_string(latency) + ".mlir");
    std::error_code ec;
    llvm::raw_fd_ostream output(path, ec);
    require(!ec, "cannot write native payload fixture");
    root->print(output);
  }
}
} // namespace

int main(int argc, char **argv) {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    checkWrapper(context);
    checkRejectedAnnotations(context);
    checkRejectedTypes(context);
    for (unsigned width : {0, 1, 3, 32, 40, 64})
      for (unsigned latency : {0, 1})
        checkQueueBehavior(context, "uint<" + std::to_string(width) + ">", latency);
    for (auto [shape, spelling] : {
        std::pair<llvm::StringRef, llvm::StringRef>{"zero", "uint<0>"},
        {"signed", "sint<13>"}, {"vector", "vector<uint<5>, 3>"},
        {"nested", "bundle<flag: uint<1>, inner: bundle<signed: sint<7>, unsigned: uint<9>>, lanes: vector<uint<5>, 3>>"}}) {
      checkTypedBoundary(context, spelling);
      for (unsigned latency : {0, 1})
        checkQueueBehavior(context, spelling, latency, argc == 2 ? shape : "",
                           argc == 2 ? argv[1] : "");
    }
  } catch (const std::exception &error) {
    llvm::errs() << "FAME PipeChannel test failed: " << error.what() << '\n';
    return 1;
  }
  llvm::outs() << "FAME PipeChannel: annotation/wiring/rejection and typed queue cycles passed\n";
  return 0;
}
