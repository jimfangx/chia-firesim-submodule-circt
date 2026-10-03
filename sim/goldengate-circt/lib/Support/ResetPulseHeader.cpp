// See LICENSE for license details.
// Oracle: ResetPulseBridge.scala.genHeader and Widget.genConstructor.
// Requires a unique live bank and matching control read/write allocation.
// Validate typed state, MMIO slots and constructor domain before appending the
// reset_pulse_t constructor to .const.h. Only rawAnnotations changes.
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <cstdint>
#include "llvm/Support/MathExtras.h"

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareResetPulseHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("ResetPulseBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed ResetPulseBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.resetPulseHeader"))
    return reject("ResetPulseBridge needs one driver header without a prior reset constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid ResetPulseBridge header top");
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
  auto bank = liveModule("GGResetPulseBridge");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("ResetPulseBridge bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "resetBridge_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("ResetPulseBridge read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing ResetPulseBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("ResetPulseBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 8)
    return reject("invalid ResetPulseBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("ResetPulseBridge MMIO region exceeds control address width");
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
      return reject("ambiguous ResetPulseBridge MMIO allocation");
  }
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 2 || bank.getNumPorts() != 4 ||
      bank.getPortName(3) != "mcr" || bank.getPortDirection(3) != Direction::Out)
    return reject("ResetPulseBridge header needs the two-word MCR bank");
  auto mcr = dyn_cast<BundleType>(bank.getPortType(3));
  for (StringRef group : {"read", "write"}) {
    auto field = mcr ? mcr.getElement(group) : std::nullopt;
    auto slots = field ? dyn_cast<FVectorType>(field->type) : FVectorType();
    auto token = slots ? dyn_cast<BundleType>(slots.getElementType()) : BundleType();
    auto bits = token ? token.getElement("bits") : std::nullopt;
    if (!slots || slots.getNumElements() != 2 || !bits ||
        bits->type != UIntType::get(circuit.getContext(), 32, false))
      return reject("ResetPulseBridge addresses require two 32-bit read/write slots");
  }
  auto isSlot = [&](Value value, StringRef groupName, unsigned index, StringRef member) {
    auto field = value.getDefiningOp<SubfieldOp>();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && slot && slot.getIndex() == index &&
           group && group.getFieldName() == groupName &&
           group.getInput() == bank.getBodyBlock()->getArgument(3);
  };
  auto key = bank->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto cls = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto maximum = key ? key.getAs<IntegerAttr>("maxPulseLength") : IntegerAttr();
  auto initial = key ? key.getAs<IntegerAttr>("defaultPulseLength") : IntegerAttr();
  auto polarity = key ? key.getAs<BoolAttr>("activeHigh") : BoolAttr();
  if (!cls || cls != "firesim.lib.bridges.ResetPulseBridgeParameters" || !maximum || !initial || !polarity ||
      maximum.getInt() <= 0 || maximum.getInt() >= INT32_MAX || initial.getInt() < 0 ||
      initial.getInt() > maximum.getInt())
    return reject("invalid ResetPulseBridge driver constructor parameters");
  unsigned width = llvm::Log2_64_Ceil(maximum.getInt() + 1);
  const char *fields[]{"pulseLength", "doneInit"};
  uint64_t addresses[2];
  Value state[2];
  for (unsigned i = 0; i < 2; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!d || !d.getAs<StringAttr>("name") || d.getAs<StringAttr>("name") != fields[i] || !offset || offset.getInt() != i * 4 ||
        !readable || !readable.getValue() || !writeable || !writeable.getValue())
      return reject("ResetPulseBridge register order/permissions differ from the driver ABI");
    unsigned matches = 0;
    for (auto connect : bank.getOps<StrictConnectOp>()) {
      if (!isSlot(connect.getDest(), "read", i, "bits")) continue;
      auto pad = connect.getSrc().getDefiningOp<PadPrimOp>();
      auto reg = pad ? pad.getInput().getDefiningOp() : nullptr;
      if (!pad || !reg || !isa<RegOp, RegResetOp>(reg) ||
          !reg->getAttrOfType<StringAttr>("name") ||
          reg->getAttrOfType<StringAttr>("name") != fields[i] ||
          pad.getInput().getType() != UIntType::get(circuit.getContext(), i ? 1 : width, false) ||
          (i == 0 ? !isa<RegOp>(reg) : !isa<RegResetOp>(reg)) ||
          reg->getOperand(0) != bank.getBodyBlock()->getArgument(0))
        return reject("ResetPulseBridge read slot differs from its advertised state/constructor width");
      state[i] = pad.getInput();
      if (i) {
        auto done = cast<RegResetOp>(reg);
        auto zero = done.getResetValue().getDefiningOp<ConstantOp>();
        if (done.getResetSignal() != bank.getBodyBlock()->getArgument(1) || !zero || !zero.getValue().isZero())
          return reject("ResetPulseBridge doneInit must reset to false on host reset");
      }
      unsigned updates = 0;
      for (auto update : bank.getOps<StrictConnectOp>()) {
        if (update.getDest() != state[i]) continue;
        auto mux = update.getSrc().getDefiningOp<MuxPrimOp>();
        auto slice = mux ? mux.getHigh().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
        if (!mux || !slice || slice.getLo() != 0 || slice.getHi() != (i ? 0 : width - 1) ||
            !isSlot(mux.getSel(), "write", i, "valid") ||
            !isSlot(slice.getInput(), "write", i, "bits") || (i && mux.getLow() != state[i]))
          return reject("ResetPulseBridge state update does not select its matching write slot");
        ++updates;
      }
      if (updates != 1) return reject("missing or ambiguous ResetPulseBridge state update");
      ++matches;
    }
    if (matches != 1) return reject("missing or ambiguous ResetPulseBridge read-slot driver");
    addresses[i] = start.getInt() + offset.getInt();
  }
  // Preserve the constructor's pulse domain: the advertised polarity must agree
  // with the emitted token logic, not merely with a retained dictionary.
  unsigned tokenBits = 0, tokenValid = 0;
  for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    auto token = field ? field.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    if (!token || token.getFieldName() != "reset" || token.getInput() != bank.getBodyBlock()->getArgument(2)) continue;
    if (field.getFieldName() == "valid") {
      if (connect.getSrc() != state[1]) return reject("ResetPulseBridge token valid is not doneInit");
      ++tokenValid;
    }
    if (field.getFieldName() == "bits") {
      auto bit = connect.getSrc().getDefiningOp<XorPrimOp>();
      auto complete = bit ? bit.getLhs().getDefiningOp<EQPrimOp>() : EQPrimOp();
      auto zero = complete ? complete.getRhs().getDefiningOp<ConstantOp>() : ConstantOp();
      auto active = bit ? bit.getRhs().getDefiningOp<ConstantOp>() : ConstantOp();
      if (!complete || complete.getLhs() != state[0] || !zero || !zero.getValue().isZero() ||
          !active || active.getValue().getZExtValue() != polarity.getValue())
        return reject("ResetPulseBridge token polarity differs from constructor parameters");
      ++tokenBits;
    }
  }
  if (tokenBits != 1 || tokenValid != 1) return reject("missing or ambiguous ResetPulseBridge token output");
  std::string snippet;
  llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/reset_pulse.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 2; ++i)
    out << "static_assert(offsetof(RESETPULSEBRIDGEMODULE_struct, " << fields[i] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << fields[i] << "\");\n";
  out << "static_assert(sizeof(RESETPULSEBRIDGEMODULE_struct) == 2 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // RESETPULSEBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
         "registry.add_widget(new reset_pulse_t(\n  simif,\nRESETPULSEBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 2; ++i) out << "    ." << fields[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args,\n  " << maximum.getInt() << "U,\n  " << initial.getInt() << "U\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush();
  OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.resetPulseHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
