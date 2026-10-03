// See LICENSE for license details.
// CPUManagedStreamEngine's TracerV Queue(UInt(512.W), 6144),
// flow=false, pipe=false. Match the SFC synchronous read address pipeline.
// Requires: uninstantiated Rocket TracerV control wrapper, host clock/reset,
// a Decoupled UInt<512> producer and retained annotations. Validates atomically.
// Consumes: no annotations. Transfers circuit identity and copied port targets;
// producer stream targets remain on the original module. Produces stream/RAM
// metadata for later driver/XDC emission (neither emitter runs in this pass).
// Mutates: creates a queue and wrapper, inserts buffering on tracerv_stream,
// adds a UInt<13> count output. No cached analyses are used or preserved.
// Output: same producer semantics; a buffered stream with synchronous lookahead
// and a decoded occupancy boundary. CPU AXI transport remains a later stage.
#include "goldengate/TracerVTokenEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addTracerVStreamQueue(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGTracerVStreamQueue6144";
  constexpr llvm::StringLiteral wrapperName = "GGTracerVStreamQueueWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGTracerVBridgeControlWrapper")
    return reject("TracerV stream queue requires the active TracerV control wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (!m) continue;
    if (m.getModuleName() == queueName || m.getModuleName() == wrapperName)
      return reject("TracerV stream queue or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("TracerV stream queue needs a top module and retained annotations");
  auto *context = circuit.getContext();
  OpBuilder b(context);
  auto bit = UIntType::get(context, 1, false), word = UIntType::get(context, 512, false);
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, word}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(context), Direction::In);
  auto stream = port("tracerv_stream", token, Direction::Out);
  auto reset = port("hostReset", bit, Direction::In);
  for (auto p : inner.getPorts()) if (p.name == "tracerv_stream_count")
    return reject("TracerV stream count boundary already exists");
  if (!clock || !stream || !reset)
    return reject("TracerV stream queue needs hostClock, hostReset and a 512-bit Decoupled stream");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("TracerV stream queue needs an uninstantiated top");

  // Validate before mutation. Raw references to the producer stay on the inner
  // module; only copied ports move to the buffered boundary.
  // Create hardware only after all boundary invariants have been checked.
  Location loc = circuit.getLoc();
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> queuePorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq"), token, Direction::In}, {b.getStringAttr("deq"), token, Direction::Out},
      {b.getStringAttr("count"), UIntType::get(context, 13, false), Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(queueName), ConventionAttr::get(context, Convention::Internal), queuePorts);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto arg = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto constant = [&](unsigned width, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, UIntType::get(context, width, false), APInt(width, n));
  };
  auto reg = [&](unsigned width, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, UIntType::get(context, width, false), arg(0), arg(1), constant(width, 0), name).getResult();
  };
  auto mux = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  Value enqPtr = reg(13, "enq_ptr_value"), deqPtr = reg(13, "deq_ptr_value");
  Value maybeFull = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value full = both(equal, maybeFull), empty = both(equal, b.create<NotPrimOp>(loc, maybeFull));
  Value ready = b.create<NotPrimOp>(loc, full), valid = b.create<NotPrimOp>(loc, empty);
  Value push = both(ready, field(arg(2), "valid")), pop = both(valid, field(arg(3), "ready"));
  connect(field(arg(2), "ready"), ready); connect(field(arg(3), "valid"), valid);
  auto increment = [&](Value pointer) -> Value {
    Value plus = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, pointer, constant(13, 1)), 12, 0);
    return mux(b.create<EQPrimOp>(loc, pointer, constant(13, 6143)), constant(13, 0), plus);
  };
  connect(enqPtr, mux(push, increment(enqPtr), enqPtr));
  connect(deqPtr, mux(pop, increment(deqPtr), deqPtr));
  connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));

  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(6144, word, MemOp::PortKind::Read),
      MemOp::getTypeForPort(6144, word, MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 6144,
      RUWAttr::Undefined, memoryNames, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), arg(0)); connect(field(reader, "en"), constant(1, 1));
  Value readAddress = b.create<RegOp>(loc, UIntType::get(context, 13, false), arg(0), "ram_read_addr").getResult();
  connect(readAddress, mux(pop, increment(deqPtr), deqPtr));
  connect(field(reader, "addr"), readAddress); connect(field(arg(3), "bits"), field(reader, "data"));
  connect(field(writer, "clk"), arg(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), constant(1, 1));
  connect(field(writer, "data"), field(arg(2), "bits"));
  // Explicit unreset address register plus RAM reproduces the golden's
  // read-during-write behavior: data observes the updated memory after the edge.
  // Reset flushes pointers, leaving RAM and the read-address capture active.
  Value diff = b.create<BitsPrimOp>(loc, b.create<SubPrimOp>(loc, enqPtr, deqPtr), 12, 0);
  Value wrapped = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, constant(13, 6144), diff), 12, 0);
  connect(arg(4), mux(equal, mux(maybeFull, constant(13, 6144), constant(13, 0)),
      mux(b.create<GTPrimOp>(loc, deqPtr, enqPtr), wrapped, diff)));
  // Retain platform intent for the later XDC emitter, separate from queue logic.
  ram->setAttr("goldengate.ramStyle", b.getStringAttr("ULTRA"));
  queue->setAttr("goldengate.streamParameters", b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr("TRACERVBRIDGEMODULE_0_to_cpu_stream")),
      b.getNamedAttr("index", b.getI64IntegerAttr(0)),
      b.getNamedAttr("depth", b.getI64IntegerAttr(6144)),
      b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64))}));

  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) { copied.push_back(i); ports.push_back(p); }
  ports.push_back({b.getStringAttr("tracerv_stream_count"), UIntType::get(context, 13, false), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto fifo = b.create<InstanceOp>(loc, queue, "TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q");
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (i == *stream) continue;
    Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(fifo.getResult(0), wrapper.getBodyBlock()->getArgument(*clock));
  connect(fifo.getResult(1), wrapper.getBodyBlock()->getArgument(*reset));
  b.create<ConnectOp>(loc, fifo.getResult(2), sim.getResult(*stream));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(*stream), fifo.getResult(3));
  connect(wrapper.getBodyBlock()->getArgument(copied.size()), fifo.getResult(4));

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
        for (auto i : copied) if (i != *stream && name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
