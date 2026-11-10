// See LICENSE for license details.
// Required input invariants: uninstantiated active top; hostClock/hostReset;
// ordered buffered Decoupled512 inputs and retained rawAnnotations.
// Annotations consumed: none; existing targets are retargeted recursively.
// Annotations produced: stream index/depth/window allocation attributes.
// IR mutations: adds FIRRTL AW/W/B transport and a top wrapper, consumes sink
// ports, preserves other ports and updates the circuit identity.
// Analyses required: caller supplies the validated ordered CPU sink catalog.
// Analyses preserved: inner module bodies and sink target module identities.
// Output invariants: 512-bit byte-assembled data; grant-qualified nine-bit counters;
// AW/B completion on the final physical beat; independent incoming address space.
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
  Value responseError = constant(1, 0);
  auto bits = [&](Value v, unsigned hi, unsigned lo) -> Value {
    return b.create<BitsPrimOp>(loc, v, hi, lo);
  };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  for (unsigned i = 0; i < sinks.size(); ++i) {
    auto stateName = [&](StringRef name) {
      return sinks.size() == 1 ? name.str() : name.str() + "_" + std::to_string(i);
    };
    auto resetReg = [&](unsigned width, StringRef name) -> Value {
      return b.create<RegResetOp>(loc, uint(width), arg(0), arg(1),
          constant(width, 0), stateName(name)).getResult();
    };
    Value beats = resetReg(9, "writeBeatCounter");
    Value mask = resetReg(64, "partialWriteMask");
    // Payload and its line tag are irrelevant until the reset-defined mask
    // is nonzero. Do not initialize storage merely to define invalid bytes.
    Value buffered = b.create<RegOp>(loc, uint(512), arg(0), stateName("partialWriteData")).getResult();
    Value line = b.create<RegOp>(loc, uint(58), arg(0), stateName("partialWriteLine")).getResult();
    Value burstError = resetReg(1, "writeBurstError");
    Value grant = b.create<EQPrimOp>(loc, bits(axi(3), 63, addressSpaceBits),
        constant(64 - addressSpaceBits, i));
    Value last = b.create<EQPrimOp>(loc, beats, b.create<PadPrimOp>(loc, axi(4), 9));
    // The shell retains byte lanes in WSTRB, including partial WC flushes.
    // AW may span multiple physical beats; tag each by its effective line.
    Value offset = b.create<PadPrimOp>(loc, beats, 64);
    for (unsigned size = 1; size <= 6; ++size)
      offset = mux(b.create<EQPrimOp>(loc, axi(5), constant(3, size)),
          b.create<PadPrimOp>(loc, b.create<ShlPrimOp>(loc, beats, size), 64), offset);
    Value effectiveAddress = bits(b.create<AddPrimOp>(loc, axi(3), offset), 63, 0);
    Value currentLine = bits(effectiveAddress, 63, 6);
    Value wrongWindow = neg(b.create<EQPrimOp>(loc, bits(effectiveAddress, 63, addressSpaceBits),
        constant(64 - addressSpaceBits, i)));
    Value pending = neg(b.create<EQPrimOp>(loc, mask, constant(64, 0)));
    Value overlap = neg(b.create<EQPrimOp>(loc, both(mask, axi(9)), constant(64, 0)));
    Value wrongLine = both(pending, neg(b.create<EQPrimOp>(loc, line, currentLine)));
    Value badSize = b.create<EQPrimOp>(loc, axi(5), constant(3, 7));
    Value emptyWrite = b.create<EQPrimOp>(loc, axi(9), constant(64, 0));
    Value malformed = b.create<OrPrimOp>(loc, overlap,
        b.create<OrPrimOp>(loc, wrongWindow, b.create<OrPrimOp>(loc, wrongLine, b.create<OrPrimOp>(loc, badSize, emptyWrite))));
    Value errorNow = b.create<OrPrimOp>(loc, burstError, malformed);
    Value mergedMask = b.create<OrPrimOp>(loc, mask, axi(9));
    Value complete = b.create<EQPrimOp>(loc, mergedMask, constant(64, ~uint64_t(0)));
    Value merged = mux(bits(axi(9), 0, 0), bits(axi(8), 7, 0), bits(buffered, 7, 0));
    for (unsigned lane = 1; lane < 64; ++lane)
      merged = b.create<CatPrimOp>(loc,
          mux(bits(axi(9), lane, lane), bits(axi(8), 8 * lane + 7, 8 * lane),
              bits(buffered, 8 * lane + 7, 8 * lane)), merged);
    Value ready = field(arg(2 + i), "ready");
    // Partial physical writes must receive B even before a full stream word
    // exists. Completed words retain the original FIFO/response backpressure.
    Value canAccept = b.create<OrPrimOp>(loc, errorNow,
        b.create<OrPrimOp>(loc, neg(complete), ready));
    Value selectedWReady = both(grant, both(axi(1), both(axi(10), canAccept)));
    Value selectedAWReady = both(grant, both(axi(7), both(axi(10), both(canAccept, last))));
    Value selectedBValid = both(grant, both(axi(1), both(axi(7), both(canAccept, last))));
    wReady = b.create<OrPrimOp>(loc, wReady, selectedWReady);
    awReady = b.create<OrPrimOp>(loc, awReady, selectedAWReady);
    bValid = b.create<OrPrimOp>(loc, bValid, selectedBValid);
    responseError = b.create<OrPrimOp>(loc, responseError, both(grant, errorNow));
    Value physicalFire = both(selectedWReady, axi(7));
    Value goodFire = both(physicalFire, neg(errorNow));
    connect(field(arg(2 + i), "valid"), both(grant,
        both(axi(1), both(axi(7), both(axi(10), both(complete, neg(errorNow)))))));
    connect(field(arg(2 + i), "bits"), merged);
    Value next = mux(last, constant(9, 0), bits(b.create<AddPrimOp>(loc, beats, constant(9, 1)), 8, 0));
    connect(beats, mux(physicalFire, next, beats));
    connect(mask, mux(goodFire, mux(complete, constant(64, 0), mergedMask), mask));
    connect(buffered, mux(goodFire, merged, buffered));
    connect(line, mux(goodFire, currentLine, line));
    connect(burstError, mux(physicalFire, mux(last, constant(1, 0), errorNow), burstError));
    Value permitted = neg(both(grant, both(axi(1), both(axi(7), malformed))));
    b.create<AssertOp>(loc, arg(0), permitted, neg(arg(1)),
        "CPUManagedStreamEngine rejects overlapping, out-of-order, empty or oversized partial writes", ValueRange{}, "");
  }
  connect(axi(0), awReady); connect(axi(6), wReady); connect(axi(11), bValid);
  connect(axi(12), axi(2)); connect(axi(13), mux(responseError, constant(2, 2), constant(2, 0)));
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
    auto p = inner.getPortDirection(old); Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p == Direction::In ? sim.getResult(old) : external,
        p == Direction::In ? external : sim.getResult(old));
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
