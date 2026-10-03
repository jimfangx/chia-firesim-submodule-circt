// See LICENSE for license details.
// Oracle: CPUManagedStreamEngine.genHeader and Widget.genConstructor.
// Required input: unique live outgoing-only CPU transport, stream queue/count
// bank and allocated read/write control bindings for the recorded Rocket shape.
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
  auto liveModule = [&](StringRef name) -> FModuleOp {
    auto *target = graph.lookup(StringAttr::get(circuit.getContext(), name));
    if (!target) return {};
    // Count instance paths, saturating at two, without expanding shared target
    // hierarchy. A module definition alone cannot authorize a constructor.
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
    llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
    bool recursive = false;
    std::function<unsigned(circt::igraph::InstanceGraphNode *)> count = [&](auto *node) {
      if (active.count(node)) { recursive = true; return 0U; }
      auto found = counts.find(node);
      if (found != counts.end()) return found->second;
      active.insert(node);
      unsigned n = node == target;
      for (auto *record : *node) n = std::min(2U, n + count(record->getTarget()));
      active.erase(node); counts[node] = n; return n;
    };
    if (count(top) != 1 || recursive) return {};
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
  auto queue = liveModule("GGTracerVStreamQueue6144");
  auto transport = liveModule("GGCPUStreamRead");
  auto incoming = liveModule("GGEmptyCPUStreamWrite");
  auto stream = queue ? queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters") : DictionaryAttr();
  auto index = stream ? stream.getAs<IntegerAttr>("index") : IntegerAttr();
  auto depth = stream ? stream.getAs<IntegerAttr>("depth") : IntegerAttr();
  auto bytes = stream ? stream.getAs<IntegerAttr>("widthBytes") : IntegerAttr();
  auto streamName = stream ? stream.getAs<StringAttr>("name") : StringAttr();
  auto space = transport ? transport->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits") : IntegerAttr();
  auto sinks = incoming ? incoming->getAttrOfType<IntegerAttr>("goldengate.fromHostCPUStreamCount") : IntegerAttr();
  // The current transport implements stream zero, with no incoming streams.
  if (!queue || !transport || !incoming || !index || index.getInt() != 0 ||
      !depth || depth.getInt() != 6144 || !bytes || bytes.getInt() != 64 ||
      !streamName || streamName.getValue().empty() || streamName.getValue().contains('\0') ||
      !space || space.getInt() != 19 || !sinks || sinks.getInt() != 0 ||
      queue.getNumPorts() != 5 || queue.getPortType(4) != uint(13) ||
      fieldType(queue.getPortType(2), "bits") != uint(512) ||
      fieldType(queue.getPortType(3), "bits") != uint(512) ||
      transport.getNumPorts() != 15 || fieldType(transport.getPortType(2), "bits") != uint(512) ||
      transport.getPortName(6) != "ar_bits_addr" || transport.getPortType(6) != uint(64) ||
      transport.getPortDirection(6) != Direction::In || transport.getPortType(12) != uint(512))
    return reject("CPU stream descriptor needs one 6144-beat 512-bit outgoing stream and no incoming streams");
  unsigned grants = 0, memories = 0;
  for (auto eq : transport.getOps<EQPrimOp>()) {
    auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
    if (!bits || bits.getInput() != transport.getBodyBlock()->getArgument(6)) continue;
    auto zero = eq.getRhs().getDefiningOp<ConstantOp>();
    if (bits.getHi() != 63 || bits.getLo() != space.getInt() || !zero || !zero.getValue().isZero())
      return reject("CPU stream DMA address does not select window zero");
    ++grants;
  }
  for (auto mem : queue.getOps<MemOp>()) {
    if (mem.getDepth() != uint64_t(depth.getInt()) || mem.getDataType() != uint(512) ||
        mem.getReadLatency() != 0 || mem.getWriteLatency() != 1)
      return reject("CPU stream descriptor capacity differs from native queue storage");
    ++memories;
  }
  if (grants != 1 || memories != 1)
    return reject("CPU stream needs one address grant and one queue memory");
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto word = registers && registers.size() == 1 ? dyn_cast<DictionaryAttr>(registers[0]) : DictionaryAttr();
  auto offset = word ? word.getAs<IntegerAttr>("offset") : IntegerAttr();
  auto readable = word ? word.getAs<BoolAttr>("readable") : BoolAttr();
  auto writeable = word ? word.getAs<BoolAttr>("writeable") : BoolAttr();
  if (!word || word.getAs<StringAttr>("name") != streamName.getValue().str() + "_count" ||
      !offset || offset.getInt() != 0 || !readable || !readable.getValue() || !writeable || writeable.getValue() ||
      bank.getNumPorts() != 4 || bank.getPortName(2) != "count" || bank.getPortName(3) != "mcr" ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1) ||
      bank.getPortType(2) != uint(13))
    return reject("CPU stream driver needs its read-only count word and 13-bit queue count");
  for (unsigned i = 0; i < 4; ++i)
    if (bank.getPortDirection(i) != (i == 3 ? Direction::Out : Direction::In))
      return reject("CPU stream count bank directions differ");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(3), group));
    if (!slots || slots.getNumElements() != 1 || fieldType(slots.getElementType(), "bits") != uint(32))
      return reject("CPU stream count address requires one 32-bit MCR slot");
  }
  auto arg = [&](FModuleOp m, unsigned i) { return m.getBodyBlock()->getArgument(i); };
  auto isSlot = [&](Value v, StringRef groupName, StringRef member) {
    auto f = v ? v.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto s = f ? f.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto g = s ? s.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return f && f.getFieldName() == member && s && s.getIndex() == 0 &&
        g && g.getFieldName() == groupName && g.getInput() == arg(bank, 3);
  };
  unsigned readsCount = 0, valids = 0, readies = 0, assertions = 0;
  for (auto connect : bank.getOps<StrictConnectOp>()) {
    if (isSlot(connect.getDest(), "read", "bits")) {
      auto pad = connect.getSrc().getDefiningOp<PadPrimOp>();
      if (!pad || pad.getInput() != arg(bank, 2) || pad.getAmount() != 32)
        return reject("CPU stream count read does not expose the live queue count");
      ++readsCount;
    }
    if (isSlot(connect.getDest(), "read", "valid") || isSlot(connect.getDest(), "write", "ready")) {
      auto one = connect.getSrc().getDefiningOp<ConstantOp>();
      if (!one || !one.getValue().isOne()) return reject("CPU stream count MCR slot must always be available");
      if (isSlot(connect.getDest(), "read", "valid")) ++valids; else ++readies;
    }
  }
  for (auto a : bank.getOps<AssertOp>()) {
    auto pred = a.getPredicate().getDefiningOp<NotPrimOp>();
    auto enable = a.getEnable().getDefiningOp<NotPrimOp>();
    if (a.getClock() != arg(bank, 0) || !pred || !isSlot(pred.getInput(), "write", "valid") ||
        !enable || enable.getInput() != arg(bank, 1))
      return reject("CPU stream count must reject writes outside host reset");
    ++assertions;
  }
  if (readsCount != 1 || valids != 1 || readies != 1 || assertions != 1)
    return reject("CPU stream count has missing or ambiguous MMIO drivers");
  // Resolve aggregate forwarding across module instances. This verifies that
  // the count and read payload describe the same live queue, rather than a
  // matching module definition or an unrelated UInt<13> signal.
  auto trace = [&](Value start, Value expected) {
    llvm::DenseSet<Value> seen;
    std::function<bool(Value)> follow = [&](Value value) {
      if (value == expected) return true;
      if (!value || !seen.insert(value).second) return false;
      auto *block = value.getParentBlock();
      Value driver; unsigned drivers = 0;
      for (auto &op : *block) {
        if (auto c = dyn_cast<ConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
        if (auto c = dyn_cast<StrictConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
      }
      if (drivers) return drivers == 1 && follow(driver);
      if (auto a = dyn_cast<BlockArgument>(value)) {
        auto module = dyn_cast<FModuleOp>(a.getOwner()->getParentOp());
        if (!module || module.getPortDirection(a.getArgNumber()) != Direction::In) return false;
        InstanceOp inst; unsigned uses = 0;
        circuit.walk([&](InstanceOp i) { if (i.getModuleName() == module.getName()) { inst = i; ++uses; } });
        return uses == 1 && follow(inst.getResult(a.getArgNumber()));
      }
      if (auto i = value.getDefiningOp<InstanceOp>()) {
        auto *node = graph.lookup(i.getModuleNameAttr().getAttr());
        auto m = node ? dyn_cast<FModuleOp>(node->getModule().getOperation()) : FModuleOp();
        auto result = cast<OpResult>(value);
        return m && m.getPortDirection(result.getResultNumber()) == Direction::Out && follow(arg(m, result.getResultNumber()));
      }
      return false;
    };
    return follow(start);
  };
  if (!trace(arg(bank, 2), arg(queue, 4)) || !trace(arg(transport, 2), arg(queue, 3)))
    return reject("CPU stream count and read transport must forward the same live queue");
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
         "  std::vector<CPUManagedStreams::StreamParameters>{\n"
         "    CPUManagedStreams::StreamParameters(std::string(\"";
  for (unsigned char ch : streamName.getValue()) {
    if (ch == '"' || ch == '\\') out << '\\' << char(ch);
    else if (ch >= 32 && ch < 127) out << char(ch);
    else out << '\\' << char('0' + ((ch >> 6) & 7)) << char('0' + ((ch >> 3) & 7)) << char('0' + (ch & 7));
  }
  out << "\"), 0ULL, " << start.getInt() + offset.getInt() << "ULL, " << depth.getInt()
      << "U, " << bytes.getInt() << "U)\n  }\n));\n#endif // GET_MANAGED_STREAM_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.cpuManagedStreamHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end()); annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return success();
}
