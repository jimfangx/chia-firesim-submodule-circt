// See LICENSE for license details.
// Requires: active uninstantiated GGBlockDevReadLatencyWrapper and retained raw
// annotations, exact qualified reset/token/response/timing/latency boundaries.
// Consumes: timing input, target response ready and both latency dequeues.
// Transfers: circuit/copied-port targets; consumed targets stay on inner sim.
// Produces: host-clocked response scheduler; no annotation classes removed.
// Mutates: adds scheduler/wrapper; invalidates hierarchy and port analyses.
// Analyses required: none. Preserves: channel, clock and constructor metadata.
// Output: ordinary FIRRTL ops. Reset clears read beats and write selection;
// state holds without tFire, write wins selection, dequeue is not reset gated.
// Oracle: BlockDevBridgeModule.scala scheduler, one tracker, 64 beats/sector.
#include "goldengate/BlockDevResponseScheduler.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addBlockDevResponseScheduler(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral schedulerName = "GGBlockDevResponseScheduler";
  constexpr llvm::StringLiteral wrapperName = "GGBlockDevResponseSchedulerWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevReadLatencyWrapper")
    return reject("BlockDev response scheduler requires the active BlockDev read latency wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == schedulerName || m.getModuleName() == wrapperName)
      return reject("BlockDev response scheduler or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("BlockDev response scheduler needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto bit = UIntType::get(ctx, 1, false), cycleType = UIntType::get(ctx, 24, false);
  auto timing = BundleType::get(ctx, {{b.getStringAttr("returnWrite"), false, bit},
      {b.getStringAttr("readRespBusy"), false, bit}, {b.getStringAttr("wAckStallN"), true, bit},
      {b.getStringAttr("rRespStallN"), true, bit}, {b.getStringAttr("tCycle"), true, cycleType}});
  auto word = UIntType::get(ctx, 32, false);
  auto writeDequeue = BundleType::get(ctx, {{b.getStringAttr("ready"), false, bit},
      {b.getStringAttr("valid"), true, bit}});
  auto readDequeue = BundleType::get(ctx, {{b.getStringAttr("ready"), false, bit},
      {b.getStringAttr("valid"), true, bit}, {b.getStringAttr("bits"), true, word}});
  const llvm::StringRef required[]{"hostClock", "blockdev_queue_reset", "blockdev_tfire",
      "blockdev_resp_ready", "blockdev_timing", "blockdev_write_latency_deq", "blockdev_read_latency_deq"};
  const Type types[]{ClockType::get(ctx), bit, bit, bit, timing, writeDequeue, readDequeue};
  const Direction directions[]{Direction::In, Direction::Out, Direction::Out, Direction::Out,
      Direction::In, Direction::In, Direction::In};
  unsigned indices[7];
  for (unsigned j = 0; j < 7; ++j) {
    std::optional<unsigned> index;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == required[j] && p.type == types[j] && p.direction == directions[j]) index = i;
    if (!index) return reject("BlockDev scheduler needs exact clock, qualified reset, token, response and latency boundaries");
    indices[j] = *index;
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("BlockDev response scheduler needs an uninstantiated top");

  // Validate the whole boundary before mutation. State is host-clocked,
  // qualified reset dominates state updates; dequeue ready is not reset-gated.
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> schedulerPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("tFire"), bit, Direction::In}, {b.getStringAttr("resp_ready"), bit, Direction::In},
      {b.getStringAttr("write_valid"), bit, Direction::In}, {b.getStringAttr("read_valid"), bit, Direction::In},
      {b.getStringAttr("read_bits"), word, Direction::In},
      {b.getStringAttr("write_ready"), bit, Direction::Out}, {b.getStringAttr("read_ready"), bit, Direction::Out},
      {b.getStringAttr("returnWrite"), bit, Direction::Out}, {b.getStringAttr("readRespBusy"), bit, Direction::Out}};
  auto scheduler = b.create<FModuleOp>(loc, b.getStringAttr(schedulerName), ConventionAttr::get(ctx, Convention::Internal), schedulerPorts);
  b.setInsertionPointToStart(scheduler.getBodyBlock());
  auto arg = [&](unsigned i) { return scheduler.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, UIntType::get(ctx, w, false), APInt(w, n)); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto select = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  Value beats = b.create<RegResetOp>(loc, word, arg(0), arg(1), constant(32, 0), "readRespBeatsLeft").getResult();
  Value returningWrite = b.create<RegResetOp>(loc, bit, arg(0), arg(1), constant(1, 0), "returnWrite").getResult();
  Value busy = b.create<NEQPrimOp>(loc, beats, constant(32, 0));
  Value idle = both(neg(returningWrite), neg(busy));
  Value responseFire = both(either(returningWrite, busy), arg(3));
  Value done = both(either(returningWrite, b.create<EQPrimOp>(loc, beats, constant(32, 1))), responseFire);
  Value choose = either(done, idle), advance = both(arg(2), choose);
  connect(arg(7), both(advance, arg(4)));
  connect(arg(8), both(both(advance, neg(arg(4))), arg(5)));
  connect(arg(9), returningWrite); connect(arg(10), busy);
  // 512-byte sectors / 64-bit beats = 64. Preserve UInt32 assignment
  // truncation of the Scala product, including zero and overflow lengths.
  Value loaded = b.create<BitsPrimOp>(loc, b.create<CatPrimOp>(loc, arg(6), constant(6, 0)), 31, 0);
  Value selectedBeats = select(arg(4), constant(32, 0), select(arg(5), loaded, constant(32, 0)));
  Value decrement = b.create<BitsPrimOp>(loc, b.create<SubPrimOp>(loc, beats, constant(32, 1)), 31, 0);
  Value remaining = select(both(busy, responseFire), decrement, beats);
  connect(beats, select(arg(2), select(choose, selectedBeats, remaining), beats));
  connect(returningWrite, select(advance, arg(4), returningWrite));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (i != indices[3] && i != indices[4] && i != indices[5] && i != indices[6]) {
      copied.push_back(i); ports.push_back(p);
    }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto timingState = b.create<InstanceOp>(loc, scheduler, "responseScheduler");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(timingState.getResult(0), outer(indices[0]));
  for (unsigned i = 1; i <= 3; ++i) connect(timingState.getResult(i), sim.getResult(indices[i]));
  connect(timingState.getResult(4), field(sim.getResult(indices[5]), "valid"));
  connect(timingState.getResult(5), field(sim.getResult(indices[6]), "valid"));
  connect(timingState.getResult(6), field(sim.getResult(indices[6]), "bits"));
  connect(field(sim.getResult(indices[5]), "ready"), timingState.getResult(7));
  connect(field(sim.getResult(indices[6]), "ready"), timingState.getResult(8));
  connect(field(sim.getResult(indices[4]), "returnWrite"), timingState.getResult(9));
  connect(field(sim.getResult(indices[4]), "readRespBusy"), timingState.getResult(10));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue();
      if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(ctx);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
