// See LICENSE for license details.
// Oracle: SimpleNICBridge's stream depths and CPUManagedStreamEngine's
// incomingQueueIO/outgoingQueueIO. Host reset flushes occupancy, never RAM.
// Copied port targets move to the wrapper; buffered stream targets stay on
// the retained producer/consumer. No annotations are consumed.
#include "goldengate/NICCPUStreamQueues.h"
#include "goldengate/CPUStreamQueue.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addNICCPUStreamQueues(CircuitOp circuit, std::string &error) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC CPU stream queues require retained annotations");
  unsigned nics = 0;
  for (auto attr : raw) {
    Annotation a(attr); auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (cast<DictionaryAttr>(attr).get("widgetConstructorKey"))
        return reject("NIC CPU stream queues require a constructor-free widget");
      ++nics;
    }
  }
  if (!nics) return success();
  if (nics != 1 || circuit.getName() != "GGNICPacketAdaptersWrapper")
    return reject("NIC CPU stream queues require one active packet-adapter boundary");
  constexpr llvm::StringLiteral queueName = "GGNICCPUStreamQueue3072";
  constexpr llvm::StringLiteral wrapperName = "GGNICCPUStreamQueuesWrapper";
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == queueName || m.getModuleName() == wrapperName)
      return reject("NIC CPU stream queue or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  if (!inner) return reject("NIC CPU stream queues require an internal top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto bit = UIntType::get(ctx, 1, false);
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, UIntType::get(ctx, 512, false)}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(ctx), Direction::In);
  auto reset = port("hostReset", bit, Direction::In);
  auto incoming = port("nic_from_host_stream", token, Direction::In);
  auto outgoing = port("nic_to_host_stream", token, Direction::Out);
  if (!clock || !reset || !incoming || !outgoing)
    return reject("NIC CPU stream queues need typed host clock/reset and 512-bit streams");
  for (auto p : inner.getPorts())
    if (p.name == "nic_from_host_stream_count" || p.name == "nic_to_host_stream_count")
      return reject("NIC CPU stream count boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("NIC CPU stream queues require an uninstantiated top");

  // All rejection checks precede hardware and target mutation.
  auto queue = createCPUStreamQueue3072(circuit, queueName);
  auto ports = inner.getPorts();
  unsigned inCount = ports.size();
  ports.push_back({b.getStringAttr("nic_from_host_stream_count"), UIntType::get(ctx, 12, false), Direction::Out});
  unsigned outCount = ports.size();
  ports.push_back({b.getStringAttr("nic_to_host_stream_count"), UIntType::get(ctx, 12, false), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto fromHost = b.create<InstanceOp>(loc, queue, "SIMPLENICBRIDGEMODULE_0_from_cpu_stream_incomingQueueIO_q");
  auto toHost = b.create<InstanceOp>(loc, queue, "SIMPLENICBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q");
  auto arg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (i == *incoming || i == *outgoing) continue;
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : arg(i),
        p.direction == Direction::In ? arg(i) : sim.getResult(i));
  }
  for (auto fifo : {fromHost, toHost}) {
    connect(fifo.getResult(0), arg(*clock)); connect(fifo.getResult(1), arg(*reset));
  }
  b.create<ConnectOp>(loc, fromHost.getResult(2), arg(*incoming));
  b.create<ConnectOp>(loc, sim.getResult(*incoming), fromHost.getResult(3));
  b.create<ConnectOp>(loc, toHost.getResult(2), sim.getResult(*outgoing));
  b.create<ConnectOp>(loc, arg(*outgoing), toHost.getResult(3));
  connect(arg(inCount), fromHost.getResult(4)); connect(arg(outCount), toHost.getResult(4));
  for (auto [fifo, direction, name] : {
      std::make_tuple(fromHost, "fromHostCPU", "SIMPLENICBRIDGEMODULE_0_from_cpu_stream"),
      std::make_tuple(toHost, "toHostCPU", "SIMPLENICBRIDGEMODULE_0_to_cpu_stream")}) {
    // Do not assign indices until the shared CPU stream catalog is rebuilt.
    fifo->setAttr("goldengate.streamParameters", b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(name)),
        b.getNamedAttr("direction", b.getStringAttr(direction)),
        b.getNamedAttr("depth", b.getI64IntegerAttr(3072)),
        b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64))}));
  }
  std::string op = "~" + circuit.getName().str(), np = "~" + wrapperName.str();
  std::string mp = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue(); if (v == op) return b.getStringAttr(np);
      if (!v.consume_front(op + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(mp)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto [i, p] : llvm::enumerate(inner.getPorts()))
          if (i != *incoming && i != *outgoing && name == p.name) {
            suffix.replace(0, mp.size(), "|" + wrapperName.str() + ">"); break;
          }
      }
      return b.getStringAttr(np + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto v : arr) out.push_back(retarget(v)); return b.getArrayAttr(out); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto v : d) out.set(v.getName(), retarget(v.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
