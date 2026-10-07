// See LICENSE for license details.
// Execute the production wrapper and independently score each broadcast FIFO.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <deque>
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
Value field(OpBuilder &b, Location loc, Value value, StringRef name) {
  return b.create<SubfieldOp>(loc, value, name);
}
#include "FIRRTLInterpreter.h"
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == name) return module;
  throw std::runtime_error("missing module " + name.str());
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  OpBuilder b(&ctx); auto loc = b.getUnknownLoc();
  auto root = ModuleOp::create(loc);
  b.setInsertionPointToStart(root.getBody());
  auto circuit = b.create<CircuitOp>(loc, b.getStringAttr("Top"));
  b.setInsertionPointToStart(circuit.getBodyBlock());
  auto bit = UIntType::get(&ctx, 1), data = UIntType::get(&ctx, 16);
  auto decoupled = [&](FIRRTLBaseType payload) {
    return BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
                            {b.getStringAttr("hostReset"), bit, Direction::In}};
  for (unsigned i = 0; i < 3; ++i)
    ports.push_back({b.getStringAttr("lane" + std::to_string(i)),
        decoupled(bad == 4 && i == 1 ? FIRRTLBaseType(SIntType::get(&ctx, 16)) : data),
        bad == 5 && i == 1 ? Direction::Out : Direction::In});
  // This clock and the ordinary ports move left when secondary inputs vanish.
  ports.push_back({b.getStringAttr("ticks"), decoupled(ClockType::get(&ctx)), Direction::In});
  for (unsigned i = 0; i < 3; ++i) {
    ports.push_back({b.getStringAttr("ready" + std::to_string(i)), bit, Direction::In});
    ports.push_back({b.getStringAttr("valid" + std::to_string(i)), bit, Direction::Out});
    ports.push_back({b.getStringAttr("bits" + std::to_string(i)), ports[2+i].type.cast<BundleType>().getElement("bits")->type, Direction::Out});
  }
  auto target = b.create<FModuleOp>(loc, b.getStringAttr("Top"),
      ConventionAttr::get(&ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(target.getBodyBlock());
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  b.create<ConnectOp>(loc, field(b, loc, target.getArgument(5), "ready"), one);
  for (unsigned i = 0; i < 3; ++i) {
    Value channel = target.getArgument(2+i);
    b.create<ConnectOp>(loc, field(b, loc, channel, "ready"), target.getArgument(6+3*i));
    b.create<ConnectOp>(loc, target.getArgument(7+3*i), field(b, loc, channel, "valid"));
    b.create<ConnectOp>(loc, target.getArgument(8+3*i), field(b, loc, channel, "bits"));
  }
  SmallVector<Attribute> annotations;
  for (unsigned i = 0; i < 3; ++i) {
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr("fork" + std::to_string(i))),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
        b.getNamedAttr("latency", b.getI64IntegerAttr(i == 1 ? 1 : 0))})),
      b.getNamedAttr(bad == 5 && i == 1 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr("~Top|Top>lane" + std::to_string(i) + ".bits")}))}));
  }
  SmallVector<Attribute> names{b.getStringAttr("fork0"), b.getStringAttr("fork1"), b.getStringAttr("fork2")};
  if (bad == 1) names.clear();
  if (bad == 2) names[2] = b.getStringAttr("missing");
  if (bad == 3) names[2] = names[0];
  auto fanout = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelFanout)),
    b.getNamedAttr("channelNames", b.getArrayAttr(names))});
  annotations.push_back(fanout);
  if (bad == 6) annotations.push_back(fanout);
  auto clock = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("clock")),
      b.getNamedAttr("multiplier", b.getI64IntegerAttr(1)), b.getNamedAttr("divisor", b.getI64IntegerAttr(1))});
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr("ticks")),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::TargetClockChannel)),
          b.getNamedAttr("clockInfo", b.getArrayAttr({clock})),
          b.getNamedAttr("perClockMFMR", b.getArrayAttr({b.getI64IntegerAttr(1)}))})),
      b.getNamedAttr("sinks", b.getArrayAttr({b.getStringAttr("~Top|Top>ticks.bits")}))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return root;
}
// One bridge output and two retained model sinks share one target producer.
OwningOpRef<ModuleOp> targetFixture(MLIRContext &ctx, unsigned bad = 0) {
  OpBuilder b(&ctx); auto loc = b.getUnknownLoc();
  auto root = ModuleOp::create(loc); b.setInsertionPointToStart(root.getBody());
  auto circuit = b.create<CircuitOp>(loc, b.getStringAttr("Top"));
  b.setInsertionPointToStart(circuit.getBodyBlock());
  auto bit = UIntType::get(&ctx, 1), data = UIntType::get(&ctx, 16);
  auto decoupled = [&](FIRRTLBaseType payload) {
    return BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In},
      {b.getStringAttr("producer_source"), decoupled(data), Direction::Out},
      {b.getStringAttr("model1_sink"), decoupled(bad == 4 ? FIRRTLBaseType(SIntType::get(&ctx,16)) : data), Direction::In},
      {b.getStringAttr("model2_sink"), decoupled(data), Direction::In},
      {b.getStringAttr("ticks"), decoupled(ClockType::get(&ctx)), Direction::In},
      {b.getStringAttr("sourceValid"), bit, Direction::In},
      {b.getStringAttr("sourceBits"), data, Direction::In},
      {b.getStringAttr("sourceReady"), bit, Direction::Out}};
  for (unsigned i = 1; i < 3; ++i) {
    ports.push_back({b.getStringAttr("ready" + std::to_string(i)), bit, Direction::In});
    ports.push_back({b.getStringAttr("valid" + std::to_string(i)), bit, Direction::Out});
    ports.push_back({b.getStringAttr("bits" + std::to_string(i)), ports[2+i].type.cast<BundleType>().getElement("bits")->type, Direction::Out});
  }
  if (bad == 6) ports.push_back({b.getStringAttr("other_source"), decoupled(data), Direction::Out});
  auto target = b.create<FModuleOp>(loc, b.getStringAttr("Top"),
      ConventionAttr::get(&ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(target.getBodyBlock());
  Value producer = target.getArgument(2);
  b.create<ConnectOp>(loc, field(b, loc, producer, "valid"), target.getArgument(6));
  b.create<ConnectOp>(loc, field(b, loc, producer, "bits"), target.getArgument(7));
  b.create<ConnectOp>(loc, target.getArgument(8), field(b, loc, producer, "ready"));
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  b.create<ConnectOp>(loc, field(b, loc, target.getArgument(5), "ready"), one);
  for (unsigned i = 1; i < 3; ++i) {
    Value channel = target.getArgument(2+i);
    b.create<ConnectOp>(loc, field(b, loc, channel, "ready"), target.getArgument(6+3*i));
    b.create<ConnectOp>(loc, target.getArgument(7+3*i), field(b, loc, channel, "valid"));
    b.create<ConnectOp>(loc, target.getArgument(8+3*i), field(b, loc, channel, "bits"));
  }
  SmallVector<Attribute> annotations;
  for (unsigned i = 0; i < 3; ++i) {
    SmallVector<NamedAttribute> fields{
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr("fork" + std::to_string(i))),
      b.getNamedAttr("channelInfo", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
        b.getNamedAttr("latency", b.getI64IntegerAttr(i == 1 ? 1 : 0))})),
      b.getNamedAttr("sources", b.getArrayAttr({b.getStringAttr(bad == 6 && i == 2 ? "~Top|Top>other_source.bits" : "~Top|Top>producer_source.bits")}))};
    if (i && !(bad == 2 && i == 2)) fields.push_back(b.getNamedAttr("sinks", b.getArrayAttr({
      b.getStringAttr("~Top|Top>model" + std::to_string(bad == 3 ? 1 : i) + "_sink.bits")})));
    annotations.push_back(b.getDictionaryAttr(fields));
  }
  if (bad != 1) annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelFanout)),
    b.getNamedAttr("channelNames", b.getArrayAttr(bad == 5 ?
        SmallVector<Attribute>{b.getStringAttr("fork0"), b.getStringAttr("fork1")} :
        SmallVector<Attribute>{b.getStringAttr("fork0"), b.getStringAttr("fork1"), b.getStringAttr("fork2")}))}));
  if (bad == 5) annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelFanout)),
    b.getNamedAttr("channelNames", b.getArrayAttr({b.getStringAttr("fork2")}))}));
  // Reuse the production clock annotation from the bridge fixture.
  auto clockRoot = fixture(ctx);
  auto clockCircuit = *clockRoot->getOps<CircuitOp>().begin();
  auto clockAnnotations = clockCircuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  annotations.push_back(clockAnnotations[clockAnnotations.size()-1]);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return root;
}
void targetRejected(MLIRContext &ctx) {
  for (unsigned bad = 1; bad <= 6; ++bad) {
    auto root = targetFixture(ctx, bad); auto circuit = *root->getOps<CircuitOp>().begin();
    auto before = dump(root->getOperation()); std::string error;
    require(failed(goldengate::addFAMEBoundaryPipeChannels(circuit, error)) && !error.empty(),
            "malformed target fanout accepted");
    require(before == dump(root->getOperation()), "target fanout rejection mutated input");
    require(failed(goldengate::addFAMEPipeWrapper(circuit, error)) &&
            before == dump(root->getOperation()), "target wrapper rejection was not atomic");
  }
}
void internalChannels(MLIRContext &ctx) {
  for (unsigned count : {1u, 2u}) {
    auto root = targetFixture(ctx); auto circuit = *root->getOps<CircuitOp>().begin();
    auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> annotations;
    for (unsigned i = 1; i <= count; ++i) annotations.push_back(raw[i]);
    OpBuilder b(&ctx);
    if (count == 2) annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelFanout)),
      b.getNamedAttr("channelNames", b.getArrayAttr({b.getStringAttr("fork2"), b.getStringAttr("fork1")}))}));
    annotations.push_back(raw[raw.size()-1]);
    circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    std::string error;
    require(succeeded(goldengate::addFAMEBoundaryPipeChannels(circuit, error)), error);
    require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
    auto wrapper = named(circuit, "GGFAMEPipeWrapper");
    for (auto port : wrapper.getPorts())
      require(port.name != "producer_source" && port.name != "model1_sink" &&
              (count != 2 || port.name != "model2_sink"), "internal channel exposed a wrapper endpoint");
    require(succeeded(goldengate::addFAMEClockChannel(circuit, error)), error);
    require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error);
    require(succeeded(verify(*root)), "internal channel circuit verification");
    for (unsigned i = 0; i < count; ++i) {
      auto channel = cast<DictionaryAttr>(circuit->getAttrOfType<ArrayAttr>("rawAnnotations")[i]);
      for (StringRef side : {StringRef("sources"), StringRef("sinks")}) {
        auto path = cast<StringAttr>(channel.getAs<ArrayAttr>(side)[0]);
        auto endpoint = goldengate::resolveAnnotationTarget(circuit, path.getValue(), error);
        require(endpoint && endpoint->module.getModuleName() == "Top", "internal endpoint identity changed");
      }
    }
  }
}
void rejected(MLIRContext &ctx) {
  for (unsigned bad = 1; bad <= 6; ++bad) {
    auto root = fixture(ctx, bad); auto circuit = *root->getOps<CircuitOp>().begin();
    auto before = dump(root->getOperation()); std::string error;
    require(failed(goldengate::addFAMEBoundaryPipeChannels(circuit, error)) && !error.empty(),
            "malformed fanout accepted");
    require(before == dump(root->getOperation()), "rejected fanout mutated input");
    require(failed(goldengate::addFAMEPipeWrapper(circuit, error)) &&
            before == dump(root->getOperation()), "wrapper rejection was not atomic");
  }
}
void run(MLIRContext &ctx, const char *output, bool targetSource) {
  auto root = targetSource ? targetFixture(ctx) : fixture(ctx); auto circuit = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addFAMEBoundaryPipeChannels(circuit, error)), error);
  require(succeeded(goldengate::addFAMEPipeWrapper(circuit, error)), error);
  auto wrapper = named(circuit, "GGFAMEPipeWrapper");
  require(wrapper.getNumPorts() == named(circuit, "Top").getNumPorts()-2 &&
          wrapper.getPortName(3) == "ticks", "secondary wrapper inputs were not omitted");
  require(succeeded(goldengate::addFAMEClockChannel(circuit, error)), error);
  require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error);
  require(succeeded(verify(*root)), "fanout circuit verification");
  for (unsigned i = 0; i < 3; ++i) {
    auto attr = cast<DictionaryAttr>(circuit->getAttrOfType<ArrayAttr>("rawAnnotations")[i]);
    for (StringRef side : {StringRef("sources"), StringRef("sinks")}) {
      auto endpoints = attr.getAs<ArrayAttr>(side);
      if (!endpoints) continue;
      auto path = cast<StringAttr>(endpoints[0]).getValue();
      auto target = goldengate::resolveAnnotationTarget(circuit, path, error);
      require(target && target->module.getModuleName() ==
              (targetSource || i ? "Top" : "GGFAMEPipeWrapper"),
              "fanout endpoint lost wrapper/inner target identity");
    }
  }
  if (output) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec); require(!ec, "export fanout fixture");
    auto lowering = cast<ModuleOp>(root->clone());
    (*lowering.getOps<CircuitOp>().begin())->removeAttr("rawAnnotations");
    lowering.print(out); lowering->destroy();
  }
  OpBuilder b(&ctx); b.setInsertionPointToEnd(wrapper.getBodyBlock());
  Value source = wrapper.getArgument(2);
  Value inReady = targetSource ? wrapper.getArgument(6) : field(b, wrapper.getLoc(), source, "ready");
  Value inValid = targetSource ? wrapper.getArgument(4) : field(b, wrapper.getLoc(), source, "valid");
  Value inBits = targetSource ? wrapper.getArgument(5) : field(b, wrapper.getLoc(), source, "bits");
  Value bridgeReady, bridgeValid, bridgeBits;
  if (targetSource) {
    bridgeReady = field(b, wrapper.getLoc(), source, "ready");
    bridgeValid = field(b, wrapper.getLoc(), source, "valid");
    bridgeBits = field(b, wrapper.getLoc(), source, "bits");
  }
  SmallVector<InstanceOp> queues(3);
  for (auto instance : wrapper.getOps<InstanceOp>())
    for (unsigned i = 0; i < 3; ++i)
      if (instance.getName() == "PipeChannel_fork" + std::to_string(i)) queues[i] = instance;
  Interpreter sim(circuit, wrapper);
  std::deque<uint64_t> fifos[3]; unsigned deliveries[3] = {}, highWater[3] = {};
  unsigned transfers = 0, stalls = 0, differing = 0, seeds = 0;
  bool pending = false, previousReset = false; unsigned counter = 0;
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    bool reset = cycle < 3 || cycle == 2048 || cycle == 2049;
    if (reset) { pending = false; counter = 0; }
    else if (!pending && cycle % 7 != 0) pending = true;
    unsigned bits = (counter * 73 + 19) & 65535;
    bool ready[3] = {cycle % 97 >= 11, cycle % 83 >= 7, cycle % 61 >= 13};
    sim.memo.clear();
    sim.memo[sim.key(wrapper.getArgument(0))] = 0;
    sim.memo[sim.key(wrapper.getArgument(1))] = reset;
    sim.memo[sim.key(inValid)] = pending; sim.memo[sim.key(inBits)] = bits;
    for (unsigned i = 0; i < 3; ++i) {
      Value sinkReady = targetSource && i == 0 ? bridgeReady : wrapper.getArgument(4+3*i);
      sim.memo[sim.key(sinkReady)] = ready[i];
    }
    unsigned sourceReady = sim.eval(inReady), outMask = 0, validMask = 0, readyMask = 0;
    uint64_t outBits[3];
    for (unsigned i = 0; i < 3; ++i) {
      auto queue = queues[i]; require(bool(queue), "queue instance missing");
      auto r = sim.eval(queue.getResult(2)), v = sim.eval(queue.getResult(3));
      auto valid = sim.eval(queue.getResult(6)), data = sim.eval(queue.getResult(7));
      outMask |= valid << i; validMask |= v << i; readyMask |= r << i;
      outBits[i] = valid ? data : 0;
      require(r == (fifos[i].size() < 2 && !(i == 1 && previousReset)), "capacity/init mismatch");
      require(valid == !fifos[i].empty() && (!valid || data == fifos[i].front()), "FIFO payload mismatch");
      highWater[i] = std::max(highWater[i], unsigned(fifos[i].size()));
      // Observe through the retained target, not only its queue output.
      Value sinkValid = targetSource && i == 0 ? bridgeValid : wrapper.getArgument(5+3*i);
      Value sinkBits = targetSource && i == 0 ? bridgeBits : wrapper.getArgument(6+3*i);
      require(sim.eval(sinkValid) == valid && sim.eval(sinkBits) == data,
              "target/bridge sink wiring mismatch");
    }
    require(sourceReady == (readyMask == 7), "broadcast readiness mismatch");
    for (unsigned i = 0; i < 3; ++i)
      require(((validMask >> i) & 1) == (pending && (readyMask | (1u << i)) == 7), "peer readiness mismatch");
    llvm::outs() << "TRACE " << cycle << ' ' << sourceReady << ' ' << outMask << ' '
        << outBits[0] << ' ' << outBits[1] << ' ' << outBits[2] << ' ' << validMask << ' ' << readyMask << '\n';
    bool transfer = pending && sourceReady;
    if (!reset) { transfers += transfer; stalls += pending && !transfer; differing += readyMask != 0 && readyMask != 7; }
    for (unsigned i = 0; i < 3; ++i) {
      if (reset) fifos[i].clear();
      else {
        if (!fifos[i].empty() && ready[i]) { fifos[i].pop_front(); ++deliveries[i]; }
        if (i == 1 && previousReset) { fifos[i].push_back(0); ++seeds; }
        else if (transfer) fifos[i].push_back(bits);
        require(fifos[i].size() <= 2, "broadcast FIFO overflow");
      }
    }
    if (!reset && transfer) { pending = false; ++counter; }
    previousReset = reset; sim.edge();
  }
  require(transfers > 1000 && stalls > 500 && differing > 500 && seeds == 2, "fanout coverage incomplete");
  for (unsigned i = 0; i < 3; ++i) require(deliveries[i] > 1000 && highWater[i] == 2, "sink coverage incomplete");
  llvm::errs() << "Passed fanout: " << transfers << " transfers, " << stalls << " stalls, " << differing
               << " independent stalls, two reset seeds\n";
}
} // namespace
int main(int argc, char **argv) {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    rejected(ctx); targetRejected(ctx); internalChannels(ctx);
    bool targetSource = argc > 1 && StringRef(argv[argc-1]) == "target";
    run(ctx, argc > 1 && StringRef(argv[1]) != "target" ? argv[1] : nullptr, targetSource);
  }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
  return 0;
}
