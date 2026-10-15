// See LICENSE for license details.
// CPUManagedStreamEngine's TracerV Queue(UInt(512.W), 6144),
// flow=false, pipe=false. Match the SFC synchronous read address pipeline.
// Requires: explicitly selected uninstantiated boundary, host clock/reset,
// a Decoupled UInt<512> producer and retained annotations. Validates atomically.
// Consumes: no annotations. Transfers circuit identity and copied port targets;
// producer stream targets remain on the original module. Produces stream/RAM
// metadata for later driver/XDC emission (neither emitter runs in this pass).
// Mutates: creates one independent queue per ordered TracerV producer and a
// wrapper with one UInt<13> count output per queue. No cached analyses are used
// or preserved.
// Output: same producer semantics; a buffered stream with synchronous lookahead
// and a decoded occupancy boundary. CPU AXI transport remains a later stage.
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/CPUStreamQueue.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addTracerVStreamQueue(CircuitOp circuit, std::string &error) {
  return attachTracerVStreamQueue(circuit, "GGTracerVBridgeControlWrapper",
                                  "GGTracerVStreamQueueWrapper", error);
}

LogicalResult goldengate::attachTracerVStreamQueue(CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGTracerVStreamQueue6144";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (inputBoundary.empty() || wrapperName.empty() || inputBoundary == wrapperName || wrapperName == queueName ||
      circuit.getName() != inputBoundary)
    return reject("TracerV stream queue requires the selected active boundary and a distinct wrapper");
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
  auto reset = port("hostReset", bit, Direction::In);
  SmallVector<unsigned> streams;
  SmallVector<std::string> suffixes;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    auto name = p.name.getValue();
    if (name.starts_with("tracerv_stream_count"))
      return reject("TracerV stream count boundary already exists");
    if (name != "tracerv_stream" && !name.starts_with("tracerv_stream_")) continue;
    std::string suffix = streams.empty() ? "" : "_" + std::to_string(streams.size());
    if (name != "tracerv_stream" + suffix || p.type != token || p.direction != Direction::Out)
      return reject("TracerV stream queues need ordered contiguous 512-bit producers");
    streams.push_back(i);
    suffixes.push_back(suffix);
  }
  if (!clock || streams.empty() || !reset)
    return reject("TracerV stream queue needs hostClock, hostReset and a 512-bit Decoupled stream");
  for (auto suffix : suffixes) {
    std::string helper = queueName.str() + suffix;
    if (wrapperName == helper) return reject("TracerV queue helper collides with wrapper");
    for (auto m : circuit.getOps<FModuleLike>())
      if (m.getModuleName() == helper) return reject("TracerV stream queue helper already exists");
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("TracerV stream queue needs an uninstantiated top");

  // Validate before mutation. Raw references to the producer stay on the inner
  // module; only copied ports move to the buffered boundary.
  // Create hardware only after all boundary invariants have been checked.
  Location loc = circuit.getLoc();
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  SmallVector<FModuleOp> queues;
  for (auto [bank, suffix] : llvm::enumerate(suffixes)) {
    auto queue = createCPUStreamQueue6144(circuit, queueName.str() + suffix);
    queue->setAttr("goldengate.streamParameters", b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr("TRACERVBRIDGEMODULE_" + std::to_string(bank) + "_to_cpu_stream")),
        b.getNamedAttr("index", b.getI64IntegerAttr(bank)),
        b.getNamedAttr("depth", b.getI64IntegerAttr(6144)),
        b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64))}));
    queues.push_back(queue);
  }

  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) { copied.push_back(i); ports.push_back(p); }
  for (auto suffix : suffixes)
    ports.push_back({b.getStringAttr("tracerv_stream_count" + suffix), UIntType::get(context, 13, false), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  SmallVector<InstanceOp> fifos;
  for (auto [bank, queue] : llvm::enumerate(queues))
    fifos.push_back(b.create<InstanceOp>(loc, queue,
        "TRACERVBRIDGEMODULE_" + std::to_string(bank) + "_to_cpu_stream_outgoingQueueIO_q"));
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (llvm::is_contained(streams, i)) continue;
    Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  for (auto [bank, stream] : llvm::enumerate(streams)) {
    auto fifo = fifos[bank];
    connect(fifo.getResult(0), wrapper.getBodyBlock()->getArgument(*clock));
    connect(fifo.getResult(1), wrapper.getBodyBlock()->getArgument(*reset));
    b.create<ConnectOp>(loc, fifo.getResult(2), sim.getResult(stream));
    b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(stream), fifo.getResult(3));
    connect(wrapper.getBodyBlock()->getArgument(copied.size() + bank), fifo.getResult(4));
  }

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
        for (auto i : copied) if (!llvm::is_contained(streams, i) && name == inner.getPortName(i)) {
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
