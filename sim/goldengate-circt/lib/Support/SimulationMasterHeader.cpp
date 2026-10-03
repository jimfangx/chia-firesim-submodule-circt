// See LICENSE for license details.
// Oracle: Master.scala.genHeader, Widget.scala.genConstructor and Lib.scala's
// MCR substruct registration order. Addresses come from the CIRCT MMIO catalog,
// checked against the bank's typed read slots and the live control binding.
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareSimulationMasterHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("SimulationMaster header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed SimulationMaster header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.simulationMasterHeader"))
    return reject("SimulationMaster needs one driver header without a prior master constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid SimulationMaster header top");
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
  auto bank = liveModule("GGSimulationMasterBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto binding = liveModule("GGSimulationMasterBoundWrapper");
  if (!bank || !decoder || !binding)
    return reject("SimulationMaster bank, decoder and binding must each have one live instance path");
  auto slave = binding->getAttrOfType<IntegerAttr>("goldengate.simulationMasterSlave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing SimulationMaster control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (!identity.consume_front("SimulationMaster_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 12)
    return reject("invalid SimulationMaster widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("SimulationMaster MMIO region exceeds control address width");
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
      return reject("ambiguous SimulationMaster MMIO allocation");
  }
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 3 || bank.getNumPorts() != 3 ||
      bank.getPortName(2) != "mcr" || bank.getPorts()[2].direction != Direction::Out)
    return reject("SimulationMaster header needs its three-word MCR bank");
  auto mcr = dyn_cast<BundleType>(bank.getPorts()[2].type);
  auto readField = mcr ? mcr.getElement("read") : std::nullopt;
  auto slots = readField ? dyn_cast<FVectorType>(readField->type) : FVectorType();
  auto token = slots ? dyn_cast<BundleType>(slots.getElementType()) : BundleType();
  auto bits = token ? token.getElement("bits") : std::nullopt;
  if (!slots || slots.getNumElements() != 3 || !bits ||
      bits->type != UIntType::get(circuit.getContext(), 32, false))
    return reject("SimulationMaster register addresses require three 32-bit read slots");
  const char *fields[]{"INIT_DONE", "PRESENCE_READ", "PRESENCE_WRITE"};
  uint64_t addresses[3];
  for (unsigned i = 0; i < 3; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto regName = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (regName != fields[i] || !offset || offset.getInt() != i * 4 ||
        !readable || !readable.getValue() || !writeable || !writeable.getValue())
      return reject("SimulationMaster register metadata differs from driver substruct order");
    // Check that the advertised register actually drives that MCR read word.
    unsigned matches = 0;
    for (auto connect : bank.getOps<StrictConnectOp>()) {
      auto field = connect.getDest().getDefiningOp<SubfieldOp>();
      auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
      auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
      if (!field || field.getFieldName() != "bits" || !slot || slot.getIndex() != i ||
          !group || group.getFieldName() != "read" ||
          group.getInput() != bank.getBodyBlock()->getArgument(2)) continue;
      auto *reg = connect.getSrc().getDefiningOp();
      if (!isa_and_nonnull<RegOp, RegResetOp>(reg) ||
          reg->getAttrOfType<StringAttr>("name") != fields[i] ||
          connect.getSrc().getType() != bits->type)
        return reject("SimulationMaster MMIO identity does not match its read-slot driver");
      ++matches;
    }
    if (matches != 1) return reject("missing or ambiguous SimulationMaster MMIO read-slot driver");
    addresses[i] = start.getInt() + offset.getInt();
  }
  std::string snippet;
  llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/master.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 3; ++i)
    out << "static_assert(offsetof(SIMULATIONMASTER_struct, " << fields[i] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << fields[i] << "\");\n";
  out << "static_assert(sizeof(SIMULATIONMASTER_struct) == 3 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // SIMULATIONMASTER_checks\n#ifdef GET_CORE_CONSTRUCTOR\n"
         "registry.add_widget(new master_t(\n  simif,\nSIMULATIONMASTER_struct{\n";
  for (unsigned i = 0; i < 3; ++i)
    out << "    ." << fields[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args\n));\n#endif // GET_CORE_CONSTRUCTOR\n";
  out.flush();
  OpBuilder b(circuit.getContext());
  NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.simulationMasterHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
