// See LICENSE for license details.
// Oracle: TracerVBridgeModule.genHeader, RationalClock.toC, Widget.genConstructor.
// Required input invariants: live Rocket trigger bank/token engine, 6144-beat
// stream queue, single-stream CPU transport and allocated host control graph.
// Annotations consumed: none; reads BridgeIO, ChannelClockInfo and OutputFile.
// Annotations produced: extends the sole .const.h OutputFile with driver ABI.
// IR mutations: only retained output annotation body and completion marker.
// Analyses required: InstanceGraph and resolved bridge target identity.
// Analyses preserved: all hardware, channel, clock and hierarchy semantics.
// Output invariants: one checked constructor; rejection precedes mutation.
#include "goldengate/TracerVHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
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
LogicalResult goldengate::prepareTracerVHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("TracerVBridge header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed TracerVBridge header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.tracervHeader"))
    return reject("TracerVBridge needs one driver header without a prior TracerV constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid TracerVBridge header top");
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
  auto bank = liveModule("GGTracerVTriggerConfig");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("TracerVBridge bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "tracerv_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("TracerVBridge read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing TracerVBridge control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("TracerVBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 60)
    return reject("invalid TracerVBridge widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("TracerVBridge MMIO region exceeds control address width");
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
      return reject("ambiguous TracerVBridge MMIO allocation");
  }
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  auto engine = liveModule("GGTracerVTokenEngine");
  auto queue = liveModule("GGTracerVStreamQueue6144");
  auto transport = liveModule("GGCPUStreamRead");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto width = [&](StringRef n, int64_t v) {
    auto a = key ? key.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == v;
  };
  if (!engine || !queue || !transport || !key || key != bank->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") ||
      key.getAs<StringAttr>("class") != "firechip.bridgeinterfaces.TraceBundleWidths" ||
      !width("retireWidth",1) || !width("iaddrWidth",40) || !width("insnWidth",32) ||
      !width("causeWidth",64) || !width("tvalWidth",40) || engine.getNumPorts() != 5 ||
      engine.getPortName(2) != "hPort" || engine.getPortName(4) != "stream" ||
      fieldType(fieldType(engine.getPortType(2),"hBits"),"tiletrace_trace_retiredinsns_0_iaddr") != uint(40) ||
      fieldType(engine.getPortType(4),"bits") != uint(512))
    return reject("TracerV header needs the live Rocket trace constructor and 512-bit stream");
  DictionaryAttr bridge, clockMap;
  for (auto attr : raw) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    if (d.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO &&
        d.getAs<StringAttr>("widgetClass") == "firechip.goldengateimplementations.TracerVBridgeModule") {
      if (bridge) return reject("ambiguous TracerV bridge identity"); bridge = d;
    }
    if (d.getAs<StringAttr>("class") == AnnotationClasses::ChannelClockInfo) {
      if (clockMap) return reject("ambiguous channel clock analysis"); clockMap = d;
    }
  }
  auto clock = bridge ? bridge.getAs<DictionaryAttr>("clockInfo") : DictionaryAttr();
  auto clockName = clock ? clock.getAs<StringAttr>("name") : StringAttr();
  auto multiplier = clock ? clock.getAs<IntegerAttr>("multiplier") : IntegerAttr();
  auto divisor = clock ? clock.getAs<IntegerAttr>("divisor") : IntegerAttr();
  auto target = bridge ? bridge.getAs<StringAttr>("target") : StringAttr();
  auto resolved = target ? resolveAnnotationTarget(circuit,target.getValue(),error) : std::nullopt;
  auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr();
  auto infoMap = clockMap ? clockMap.getAs<DictionaryAttr>("infoMap") : DictionaryAttr();
  if (!bridge || bridge.getAs<DictionaryAttr>("widgetConstructorKey") != key || !resolved ||
      resolved->module != engine || !resolved->port || *resolved->port != 2 || resolved->fieldID != 0 ||
      !clockName || clockName.getValue().empty() || clockName.getValue().contains('\0') ||
      !multiplier || multiplier.getInt() <= 0 || multiplier.getInt() > UINT32_MAX ||
      !divisor || divisor.getInt() <= 0 || divisor.getInt() > UINT32_MAX || !mapping || mapping.size() != 12 || !infoMap)
    return reject("TracerV driver needs a resolved engine HostPort and rational clock analysis");
  for (auto member : mapping) {
    auto channel = dyn_cast<StringAttr>(member.getValue());
    if (!channel || infoMap.getAs<DictionaryAttr>(channel.getValue()) != clock)
      return reject("TracerV driver clock differs from its analyzed channels");
  }
  auto stream = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
  auto index = stream ? stream.getAs<IntegerAttr>("index") : IntegerAttr();
  auto depth = stream ? stream.getAs<IntegerAttr>("depth") : IntegerAttr();
  auto bytes = stream ? stream.getAs<IntegerAttr>("widthBytes") : IntegerAttr();
  auto streamName = stream ? stream.getAs<StringAttr>("name") : StringAttr();
  auto space = transport->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits");
  // CPUStreamRead currently implements only stream zero at address window zero.
  if (!index || index.getInt() != 0 || !depth || depth.getInt() != 6144 || !bytes || bytes.getInt() != 64 ||
      !streamName || streamName.getValue() != "TRACERVBRIDGEMODULE_" + std::to_string(widgetIndex) + "_to_cpu_stream" ||
      !space || space.getInt() != 19 || queue.getNumPorts() != 5 ||
      fieldType(queue.getPortType(2),"bits") != uint(512) || fieldType(queue.getPortType(3),"bits") != uint(512) ||
      transport.getNumPorts() != 15 || fieldType(transport.getPortType(2),"bits") != uint(512))
    return reject("TracerV stream index, name, capacity or CPU transport differs from its driver");
  if (transport.getPortName(6) != "ar_bits_addr" || transport.getPortType(6) != uint(64) ||
      transport.getPortDirection(6) != Direction::In || transport.getPortType(12) != uint(512))
    return reject("TracerV CPU stream address/data boundary differs");
  unsigned grants = 0;
  for (auto eq : transport.getOps<EQPrimOp>()) {
    auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
    if (!bits || bits.getInput() != transport.getBodyBlock()->getArgument(6)) continue;
    auto zero = eq.getRhs().getDefiningOp<ConstantOp>();
    if (bits.getHi() != 63 || bits.getLo() != space.getInt() || !zero || !zero.getValue().isZero())
      return reject("TracerV CPU stream grant does not select stream zero");
    ++grants;
  }
  if (grants != 1) return reject("TracerV CPU stream has missing or ambiguous address grant");
  unsigned memories = 0;
  for (auto mem : queue.getOps<MemOp>()) {
    if (mem.getDepth() != uint64_t(depth.getInt()) || mem.getDataType() != uint(512) ||
        mem.getReadLatency() != 0 || mem.getWriteLatency() != 1)
      return reject("TracerV stream capacity differs from its native memory");
    ++memories;
  }
  if (memories != 1) return reject("TracerV stream requires one live queue memory");
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 15 || bank.getNumPorts() != 5 || bank.getPortName(4) != "mcr" ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1) ||
      bank.getPortType(3) != engine.getPortType(3) ||
      fieldType(bank.getPortType(2),"valid") != uint(1) || fieldType(bank.getPortType(2),"iaddr") != uint(40) ||
      fieldType(bank.getPortType(2),"insn") != uint(32))
    return reject("TracerV driver requires the fifteen-word trigger MCR bank");
  for (unsigned i = 0; i < 5; ++i)
    if (bank.getPortDirection(i) != (i == 4 ? Direction::Out : Direction::In))
      return reject("TracerV MCR bank directions differ");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(4), group));
    if (!slots || slots.getNumElements() != 15 || fieldType(slots.getElementType(),"bits") != uint(32))
      return reject("TracerV MMIO addresses require fifteen 32-bit slots");
  }
  const char *words[]{"initDone", "traceEnable", "hostTriggerPCStartHigh", "hostTriggerPCStartLow",
      "hostTriggerPCEndHigh", "hostTriggerPCEndLow", "hostTriggerCycleCountStartHigh", "hostTriggerCycleCountStartLow",
      "hostTriggerCycleCountEndHigh", "hostTriggerCycleCountEndLow", "hostTriggerStartInst", "hostTriggerStartInstMask",
      "hostTriggerEndInst", "hostTriggerEndInstMask", "triggerSelector"};
  const unsigned widths[]{1,1,8,32,8,32,32,32,32,32,32,32,32,32,32};
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto isSlot = [&](Value v, StringRef groupName, unsigned index, StringRef member) {
    auto f = v ? v.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto s = f ? f.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto g = s ? s.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return f && f.getFieldName() == member && s && s.getIndex() == index &&
        g && g.getFieldName() == groupName && g.getInput() == arg(4);
  };
  Value states[15]; uint64_t addresses[15];
  for (unsigned i = 0; i < 15; ++i) {
    auto d = dyn_cast<DictionaryAttr>(registers[i]);
    auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!d || d.getAs<StringAttr>("name") != words[i] || !offset || offset.getInt() != i * 4 ||
        !readable || readable.getValue() != (i < 2) || !writeable || !writeable.getValue())
      return reject("TracerV register order/permissions differ from its driver ABI");
    addresses[i] = start.getInt() + offset.getInt();
    unsigned matches = 0;
    for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == words[i]) {
      auto init = reg.getResetValue().getDefiningOp<ConstantOp>();
      if (reg.getClockVal() != arg(0) || reg.getResetSignal() != arg(1) ||
          reg.getResult().getType() != uint(widths[i]) || !init || init.getValue() != (i == 1 ? 1 : 0))
        return reject("TracerV register width, host clock or reset differs");
      states[i] = reg.getResult(); ++matches;
    }
    if (matches != 1) return reject("TracerV register identity differs from its allocated word");
  }
  unsigned readsCount[15]{}, updates[15]{}, valids[15]{}, readies[15]{};
  for (auto connect : bank.getOps<StrictConnectOp>()) for (unsigned i = 0; i < 15; ++i) {
    auto src = connect.getSrc();
    if (isSlot(connect.getDest(),"read",i,"bits")) {
      auto pad = src.getDefiningOp<PadPrimOp>(); auto zero = src.getDefiningOp<ConstantOp>();
      if (i < 2 ? !pad || pad.getInput() != states[i] || pad.getAmount() != 32 :
          !zero || !zero.getValue().isZero()) return reject("TracerV read slot differs from its register permissions");
      ++readsCount[i];
    }
    if (connect.getDest() == states[i]) {
      auto mux = src.getDefiningOp<MuxPrimOp>();
      auto bits = mux ? mux.getHigh().getDefiningOp<BitsPrimOp>() : BitsPrimOp();
      if (!mux || !isSlot(mux.getSel(),"write",i,"valid") || mux.getLow() != states[i] ||
          !bits || bits.getLo() != 0 || bits.getHi() != widths[i]-1 || !isSlot(bits.getInput(),"write",i,"bits"))
        return reject("TracerV register update differs from its allocated write slot");
      ++updates[i];
    }
    if (isSlot(connect.getDest(),"read",i,"valid") || isSlot(connect.getDest(),"write",i,"ready")) {
      auto one = src.getDefiningOp<ConstantOp>();
      if (!one || !one.getValue().isOne()) return reject("TracerV MCR slots must always be available");
      if (isSlot(connect.getDest(),"read",i,"valid")) ++valids[i]; else ++readies[i];
    }
  }
  for (unsigned i = 0; i < 15; ++i)
    if (readsCount[i] != 1 || updates[i] != 1 || valids[i] != 1 || readies[i] != 1)
      return reject("TracerV register has missing or ambiguous read/write drivers");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/tracerv.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 15; ++i)
    out << "static_assert(offsetof(TRACERVBRIDGEMODULE_struct, " << words[i] << ") == " << i
        << " * sizeof(uint64_t), \"invalid " << words[i] << "\");\n";
  out << "static_assert(sizeof(TRACERVBRIDGEMODULE_struct) == 15 * sizeof(uint64_t), \"invalid structure\");\n"
      "#endif // TRACERVBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
      "registry.add_widget(new tracerv_t(\n  simif,\n  *registry.get_stream_engine(),\nTRACERVBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 15; ++i) out << "    ." << words[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args,\n  " << index.getInt() << ",\n  " << depth.getInt()
      << ",\n  " << key.getAs<IntegerAttr>("retireWidth").getInt() << ",\n  ClockInfo{\"";
  // Escape arbitrary clock names as C++ bytes, including quote and backslash.
  for (unsigned char ch : clockName.getValue()) {
    if (ch == '"' || ch == '\\') { out << '\\' << char(ch); }
    else if (ch >= 32 && ch < 127) out << char(ch);
    else { out << '\\' << char('0' + ((ch >> 6) & 7)) << char('0' + ((ch >> 3) & 7)) << char('0' + (ch & 7)); }
  }
  out << "\", " << multiplier.getInt() << "U, " << divisor.getInt() << "U}\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body",b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.tracervHeader",b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(),raw.end()); annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return success();
}
