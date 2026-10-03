// See LICENSE for license details.
// Oracle: UARTBridgeModule.genHeader and Widget.genConstructor.
// Derive the six-address UART ABI from a unique live CIRCT bank/allocation.
#include "goldengate/UARTHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <cstdint>

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareUARTHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("UARTBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed UARTBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.uartHeader"))
    return reject("UARTBridge needs one driver header without a prior UART constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid UARTBridge header top");
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
  auto bank = liveModule("GGUARTMMIOBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("UARTBridge bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "uartBridge_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("UARTBridge read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing UARTBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("UARTBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 24)
    return reject("invalid UARTBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("UARTBridge MMIO region exceeds control address width");
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
      return reject("ambiguous UARTBridge MMIO allocation");
  }
  auto engine = liveModule("GGUARTSerialEngine");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto cls = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto div = key ? key.getAs<IntegerAttr>("div") : IntegerAttr();
  if (!engine || !liveModule("GGUARTQueueWrapper") || !cls ||
      cls != "firechip.bridgeinterfaces.UARTKey" || !div || div.getInt() <= 0 || div.getInt() > INT32_MAX)
    return reject("UART header needs a unique live UARTKey serial engine and byte queue wrapper");
  // Baud timing is implemented by the engine; uart_t takes only the addresses,
  // UART instance number and runtime arguments, as in the Scala constructor.
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 6 || bank.getNumPorts() != 5 ||
      bank.getPortName(4) != "mcr" || bank.getPortDirection(4) != Direction::Out ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1) ||
      bank.getPortDirection(0) != Direction::In || bank.getPortDirection(1) != Direction::In ||
      bank.getPortDirection(2) != Direction::In || bank.getPortDirection(3) != Direction::Out)
    return reject("UART driver requires the six-word host-clock MCR bank");
  for (unsigned port : {2U, 3U})
    if (fieldType(bank.getPortType(port), "bits") != uint(8) ||
        fieldType(bank.getPortType(port), "valid") != uint(1) ||
        fieldType(bank.getPortType(port), "ready") != uint(1))
      return reject("UART MMIO payload and handshake widths differ from the byte interface");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(4), group));
    if (!slots || slots.getNumElements() != 6 || fieldType(slots.getElementType(), "bits") != uint(32))
      return reject("UART MMIO addresses require six 32-bit read/write slots");
  }
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto isSlot = [&](Value value, StringRef groupName, unsigned index, StringRef member) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && slot && slot.getIndex() == index &&
           group && group.getFieldName() == groupName && group.getInput() == arg(4);
  };
  auto isByteField = [&](Value value, unsigned port, StringRef name) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getInput() == arg(port) && field.getFieldName() == name;
  };
  const char *words[]{"out_bits", "out_valid", "out_ready", "in_bits", "in_valid", "in_ready"};
  const unsigned widths[]{8, 1, 1, 8, 1, 1};
  Value states[6]; uint64_t addresses[6];
  for (unsigned i = 0; i < 6; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto name = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!name || name != words[i] || !offset || offset.getInt() != i * 4 ||
        !readable || !readable.getValue() || !writeable || !writeable.getValue())
      return reject("UART MMIO order/permissions differ from its driver ABI");
    addresses[i] = start.getInt() + offset.getInt();
    unsigned matches = 0;
    for (auto reg : bank.getOps<RegOp>()) if (reg.getName() == words[i]) {
      if (i == 2 || i == 4 || reg.getClockVal() != arg(0) || reg.getResult().getType() != uint(widths[i]))
        return reject("UART data/status register width or host clock differs");
      states[i] = reg.getResult(); ++matches;
    }
    for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == words[i]) {
      auto zero = reg.getResetValue().getDefiningOp<ConstantOp>();
      if ((i != 2 && i != 4) || reg.getClockVal() != arg(0) || reg.getResetSignal() != arg(1) ||
          reg.getResult().getType() != uint(widths[i]) || !zero || !zero.getValue().isZero())
        return reject("UART pulse state must reset to zero on the host clock");
      states[i] = reg.getResult(); ++matches;
    }
    if (matches != 1) return reject("UART register identity differs from its driver address");
  }
  unsigned readsCount[6]{}, updates[6]{}, valids[6]{}, readies[6]{}, byteOutputs[3]{};
  for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto src = connect.getSrc();
    for (unsigned i = 0; i < 6; ++i) {
      if (isSlot(connect.getDest(), "read", i, "bits")) {
        auto pad = src.getDefiningOp<PadPrimOp>();
        if (!pad || pad.getInput() != states[i] || pad.getAmount() != 32)
          return reject("UART read slot differs from its driver register identity");
        ++readsCount[i];
      }
      if (isSlot(connect.getDest(), "read", i, "valid") || isSlot(connect.getDest(), "write", i, "ready")) {
        auto one = src.getDefiningOp<ConstantOp>();
        if (!one || !one.getValue().isOne()) return reject("UART MCR slots must always accept writes and provide reads");
        if (isSlot(connect.getDest(), "read", i, "valid")) ++valids[i]; else ++readies[i];
      }
      if (connect.getDest() == states[i]) {
        auto mux = src.getDefiningOp<MuxPrimOp>();
        auto bits = mux ? mux.getHigh().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
        if (!mux || !isSlot(mux.getSel(), "write", i, "valid") || !bits ||
            bits.getLo() != 0 || bits.getHi() != widths[i]-1 || !isSlot(bits.getInput(), "write", i, "bits"))
          return reject("UART register update differs from its allocated write slot");
        auto sample = mux.getLow(); auto zero = sample.getDefiningOp<ConstantOp>();
        bool ok = i == 0 ? isByteField(sample, 2, "bits") : i == 1 ? isByteField(sample, 2, "valid") :
                  i == 3 ? sample == states[i] : i == 5 ? isByteField(sample, 3, "ready") :
                  zero && zero.getValue().isZero();
        if (!ok) return reject("UART status samples, byte hold or single-cycle pulses differ from the driver protocol");
        ++updates[i];
      }
    }
    const unsigned ports[]{2, 3, 3}, stateSlots[]{2, 3, 4};
    const char *members[]{"ready", "bits", "valid"};
    for (unsigned i = 0; i < 3; ++i) if (isByteField(connect.getDest(), ports[i], members[i])) {
      if (src != states[stateSlots[i]]) return reject("UART software byte/handshake output differs from its address");
      ++byteOutputs[i];
    }
  }
  for (unsigned i = 0; i < 6; ++i)
    if (readsCount[i] != 1 || updates[i] != 1 || valids[i] != 1 || readies[i] != 1)
      return reject("UART register slot has missing or ambiguous read/write drivers");
  for (auto count : byteOutputs) if (count != 1) return reject("UART byte/handshake output binding is missing or ambiguous");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/uart.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 6; ++i)
    out << "static_assert(offsetof(UARTBRIDGEMODULE_struct, " << words[i] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << words[i] << "\");\n";
  out << "static_assert(sizeof(UARTBRIDGEMODULE_struct) == 6 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // UARTBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
         "registry.add_widget(new uart_t(\n  simif,\nUARTBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 6; ++i) out << "    ." << words[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.uartHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
