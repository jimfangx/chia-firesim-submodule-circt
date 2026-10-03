// See LICENSE for license details.
// Requires: retained annotations, a unique live BlockDev bank/engine/binding and
// an allocated control region. Oracle: BlockDevBridgeModule.genHeader/Widget.
// Consumes: no annotation classes; requires one OutputFileAnnotation .const.h.
// Produces: appended BlockDev driver ABI/constructor in that output annotation.
// Mutates: its body and completion marker only; no hardware operations.
// Analyses: instance graph and typed MMIO connectivity. All hardware, target,
// hierarchy, clock and channel analyses are preserved.
// Output: 26 addresses from live slots, instance index from allocation, tracker
// count from the retained constructor, latency width from actual FIRRTL state.
#include "goldengate/BlockDevHeader.h"
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
LogicalResult goldengate::prepareBlockDevHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("BlockDevBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed BlockDevBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.blockDevHeader"))
    return reject("BlockDevBridge needs one driver header without a prior BlockDev constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid BlockDevBridge header top");
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
  auto bank = liveModule("GGBlockDevMMIOBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto binding = liveModule("GGBlockDevBridgeBoundWrapper");
  if (!bank || !decoder || !binding)
    return reject("BlockDev bank, decoder and control binding need unique live instance paths");
  auto slave = binding->getAttrOfType<IntegerAttr>("goldengate.blockdevSlave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing BlockDevBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (!identity.consume_front("BlockDevBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 104)
    return reject("invalid BlockDevBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("BlockDevBridge MMIO region exceeds control address width");
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
      return reject("ambiguous BlockDevBridge MMIO allocation");
  }
  auto engine = liveModule("GGBlockDevTokenEngine");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto cls = key ? key.getAs<StringAttr>("class") : StringAttr();
  auto trackers = key ? key.getAs<IntegerAttr>("nTrackers") : IntegerAttr();
  if (!engine || !cls || cls != "firechip.bridgeinterfaces.BlockDeviceConfig" ||
      !trackers || trackers.getInt() != 1)
    return reject("BlockDev header requires the implemented one-tracker constructor");
  unsigned latencyBits = 0, cycles = 0;
  for (auto reg : engine.getOps<RegResetOp>()) if (reg.getName() == "tCycle") {
    auto type = dyn_cast<UIntType>(reg.getResult().getType());
    if (!type || !type.getWidth() || *type.getWidth() < 9 || *type.getWidth() > 32)
      return reject("BlockDev driver needs an unsigned latency cycle counter");
    latencyBits = *type.getWidth(); ++cycles;
  }
  if (cycles != 1) return reject("BlockDev latency width needs a unique live cycle register");
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 26 || bank.getNumPorts() != 10 ||
      bank.getPortName(9) != "mcr" || bank.getPortDirection(9) != Direction::Out ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1))
    return reject("BlockDev driver requires its 26-word host-clock MCR bank");
  for (unsigned i = 0; i < 10; ++i)
    if (bank.getPortDirection(i) != (i == 4 || i == 5 || i >= 7 ? Direction::Out : Direction::In))
      return reject("BlockDev MMIO port directions differ from its driver protocol");
  for (unsigned port : {2U,3U,4U,5U})
    if (fieldType(bank.getPortType(port),"ready") != uint(1) ||
        fieldType(bank.getPortType(port),"valid") != uint(1))
      return reject("BlockDev queue handshake widths differ");
  for (unsigned port : {2U,3U,4U})
    if (fieldType(fieldType(bank.getPortType(port),"bits"),"tag") != uint(1))
      return reject("BlockDev queue tags differ from its one-tracker driver");
  for (unsigned port : {3U,4U})
    if (fieldType(fieldType(bank.getPortType(port),"bits"),"data") != uint(64))
      return reject("BlockDev driver data halves require 64-bit queue beats");
  for (StringRef member : {"len","offset"})
    if (fieldType(fieldType(bank.getPortType(2),"bits"),member) != uint(32))
      return reject("BlockDev driver request fields require 32-bit words");
  if (fieldType(fieldType(bank.getPortType(2),"bits"),"write") != uint(1) ||
      fieldType(bank.getPortType(5),"bits") != uint(1) ||
      fieldType(bank.getPortType(6),"wAckStallN") != uint(1) ||
      fieldType(bank.getPortType(6),"rRespStallN") != uint(1) ||
      fieldType(bank.getPortType(7),"nsectors") != uint(32) ||
      fieldType(bank.getPortType(7),"max_req_len") != uint(32) ||
      fieldType(bank.getPortType(8),"read_latency") != uint(latencyBits) ||
      fieldType(bank.getPortType(8),"write_latency") != uint(latencyBits))
    return reject("BlockDev geometry, timing or latency widths differ from its driver ABI");
  for (StringRef group : {"read","write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(9),group));
    if (!slots || slots.getNumElements() != 26 || fieldType(slots.getElementType(),"bits") != uint(32) ||
        fieldType(slots.getElementType(),"valid") != uint(1) || fieldType(slots.getElementType(),"ready") != uint(1))
      return reject("BlockDev MMIO addresses require 26 32-bit read/write slots");
  }
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto isSlot = [&](Value value, StringRef groupName, unsigned index, StringRef member) {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto group = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && slot && slot.getIndex() == index &&
        group && group.getFieldName() == groupName && group.getInput() == arg(9);
  };
  auto isField = [&](Value value, unsigned port, StringRef member, StringRef parent = "") {
    auto field = value ? value.getDefiningOp<SubfieldOp>() : SubfieldOp();
    if (!field || field.getFieldName() != member) return false;
    value = field.getInput();
    if (!parent.empty()) {
      auto outer = value.getDefiningOp<SubfieldOp>();
      if (!outer || outer.getFieldName() != parent) return false;
      value = outer.getInput();
    }
    return value == arg(port);
  };
  auto constant = [](Value v, uint64_t n) {
    auto c = v ? v.getDefiningOp<ConstantOp>() : ConstantOp();
    return c && c.getValue().getZExtValue() == n;
  };
  const char *words[]{"read_latency","write_latency","bdev_nsectors","bdev_max_req_len",
      "bdev_req_valid","bdev_req_write","bdev_req_offset","bdev_req_len","bdev_req_tag","bdev_req_ready",
      "bdev_data_valid","bdev_data_data_upper","bdev_data_data_lower","bdev_data_tag","bdev_data_ready",
      "bdev_rresp_data_upper","bdev_rresp_data_lower","bdev_rresp_tag","bdev_rresp_valid","bdev_rresp_ready",
      "bdev_wack_tag","bdev_wack_valid","bdev_wack_ready","bdev_reqs_pending","bdev_wack_stalled","bdev_rresp_stalled"};
  const unsigned widths[]{latencyBits,latencyBits,32,32,1,1,32,32,1,1,1,32,32,1,1,32,32,1,1,1,1,1,1,1,1,1};
  auto pulse = [](unsigned i) { return i == 9 || i == 14 || i == 18 || i == 21; };
  Value states[26]; uint64_t addresses[26];
  for (unsigned i = 0; i < 26; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto name = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!name || name != words[i] || !offset || offset.getInt() != i*4 ||
        !readable || readable.getValue() != (i != 2 && i != 3) || !writeable || !writeable.getValue())
      return reject("BlockDev MMIO order/permissions differ from its driver ABI");
    addresses[i] = start.getInt() + offset.getInt();
    StringRef registerName = i == 2 ? "nsectorReg" : i == 3 ? "max_req_lenReg" : words[i];
    unsigned matches = 0;
    for (auto reg : bank.getOps<RegOp>()) if (reg.getName() == registerName) {
      if (i < 2 || pulse(i) || reg.getClockVal() != arg(0) || reg.getResult().getType() != uint(widths[i]))
        return reject("BlockDev sampled/held state width or host clock differs");
      states[i] = reg.getResult(); ++matches;
    }
    for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == registerName) {
      if ((i >= 2 && !pulse(i)) || reg.getClockVal() != arg(0) || reg.getResetSignal() != arg(1) ||
          reg.getResult().getType() != uint(widths[i]) || !constant(reg.getResetValue(), i < 2 ? 256 : 0))
        return reject("BlockDev latency/pulse reset policy differs from its driver protocol");
      states[i] = reg.getResult(); ++matches;
    }
    if (matches != 1) return reject("BlockDev register identity differs from its driver address");
  }
  unsigned reads[26]{}, updates[26]{}, valids[26]{}, readies[26]{}, outputs[11]{};
  for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto src = connect.getSrc();
    for (unsigned i = 0; i < 26; ++i) {
      if (isSlot(connect.getDest(),"read",i,"bits")) {
        auto pad = src.getDefiningOp<PadPrimOp>();
        if (!pad || pad.getAmount() != 32 ||
            (i == 2 || i == 3 ? !constant(pad.getInput(),0) : pad.getInput() != states[i]))
          return reject("BlockDev read slot differs from its register/permission identity");
        ++reads[i];
      }
      if (isSlot(connect.getDest(),"read",i,"valid") || isSlot(connect.getDest(),"write",i,"ready")) {
        if (!constant(src,1)) return reject("BlockDev MCR slots must always accept writes and provide reads");
        if (isSlot(connect.getDest(),"read",i,"valid")) ++valids[i]; else ++readies[i];
      }
      if (connect.getDest() == states[i]) {
        auto mux = src.getDefiningOp<MuxPrimOp>();
        auto bits = mux ? mux.getHigh().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
        if (!mux || !isSlot(mux.getSel(),"write",i,"valid") || !bits || bits.getLo() != 0 ||
            bits.getHi() != widths[i]-1 || !isSlot(bits.getInput(),"write",i,"bits"))
          return reject("BlockDev state update differs from its allocated write slot");
        Value sample = mux.getLow(); bool ok = sample == states[i];
        if (pulse(i)) ok = constant(sample,0);
        if (i == 4 || i == 10) ok = isField(sample,i == 4 ? 2 : 3,"valid");
        if (i >= 5 && i <= 8) ok = isField(sample,2,i == 5 ? "write" : i == 6 ? "offset" : i == 7 ? "len" : "tag","bits");
        if (i == 11 || i == 12) {
          auto half = sample.getDefiningOp<BitsPrimOp>();
          ok = half && half.getHi() == (i == 11 ? 63 : 31) && half.getLo() == (i == 11 ? 32 : 0) &&
              isField(half.getInput(),3,"data","bits");
        }
        if (i == 13) ok = isField(sample,3,"tag","bits");
        if (i == 19 || i == 22) ok = isField(sample,i == 19 ? 4 : 5,"ready");
        if (i == 23) {
          auto pending = sample.getDefiningOp<OrPrimOp>();
          ok = pending && isField(pending.getLhs(),2,"valid") && isField(pending.getRhs(),3,"valid");
        }
        if (i == 24 || i == 25) {
          auto stalled = sample.getDefiningOp<NotPrimOp>();
          ok = stalled && isField(stalled.getInput(),6,i == 24 ? "wAckStallN" : "rRespStallN");
        }
        if (!ok) return reject("BlockDev samples, data halves, holds or pulses differ from its driver protocol");
        ++updates[i];
      }
    }
    const unsigned ports[]{2,3,4,4,4,5,5,7,7,8,8}, slots[]{9,14,15,17,18,20,21,2,3,0,1};
    const char *members[]{"ready","ready","data","tag","valid","bits","valid","nsectors","max_req_len","read_latency","write_latency"};
    for (unsigned i = 0; i < 11; ++i) if (isField(connect.getDest(),ports[i],members[i],i == 2 || i == 3 ? "bits" : "")) {
      auto cat = src.getDefiningOp<CatPrimOp>();
      if (i == 2 ? !cat || cat.getLhs() != states[15] || cat.getRhs() != states[16] : src != states[slots[i]])
        return reject("BlockDev geometry/latency or queue output differs from its driver address");
      ++outputs[i];
    }
  }
  for (unsigned i = 0; i < 26; ++i)
    if (reads[i] != 1 || updates[i] != 1 || valids[i] != 1 || readies[i] != 1)
      return reject("BlockDev MMIO has missing or ambiguous register drivers");
  for (auto n : outputs) if (n != 1) return reject("BlockDev functional output binding is missing or ambiguous");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/blockdev.h\"\n#endif // GET_INCLUDES\n#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 26; ++i)
    out << "static_assert(offsetof(BLOCKDEVBRIDGEMODULE_struct, " << words[i] << ") == " << i
        << " * sizeof(uint64_t), \"invalid " << words[i] << "\");\n";
  out << "static_assert(sizeof(BLOCKDEVBRIDGEMODULE_struct) == 26 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // BLOCKDEVBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
         "registry.add_widget(new blockdev_t(\n  simif,\nBLOCKDEVBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 26; ++i) out << "    ." << words[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args,\n  " << trackers.getInt() << ",\n  " << latencyBits
      << "\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body",b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.blockDevHeader",b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(),raw.end());
  annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return success();
}
