// See LICENSE for license details.
// Oracle: PeekPokeIO.scala.genHeader/genPortLists and Widget.genConstructor.
// Requires a unique live bank and matching control read/write allocation.
// Validate the implemented one-bit reset poke, typed bank and live allocation
// before appending the three-register driver ABI and port maps.
#include "goldengate/PeekPokeHeader.h"
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
LogicalResult goldengate::preparePeekPokeHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("PeekPokeBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed PeekPokeBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.peekPokeHeader"))
    return reject("PeekPokeBridge needs one driver header without a prior PeekPoke constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid PeekPokeBridge header top");
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
  auto bank = liveModule("GGPeekPokeMMIOBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("PeekPokeBridge bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "peekPokeBridge_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("PeekPokeBridge read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing PeekPokeBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("PeekPokeBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 28)
    return reject("invalid PeekPokeBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("PeekPokeBridge MMIO region exceeds control address width");
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
      return reject("ambiguous PeekPokeBridge MMIO allocation");
  }
  auto engine = liveModule("GGPeekPokeCycleEngine");
  auto queue = liveModule("GGPeekPokeStepQueue");
  if (!engine || !queue || engine.getNumPorts() != 4 || queue.getNumPorts() != 4)
    return reject("PeekPoke header needs a unique live cycle engine and STEP queue");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto cls = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto maximum = key ? key.getAs<IntegerAttr>("maxChannelDecoupling") : IntegerAttr();
  auto pokes = key ? key.getAs<ArrayAttr>("pokes") : ArrayAttr();
  auto peeks = key ? key.getAs<ArrayAttr>("peeks") : ArrayAttr();
  auto poke = pokes && pokes.size() == 1 ? dyn_cast<DictionaryAttr>(pokes[0]) : DictionaryAttr();
  auto inputName = poke ? poke.getAs<StringAttr>("name") : StringAttr();
  auto width = poke ? poke.getAs<IntegerAttr>("fieldWidth") : IntegerAttr();
  auto type = poke ? poke.getAs<DictionaryAttr>("tpe") : DictionaryAttr();
  auto typeName = type ? type.getAs<StringAttr>("typeString") : StringAttr();
  if (!cls || cls != "firesim.lib.bridges.PeekPokeKey" || !maximum || maximum.getInt() != 2 ||
      !peeks || !peeks.empty() || !inputName || inputName != "reset" ||
      !width || width.getInt() != 1 || !typeName || typeName != "UInt")
    return reject("PeekPoke driver supports only the implemented UInt<1> reset poke, no peeks and decoupling 2");
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  if (fieldType(fieldType(engine.getPortType(2), inputName.getValue()), "bits") != uint(width.getInt()) ||
      fieldType(engine.getPortType(3), "resetValue") != uint(width.getInt()))
    return reject("PeekPoke constructor input width/name differs from the emitted channel");
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 7 || bank.getNumPorts() != 4 ||
      bank.getPortName(3) != "mcr" || bank.getPortDirection(3) != Direction::Out ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1))
    return reject("PeekPoke driver requires the seven-word host-clock MCR bank");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(3), group));
    if (!slots || slots.getNumElements() != 7 || fieldType(slots.getElementType(), "bits") != uint(32))
      return reject("PeekPoke MMIO addresses require seven 32-bit read/write slots");
  }
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto isSlot = [&](Value value, StringRef groupName, unsigned index, StringRef member) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && slot && slot.getIndex() == index &&
           group && group.getFieldName() == groupName && group.getInput() == arg(3);
  };
  auto isControl = [&](Value value, StringRef name) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getInput() == arg(2) && field.getFieldName() == name;
  };
  const char *words[]{"tCycle_0", "tCycle_1", "tCycle_latch", "STEP", "DONE", "reset_0", "PRECISE_PEEKABLE"};
  uint64_t addresses[7];
  for (unsigned i = 0; i < 7; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto name = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!name || name != words[i] || !offset || offset.getInt() != i * 4 ||
        !readable || readable.getValue() != (i != 2 && i != 3) ||
        !writeable || writeable.getValue() != (i >= 2))
      return reject("PeekPoke MMIO order/permissions differ from its driver port maps");
    addresses[i] = start.getInt() + offset.getInt();
  }
  // Check actual SSA reads/updates, including registers deliberately omitted
  // from the C++ substruct by Scala attach(..., false).
  Value snapshot, resetValue, done, precise;
  for (auto reg : bank.getOps<RegOp>()) {
    if (reg.getClockVal() != arg(0)) return reject("PeekPoke payload state must use host clock");
    if (reg.getName() == "tCycle_tCycle_mmreg" && reg.getResult().getType() == uint(64)) snapshot = reg.getResult();
    if (reg.getName() == "target_reset_i" && reg.getResult().getType() == uint(width.getInt())) resetValue = reg.getResult();
  }
  for (auto reg : bank.getOps<RegResetOp>()) {
    auto zero = reg.getResetValue().getDefiningOp<ConstantOp>();
    if (reg.getClockVal() != arg(0) || reg.getResetSignal() != arg(1) || !zero || !zero.getValue().isZero())
      return reject("PeekPoke status state must reset to zero on the host clock");
    if (reg.getName() == "DONE" && reg.getResult().getType() == uint(32)) done = reg.getResult();
    if (reg.getName() == "PRECISE_PEEKABLE" && reg.getResult().getType() == uint(32)) precise = reg.getResult();
  }
  if (!snapshot || !resetValue || !done || !precise)
    return reject("PeekPoke register widths/state identities differ from the port map");
  unsigned readCounts[7]{}, updateCounts[4]{}, controlCounts[2]{};
  for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto src = connect.getSrc();
    for (unsigned i = 0; i < 7; ++i) if (isSlot(connect.getDest(), "read", i, "bits")) {
      ++readCounts[i];
      auto bits = src.getDefiningOp<BitsPrimOp>(); auto pad = src.getDefiningOp<PadPrimOp>();
      auto zero = src.getDefiningOp<ConstantOp>();
      bool ok = i < 2 ? bits && bits.getInput() == snapshot && bits.getLo() == i * 32 && bits.getHi() == i * 32 + 31 :
                i == 2 || i == 3 ? zero && zero.getValue().isZero() :
                i == 4 ? src == done : i == 5 ? pad && pad.getInput() == resetValue && pad.getAmount() == 32 : src == precise;
      if (!ok) return reject("PeekPoke read slot differs from its driver address identity");
    }
    Value states[]{snapshot, resetValue, done, precise};
    for (unsigned i = 0; i < 4; ++i) if (connect.getDest() == states[i]) {
      ++updateCounts[i]; auto mux = src.getDefiningOp<MuxPrimOp>();
      if (!mux) return reject("PeekPoke state update must select its software write slot");
      auto bits = mux.getHigh().getDefiningOp<BitsPrimOp>();
      if (i == 0) {
        auto gate = mux.getSel().getDefiningOp<AndPrimOp>();
        auto enable = gate ? gate.getRhs().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
        if (!gate || !isSlot(gate.getLhs(), "write", 2, "valid") || !enable ||
            enable.getLo() != 0 || enable.getHi() != 0 || !isSlot(enable.getInput(), "write", 2, "bits") ||
            !isControl(mux.getHigh(), "tCycle") || mux.getLow() != snapshot)
          return reject("PeekPoke tCycle latch differs from its allocated write slot");
      } else if (i == 1) {
        if (!isSlot(mux.getSel(), "write", 5, "valid") || !bits || bits.getLo() != 0 || bits.getHi() != width.getInt()-1 ||
            !isSlot(bits.getInput(), "write", 5, "bits") || mux.getLow() != resetValue)
          return reject("PeekPoke input chunk differs from its write slot");
      } else {
        unsigned slot = i == 2 ? 4 : 6; auto pad = mux.getLow().getDefiningOp<PadPrimOp>();
        if (!isSlot(mux.getSel(), "write", slot, "valid") || !isSlot(mux.getHigh(), "write", slot, "bits") ||
            !pad || !isControl(pad.getInput(), i == 2 ? "done" : "precisePeekable"))
          return reject("PeekPoke status update differs from its driver control slot");
      }
    }
    for (unsigned i = 0; i < 2; ++i) if (isControl(connect.getDest(), i ? "resetValue" : "poke")) {
      ++controlCounts[i];
      if (i ? src != resetValue : !isSlot(src, "write", 5, "valid"))
        return reject("PeekPoke input map does not drive the implemented poke channel");
    }
  }
  for (auto n : readCounts) if (n != 1) return reject("missing or ambiguous PeekPoke read slot");
  for (auto n : updateCounts) if (n != 1) return reject("missing or ambiguous PeekPoke state update");
  for (auto n : controlCounts) if (n != 1) return reject("missing or ambiguous PeekPoke poke control");
  unsigned enqueues = 0, dequeues = 0;
  for (auto instance : bank.getOps<InstanceOp>()) if (instance.getModuleName() == queue.getName()) {
    for (auto connect : bank.getOps<ConnectOp>()) {
      if (connect.getDest() == instance.getResult(2)) {
        auto slot = connect.getSrc().getDefiningOp<SubindexOp>();
        auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
        if (!slot || slot.getIndex() != 3 || !group || group.getFieldName() != "write" || group.getInput() != arg(3))
          return reject("PeekPoke STEP address does not feed the STEP queue");
        ++enqueues;
      }
      if (isControl(connect.getDest(), "step") && connect.getSrc() == instance.getResult(3)) ++dequeues;
    }
  }
  if (enqueues != 1 || dequeues != 1) return reject("PeekPoke STEP queue binding differs from the driver ABI");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/peek_poke.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  unsigned substructSlots[]{3, 4, 6};
  for (unsigned i = 0; i < 3; ++i)
    out << "static_assert(offsetof(PEEKPOKEBRIDGEMODULE_struct, " << words[substructSlots[i]] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << words[substructSlots[i]] << "\");\n";
  out << "static_assert(sizeof(PEEKPOKEBRIDGEMODULE_struct) == 3 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // PEEKPOKEBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
         "registry.add_widget(new peek_poke_t(\n  simif,\nPEEKPOKEBRIDGEMODULE_struct{\n";
  for (auto slot : substructSlots) out << "    ." << words[slot] << " = " << addresses[slot] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args,\n  peek_poke_t::PortMap{{\"" << inputName.getValue()
      << "\", peek_poke_t::Port{.address = " << addresses[5] << ", .chunks = " << (width.getInt()+31)/32
      << "}}},\n  peek_poke_t::PortMap{}\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.peekPokeHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
