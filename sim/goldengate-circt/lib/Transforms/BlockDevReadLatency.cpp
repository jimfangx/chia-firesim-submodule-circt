// See LICENSE for license details.
// Requires: active uninstantiated GGBlockDevWriteLatencyWrapper, retained raw
// annotations, exact qualified reset, committed read request, timing and latency ports.
// Consumes: read enqueue boundary; no annotation classes are removed.
// Transfers: circuit/copied-port targets; consumed targets stay on inner sim.
// Produces: one-entry read latency pipe and explicit dequeue/ready boundaries.
// Mutates: adds pipe/wrapper; invalidates hierarchy and port analyses.
// Analyses required: none. Preserves: channel, clock and constructor metadata.
// Output: ordinary FIRRTL ops; payload/deadline unreset, pending/full reset to zero.
// Deadline matching is host-clocked; enqueue wins pending clear, including
// latency one. Ready uses old full (no flow/pipe); reset does not gate enqueue.
// Oracle: midas/models/dram/Util.scala DynamicLatencyPipe, entries=1/countBits=24.
#include "goldengate/BlockDevReadLatency.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addBlockDevReadLatency(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral pipeName = "GGBlockDevReadLatencyPipe";
  constexpr llvm::StringLiteral wrapperName = "GGBlockDevReadLatencyWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevWriteLatencyWrapper")
    return reject("BlockDev read latency requires the active BlockDev write latency wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == pipeName || m.getModuleName() == wrapperName)
      return reject("BlockDev read latency pipe or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("BlockDev read latency needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto bit = UIntType::get(ctx, 1, false), cycleType = UIntType::get(ctx, 24, false);
  auto timing = BundleType::get(ctx, {{b.getStringAttr("returnWrite"), false, bit},
      {b.getStringAttr("readRespBusy"), false, bit}, {b.getStringAttr("wAckStallN"), true, bit},
      {b.getStringAttr("rRespStallN"), true, bit}, {b.getStringAttr("tCycle"), true, cycleType}});
  auto latency = BundleType::get(ctx, {{b.getStringAttr("read_latency"), false, cycleType},
      {b.getStringAttr("write_latency"), false, cycleType}});
  auto word = UIntType::get(ctx, 32, false);
  auto enqueue = BundleType::get(ctx, {{b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, word}});
  auto dequeue = BundleType::get(ctx, {{b.getStringAttr("ready"), false, bit},
      {b.getStringAttr("valid"), true, bit}, {b.getStringAttr("bits"), true, word}});
  const llvm::StringRef required[]{"hostClock", "hostReset", "blockdev_queue_reset",
      "blockdev_read_latency_enq", "blockdev_latency", "blockdev_timing"};
  const Type types[]{ClockType::get(ctx), bit, bit, enqueue, latency, timing};
  const Direction directions[]{Direction::In, Direction::In, Direction::Out,
      Direction::Out, Direction::Out, Direction::In};
  unsigned indices[6];
  for (auto p : inner.getPorts())
    if (p.name == "blockdev_read_latency_deq" || p.name == "blockdev_read_latency_enq_ready")
      return reject("BlockDev read latency boundary already exists");
  for (unsigned j = 0; j < 6; ++j) {
    std::optional<unsigned> index;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == required[j] && p.type == types[j] && p.direction == directions[j]) index = i;
    if (!index) return reject("BlockDev read latency needs exact host, reset, read request, latency and timing ports");
    indices[j] = *index;
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("BlockDev read latency needs an uninstantiated top");

  // All boundary checks precede mutation. The one-entry async payload RAM
  // remains visible even while empty; reset flushes timing, not stored length.
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> pipePorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq_valid"), bit, Direction::In}, {b.getStringAttr("enq_ready"), bit, Direction::Out},
      {b.getStringAttr("deq_ready"), bit, Direction::In}, {b.getStringAttr("deq_valid"), bit, Direction::Out},
      {b.getStringAttr("latency"), cycleType, Direction::In}, {b.getStringAttr("tCycle"), cycleType, Direction::In},
      {b.getStringAttr("enq_bits"), word, Direction::In}, {b.getStringAttr("deq_bits"), word, Direction::Out}};
  auto pipe = b.create<FModuleOp>(loc, b.getStringAttr(pipeName), ConventionAttr::get(ctx, Convention::Internal), pipePorts);
  b.setInsertionPointToStart(pipe.getBodyBlock());
  auto arg = [&](unsigned i) { return pipe.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, UIntType::get(ctx, w, false), APInt(w, n)); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto select = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  Value full = b.create<RegResetOp>(loc, bit, arg(0), arg(1), constant(1, 0), "maybe_full").getResult();
  Value pending = b.create<RegResetOp>(loc, bit, arg(0), arg(1), constant(1, 0), "pendingRegisters_0").getResult();
  Value deadline = b.create<RegOp>(loc, cycleType, arg(0), "latencies_0").getResult();
  Value ready = neg(full);
  Value match = b.create<EQPrimOp>(loc, deadline, arg(7));
  Value done = b.create<OrPrimOp>(loc, match, neg(pending));
  Value valid = both(full, done), put = both(ready, arg(2)), take = both(valid, arg(4));
  connect(arg(3), ready); connect(arg(5), valid);
  // Match DynamicLatencyPipe's Mem(1, UInt(32.W)): asynchronous read,
  // synchronous accepted-enqueue write, no reset gating or payload clearing.
  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(1, word, MemOp::PortKind::Read),
      MemOp::getTypeForPort(1, word, MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 1, RUWAttr::Undefined, memoryNames, "ram");
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), arg(0)); connect(field(reader, "en"), constant(1, 1));
  connect(field(reader, "addr"), constant(1, 0)); connect(arg(9), field(reader, "data"));
  connect(field(writer, "clk"), arg(0)); connect(field(writer, "en"), put);
  connect(field(writer, "addr"), constant(1, 0)); connect(field(writer, "mask"), constant(1, 1));
  connect(field(writer, "data"), arg(8));
  Value changing = b.create<XorPrimOp>(loc, put, take);
  connect(full, select(changing, put, full));
  Value nextDeadline = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, arg(7), arg(6)), 23, 0);
  connect(deadline, select(put, nextDeadline, deadline));
  Value notOne = neg(b.create<EQPrimOp>(loc, arg(6), constant(24, 1)));
  connect(pending, select(put, notOne, select(match, constant(1, 0), pending)));
  Value nonzero = neg(b.create<EQPrimOp>(loc, arg(6), constant(24, 0)));
  Value permitted = b.create<OrPrimOp>(loc, neg(put), nonzero);
  b.create<AssertOp>(loc, arg(0), permitted, neg(arg(1)),
      "DynamicLatencyPipe only supports latencies > 0", ValueRange{}, "");

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (i != indices[3]) { copied.push_back(i); ports.push_back(p); }
  unsigned dequeueIndex = ports.size();
  ports.push_back({b.getStringAttr("blockdev_read_latency_deq"), dequeue, Direction::In});
  ports.push_back({b.getStringAttr("blockdev_read_latency_enq_ready"), bit, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto queue = b.create<InstanceOp>(loc, pipe, "readLatencyPipe");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(queue.getResult(0), outer(indices[0]));
  connect(queue.getResult(1), sim.getResult(indices[2]));
  connect(queue.getResult(2), field(sim.getResult(indices[3]), "valid"));
  connect(queue.getResult(8), field(sim.getResult(indices[3]), "bits"));
  connect(queue.getResult(6), field(sim.getResult(indices[4]), "read_latency"));
  connect(queue.getResult(7), field(sim.getResult(indices[5]), "tCycle"));
  Value external = wrapper.getBodyBlock()->getArgument(dequeueIndex);
  connect(queue.getResult(4), field(external, "ready"));
  connect(field(external, "valid"), queue.getResult(5));
  connect(field(external, "bits"), queue.getResult(9));
  connect(wrapper.getBodyBlock()->getArgument(dequeueIndex + 1), queue.getResult(3));

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
