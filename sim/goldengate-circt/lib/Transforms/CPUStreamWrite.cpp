// See LICENSE for license details.
// Required input invariants: uninstantiated active top; hostClock/hostReset;
// ordered buffered Decoupled512 inputs and retained rawAnnotations.
// Annotations consumed: none; existing targets are retargeted recursively.
// Annotations produced: stream index/depth/window allocation attributes.
// IR mutations: adds FIRRTL AW/W/B transport and a top wrapper, consumes sink
// ports, preserves other ports and updates the circuit identity.
// Analyses required: caller supplies the validated ordered CPU sink catalog.
// Analyses preserved: inner module bodies and sink target module identities.
// Output invariants: 512-bit live W data; grant-qualified nine-bit counters;
// AW/B completion on the final beat; independent incoming address space.
// Queue storage, AXI buffering, controls and driver emission are separate passes.
#include "goldengate/CPUStreamWrite.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <limits>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addCPUStreamWrite(
    CircuitOp circuit, ArrayRef<CPUStreamSourcePort> sinks, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGCPUStreamWrite";
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamWriteWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (sinks.empty()) return reject("CPU stream write requires incoming streams");
  llvm::StringSet<> names, portNames;
  uint64_t maxDepth = 0;
  for (const auto &sink : sinks) {
    if (sink.streamName.empty() || sink.streamName.find('\0') != std::string::npos ||
        sink.portName.empty() || sink.portName.find('\0') != std::string::npos ||
        !names.insert(sink.streamName).second || !portNames.insert(sink.portName).second)
      return reject("CPU stream write requires unique nonempty stream and port identities");
    if (!sink.depth || sink.depth > uint64_t(std::numeric_limits<int32_t>::max()) / 64)
      return reject("CPU stream queue depth exceeds supported Scala address allocation");
    maxDepth = std::max(maxDepth, uint64_t(sink.depth));
  }
  unsigned addressSpaceBits = llvm::Log2_64_Ceil(64 * maxDepth);
  uint64_t windowBytes = uint64_t(1) << addressSpaceBits;
  if (uint64_t(sinks.size() - 1) > uint64_t(std::numeric_limits<int32_t>::max()) / windowBytes)
    return reject("CPU stream base addresses exceed supported Scala driver allocation");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op); if (!m) continue;
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("CPU stream write helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("CPU stream write needs a top and retained annotations");
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
    return reject("CPU stream write needs host clock/reset");
  SmallVector<unsigned> streamPorts;
  for (const auto &sink : sinks) {
    auto stream = port(sink.portName, token, Direction::In);
    if (!stream) return reject("CPU stream write needs every declared Decoupled512 input");
    streamPorts.push_back(*stream);
  }
  for (auto p : inner.getPorts()) if (p.name.getValue().starts_with("cpu_stream_aw_") ||
      p.name.getValue().starts_with("cpu_stream_w_") ||
      p.name.getValue().starts_with("cpu_stream_b_"))
    return reject("CPU stream write boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("CPU stream write needs an uninstantiated top");

  Location loc = circuit.getLoc();
  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  for (unsigned i = 0; i < sinks.size(); ++i)
    helperPorts.push_back({b.getStringAttr(sinks.size() == 1 ? "stream" : "stream_" + std::to_string(i)), token, Direction::Out});
  unsigned firstHelperAXI = helperPorts.size();
  auto append = [&](llvm::StringRef n, unsigned w, Direction d) {
    helperPorts.push_back({b.getStringAttr(n), uint(w), d});
  };
  append("aw_ready", 1, Direction::Out); append("aw_valid", 1, Direction::In);
  append("aw_bits_id", 16, Direction::In); append("aw_bits_addr", 64, Direction::In);
  append("aw_bits_len", 8, Direction::In); append("aw_bits_size", 3, Direction::In);
  append("w_ready", 1, Direction::Out); append("w_valid", 1, Direction::In);
  append("w_bits_data", 512, Direction::In); append("w_bits_strb", 64, Direction::In);
  append("b_ready", 1, Direction::In); append("b_valid", 1, Direction::Out);
  append("b_bits_id", 16, Direction::Out); append("b_bits_resp", 2, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(context, Convention::Internal), helperPorts);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
  // DecoupledHelper excludes each destination's own handshake predicate.
  // AW remains live until the last beat; W.last is unused by this oracle.
  // Unlike reads, only the selected sink's W.fire advances its counter.
  auto axi = [&](unsigned i) { return arg(firstHelperAXI + i); };
  Value awReady = constant(1, 0), wReady = constant(1, 0), bValid = constant(1, 0);
  for (unsigned i = 0; i < sinks.size(); ++i) {
    Value beats = b.create<RegResetOp>(loc, uint(9), arg(0), arg(1), constant(9, 0),
        sinks.size() == 1 ? "writeBeatCounter" : "writeBeatCounter_" + std::to_string(i)).getResult();
    Value grant = b.create<EQPrimOp>(loc, b.create<BitsPrimOp>(loc, axi(3), 63, addressSpaceBits),
        constant(64 - addressSpaceBits, i));
    Value last = b.create<EQPrimOp>(loc, beats, b.create<PadPrimOp>(loc, axi(4), 9));
    Value ready = field(arg(2 + i), "ready");
    Value selectedWReady = both(grant, both(axi(1), both(axi(10), ready)));
    Value selectedAWReady = both(grant, both(axi(7), both(axi(10), both(ready, last))));
    Value selectedBValid = both(grant, both(axi(1), both(axi(7), both(ready, last))));
    wReady = b.create<OrPrimOp>(loc, wReady, selectedWReady);
    awReady = b.create<OrPrimOp>(loc, awReady, selectedAWReady);
    bValid = b.create<OrPrimOp>(loc, bValid, selectedBValid);
    connect(field(arg(2 + i), "valid"), both(grant, both(axi(1), both(axi(7), axi(10)))));
    connect(field(arg(2 + i), "bits"), axi(8));
    Value next = mux(last, constant(9, 0), b.create<BitsPrimOp>(loc,
        b.create<AddPrimOp>(loc, beats, constant(9, 1)), 8, 0));
    connect(beats, mux(both(selectedWReady, axi(7)), next, beats));
  }
  connect(axi(0), awReady); connect(axi(6), wReady); connect(axi(11), bValid);
  connect(axi(12), axi(2)); connect(axi(13), constant(2, 0));
  auto assertValid = [&](unsigned valid, unsigned bits, Value expected, StringRef message) {
    Value permitted = b.create<OrPrimOp>(loc, b.create<NotPrimOp>(loc, axi(valid)),
        b.create<EQPrimOp>(loc, axi(bits), expected));
    b.create<AssertOp>(loc, arg(0), permitted, b.create<NotPrimOp>(loc, arg(1)),
        message, ValueRange{}, "");
  };
  assertValid(1, 5, constant(3, 6), "CPUManagedStreamEngine requires 64-byte write beats");
  assertValid(7, 9, constant(64, ~uint64_t(0)), "CPUManagedStreamEngine requires all write strobes");
  helper->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(sinks.size()));
  helper->setAttr("goldengate.streamAddressSpaceBits", b.getI64IntegerAttr(addressSpaceBits));
  SmallVector<Attribute> allocations;
  for (auto [i, sink] : llvm::enumerate(sinks))
    allocations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(sink.streamName)),
        b.getNamedAttr("port", b.getStringAttr(sink.portName)),
        b.getNamedAttr("index", b.getI64IntegerAttr(i)),
        b.getNamedAttr("depth", b.getI64IntegerAttr(sink.depth)),
        b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64)),
        b.getNamedAttr("bufferBaseAddress", b.getI64IntegerAttr(i * windowBytes))}));
  helper->setAttr("goldengate.sinkStreams", b.getArrayAttr(allocations));

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
  auto transport = b.create<InstanceOp>(loc, helper, "cpuStreamWrite");
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
    b.create<ConnectOp>(loc, sim.getResult(innerPort), transport.getResult(2 + i));
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
