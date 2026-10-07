// See LICENSE for license details.
// Required input: a single live clock bridge, decoder, read/write bindings and
// an existing .const.h output annotation. Oracle: ClockBridge.scala.genHeader,
// Widget.genWideRORegInit/genConstructor and Lib.scala MMIO allocation.
// Consumes no annotations; appends clockmodule_t constructor/ABI checks to the
// OutputFile body. Only rawAnnotations changes; all hardware/analyses survive.
// Addresses/index derive from matching live read/write allocation catalogs.
// Validate six 32-bit slots, 64-bit snapshot slices and latch SSA before mutation.
// Output: GET_BRIDGE_CONSTRUCTOR and GET_SUBSTRUCT_CHECKS clock driver sections.
// Scope: ordered rational-clock producer with the 32-bit MCR snapshot bank.
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareClockBridgeHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("ClockBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed ClockBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.clockBridgeHeader"))
    return reject("ClockBridge needs one driver header without a prior clock constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid ClockBridge header top");
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
  auto bank = liveModule("GGSingleClockBridge");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("ClockBridge bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (port != "clockBridge_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("ClockBridge read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing ClockBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("ClockBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 24)
    return reject("invalid ClockBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("ClockBridge MMIO region exceeds control address width");
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
      return reject("ambiguous ClockBridge MMIO allocation");
  }
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 6 || bank.getNumPorts() != 6 ||
      bank.getPortName(5) != "mcr" || bank.getPortDirection(5) != Direction::Out)
    return reject("ClockBridge header needs the six-word MCR bank");
  auto mcr = dyn_cast<BundleType>(bank.getPortType(5));
  for (StringRef group : {"read", "write"}) {
    auto field = mcr ? mcr.getElement(group) : std::nullopt;
    auto slots = field ? dyn_cast<FVectorType>(field->type) : FVectorType();
    auto token = slots ? dyn_cast<BundleType>(slots.getElementType()) : BundleType();
    auto bits = token ? token.getElement("bits") : std::nullopt;
    if (!slots || slots.getNumElements() != 6 || !bits ||
        bits->type != UIntType::get(circuit.getContext(), 32, false))
      return reject("ClockBridge addresses require six 32-bit read/write slots");
  }
  auto isSlot = [&](Value value, StringRef groupName, unsigned index, StringRef member) {
    auto field = value.getDefiningOp<SubfieldOp>();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && slot && slot.getIndex() == index &&
           group && group.getFieldName() == groupName &&
           group.getInput() == bank.getBodyBlock()->getArgument(5);
  };
  const char *fields[]{"hCycle_0", "hCycle_1", "hCycle_latch", "tCycle_0", "tCycle_1", "tCycle_latch"};
  uint64_t addresses[6];
  for (unsigned i = 0; i < 6; ++i) {
    bool latch = i % 3 == 2;
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!d || d.getAs<StringAttr>("name") != fields[i] || !offset || offset.getInt() != i * 4 ||
        !readable || readable.getValue() == latch || !writeable || writeable.getValue() != latch)
      return reject("ClockBridge register order/permissions differ from the driver ABI");
    unsigned matches = 0;
    for (auto connect : bank.getOps<StrictConnectOp>()) {
      if (!isSlot(connect.getDest(), "read", i, "bits")) continue;
      if (latch) {
        auto zero = connect.getSrc().getDefiningOp<ConstantOp>();
        if (!zero || !zero.getValue().isZero()) return reject("ClockBridge write-only read slot is not zero");
      } else {
        auto slice = connect.getSrc().getDefiningOp<BitsPrimOp>();
        auto shadow = slice ? slice.getInput().getDefiningOp<RegOp>() : RegOp();
        if (!slice || slice.getLo() != (i % 3) * 32 || slice.getHi() != (i % 3) * 32 + 31 ||
            !shadow || shadow.getName() != (i < 3 ? "hCycle_mmreg" : "tCycle_mmreg") ||
            shadow.getResult().getType() != UIntType::get(circuit.getContext(), 64, false))
          return reject("ClockBridge snapshot read slot differs from its advertised identity");
        unsigned updates = 0;
        for (auto update : bank.getOps<StrictConnectOp>()) {
          if (update.getDest() != shadow.getResult()) continue;
          auto mux = update.getSrc().getDefiningOp<MuxPrimOp>();
          auto gate = mux ? mux.getSel().getDefiningOp<AndPrimOp>() : AndPrimOp();
          auto bit = gate ? gate.getRhs().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
          auto counter = mux ? mux.getHigh().getDefiningOp<RegResetOp>() : RegResetOp();
          if (!mux || mux.getLow() != shadow.getResult() || !gate || !bit ||
              bit.getLo() != 0 || bit.getHi() != 0 ||
              !isSlot(gate.getLhs(), "write", i / 3 * 3 + 2, "valid") ||
              !isSlot(bit.getInput(), "write", i / 3 * 3 + 2, "bits") || !counter ||
              counter.getName() != (i < 3 ? "hCycle" : "tCycleFastest") ||
              counter.getResult().getType() != shadow.getResult().getType())
            return reject("ClockBridge snapshot latch does not select the matching counter/write slot");
          ++updates;
        }
        if (updates != 1) return reject("missing or ambiguous ClockBridge snapshot update");
      }
      ++matches;
    }
    if (matches != 1) return reject("missing or ambiguous ClockBridge read-slot driver");
    addresses[i] = start.getInt() + offset.getInt();
  }
  std::string snippet;
  llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/clock.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 6; ++i)
    out << "static_assert(offsetof(CLOCKBRIDGEMODULE_struct, " << fields[i] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << fields[i] << "\");\n";
  out << "static_assert(sizeof(CLOCKBRIDGEMODULE_struct) == 6 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // CLOCKBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
         "registry.add_widget(new clockmodule_t(\n  simif,\nCLOCKBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 6; ++i) out << "    ." << fields[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush();
  OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.clockBridgeHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
