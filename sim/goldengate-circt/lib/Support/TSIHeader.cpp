// See LICENSE for license details.
// Oracle: TSIBridgeModule.genHeader and Widget.genConstructor.
// Derive addresses, instance number and memory offset from live FIRRTL operations.
#include "goldengate/TSIHeader.h"
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
LogicalResult goldengate::prepareTSIHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("TSIBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed TSIBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.tsiHeader"))
    return reject("TSIBridge needs one driver header without a prior TSI constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid TSIBridge header top");
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
  auto bank = liveModule("GGTSIMMIOBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto binding = liveModule("GGTSIBridgeBoundWrapper");
  if (!bank || !decoder || !binding)
    return reject("TSI bank, decoder and control binding need unique live instance paths");
  auto slave = binding->getAttrOfType<IntegerAttr>("goldengate.tsiSlave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing TSIBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (!identity.consume_front("TSIBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 36)
    return reject("invalid TSIBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("TSIBridge MMIO region exceeds control address width");
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
      return reject("ambiguous TSIBridge MMIO allocation");
  }
  auto engine = liveModule("GGTSITokenEngine");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto cls = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto regionName = key ? key.getAs<StringAttr>("memoryRegionNameOpt") : StringAttr();
  auto loadMemHeader = output.getAs<BoolAttr>("goldengate.loadMemHeader");
  if (!engine || !liveModule("GGTSIWordQueuesWrapper") || !cls ||
      cls != "firechip.bridgeinterfaces.TSIBridgeParams" || !regionName || regionName != "MainMemory_0" ||
      !loadMemHeader || !loadMemHeader.getValue())
    return reject("TSI header needs its live token engine, MainMemory_0 key, word queues and LoadMem header");
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  // FPGATop passes hostBase - virtualBase to TSIBridge.genHeader. Check the
  // retained allocation against both actual AXI address translation drivers.
  auto translation = liveModule("GGFASEDAddressTranslation");
  auto region = translation ? translation->getAttrOfType<DictionaryAttr>("goldengate.memoryRegion") : DictionaryAttr();
  auto base = region ? region.getAs<IntegerAttr>("virtualBase") : IntegerAttr();
  auto bound = region ? region.getAs<IntegerAttr>("virtualBound") : IntegerAttr();
  auto host = region ? region.getAs<IntegerAttr>("hostBase") : IntegerAttr();
  auto offset = region ? region.getAs<IntegerAttr>("offset") : IntegerAttr();
  if (!translation || !region || region.getAs<StringAttr>("name") != regionName ||
      !base || base.getInt() < 0 || !bound || bound.getInt() < base.getInt() ||
      !host || host.getInt() < 0 || !offset || offset.getInt() != host.getInt() - base.getInt() ||
      translation.getNumPorts() != 4 || translation.getPortDirection(2) != Direction::In ||
      translation.getPortDirection(3) != Direction::Out)
    return reject("TSI memory offset needs a unique live, consistent host memory allocation");
  auto ta = [&](unsigned i) { return translation.getBodyBlock()->getArgument(i); };
  auto isAddress = [&](Value value, unsigned port, StringRef channel) {
    for (StringRef member : {StringRef("addr"), StringRef("bits"), channel}) {
      auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
      if (!field || field.getFieldName() != member) return false;
      value = field.getInput();
    }
    return value == ta(port);
  };
  for (StringRef channel : {"aw", "ar"}) {
    auto in = dyn_cast_or_null<UIntType>(fieldType(fieldType(fieldType(translation.getPortType(2),channel),"bits"),"addr"));
    auto out = dyn_cast_or_null<UIntType>(fieldType(fieldType(fieldType(translation.getPortType(3),channel),"bits"),"addr"));
    if (!in || !out || !in.getWidth() || !out.getWidth() || *out.getWidth() < 1 ||
        *out.getWidth() > 62 || *in.getWidth() != *out.getWidth() + 1)
      return reject("TSI memory allocation needs the implemented AXI address widths");
    uint64_t capacity = uint64_t(1) << *out.getWidth();
    uint64_t span = uint64_t(bound.getInt()) - base.getInt() + 1;
    if (uint64_t(bound.getInt()) >= (uint64_t(1) << *in.getWidth()) ||
        uint64_t(host.getInt()) >= capacity || span > capacity - host.getInt())
      return reject("TSI memory allocation exceeds the host or target address width");
    unsigned drivers = 0;
    for (auto connect : translation.getOps<StrictConnectOp>()) {
      if (!isAddress(connect.getDest(), 3, channel)) continue;
      auto bits = connect.getSrc().getDefiningOp<BitsPrimOp>();
      auto add = bits ? bits.getInput().getDefiningOp<AddPrimOp>() : AddPrimOp();
      auto shift = add ? add.getRhs().getDefiningOp<ConstantOp>() : ConstantOp();
      if (!bits || bits.getLo() != 0 || bits.getHi() != *out.getWidth()-1 || !add ||
          !isAddress(add.getLhs(),2,channel) || !shift ||
          shift.getResult().getType() != in ||
          static_cast<const APInt &>(shift.getValue()) != APInt(*in.getWidth(), uint64_t(offset.getInt()) & (capacity-1)))
        return reject("TSI driver offset differs from the actual AXI translation");
      ++drivers;
    }
    if (drivers != 1) return reject("TSI memory address translation is missing or ambiguous");
  }
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 9 || bank.getNumPorts() != 6 ||
      bank.getPortName(5) != "mcr" || bank.getPortDirection(5) != Direction::Out ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1) ||
      bank.getPortDirection(0) != Direction::In || bank.getPortDirection(1) != Direction::In ||
      bank.getPortDirection(2) != Direction::Out || bank.getPortDirection(3) != Direction::In || bank.getPortDirection(4) != Direction::In)
    return reject("TSI driver requires the nine-word host-clock MCR bank");
  for (unsigned port : {2U, 3U})
    if (fieldType(bank.getPortType(port), "bits") != uint(32) ||
        fieldType(bank.getPortType(port), "valid") != uint(1) ||
        fieldType(bank.getPortType(port), "ready") != uint(1))
      return reject("TSI MMIO payload and handshake widths differ from the word interface");
  if (fieldType(bank.getPortType(4), "step_size") != uint(32) ||
      fieldType(bank.getPortType(4), "start") != uint(1) ||
      fieldType(bank.getPortType(4), "done") != uint(1))
    return reject("TSI scheduler MMIO widths differ from its driver ABI");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(5), group));
    if (!slots || slots.getNumElements() != 9 || fieldType(slots.getElementType(), "bits") != uint(32))
      return reject("TSI MMIO addresses require nine 32-bit read/write slots");
  }
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto isSlot = [&](Value value, StringRef groupName, unsigned index, StringRef member) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && slot && slot.getIndex() == index &&
           group && group.getFieldName() == groupName && group.getInput() == arg(5);
  };
  auto isWordField = [&](Value value, unsigned port, StringRef name) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getInput() == arg(port) && field.getFieldName() == name;
  };
  const char *words[]{"in_bits", "in_valid", "in_ready", "out_bits", "out_valid", "out_ready", "step_size", "done", "start"};
  const unsigned widths[]{32, 1, 1, 32, 1, 1, 32, 1, 1};
  Value states[9]; uint64_t addresses[9];
  for (unsigned i = 0; i < 9; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto name = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!name || name != words[i] || !offset || offset.getInt() != i * 4 ||
        !readable || !readable.getValue() || !writeable || !writeable.getValue())
      return reject("TSI MMIO order/permissions differ from its driver ABI");
    addresses[i] = start.getInt() + offset.getInt();
    unsigned matches = 0;
    for (auto reg : bank.getOps<RegOp>()) if (reg.getName() == words[i]) {
      if (i == 1 || i == 5 || i == 8 || reg.getClockVal() != arg(0) || reg.getResult().getType() != uint(widths[i]))
        return reject("TSI data/status register width or host clock differs");
      states[i] = reg.getResult(); ++matches;
    }
    for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == words[i]) {
      auto zero = reg.getResetValue().getDefiningOp<ConstantOp>();
      if ((i != 1 && i != 5 && i != 8) || reg.getClockVal() != arg(0) || reg.getResetSignal() != arg(1) ||
          reg.getResult().getType() != uint(widths[i]) || !zero || !zero.getValue().isZero())
        return reject("TSI pulse state must reset to zero on the host clock");
      states[i] = reg.getResult(); ++matches;
    }
    if (matches != 1) return reject("TSI register identity differs from its driver address");
  }
  unsigned readsCount[9]{}, updates[9]{}, valids[9]{}, readies[9]{}, wordOutputs[5]{};
  for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto src = connect.getSrc();
    for (unsigned i = 0; i < 9; ++i) {
      if (isSlot(connect.getDest(), "read", i, "bits")) {
        auto pad = src.getDefiningOp<PadPrimOp>();
        if (!pad || pad.getInput() != states[i] || pad.getAmount() != 32)
          return reject("TSI read slot differs from its driver register identity");
        ++readsCount[i];
      }
      if (isSlot(connect.getDest(), "read", i, "valid") || isSlot(connect.getDest(), "write", i, "ready")) {
        auto one = src.getDefiningOp<ConstantOp>();
        if (!one || !one.getValue().isOne()) return reject("TSI MCR slots must always accept writes and provide reads");
        if (isSlot(connect.getDest(), "read", i, "valid")) ++valids[i]; else ++readies[i];
      }
      if (connect.getDest() == states[i]) {
        auto mux = src.getDefiningOp<MuxPrimOp>();
        auto bits = mux ? mux.getHigh().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
        if (!mux || !isSlot(mux.getSel(), "write", i, "valid") || !bits ||
            bits.getLo() != 0 || bits.getHi() != widths[i]-1 || !isSlot(bits.getInput(), "write", i, "bits"))
          return reject("TSI register update differs from its allocated write slot");
        auto sample = mux.getLow(); auto zero = sample.getDefiningOp<ConstantOp>();
        bool ok = i == 2 ? isWordField(sample, 2, "ready") : i == 3 ? isWordField(sample, 3, "bits") :
                  i == 4 ? isWordField(sample, 3, "valid") : i == 7 ? isWordField(sample, 4, "done") :
                  (i == 0 || i == 6) ? sample == states[i] : zero && zero.getValue().isZero();
        if (!ok) return reject("TSI status samples, word hold or single-cycle pulses differ from the driver protocol");
        ++updates[i];
      }
    }
    const unsigned ports[]{2, 2, 3, 4, 4}, stateSlots[]{0, 1, 5, 6, 8};
    const char *members[]{"bits", "valid", "ready", "step_size", "start"};
    for (unsigned i = 0; i < 5; ++i) if (isWordField(connect.getDest(), ports[i], members[i])) {
      if (src != states[stateSlots[i]]) return reject("TSI software word/handshake output differs from its address");
      ++wordOutputs[i];
    }
  }
  for (unsigned i = 0; i < 9; ++i)
    if (readsCount[i] != 1 || updates[i] != 1 || valids[i] != 1 || readies[i] != 1)
      return reject("TSI register slot has missing or ambiguous read/write drivers");
  for (auto count : wordOutputs) if (count != 1) return reject("TSI word/handshake output binding is missing or ambiguous");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/tsibridge.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 9; ++i)
    out << "static_assert(offsetof(TSIBRIDGEMODULE_struct, " << words[i] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << words[i] << "\");\n";
  out << "static_assert(sizeof(TSIBRIDGEMODULE_struct) == 9 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // TSIBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
         "registry.add_widget(new tsibridge_t(\n  simif,\n  registry.get_widget<loadmem_t>(),\nTSIBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 9; ++i) out << "    ." << words[i] << " = " << addresses[i] << ",\n";
  // Match bridgeutils.UInt64: negative offsets retain the oracle's ULL
  // spelling; tsibridge_t consumes the resulting two's complement int64_t.
  out << "},\n  " << widgetIndex << ",\n  args,\n  true,\n  " << offset.getInt() << "ULL\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.tsiHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
