// See LICENSE for license details.
// Oracle: FASEDMemoryTimingModel.genHeader and MCRFileMap.genAddressMap.
// Input: recorded ten-flight 21-word profile, retained constructor, unique live
// typed MMIO/engine/control paths and allocated region. Read rawAnnotations;
// consume none. Append only to the sole .const.h OutputFile after preflight.
// InstanceGraph and SSA connections establish identity, register order, access
// permissions and target AXI address width. Hardware and other annotations are
// preserved. No cached analysis is invalidated; rejection is atomic.
#include "goldengate/FASEDHeader.h"
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
LogicalResult goldengate::prepareFASEDHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("FASEDMemoryTimingModel header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed FASEDMemoryTimingModel header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.fasedHeader"))
    return reject("FASEDMemoryTimingModel needs one driver header without a prior FASED constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid FASEDMemoryTimingModel header top");
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
  auto bank = liveModule("GGFASEDMMIOWrapper");
  auto engine = liveModule("GGFASEDTokenEngine");
  auto decoder = liveModule("GGControlAddressDecode");
  auto bound = liveModule("GGFASEDBridgeBoundWrapper");
  auto control = liveModule("GGFASEDBridgeControlWrapper");
  auto adapter = liveModule("GGFASEDMCRFile");
  if (!bank || !engine || !decoder || !bound || !control || !adapter)
    return reject("FASED header needs unique live engine, MMIO, MCRFile and control binding paths");
  auto slave = bound->getAttrOfType<IntegerAttr>("goldengate.fasedSlave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing FASEDMemoryTimingModel control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (!identity.consume_front("FASEDMemoryTimingModel_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 128 || !size || size.getInt() != 128)
    return reject("invalid FASEDMemoryTimingModel widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("FASEDMemoryTimingModel MMIO region exceeds control address width");
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
      return reject("ambiguous FASEDMemoryTimingModel MMIO allocation");
  }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  auto port = [](FModuleOp m, StringRef name) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(m.getPorts())) if (p.name == name) return i;
    return std::nullopt;
  };
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto addrBits = widths ? widths.getAs<IntegerAttr>("addrBits") : IntegerAttr();
  auto dataBits = widths ? widths.getAs<IntegerAttr>("dataBits") : IntegerAttr();
  auto idBits = widths ? widths.getAs<IntegerAttr>("idBits") : IntegerAttr();
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  auto timing = port(engine, "timing");
  auto hPort = port(engine, "hPort");
  if (!key || key.getAs<StringAttr>("class") != "firesim.lib.bridges.CompleteConfig" ||
      !addrBits || addrBits.getInt() < 1 || addrBits.getInt() > 62 ||
      !dataBits || dataBits.getInt() != 64 || !idBits || idBits.getInt() != 4 ||
      !flight || flight.getInt() != 10 || !timing || !hPort ||
      engine.getPortDirection(*timing) != Direction::Out || engine.getPortDirection(*hPort) != Direction::In)
    return reject("FASED header requires retained ten-flight constructor and target AXI types");
  // genHeader uses the target Nasti address space, before host translation.
  Type target = fieldType(fieldType(engine.getPortType(*hPort), "hBits"), "axi4");
  for (Type axi : {engine.getPortType(*timing), target}) {
    for (StringRef channel : {"aw", "ar"}) {
      Type bits = fieldType(fieldType(axi, channel), "bits");
      if (fieldType(bits, "addr") != uint(addrBits.getInt()) || fieldType(bits, "id") != uint(idBits.getInt()))
        return reject("FASED memory size differs from typed target AW/AR address width");
    }
    for (StringRef channel : {"w", "r"})
      if (fieldType(fieldType(axi, channel), "bits") == Type() ||
          fieldType(fieldType(fieldType(axi, channel), "bits"), "data") != uint(dataBits.getInt()))
        return reject("FASED data width differs from retained constructor");
  }
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(32)}});
  auto mcr = [&](unsigned n) { auto words = FVectorType::get(token, n);
    return BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
        {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}}); };
  auto aggregatePort = port(bank, "fasedBridge_mcr");
  if (!aggregatePort || bank.getPortDirection(*aggregatePort) != Direction::Out ||
      bank.getPortType(*aggregatePort) != mcr(21) || adapter.getNumPorts() != 4 ||
      adapter.getPortName(3) != "mcr" || adapter.getPortType(3) != mcr(21) ||
      adapter.getPortDirection(3) != Direction::In)
    return reject("FASED AddressMap needs 21 typed 32-bit MCR lanes");
  const StringRef names[]{"writeLatency", "readLatency", "writeMaxReqs", "readMaxReqs",
      "writeOutstandingHistogram_0", "writeOutstandingHistogram_1", "writeOutstandingHistogram_2",
      "writeOutstandingHistogram_3", "writeOutstandingHistogram_4", "readOutstandingHistogram_0",
      "readOutstandingHistogram_1", "readOutstandingHistogram_2", "readOutstandingHistogram_3",
      "readOutstandingHistogram_4", "totalWriteBeats", "totalReadBeats", "totalWrites", "totalReads",
      "relaxFunctionalModel", "rrespError", "brespError"};
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != 21)
    return reject("FASED AddressMap needs the complete register catalog");
  for (auto [i, attr] : llvm::enumerate(registers)) {
    auto word = dyn_cast<DictionaryAttr>(attr);
    auto offset = word ? word.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto read = word ? word.getAs<BoolAttr>("readable") : BoolAttr();
    auto write = word ? word.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!word || word.getAs<StringAttr>("name") != names[i] || !offset || offset.getInt() != 4 * i ||
        !read || !read.getValue() || !write || write.getValue() != (i < 4 || i == 18) ||
        uint64_t(start.getInt()) + offset.getInt() > UINT32_MAX)
      return reject("FASED register names, offsets, permissions or uint32_t addresses differ");
  }
  // Verify the six native banks are the source of the aggregate lanes. Walk
  // stateless aggregate forwarding through the retained wrapper hierarchy.
  auto arg = [](FModuleOp m, unsigned i) { return m.getArgument(i); };
  auto trace = [&](Value startValue, Value expected) {
    llvm::DenseSet<Value> seen;
    std::function<bool(Value)> follow = [&](Value value) {
      if (value == expected) return true;
      if (!value || !seen.insert(value).second) return false;
      Value driver; unsigned drivers = 0;
      for (auto &op : *value.getParentBlock()) {
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
        unsigned n = cast<OpResult>(value).getResultNumber();
        return m && m.getPortDirection(n) == Direction::Out && follow(arg(m, n));
      }
      return false;
    };
    return follow(startValue);
  };
  const StringRef modules[]{"GGFASEDLatencyRegisters", "GGFASEDRequestLimits", "GGFASEDHistograms",
      "GGFASEDStatistics", "GGFASEDFunctionalModelRegister", "GGFASEDResponseErrors"};
  const StringRef fragments[]{"fased_latency_mcr", "fased_request_limits_mcr", "fased_histograms_mcr",
      "fased_statistics_mcr", "fased_functional_model_mcr", "fased_response_errors_mcr"};
  const unsigned starts[]{0, 2, 4, 14, 18, 19}, lengths[]{2, 2, 10, 4, 1, 2};
  Value aggregate = arg(bank, *aggregatePort);
  auto lane = [](Value v, Value base, StringRef group, unsigned index) {
    auto slot = v.getDefiningOp<SubindexOp>();
    auto field = slot ? slot.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return slot && slot.getIndex() == index && field && field.getFieldName() == group && field.getInput() == base;
  };
  InstanceOp inner;
  for (auto inst : bank.getOps<InstanceOp>()) if (inst.getModuleName() == "GGFASEDHistogramsWrapper") {
    if (inner) return reject("ambiguous FASED bank forwarding instance"); inner = inst;
  }
  if (!inner) return reject("FASED bank lost its native fragments");
  for (unsigned j = 0; j < 6; ++j) {
    auto fragmentBank = liveModule(modules[j]);
    auto fragmentPort = fragmentBank ? port(fragmentBank, "mcr") : std::nullopt;
    std::optional<unsigned> innerPort;
    for (auto [i, name] : llvm::enumerate(inner.getPortNames()))
      if (cast<StringAttr>(name).getValue() == fragments[j]) innerPort = i;
    auto fragmentRows = fragmentBank ? fragmentBank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters") : ArrayAttr();
    if (!fragmentBank || !fragmentPort || !innerPort || fragmentBank.getPortType(*fragmentPort) != mcr(lengths[j]) ||
        fragmentBank.getPortDirection(*fragmentPort) != Direction::Out || !fragmentRows || fragmentRows.size() != lengths[j] ||
        !trace(inner.getResult(*innerPort), arg(fragmentBank, *fragmentPort)))
      return reject("FASED catalog does not forward the unique live register fragments");
    Value fragment = inner.getResult(*innerPort);
    for (unsigned k = 0; k < lengths[j]; ++k) {
      if (fragmentRows[k] != registers[starts[j] + k]) return reject("FASED aggregate and fragment catalogs differ");
      unsigned reads = 0, writes = 0;
      for (auto c : bank.getOps<ConnectOp>()) {
        if (lane(c.getDest(), aggregate, "read", starts[j] + k)) {
          if (!lane(c.getSrc(), fragment, "read", k)) return reject("FASED read lane does not match its AddressMap");
          ++reads;
        }
        if (lane(c.getDest(), fragment, "write", k)) {
          if (!lane(c.getSrc(), aggregate, "write", starts[j] + k)) return reject("FASED write lane does not match its AddressMap");
          ++writes;
        }
      }
      if (reads != 1 || writes != 1) return reject("missing or ambiguous FASED register lane forwarding");
    }
  }
  if (!trace(arg(adapter, 3), aggregate)) return reject("FASED MCRFile is disconnected from the advertised bank");
  auto controlPort = port(control, "fasedBridge_ctrl");
  if (!controlPort || control.getPortDirection(*controlPort) != Direction::In)
    return reject("FASED header needs a typed host control port");
  for (StringRef channel : {"aw", "ar"})
    if (fieldType(fieldType(fieldType(control.getPortType(*controlPort), channel), "bits"), "addr") != address)
      return reject("FASED control allocation and AW/AR widths differ");
  if (adapter.getPortType(2) != control.getPortType(*controlPort) ||
      !trace(arg(adapter, 2), arg(control, *controlPort)))
    return reject("FASED host control is disconnected from its MCRFile");
  unsigned awSelectors = 0, arSelectors = 0;
  for (auto slice : adapter.getOps<BitsPrimOp>()) {
    auto addr = slice.getInput().getDefiningOp<SubfieldOp>();
    auto bits = addr ? addr.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto request = bits ? bits.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    if (!addr || addr.getFieldName() != "addr" || !bits || bits.getFieldName() != "bits" ||
        !request || request.getInput() != arg(adapter, 2)) continue;
    if ((request.getFieldName() != "aw" && request.getFieldName() != "ar") ||
        slice.getHi() != 6 || slice.getLo() != 2)
      return reject("FASED MCRFile local word selection differs from the 128-byte allocation");
    if (request.getFieldName() == "aw") ++awSelectors; else ++arSelectors;
  }
  if (awSelectors != 1 || arSelectors != 1)
    return reject("FASED MCRFile needs one five-bit AW and AR word selector");
  // Both request channels must select the allocated slave, and both response
  // ready signals must return from that slave's arbiters.
  std::function<std::string(Value)> path = [&](Value v) -> std::string {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return path(f.getInput()) + "." + f.getFieldName().str();
    if (auto a = dyn_cast<BlockArgument>(v)) return bound.getPortName(a.getArgNumber()).str();
    if (auto inst = v.getDefiningOp<InstanceOp>())
      return inst.getName().str() + "." + cast<StringAttr>(inst.getPortNames()[cast<OpResult>(v).getResultNumber()]).getValue().str();
    return {};
  };
  auto route = [&](StringRef dest, StringRef src) {
    unsigned matches = 0;
    for (auto &op : *bound.getBodyBlock()) {
      Value d, s;
      if (auto c = dyn_cast<ConnectOp>(op)) { d = c.getDest(); s = c.getSrc(); }
      if (auto c = dyn_cast<StrictConnectOp>(op)) { d = c.getDest(); s = c.getSrc(); }
      if (d && path(d) == dest) { if (path(s) != src) return false; ++matches; }
    }
    return matches == 1;
  };
  std::string si = std::to_string(slave.getInt());
  unsigned controlInstances = 0;
  for (auto inst : bound.getOps<InstanceOp>()) if (inst.getModuleName() == control.getName() && inst.getName() == "sim") ++controlInstances;
  if (controlInstances != 1 ||
      !route("sim.fasedBridge_ctrl.aw.bits.addr", "sim.ctrl_write_dispatch_slave_" + si + "_aw_bits_addr") ||
      !route("sim.fasedBridge_ctrl.ar", "sim.ctrl_read_dispatch_slave_" + si + "_ar") ||
      !route("sim.fasedBridge_ctrl.r.ready", "sim.ctrl_read_arb_in_" + si + "_ready") ||
      !route("sim.fasedBridge_ctrl.b.ready", "sim.ctrl_write_arb_in_" + si + "_ready"))
    return reject("FASED control request/response routes disagree with the allocated slave");

  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/fased_memory_timing_model.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_BRIDGE_CONSTRUCTOR\nregistry.add_widget(new FASEDMemoryTimingModel(\n"
         "  simif,\n  AddressMap{\n";
  for (bool write : {false, true}) {
    out << "    std::vector<std::pair<std::string, uint32_t>>{\n";
    for (auto attr : registers) {
      auto word = cast<DictionaryAttr>(attr);
      if (!word.getAs<BoolAttr>(write ? "writeable" : "readable").getValue()) continue;
      out << "      { std::string(\"" << word.getAs<StringAttr>("name").getValue() << "\"), "
          << start.getInt() + word.getAs<IntegerAttr>("offset").getInt() << "U },\n";
    }
    out << (write ? "    }\n" : "    },\n");
  }
  out << "  },\n  " << widgetIndex << ",\n  args,\n  std::string(\"memory_stats" << widgetIndex
      << ".csv\"),\n  1L << " << addrBits.getInt() << "U\n));\n#endif // GET_BRIDGE_CONSTRUCTOR\n";
  out.flush(); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.fasedHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end()); annotations[outputIndex] = updated.getDictionary(ctx);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return success();
}
