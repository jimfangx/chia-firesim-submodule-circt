// See LICENSE for license details.
// Oracle: SimpleNICBridgeModule.genHeader, Widget.genConstructor and MCRIO.
// Input: allocated six-word normal-mode NIC bank and validated duplex CPU
// stream header. InstanceGraph, typed ports and SSA drivers establish the ABI.
// Consumes no annotations. All failures precede the sole OutputFile mutation;
// hardware, target identities, hierarchy, clocks and channel analyses survive.
#include "goldengate/NICHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareNICHeader(CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("SimpleNIC header needs retained annotations");
  DictionaryAttr output; unsigned outputIndex = 0, nics = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed SimpleNIC header annotation");
    if (cls == AnnotationClasses::BridgeIO && d.getAs<StringAttr>("widgetClass") ==
        "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (d.get("widgetConstructorKey")) return reject("SimpleNIC header requires the constructor-free normal-mode widget");
      ++nics;
    }
    if (cls == AnnotationClasses::OutputFile && d.getAs<StringAttr>("fileSuffix") == ".const.h") {
      if (output) return reject("ambiguous SimpleNIC driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  auto cpuHeader = output ? output.getAs<BoolAttr>("goldengate.cpuManagedStreamHeader") : BoolAttr();
  if (nics != 1 || !previous || !cpuHeader || !cpuHeader.getValue() || output.get("goldengate.nicHeader"))
    return reject("SimpleNIC needs one unextended driver header with validated CPU streams and one NIC");
  InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid SimpleNIC header top");
  auto live = [&](StringRef name) -> FModuleOp {
    auto *target = graph.lookup(StringAttr::get(circuit.getContext(), name));
    if (!target) return {};
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
    llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
    bool recursive = false;
    std::function<unsigned(circt::igraph::InstanceGraphNode *)> count = [&](auto *node) {
      if (active.count(node)) { recursive = true; return 0U; }
      auto it = counts.find(node); if (it != counts.end()) return it->second;
      active.insert(node); unsigned n = node == target;
      for (auto *record : *node) n = std::min(2U, n + count(record->getTarget()));
      active.erase(node); counts[node] = n; return n;
    };
    if (count(top) != 1 || recursive) return {};
    return dyn_cast<FModuleOp>(target->getModule().getOperation());
  };
  auto bank = live("GGNICMMIOBank"), decoder = live("GGControlAddressDecode");
  auto writes = live("GGControlWidgetWriteWrapper"), reads = live("GGControlReadDispatchWrapper");
  auto transportOut = live("GGCPUStreamRead"), transportIn = live("GGCPUStreamWrite");
  if (!bank || !decoder || !writes || !reads || !transportOut || !transportIn)
    return reject("SimpleNIC bank, decoder, bindings and duplex transports need unique live paths");
  auto binding = [&](FModuleOp m, StringRef key) -> DictionaryAttr {
    auto rows = m->getAttrOfType<ArrayAttr>(key); DictionaryAttr found;
    if (!rows) return {};
    for (auto attr : rows) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      if (!d || d.getAs<StringAttr>("port") != "nicBridge_ctrl") continue;
      if (found) return {}; found = d;
    }
    return found;
  };
  auto bound = binding(writes,"goldengate.controlWriteBindings");
  if (!bound || bound != binding(reads,"goldengate.controlReadBindings"))
    return reject("SimpleNIC read/write control identities differ");
  auto slave = bound.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("SimpleNIC control allocation is missing");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0; StringRef identity = name ? name.getValue() : StringRef();
  if (!name || name != bound.getAs<StringAttr>("name") ||
      !identity.consume_front("SimpleNICBridgeModule_") || identity.empty() ||
      identity.getAsInteger(10,widgetIndex) || !regionSlave || regionSlave != slave ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 24)
    return reject("invalid SimpleNIC widget identity or MMIO allocation");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPortType(0)) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63)
    return reject("SimpleNIC control address width is unsupported");
  uint64_t capacity = uint64_t(1) << *address.getWidth();
  if (uint64_t(start.getInt()) >= capacity || uint64_t(size.getInt()) > capacity - start.getInt())
    return reject("SimpleNIC MMIO allocation exceeds control address width");
  for (auto [i, attr] : llvm::enumerate(regions)) {
    if (i == uint64_t(slave.getInt())) continue;
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto other = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto s = d ? d.getAs<IntegerAttr>("start") : IntegerAttr();
    auto z = d ? d.getAs<IntegerAttr>("size") : IntegerAttr();
    if (!other || other == name || !s || s.getInt() < 0 || !z || z.getInt() <= 0 ||
        uint64_t(s.getInt()) >= capacity || uint64_t(z.getInt()) > capacity - s.getInt() ||
        (uint64_t(s.getInt()) < uint64_t(start.getInt()) + size.getInt() &&
         uint64_t(start.getInt()) < uint64_t(s.getInt()) + z.getInt()))
      return reject("ambiguous SimpleNIC MMIO allocation");
  }
  // Indices belong to each direction's shared transport catalog. Never use a
  // queue creator's local index, nor assume that NIC is the first stream.
  auto queues = live("GGNICCPUStreamQueuesWrapper");
  if (!queues) return reject("SimpleNIC CPU queues need a unique live owner");
  uint64_t indices[2]{}, depths[2]{};
  for (unsigned direction = 0; direction < 2; ++direction) {
    auto transport = direction ? transportIn : transportOut;
    auto catalog = transport->getAttrOfType<ArrayAttr>(direction ? "goldengate.sinkStreams" : "goldengate.sourceStreams");
    std::string streamName = "SIMPLENICBRIDGEMODULE_" + std::to_string(widgetIndex) +
        (direction ? "_from_cpu_stream" : "_to_cpu_stream");
    unsigned found = 0;
    if (!catalog) return reject("SimpleNIC CPU stream allocation is missing");
    for (auto [i, attr] : llvm::enumerate(catalog)) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto index = d ? d.getAs<IntegerAttr>("index") : IntegerAttr();
      auto stream = d ? d.getAs<StringAttr>("name") : StringAttr();
      if (!stream || !index || index.getInt() != int64_t(i)) return reject("malformed SimpleNIC CPU stream catalog");
      if (stream != streamName) continue;
      auto depth = d.getAs<IntegerAttr>("depth"), bytes = d.getAs<IntegerAttr>("widthBytes");
      if (!depth || depth.getInt() != 3072 || !bytes || bytes.getInt() != 64)
        return reject("SimpleNIC stream geometry differs from the implemented packet adapters");
      indices[direction] = i; depths[direction] = depth.getInt(); ++found;
    }
    if (found != 1) return reject("SimpleNIC stream identity is missing or ambiguous");
    unsigned queueCount = 0;
    for (auto inst : queues.getOps<InstanceOp>()) {
      auto info = inst->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
      if (!info || info.getAs<StringAttr>("name") != streamName) continue;
      if (info.getAs<StringAttr>("direction") != (direction ? "fromHostCPU" : "toHostCPU") ||
          !info.getAs<IntegerAttr>("depth") || info.getAs<IntegerAttr>("depth").getInt() != 3072 ||
          !info.getAs<IntegerAttr>("widthBytes") || info.getAs<IntegerAttr>("widthBytes").getInt() != 64)
        return reject("SimpleNIC live queue identity differs from transport allocation");
      FModuleOp queue;
      for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == inst.getModuleName()) queue = m;
      if (!queue || queue.getNumPorts() != 5 ||
          queue.getPortType(4) != UIntType::get(circuit.getContext(),12,false))
        return reject("SimpleNIC live queue occupancy width differs");
      unsigned memories = 0;
      for (auto mem : queue.getOps<MemOp>()) {
        if (mem.getDepth() != 3072 || mem.getDataType() != UIntType::get(circuit.getContext(),512,false) ||
            mem.getReadLatency() != 0 || mem.getWriteLatency() != 1)
          return reject("SimpleNIC live queue storage differs from driver capacity");
        ++memories;
      }
      if (memories != 1) return reject("SimpleNIC driver needs one memory per CPU stream queue");
      ++queueCount;
    }
    if (queueCount != 1) return reject("SimpleNIC live stream queue is missing or ambiguous");
  }
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(),w,false); };
  auto fieldType = [](Type t, StringRef n) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t); auto f = bundle ? bundle.getElement(n) : std::nullopt;
    return f ? f->type : Type();
  };
  const StringRef words[]{"macaddr_upper","macaddr_lower","rlimit_settings","pause_threshold","pause_times","done"};
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  const unsigned widths[]{0,1,48,8,8,8,16,16,16,1};
  if (!regs || regs.size() != 6 || bank.getNumPorts() != 11 || bank.getPortName(10) != "mcr" ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()))
    return reject("SimpleNIC header needs its six-word normal-mode bank");
  for (unsigned p = 0; p < 11; ++p) {
    if (bank.getPortDirection(p) != (p < 2 || p == 9 ? Direction::In : Direction::Out) ||
        (p && p < 10 && bank.getPortType(p) != uint(widths[p])))
      return reject("SimpleNIC configuration/status port ABI differs");
  }
  for (StringRef group : {"read","write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(10),group));
    if (!slots || slots.getNumElements() != 6 || fieldType(slots.getElementType(),"bits") != uint(32) ||
        fieldType(slots.getElementType(),"valid") != uint(1) || fieldType(slots.getElementType(),"ready") != uint(1))
      return reject("SimpleNIC MMIO requires six 32-bit handshake slots");
  }
  auto arg = [&](unsigned p) { return bank.getArgument(p); };
  auto slot = [&](Value v, StringRef g, unsigned i, StringRef n) {
    auto f = v ? v.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto s = f ? f.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto a = s ? s.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return f && f.getFieldName() == n && s && s.getIndex() == i && a && a.getFieldName() == g && a.getInput() == arg(10);
  };
  auto constant = [](Value v, uint64_t n) { auto c = v ? v.getDefiningOp<ConstantOp>() : ConstantOp();
    return c && c.getValue().getZExtValue() == n; };
  Value states[6]; uint64_t addresses[6];
  for (unsigned i = 0; i < 6; ++i) {
    auto d = dyn_cast<DictionaryAttr>(regs[i]);
    auto off = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto read = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
    auto write = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!d || d.getAs<StringAttr>("name") != words[i] || !off || off.getInt() != i*4 ||
        !read || read.getValue() != (i == 5) || !write || !write.getValue())
      return reject("SimpleNIC MMIO register order or permissions differ");
    addresses[i] = start.getInt() + off.getInt(); unsigned found = 0;
    for (auto reg : bank.getOps<RegOp>()) if (reg.getName() == words[i]) {
      if (reg.getClockVal() != arg(0) || reg.getResult().getType() != uint(i == 5 ? 1 : 32))
        return reject("SimpleNIC MMIO state width or host clock differs");
      states[i] = reg.getResult(); ++found;
    }
    if (found != 1) return reject("SimpleNIC MMIO requires unique unreset configuration/status state");
  }
  if (!bank.getOps<RegResetOp>().empty()) return reject("SimpleNIC MMIO state must remain unreset");
  unsigned updates[6]{}, readbacks[6]{}, valids[6]{}, readies[6]{}, assertions[5]{}, outputs[7]{};
  auto bits = [](Value v, Value source, unsigned hi, unsigned lo) { auto b = v ? v.getDefiningOp<BitsPrimOp>() : BitsPrimOp();
    return b && b.getInput() == source && b.getHi() == hi && b.getLo() == lo; };
  for (auto op : bank.getOps<StrictConnectOp>()) {
    auto dest = op.getDest(), src = op.getSrc();
    for (unsigned i = 0; i < 6; ++i) {
      if (dest == states[i]) {
        auto mux = src.getDefiningOp<MuxPrimOp>();
        auto data = mux ? mux.getHigh() : Value();
        auto low = data ? data.getDefiningOp<BitsPrimOp>() : BitsPrimOp();
        if (!mux || !slot(mux.getSel(),"write",i,"valid") ||
            mux.getLow() != (i == 5 ? arg(9) : states[i]) ||
            (i == 5 ? (!low || low.getHi() || low.getLo() || !slot(low.getInput(),"write",i,"bits")) :
                      !slot(data,"write",i,"bits")))
          return reject("SimpleNIC write/sample/hold behavior differs from driver ABI");
        ++updates[i];
      }
      if (slot(dest,"read",i,"bits")) {
        auto pad = src.getDefiningOp<PadPrimOp>();
        if (i == 5 ? (!pad || pad.getAmount() != 32 || pad.getInput() != states[5]) : !constant(src,0))
          return reject("SimpleNIC readback behavior differs");
        ++readbacks[i];
      }
      if (slot(dest,"read",i,"valid")) { if (!constant(src,1)) return reject("SimpleNIC read valid differs"); ++valids[i]; }
      if (slot(dest,"write",i,"ready")) { if (!constant(src,1)) return reject("SimpleNIC write ready differs"); ++readies[i]; }
    }
    for (unsigned i = 0; i < 7; ++i) if (dest == arg(i+2)) {
      bool good = false;
      if (!i) { auto cat = src.getDefiningOp<CatPrimOp>();
        good = cat && bits(cat.getLhs(),states[0],15,0) && cat.getRhs() == states[1]; }
      else { const unsigned registerIndex[]{0,2,2,2,3,4,4};
        const unsigned hi[]{0,23,15,7,15,15,31}, lo[]{0,16,8,0,0,0,16};
        good = bits(src,states[registerIndex[i]],hi[i],lo[i]); }
      if (!good) return reject("SimpleNIC MAC/rate limiter/pause packing differs");
      ++outputs[i];
    }
  }
  for (auto a : bank.getOps<AssertOp>()) {
    auto pred = a.getPredicate().getDefiningOp<NotPrimOp>(), enable = a.getEnable().getDefiningOp<NotPrimOp>();
    if (a.getClock() != arg(0) || !pred || !enable || enable.getInput() != arg(1))
      return reject("SimpleNIC write-only assertion clock/reset differs");
    bool found = false;
    for (unsigned i = 0; i < 5; ++i) if (slot(pred.getInput(),"read",i,"ready")) { ++assertions[i]; found = true; }
    if (!found) return reject("SimpleNIC write-only assertion predicate differs");
  }
  for (unsigned i = 0; i < 6; ++i)
    if (updates[i] != 1 || readbacks[i] != 1 || valids[i] != 1 || readies[i] != 1 || (i < 5 && assertions[i] != 1))
      return reject("SimpleNIC register drivers/assertions are missing or ambiguous");
  for (auto n : outputs) if (n != 1) return reject("SimpleNIC control output driver is missing or ambiguous");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/simplenic.h\"\n#endif // GET_INCLUDES\n#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i = 0; i < 6; ++i)
    out << "static_assert(offsetof(SIMPLENICBRIDGEMODULE_struct, " << words[i] << ") == " << i
        << " * sizeof(uint64_t), \"invalid " << words[i] << "\");\n";
  out << "static_assert(sizeof(SIMPLENICBRIDGEMODULE_struct) == 6 * sizeof(uint64_t), \"invalid structure\");\n"
      "#endif // SIMPLENICBRIDGEMODULE_checks\n#ifdef GET_BRIDGE_CONSTRUCTOR\n"
      "registry.add_widget(new simplenic_t(\n  simif,\n  *registry.get_stream_engine(),\nSIMPLENICBRIDGEMODULE_struct{\n";
  for (unsigned i = 0; i < 6; ++i) out << "    ." << words[i] << " = " << addresses[i] << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args,\n  " << indices[0] << ",\n  " << depths[0]
      << ",\n  " << indices[1] << ",\n  " << depths[1] << "\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body",b.getStringAttr(previous.getValue().str()+snippet)); updated.set("goldengate.nicHeader",b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(),raw.end()); annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return success();
}
