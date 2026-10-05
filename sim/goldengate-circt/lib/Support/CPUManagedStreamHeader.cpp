// See LICENSE for license details.
// Oracle: CPUManagedStreamEngine.genHeader and Widget.genConstructor.
// Required input: unique live outgoing-only CPU transport, stream queue/count
// bank and allocated read/write control bindings. Outgoing order is the native
// transport allocation order; incoming streams remain unsupported.
// Annotations read: sole .const.h OutputFile; none consumed. Produces one
// managed-stream constructor and a completion marker in that annotation only.
// Analyses required: InstanceGraph plus SSA/instance forwarding of count/data.
// Hardware, hierarchy, clocks and all other annotations are preserved. Driver
// descriptors must match typed queue storage, address grant and count read.
// All rejection paths precede mutation; duplicate construction is an error.
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <cstdint>

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareCPUManagedStreamHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("CPUManagedStreamEngine header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed CPUManagedStreamEngine header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.cpuManagedStreamHeader"))
    return reject("CPUManagedStreamEngine needs one driver header without a prior CPU stream constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid CPUManagedStreamEngine header top");
  auto countPaths = [&](circt::igraph::InstanceGraphNode *target, unsigned limit) {
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
    llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
    bool recursive = false;
    std::function<unsigned(circt::igraph::InstanceGraphNode *)> count = [&](auto *node) {
      if (active.count(node)) { recursive = true; return 0U; }
      auto found = counts.find(node);
      if (found != counts.end()) return found->second;
      active.insert(node);
      unsigned n = node == target;
      for (auto *record : *node) n = std::min(limit, n + count(record->getTarget()));
      active.erase(node); counts[node] = n; return n;
    };
    unsigned n = count(top);
    return recursive ? limit : n;
  };
  auto liveModule = [&](StringRef name) -> FModuleOp {
    auto *target = graph.lookup(StringAttr::get(circuit.getContext(), name));
    if (!target || countPaths(target, 2) != 1) return {};
    return dyn_cast<FModuleOp>(target->getModule().getOperation());
  };
  auto bank = liveModule("GGCPUStreamCountBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("CPUManagedStreamEngine bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "cpuStream_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("CPUManagedStreamEngine read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing CPUManagedStreamEngine control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("CPUManagedStreamEngine_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 4)
    return reject("invalid CPUManagedStreamEngine widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("CPUManagedStreamEngine MMIO region exceeds control address width");
  for (auto [i, attr] : llvm::enumerate(regions)) {
    if (i == uint64_t(slave.getInt())) continue;
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto otherName = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto otherStart = d ? d.getAs<IntegerAttr>("start") : IntegerAttr();
    auto otherSize = d ? d.getAs<IntegerAttr>("size") : IntegerAttr();
    if (!otherName || otherName == name || !otherStart || otherStart.getInt() < 0 ||
        !otherSize || otherSize.getInt() <= 0 ||
        uint64_t(otherStart.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
        uint64_t(otherSize.getInt()) > (uint64_t(1) << *address.getWidth()) - otherStart.getInt() ||
        (uint64_t(otherStart.getInt()) < uint64_t(start.getInt()) + size.getInt() &&
         uint64_t(start.getInt()) < uint64_t(otherStart.getInt()) + otherSize.getInt()))
      return reject("ambiguous CPUManagedStreamEngine MMIO allocation");
  }
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  auto transport = liveModule("GGCPUStreamRead");
  auto incoming = liveModule("GGEmptyCPUStreamWrite");
  auto sources = transport ? transport->getAttrOfType<ArrayAttr>("goldengate.sourceStreams") : ArrayAttr();
  auto space = transport ? transport->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits") : IntegerAttr();
  auto sinks = incoming ? incoming->getAttrOfType<IntegerAttr>("goldengate.fromHostCPUStreamCount") : IntegerAttr();
  if (!transport || !incoming || !sources || sources.empty() || sources.size() > INT32_MAX / 4 ||
      !space || space.getInt() < 6 || space.getInt() > 30 || !sinks || sinks.getInt() != 0)
    return reject("CPU stream descriptors need ordered outgoing allocations and no incoming streams");
  unsigned n = sources.size(), mcrPort = n + 2, addressPort = n + 5;
  if (transport.getNumPorts() != n + 14 || bank.getNumPorts() != n + 3 ||
      transport.getPortName(addressPort) != "ar_bits_addr" ||
      transport.getPortType(addressPort) != uint(64) || transport.getPortDirection(addressPort) != Direction::In ||
      transport.getPortType(n + 11) != uint(512) || bank.getPortName(mcrPort) != "mcr" ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1) ||
      uint64_t(size.getInt()) < 4ULL * n)
    return reject("CPU stream transport/count ports or MMIO region differ from allocation");
  for (unsigned i = 0; i < n + 3; ++i)
    if (bank.getPortDirection(i) != (i == mcrPort ? Direction::Out : Direction::In))
      return reject("CPU stream count bank directions differ");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(mcrPort), group));
    if (!slots || slots.getNumElements() != n || fieldType(slots.getElementType(), "bits") != uint(32))
      return reject("CPU stream counts require one 32-bit MCR slot per allocation");
  }
  auto arg = [&](FModuleOp m, unsigned i) { return m.getBodyBlock()->getArgument(i); };
  // Carry the full instance path while following aggregate connects. Shared
  // queue definitions must not collapse two independent storage instances.
  using Path = SmallVector<InstanceOp>;
  auto uniquePath = [&](FModuleOp module) {
    Path result;
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, bool> contains;
    std::function<bool(circt::igraph::InstanceGraphNode *)> hasTarget = [&](auto *node) {
      auto found = contains.find(node);
      if (found != contains.end()) return found->second;
      bool yes = node->getModule().getOperation() == module.getOperation();
      for (auto *record : *node) yes |= hasTarget(record->getTarget());
      contains[node] = yes; return yes;
    };
    std::function<bool(circt::igraph::InstanceGraphNode *)> find = [&](auto *node) {
      if (node->getModule().getOperation() == module.getOperation()) return true;
      for (auto *record : *node) {
        if (!hasTarget(record->getTarget())) continue;
        // Only descend into branches containing the requested definition.
        auto inst = dyn_cast<InstanceOp>(record->getInstance().getOperation());
        if (!inst) continue;
        result.push_back(inst);
        if (find(record->getTarget())) return true;
        result.pop_back();
      }
      return false;
    };
    find(top); return result;
  };
  struct Endpoint { FModuleOp queue; Path path; unsigned port = 0; bool namedPort = false; };
  auto trace = [&](Value value, Path path, StringRef sourcePort = {}) -> Endpoint {
    bool namedPort = sourcePort.empty();
    SmallVector<std::pair<Value, Path>> seen;
    while (value && seen.size() < 4096) {
      for (auto &old : seen) if (old.first == value && old.second == path) return {};
      seen.emplace_back(value, path);
      auto *block = value.getParentBlock(); Value driver; unsigned drivers = 0;
      for (auto &op : *block) {
        if (auto c = dyn_cast<ConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
        if (auto c = dyn_cast<StrictConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
      }
      if (drivers) { if (drivers != 1) return {}; value = driver; continue; }
      if (auto a = dyn_cast<BlockArgument>(value)) {
        auto module = dyn_cast<FModuleOp>(a.getOwner()->getParentOp());
        if (!module || module.getPortDirection(a.getArgNumber()) != Direction::In || path.empty()) return {};
        auto inst = path.pop_back_val();
        if (inst.getModuleName() != module.getName()) return {};
        value = inst.getResult(a.getArgNumber()); continue;
      }
      auto inst = value.getDefiningOp<InstanceOp>();
      auto *node = inst ? graph.lookup(inst.getModuleNameAttr().getAttr()) : nullptr;
      auto module = node ? dyn_cast<FModuleOp>(node->getModule().getOperation()) : FModuleOp();
      if (!module) return {};
      unsigned port = cast<OpResult>(value).getResultNumber();
      if (module.getPortDirection(port) != Direction::Out) return {};
      namedPort |= module.getPortName(port) == sourcePort;
      path.push_back(inst);
      if (module.getNumPorts() == 5 && !module.getOps<MemOp>().empty()) return {module, path, port, namedPort};
      value = arg(module, port);
    }
    return {};
  };
  auto bankPath = uniquePath(bank), transportPath = uniquePath(transport);
  auto isSlot = [&](Value v, StringRef groupName, StringRef member, unsigned index) {
    auto f = v ? v.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto s = f ? f.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto g = s ? s.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return f && f.getFieldName() == member && s && s.getIndex() == index &&
        g && g.getFieldName() == groupName && g.getInput() == arg(bank, mcrPort);
  };
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != n) return reject("CPU stream count allocation size differs");
  llvm::StringSet<> names, ports;
  SmallVector<Endpoint> queues;
  SmallVector<DictionaryAttr> allocations;
  uint64_t maxDepth = 0, windowBytes = uint64_t(1) << space.getInt();
  for (auto [i, attr] : llvm::enumerate(sources)) {
    auto source = dyn_cast<DictionaryAttr>(attr);
    auto name = source ? source.getAs<StringAttr>("name") : StringAttr();
    auto port = source ? source.getAs<StringAttr>("port") : StringAttr();
    auto index = source ? source.getAs<IntegerAttr>("index") : IntegerAttr();
    auto depth = source ? source.getAs<IntegerAttr>("depth") : IntegerAttr();
    auto bytes = source ? source.getAs<IntegerAttr>("widthBytes") : IntegerAttr();
    auto base = source ? source.getAs<IntegerAttr>("bufferBaseAddress") : IntegerAttr();
    if (!name || name.getValue().empty() || name.getValue().contains('\0') || !names.insert(name.getValue()).second ||
        !port || port.getValue().empty() || port.getValue().contains('\0') || !ports.insert(port.getValue()).second ||
        !index || index.getInt() != int64_t(i) || !depth || depth.getInt() <= 0 || depth.getInt() > INT32_MAX / 64 ||
        !bytes || bytes.getInt() != 64 || !base || base.getInt() != int64_t(i * windowBytes) || base.getInt() > INT32_MAX ||
        transport.getPortName(i + 2) != (n == 1 ? "stream" : "stream_" + std::to_string(i)) ||
        transport.getPortDirection(i + 2) != Direction::In || fieldType(transport.getPortType(i + 2), "bits") != uint(512))
      return reject("CPU stream identity, order, capacity or DMA base differs from native allocation");
    maxDepth = std::max(maxDepth, uint64_t(depth.getInt()));
    auto data = trace(arg(transport, i + 2), transportPath, port.getValue());
    auto count = trace(arg(bank, i + 2), bankPath);
    if (!data.queue || !count.queue || !data.namedPort || data.port != 3 || count.port != 4 || data.path != count.path)
      return reject("CPU stream count and read transport must forward the same live queue instance");
    for (auto &old : queues) if (old.path == data.path)
      return reject("CPU outgoing allocations cannot share one queue instance");
    auto queue = data.queue;
    unsigned countBits = llvm::Log2_64_Ceil(uint64_t(depth.getInt()) + 1);
    if (queue.getPortType(0) != ClockType::get(circuit.getContext()) || queue.getPortType(1) != uint(1) ||
        fieldType(queue.getPortType(2), "bits") != uint(512) || fieldType(queue.getPortType(3), "bits") != uint(512) ||
        queue.getPortType(4) != uint(countBits) || bank.getPortType(i + 2) != uint(countBits) ||
        bank.getPortName(i + 2) != (n == 1 ? "count" : "count_" + std::to_string(i)))
      return reject("CPU stream descriptor width differs from native queue/count ports");
    for (unsigned p = 0; p < 5; ++p)
      if (queue.getPortDirection(p) != (p < 3 ? Direction::In : Direction::Out))
        return reject("CPU queue directions differ");
    auto legacy = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
    if (queue->hasAttr("goldengate.streamParameters") && !legacy)
      return reject("malformed CPU queue identity");
    if (legacy) {
      if (legacy.getAs<StringAttr>("name") != name)
        return reject("CPU queue identity differs from transport allocation");
      for (StringRef key : {"index", "depth", "widthBytes"}) {
        auto actual = legacy.getAs<IntegerAttr>(key);
        if (!actual || actual.getInt() != source.getAs<IntegerAttr>(key).getInt())
          return reject("CPU queue identity differs from transport allocation");
      }
    }
    unsigned memories = 0;
    for (auto mem : queue.getOps<MemOp>()) {
      if (mem.getDepth() != uint64_t(depth.getInt()) || mem.getDataType() != uint(512) ||
          mem.getReadLatency() != 0 || mem.getWriteLatency() != 1)
        return reject("CPU stream descriptor capacity differs from native queue storage");
      ++memories;
    }
    if (memories != 1) return reject("CPU stream needs one native queue memory");
    unsigned grants = 0; Value grant;
    for (auto eq : transport.getOps<EQPrimOp>()) {
      auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
      if (!bits || bits.getInput() != arg(transport, addressPort)) continue;
      auto selector = eq.getRhs().getDefiningOp<ConstantOp>();
      if (bits.getHi() != 63 || bits.getLo() != space.getInt() || !selector || selector.getValue().getLimitedValue() >= n)
        return reject("CPU stream DMA address grant differs from ordered windows");
      if (selector.getValue().getLimitedValue() == i) { ++grants; grant = eq.getResult(); }
    }
    if (grants != 1) return reject("CPU stream needs one DMA address grant per allocation");
    // Check that this selector actually controls this queue's dequeue handshake.
    // An unused equality or swapped selector cannot authorize its descriptor.
    unsigned readyDrivers = 0;
    for (auto c : transport.getOps<StrictConnectOp>()) {
      auto field = c.getDest().getDefiningOp<SubfieldOp>();
      if (!field || field.getFieldName() != "ready" || field.getInput() != arg(transport, i + 2)) continue;
      auto outer = c.getSrc().getDefiningOp<AndPrimOp>();
      auto inner = outer ? outer.getRhs().getDefiningOp<AndPrimOp>() : AndPrimOp();
      if (!outer || outer.getLhs() != grant || !inner || inner.getLhs() != arg(transport, n + 3) ||
          inner.getRhs() != arg(transport, n + 8))
        return reject("CPU stream dequeue handshake differs from its DMA selector");
      ++readyDrivers;
    }
    if (readyDrivers != 1) return reject("CPU stream dequeue ready has missing or ambiguous drivers");
    unsigned payloads = 0;
    auto isPayload = [&](Value value) {
      auto field = value.getDefiningOp<SubfieldOp>();
      return field && field.getFieldName() == "bits" && field.getInput() == arg(transport, i + 2);
    };
    for (auto c : transport.getOps<StrictConnectOp>()) if (c.getDest() == arg(transport, n + 11)) {
      if (n == 1) { if (isPayload(c.getSrc())) ++payloads; }
      else {
        Value value = c.getSrc();
        while (auto mux = value.getDefiningOp<MuxPrimOp>()) {
          if (mux.getSel() == grant && isPayload(mux.getHigh())) ++payloads;
          value = mux.getLow();
        }
      }
    }
    if (payloads != 1) return reject("CPU stream read payload differs from its DMA selector");
    auto word = dyn_cast<DictionaryAttr>(registers[i]);
    auto offset = word ? word.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = word ? word.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = word ? word.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!word || word.getAs<StringAttr>("name") != name.getValue().str() + "_count" || !offset || offset.getInt() != int64_t(4 * i) ||
        !readable || !readable.getValue() || !writeable || writeable.getValue())
      return reject("CPU stream driver count word differs from ordered read-only MMIO allocation");
    unsigned readsCount = 0, valids = 0, readies = 0, assertions = 0;
    for (auto connect : bank.getOps<StrictConnectOp>()) {
      if (isSlot(connect.getDest(), "read", "bits", i)) {
        auto pad = connect.getSrc().getDefiningOp<PadPrimOp>();
        if (!pad || pad.getInput() != arg(bank, i + 2) || pad.getAmount() != 32)
          return reject("CPU stream count read does not expose the allocated live queue count");
        ++readsCount;
      }
      if (isSlot(connect.getDest(), "read", "valid", i) || isSlot(connect.getDest(), "write", "ready", i)) {
        auto one = connect.getSrc().getDefiningOp<ConstantOp>();
        if (!one || !one.getValue().isOne()) return reject("CPU stream count MCR slot must always be available");
        if (isSlot(connect.getDest(), "read", "valid", i)) ++valids; else ++readies;
      }
    }
    for (auto a : bank.getOps<AssertOp>()) {
      auto pred = a.getPredicate().getDefiningOp<NotPrimOp>();
      auto enable = a.getEnable().getDefiningOp<NotPrimOp>();
      if (!pred || !isSlot(pred.getInput(), "write", "valid", i)) continue;
      if (a.getClock() != arg(bank, 0) || !enable || enable.getInput() != arg(bank, 1))
        return reject("CPU stream count must reject writes outside host reset");
      ++assertions;
    }
    if (readsCount != 1 || valids != 1 || readies != 1 || assertions != 1)
      return reject("CPU stream count has missing or ambiguous MMIO drivers");
    queues.push_back(data); allocations.push_back(source);
  }
  if (llvm::Log2_64_Ceil(64 * maxDepth) != unsigned(space.getInt()))
    return reject("CPU stream DMA window differs from maximum outgoing capacity");
  for (auto &endpoint : queues) {
    unsigned allocated = llvm::count_if(queues, [&](const auto &q) { return q.queue == endpoint.queue; });
    auto *node = graph.lookup(endpoint.queue.getNameAttr());
    if (countPaths(node, allocated + 1) != allocated)
      return reject("CPU queue has live instances outside the outgoing allocation");
  }
  // Empty incoming vectors are meaningful ABI state; check the actual transport.
  for (StringRef port : {"aw_ready", "w_ready", "b_valid"}) {
    std::optional<unsigned> n;
    for (auto [i, p] : llvm::enumerate(incoming.getPorts())) if (p.name == port) n = i;
    if (!n || incoming.getPortType(*n) != uint(1) || incoming.getPortDirection(*n) != Direction::Out)
      return reject("CPU incoming stream boundary differs");
    unsigned matches = 0;
    for (auto c : incoming.getOps<StrictConnectOp>()) if (c.getDest() == arg(incoming, *n)) {
      auto zero = c.getSrc().getDefiningOp<ConstantOp>();
      if (!zero || !zero.getValue().isZero()) return reject("CPU incoming stream vector is not empty");
      ++matches;
    }
    if (matches != 1) return reject("CPU incoming stream boundary has ambiguous drivers");
  }
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/cpu_managed_stream.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_MANAGED_STREAM_CONSTRUCTOR\nregistry.add_widget(new CPUManagedStreamWidget(\n"
         "  simif,\n  " << widgetIndex << ",\n  args,\n"
         "  std::vector<CPUManagedStreams::StreamParameters>{},\n"
         "  std::vector<CPUManagedStreams::StreamParameters>{\n";
  for (auto [i, source] : llvm::enumerate(allocations)) {
    out << "    CPUManagedStreams::StreamParameters(std::string(\"";
    for (unsigned char ch : source.getAs<StringAttr>("name").getValue()) {
      if (ch == '"' || ch == '\\') out << '\\' << char(ch);
      else if (ch >= 32 && ch < 127) out << char(ch);
      else out << '\\' << char('0' + ((ch >> 6) & 7)) << char('0' + ((ch >> 3) & 7)) << char('0' + (ch & 7));
    }
    out << "\"), " << source.getAs<IntegerAttr>("bufferBaseAddress").getInt() << "ULL, "
        << start.getInt() + 4 * i << "ULL, " << source.getAs<IntegerAttr>("depth").getInt()
        << "U, " << source.getAs<IntegerAttr>("widthBytes").getInt() << "U)" << (i + 1 == n ? "\n" : ",\n");
  }
  out << "  }\n));\n#endif // GET_MANAGED_STREAM_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.cpuManagedStreamHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end()); annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return success();
}
