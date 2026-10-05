// See LICENSE for license details.
// CPUManagedStreamEngine.elaborateToHostCPUStream and implementStreams.
// Requires: uninstantiated active top, host clock/reset, ordered buffered
// Decoupled512 outputs and retained annotations. Rejects unsupported boundaries before
// mutation. The 16-bit ID, 64-bit address and equal-width adapter match U250.
// Consumes no annotations; copied targets move to the new wrapper, buffered
// stream targets remain on the inner module. Adds native read transport and
// its wrapper, replacing the external stream with CPU AXI AR/R scalar ports.
// No cached analyses. Count MMIO, writes and driver/header emission are separate
// stages. AR is held until the last R handshake, as in the Scala oracle.
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/CPUStreamRead.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include <limits>
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addCPUStreamRead(CircuitOp circuit, std::string &error) {
  if (circuit.getName() != "GGTracerVStreamQueueWrapper") {
    error = "CPU stream read requires the TracerV queue wrapper";
    return failure();
  }
  // The recorded Rocket adapter retains its count boundary contract. General
  // transport bindings leave count allocation to addCPUStreamCountBank.
  FModuleOp top;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  bool count = false;
  if (top) for (auto p : top.getPorts())
    count |= p.name == "tracerv_stream_count" && p.direction == Direction::Out &&
        p.type == UIntType::get(circuit.getContext(), 13, false);
  if (!count) {
    error = "CPU stream read needs the TracerV count13 output";
    return failure();
  }
  return addCPUStreamRead(circuit,
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream", 6144}}, error);
}

LogicalResult goldengate::addCPUStreamRead(
    CircuitOp circuit, ArrayRef<CPUStreamSourcePort> sources, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGCPUStreamRead";
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamReadWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (sources.empty()) return reject("CPU stream read requires outgoing streams");
  llvm::StringSet<> names, portNames;
  uint64_t maxDepth = 0;
  for (const auto &source : sources) {
    if (source.streamName.empty() || source.streamName.find('\0') != std::string::npos ||
        source.portName.empty() || source.portName.find('\0') != std::string::npos ||
        !names.insert(source.streamName).second || !portNames.insert(source.portName).second)
      return reject("CPU stream read requires unique nonempty stream and port identities");
    if (!source.depth || source.depth > uint64_t(std::numeric_limits<int32_t>::max()) / 64)
      return reject("CPU stream queue depth exceeds supported Scala address allocation");
    maxDepth = std::max(maxDepth, uint64_t(source.depth));
  }
  unsigned addressSpaceBits = llvm::Log2_64_Ceil(64 * maxDepth);
  uint64_t windowBytes = uint64_t(1) << addressSpaceBits;
  if (uint64_t(sources.size() - 1) > uint64_t(std::numeric_limits<int32_t>::max()) / windowBytes)
    return reject("CPU stream base addresses exceed supported Scala driver allocation");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op); if (!m) continue;
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("CPU stream read helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("CPU stream read needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context);
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(512)}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(context), Direction::In);
  auto reset = port("hostReset", bit, Direction::In);
  if (!clock || !reset)
    return reject("CPU stream read needs host clock/reset");
  SmallVector<unsigned> streamPorts;
  for (const auto &source : sources) {
    auto stream = port(source.portName, token, Direction::Out);
    if (!stream) return reject("CPU stream read needs every declared Decoupled512 output");
    streamPorts.push_back(*stream);
  }
  for (auto p : inner.getPorts()) if (p.name.getValue().starts_with("cpu_stream_"))
    return reject("CPU stream read boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("CPU stream read needs an uninstantiated top");

  Location loc = circuit.getLoc();
  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  for (unsigned i = 0; i < sources.size(); ++i)
    helperPorts.push_back({b.getStringAttr(sources.size() == 1 ? "stream" : "stream_" + std::to_string(i)), token, Direction::In});
  unsigned firstHelperAXI = helperPorts.size();
  auto append = [&](llvm::StringRef n, unsigned w, Direction d) {
    helperPorts.push_back({b.getStringAttr(n), uint(w), d});
  };
  append("ar_ready", 1, Direction::Out); append("ar_valid", 1, Direction::In);
  append("ar_bits_id", 16, Direction::In); append("ar_bits_addr", 64, Direction::In);
  append("ar_bits_len", 8, Direction::In); append("ar_bits_size", 3, Direction::In);
  append("r_ready", 1, Direction::In); append("r_valid", 1, Direction::Out);
  append("r_bits_id", 16, Direction::Out); append("r_bits_data", 512, Direction::Out);
  append("r_bits_last", 1, Direction::Out); append("r_bits_resp", 2, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(context, Convention::Internal), helperPorts);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
  // AXI ports follow the ordered source ports. Equal-width StreamWidthAdapter
  // forwards each buffered payload directly. Every counter observes the shared
  // R.fire, including counters for unselected streams (Scala's exact behavior).
  auto axi = [&](unsigned i) { return arg(firstHelperAXI + i); };
  SmallVector<Value> counters, grants, lasts, valids;
  for (unsigned i = 0; i < sources.size(); ++i) {
    counters.push_back(b.create<RegResetOp>(loc, uint(9), arg(0), arg(1), constant(9, 0),
        sources.size() == 1 ? "readBeatCounter" : "readBeatCounter_" + std::to_string(i)).getResult());
    grants.push_back(b.create<EQPrimOp>(loc, b.create<BitsPrimOp>(loc, axi(3), 63, addressSpaceBits), constant(64 - addressSpaceBits, i)));
    lasts.push_back(b.create<EQPrimOp>(loc, counters.back(), b.create<PadPrimOp>(loc, axi(4), 9)));
    valids.push_back(both(grants.back(), both(axi(1), field(arg(2 + i), "valid"))));
  }
  Value valid = valids.front();
  for (Value v : ArrayRef<Value>(valids).drop_front()) valid = b.create<OrPrimOp>(loc, valid, v);
  Value fire = both(valid, axi(6));
  Value arReady;
  Value data = sources.size() == 1 ? field(arg(2), "bits") : constant(512, 0);
  Value last = sources.size() == 1 ? lasts.front() : constant(1, 0);
  for (unsigned i = 0; i < sources.size(); ++i) {
    Value beats = counters[i];
    Value next = mux(lasts[i], constant(9, 0), b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, beats, constant(9, 1)), 8, 0));
    connect(beats, mux(fire, next, beats));
    // DecoupledHelper excludes its own handshake predicate: AR ready is not
    // gated by AR valid, nor queue ready by queue valid.
    Value ready = both(grants[i], both(axi(6), both(field(arg(2 + i), "valid"), lasts[i])));
    arReady = arReady ? Value(b.create<OrPrimOp>(loc, arReady, ready)) : ready;
    connect(field(arg(2 + i), "ready"), both(grants[i], both(axi(1), axi(6))));
    if (sources.size() != 1) {
      data = mux(grants[i], field(arg(2 + i), "bits"), data);
      last = mux(grants[i], lasts[i], last);
    }
  }
  connect(axi(0), arReady); connect(axi(7), valid); connect(axi(8), axi(2));
  connect(axi(9), data); connect(axi(10), last); connect(axi(11), constant(2, 0));
  Value permitted = b.create<OrPrimOp>(loc, b.create<NotPrimOp>(loc, axi(1)), b.create<EQPrimOp>(loc, axi(5), constant(3, 6)));
  b.create<AssertOp>(loc, arg(0), permitted, b.create<NotPrimOp>(loc, arg(1)),
      "CPUManagedStreamEngine requires 64-byte read beats", ValueRange{}, "");
  helper->setAttr("goldengate.streamAddressSpaceBits", b.getI64IntegerAttr(addressSpaceBits));
  SmallVector<Attribute> allocations;
  for (auto [i, source] : llvm::enumerate(sources))
    allocations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(source.streamName)),
        b.getNamedAttr("port", b.getStringAttr(source.portName)),
        b.getNamedAttr("index", b.getI64IntegerAttr(i)),
        b.getNamedAttr("depth", b.getI64IntegerAttr(source.depth)),
        b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64)),
        b.getNamedAttr("bufferBaseAddress", b.getI64IntegerAttr(i * windowBytes))}));
  helper->setAttr("goldengate.sourceStreams", b.getArrayAttr(allocations));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!llvm::is_contained(streamPorts, i)) { copied.push_back(i); ports.push_back(p); }
  unsigned firstAXI = ports.size();
  for (auto p : ArrayRef<PortInfo>(helperPorts).drop_front(firstHelperAXI)) {
    p.name = b.getStringAttr("cpu_stream_" + p.name.getValue().str()); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto transport = b.create<InstanceOp>(loc, helper, "cpuStreamRead");
  auto externalFor = [&](unsigned innerIndex) -> Value {
    return wrapper.getBodyBlock()->getArgument(llvm::find(copied, innerIndex) - copied.begin());
  };
  for (auto [i, old] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[old]; Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(old) : external,
        p.direction == Direction::In ? external : sim.getResult(old));
  }
  connect(transport.getResult(0), externalFor(*clock)); connect(transport.getResult(1), externalFor(*reset));
  for (auto [i, innerPort] : llvm::enumerate(streamPorts))
    b.create<ConnectOp>(loc, transport.getResult(2 + i), sim.getResult(innerPort));
  for (unsigned i = firstHelperAXI; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(firstAXI + i - firstHelperAXI);
    if (helperPorts[i].direction == Direction::In) connect(transport.getResult(i), external);
    else connect(external, transport.getResult(i));
  }
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
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
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
