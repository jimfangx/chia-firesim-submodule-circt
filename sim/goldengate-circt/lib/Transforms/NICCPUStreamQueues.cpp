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
#include <set>
using namespace mlir;
using namespace circt::firrtl;

static LogicalResult addNICCPUStreamQueuesImpl(CircuitOp circuit, std::string &error, bool loopback) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC CPU stream queues require retained annotations");
  unsigned nics = 0;
  for (auto attr : raw) {
    Annotation a(attr); auto widget = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(goldengate::AnnotationClasses::BridgeIO) && widget &&
        widget.getValue() == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (cast<DictionaryAttr>(attr).get("widgetConstructorKey"))
        return reject("NIC CPU stream queues require a constructor-free widget");
      ++nics;
    }
  }
  if (!nics) return success();
  if (circuit.getName() != (loopback ? "GGNICLoopbackTokenQueuesWrapper" : "GGNICPacketAdaptersWrapper"))
    return reject("NIC CPU stream queues require an active packet-adapter boundary");
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
  if (!clock || !reset)
    return reject("NIC CPU stream queues need typed host clock/reset and 512-bit streams");
  auto externalName = [&](unsigned index, StringRef name) {
    return nics == 1 ? name.str() : "nic" + std::to_string(index) + name.drop_front(3).str();
  };
  struct Widget { unsigned incoming, outgoing; };
  SmallVector<Widget> widgets;
  std::set<unsigned> consumed;
  auto priorBindings = inner->getAttrOfType<ArrayAttr>(loopback ? "goldengate.nicTokenBindings" : "goldengate.nicPacketBindings");
  if ((priorBindings && priorBindings.size() != nics) || (!priorBindings && (nics > 1 || loopback)))
    return reject("NIC CPU stream queues need matching per-widget packet bindings");
  for (unsigned index = 0; index < nics; ++index) {
    auto fromName = externalName(index, "nic_from_host_stream");
    auto toName = externalName(index, "nic_to_host_stream");
    std::optional<unsigned> incoming, outgoing;
    if (loopback) {
      // These CPU queues remain allocated even though the NIC uses only its
      // internal token generator. They are deliberately absent at the token
      // boundary; rejecting collisions prevents duplicate stream allocation.
      for (auto p : inner.getPorts())
        if (p.name == fromName || p.name == toName)
          return reject("NIC loopback stream boundary already exists");
      auto binding = dyn_cast<DictionaryAttr>(priorBindings[index]);
      auto widget = binding ? binding.getAs<IntegerAttr>("widget") : IntegerAttr();
      if (!widget || widget.getInt() != index ||
          binding.getAs<StringAttr>("mode") != "compile_time_loopback")
        return reject("NIC CPU stream loopback binding identity differs");
      incoming = inner.getNumPorts() + 2 * index;
      outgoing = *incoming + 1;
    } else {
      incoming = port(fromName, token, Direction::In);
      outgoing = port(toName, token, Direction::Out);
      if (!incoming || !outgoing || !consumed.insert(*incoming).second ||
          !consumed.insert(*outgoing).second)
        return reject("NIC CPU stream queues need distinct typed streams for each widget");
      if (priorBindings) {
        auto binding = dyn_cast<DictionaryAttr>(priorBindings[index]);
        auto widget = binding ? binding.getAs<IntegerAttr>("widget") : IntegerAttr();
        if (!widget || widget.getInt() != index ||
            binding.getAs<StringAttr>("fromHostStream") != fromName ||
            binding.getAs<StringAttr>("toHostStream") != toName)
          return reject("NIC CPU stream packet binding identity differs");
      }
    }
    widgets.push_back({*incoming, *outgoing});
    for (auto p : inner.getPorts())
      if (p.name == externalName(index, "nic_from_host_stream_count") ||
          p.name == externalName(index, "nic_to_host_stream_count"))
        return reject("NIC CPU stream count boundary already exists");
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("NIC CPU stream queues require an uninstantiated top");

  // All rejection checks precede hardware and target mutation.
  auto queue = goldengate::createCPUStreamQueue3072(circuit, queueName);
  auto ports = inner.getPorts();
  if (loopback) for (unsigned index = 0; index < nics; ++index) {
    ports.push_back({b.getStringAttr(externalName(index, "nic_from_host_stream")), token, Direction::In});
    ports.push_back({b.getStringAttr(externalName(index, "nic_to_host_stream")), token, Direction::Out});
  }
  unsigned countStart = ports.size();
  for (unsigned index = 0; index < nics; ++index) {
    ports.push_back({b.getStringAttr(externalName(index, "nic_from_host_stream_count")), UIntType::get(ctx, 12, false), Direction::Out});
    ports.push_back({b.getStringAttr(externalName(index, "nic_to_host_stream_count")), UIntType::get(ctx, 12, false), Direction::Out});
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto arg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (consumed.count(i)) continue;
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : arg(i),
        p.direction == Direction::In ? arg(i) : sim.getResult(i));
  }
  SmallVector<Attribute> bindings;
  for (auto [index, widget] : llvm::enumerate(widgets)) {
    auto streamPrefix = "SIMPLENICBRIDGEMODULE_" + std::to_string(index);
    auto fromName = streamPrefix + "_from_cpu_stream";
    auto toName = streamPrefix + "_to_cpu_stream";
    auto fromHost = b.create<InstanceOp>(loc, queue, fromName + "_incomingQueueIO_q");
    auto toHost = b.create<InstanceOp>(loc, queue, toName + "_outgoingQueueIO_q");
    for (auto fifo : {fromHost, toHost}) {
      connect(fifo.getResult(0), arg(*clock)); connect(fifo.getResult(1), arg(*reset));
    }
    b.create<ConnectOp>(loc, fromHost.getResult(2), arg(widget.incoming));
    if (loopback) {
      auto field = [&](Value v, StringRef name) { return b.create<SubfieldOp>(loc, v, name).getResult(); };
      auto zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
      // BigTokenToNICTokenAdapter never advances its reset loop selector when
      // htnt.ready is false. NICTokenToBigTokenAdapter likewise never accepts
      // a token when ntht.valid is false. Thus no CPU traffic reaches either
      // loopback token path after host reset, but occupancy remains real state.
      connect(field(fromHost.getResult(3), "ready"), zero);
      connect(field(toHost.getResult(2), "valid"), zero);
      connect(field(toHost.getResult(2), "bits"),
              b.create<InvalidValueOp>(loc, UIntType::get(ctx, 512, false)));
    } else {
      b.create<ConnectOp>(loc, sim.getResult(widget.incoming), fromHost.getResult(3));
      b.create<ConnectOp>(loc, toHost.getResult(2), sim.getResult(widget.outgoing));
    }
    b.create<ConnectOp>(loc, arg(widget.outgoing), toHost.getResult(3));
    connect(arg(countStart + 2 * index), fromHost.getResult(4));
    connect(arg(countStart + 2 * index + 1), toHost.getResult(4));
    for (auto [fifo, direction, name] : {
        std::make_tuple(fromHost, "fromHostCPU", fromName),
        std::make_tuple(toHost, "toHostCPU", toName)}) {
    // Do not assign indices until the shared CPU stream catalog is rebuilt.
    fifo->setAttr("goldengate.streamParameters", b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(name)),
        b.getNamedAttr("direction", b.getStringAttr(direction)),
        b.getNamedAttr("depth", b.getI64IntegerAttr(3072)),
        b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64))}));
    }
    NamedAttrList binding;
    if (priorBindings) binding = NamedAttrList(cast<DictionaryAttr>(priorBindings[index]));
    binding.set("widget", b.getI64IntegerAttr(index));
    if (loopback) {
      binding.set("fromHostStream", b.getStringAttr(externalName(index, "nic_from_host_stream")));
      binding.set("toHostStream", b.getStringAttr(externalName(index, "nic_to_host_stream")));
    }
    binding.set("fromHostQueue", b.getStringAttr(fromHost.getName()));
    binding.set("toHostQueue", b.getStringAttr(toHost.getName()));
    binding.set("fromHostCount", b.getStringAttr(externalName(index, "nic_from_host_stream_count")));
    binding.set("toHostCount", b.getStringAttr(externalName(index, "nic_to_host_stream_count")));
    bindings.push_back(binding.getDictionary(ctx));
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
          if (!consumed.count(i) && name == p.name) {
            suffix.replace(0, mp.size(), "|" + wrapperName.str() + ">"); break;
          }
      }
      return b.getStringAttr(np + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto v : arr) out.push_back(retarget(v)); return b.getArrayAttr(out); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto v : d) out.set(v.getName(), retarget(v.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  wrapper->setAttr("goldengate.nicCPUStreamBindings", retarget(b.getArrayAttr(bindings)));
  if (loopback) wrapper->setAttr("goldengate.nicTokenBindings", retarget(priorBindings));
  for (auto model : circuit.getOps<FModuleOp>())
    for (auto key : {"goldengate.nicTokenBindings", "goldengate.nicPacketBindings",
                     "goldengate.nicCPUStreamBindings"})
      if (auto metadata = model->getAttr(key)) model->setAttr(key, retarget(metadata));
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}

LogicalResult goldengate::addNICCPUStreamQueues(CircuitOp circuit, std::string &error) {
  return addNICCPUStreamQueuesImpl(circuit, error, false);
}
LogicalResult goldengate::addNICLoopbackCPUStreamQueues(CircuitOp circuit, std::string &error) {
  return addNICCPUStreamQueuesImpl(circuit, error, true);
}
