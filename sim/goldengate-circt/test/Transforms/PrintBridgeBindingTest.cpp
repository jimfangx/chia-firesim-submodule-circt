// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/PrintBridgeHeader.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/CPUStreamCountBank.h"
#include "goldengate/ControlAddressDecode.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <algorithm>
#include <random>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
unsigned port(FModuleOp m, StringRef name) {
  for (unsigned p = 0; p < m.getNumPorts(); ++p) if (m.getPortName(p) == name) return p;
  throw std::runtime_error("missing port " + name.str());
}
InstanceOp child(FModuleOp m, FModuleOp target) {
  InstanceOp found;
  for (auto i : m.getOps<InstanceOp>()) if (i.getModuleName() == target.getName()) {
    require(!found, "multiple child instances"); found = i;
  }
  require(bool(found), "missing child instance"); return found;
}
// The bridge anchor is intentionally unresolved: the constructor, not an
// assumed top-level signal named synthesizedPrintf, identifies the host.
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  SmallVector<FModuleOp> hosts;
  Fixture(MLIRContext &ctx, bool multipleRecords = false, bool reverseRecords = false) {
    unsigned fields = multipleRecords ? 5 : 3;
    std::string text = "module { firrtl.circuit \"Top\" { firrtl.module @Top(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>";
    for (unsigned d = 0; d < 2; ++d) for (unsigned f = 0; f < fields; ++f)
      text += ", out %source" + std::to_string(d) + "_" + std::to_string(f) +
          ": !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: " + std::string(f == 2 ? "sint<5>" : f == 4 ? "uint<3>" : "uint<1>") + ">";
    text += ", out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>, out %tracerv_stream_count: !firrtl.uint<13>, out %other: !firrtl.uint<8>) {} } }";
    root = parseSourceString<ModuleOp>(text, &ctx); require(bool(root), "fixture parse");
    circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    SmallVector<Attribute> raw;
    raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Opaque")),
        b.getNamedAttr("target", b.getStringAttr("~Top|Top>other")),
        b.getNamedAttr("nested", b.getArrayAttr({b.getStringAttr("preserve")}))}));
    for (unsigned d = 0; d < 2; ++d) {
      std::string reset = "reset" + std::to_string(d), record = "record" + std::to_string(d);
      auto entry = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(record)),
          b.getNamedAttr("format", b.getStringAttr("signed=%d\n")),
          b.getNamedAttr("ports", b.getArrayAttr({
              b.getDictionaryAttr({b.getNamedAttr("enable", b.getStringAttr("UInt<1>"))}),
              b.getDictionaryAttr({b.getNamedAttr("argument", b.getStringAttr("SInt<5>"))})}))});
      SmallVector<Attribute> records{entry};
      if (multipleRecords) {
        records.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(record + "extra")),
            b.getNamedAttr("format", b.getStringAttr("quote=\" slash=\\n question=?" "?/\n%03x")),
            b.getNamedAttr("ports", b.getArrayAttr({
                b.getDictionaryAttr({b.getNamedAttr("enable", b.getStringAttr("UInt<1>"))}),
                b.getDictionaryAttr({b.getNamedAttr("argument", b.getStringAttr("UInt<3>"))})}))}));
        if (reverseRecords) std::reverse(records.begin(), records.end());
      }
      auto key = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeParameters)),
          b.getNamedAttr("resetPortName", b.getStringAttr(reset)), b.getNamedAttr("printPorts", b.getArrayAttr(records))});
      NamedAttrList mapping;
      for (unsigned f = 0; f < fields; ++f) {
        auto local = f == 0 ? reset : record + (f > 2 ? "extra" : "") + (f == 1 || f == 3 ? "_enable" : "_argument");
        mapping.set(local, b.getStringAttr("global" + std::to_string(d) + "_" + std::to_string(f)));
      }
      raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
          b.getNamedAttr("target", b.getStringAttr("~Top|Top>synthesizedPrintf")),
          b.getNamedAttr("widgetClass", b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeModule)),
          b.getNamedAttr("clockInfo", b.getDictionaryAttr({
              b.getNamedAttr("name", b.getStringAttr("clock\"\\?\n")),
              b.getNamedAttr("multiplier", b.getI64IntegerAttr(d + 1)),
              b.getNamedAttr("divisor", b.getI64IntegerAttr(3))})),
          b.getNamedAttr("widgetConstructorKey", key), b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))}));
    }
    // Reverse annotation order and use global names unrelated to source ports.
    for (int d = 1; d >= 0; --d) for (int f = fields - 1; f >= 0; --f) {
      auto info = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
          b.getNamedAttr("latency", b.getI64IntegerAttr(0))});
      raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
          b.getNamedAttr("globalName", b.getStringAttr("global" + std::to_string(d) + "_" + std::to_string(f))),
          b.getNamedAttr("channelInfo", info), b.getNamedAttr("sources", b.getArrayAttr({b.getStringAttr("~Top|Top>source" + std::to_string(d) + "_" + std::to_string(f) + ".bits")})),
          b.getNamedAttr("sinks", b.getArrayAttr({}))}));
    }
    circuit->setAttr("rawAnnotations", b.getArrayAttr(raw));
    SmallVector<FModuleOp> payloads, stages, controls; std::string error;
    require(succeeded(goldengate::materializePrintBridgePayloads(circuit, payloads, error)), error);
    require(succeeded(goldengate::materializePrintBridgeTokenStages(circuit, payloads, stages, error)), error);
    require(succeeded(goldengate::materializePrintBridgeControls(circuit, payloads, stages, controls, error)), error);
    require(succeeded(goldengate::materializePrintBridgeHosts(circuit, controls, 25, 12, hosts, error)), error);
  }
};

struct Interpreter {
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> inputs;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) {
    for (auto &op : *m.getBodyBlock()) {
      if (auto c = dyn_cast<ConnectOp>(op)) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple binding drivers");
      if (auto c = dyn_cast<StrictConnectOp>(op)) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple binding drivers");
    }
  }
  uint64_t eval(Value v) {
    auto k = key(v); if (inputs.count(k)) return inputs.at(k);
    if (drivers.count(k)) return eval(drivers.at(k));
    auto *op = v.getDefiningOp();
    if (auto c = dyn_cast_or_null<ConstantOp>(op)) return c.getValue().getZExtValue();
    if (isa_and_nonnull<AndPrimOp>(op)) return eval(op->getOperand(0)) & eval(op->getOperand(1));
    if (isa_and_nonnull<OrPrimOp>(op)) return eval(op->getOperand(0)) | eval(op->getOperand(1));
    if (isa_and_nonnull<AsSIntPrimOp, AsUIntPrimOp, PadPrimOp>(op)) return eval(op->getOperand(0));
    throw std::runtime_error("unknown binding expression or missing source " + k);
  }
  Value field(FModuleOp m, Value base, StringRef name) {
    for (auto f : m.getOps<SubfieldOp>()) if (key(f.getInput()) == key(base) && f.getFieldName() == name) return f.getResult();
    throw std::runtime_error("missing subfield " + name.str());
  }
};

void checkBinding(MLIRContext &ctx, bool reverse, StringRef output) {
  Fixture f(ctx); if (reverse) std::reverse(f.hosts.begin(), f.hosts.end());
  auto old = named(f.circuit, "Top"); std::string oldText = dump(old), error;
  require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
  auto wrapper = named(f.circuit, "GGPrintBridgeHostWrapper");
  std::string header;
  auto beforeHeader = dump(*f.root);
  require(succeeded(goldengate::preparePrintBridgeDecoderHeader(f.circuit, f.hosts, header, error)), error);
  require(dump(*f.root) == beforeHeader && header.find("{1U, \"signed=%d\\012\", std::vector<unsigned>{5U}}") != std::string::npos &&
      header.find("1U, 254U, stream_index, 6144U") != std::string::npos &&
      header.find("ClockInfo{\"clock\\\"\\\\\\077\\012\", " + std::to_string(reverse ? 2 : 1) + "U, 3U}") != std::string::npos,
      "Print decoder layout, escaping or host order changed");
  if (!output.empty()) {
    std::error_code ec;
    llvm::raw_fd_ostream file((output.str() + ".h"), ec);
    require(!ec, "cannot write Print decoder header fixture"); file << header;
  }
  // A corrupt offset must never change the caller's last successful header.
  auto info = f.hosts[1]->getAttrOfType<DictionaryAttr>("goldengate.printHost");
  auto records = info.getAs<ArrayAttr>("records"); OpBuilder hb(&ctx);
  NamedAttrList corruptRecord(cast<DictionaryAttr>(records[0]));
  corruptRecord.set("offset", hb.getI64IntegerAttr(2)); NamedAttrList corruptInfo(info);
  corruptInfo.set("records", hb.getArrayAttr({corruptRecord.getDictionary(&ctx)}));
  f.hosts[1]->setAttr("goldengate.printHost", corruptInfo.getDictionary(&ctx));
  auto lastHeader = header, corruptIR = dump(*f.root);
  require(failed(goldengate::preparePrintBridgeDecoderHeader(f.circuit, f.hosts, header, error)) &&
      header == lastHeader && dump(*f.root) == corruptIR, "Print decoder rejection not transactional");
  f.hosts[1]->setAttr("goldengate.printHost", info);
  require(f.circuit.getName() == wrapper.getName() && wrapper.getNumPorts() == 11 && dump(old) == oldText && succeeded(verify(*f.root)), "invalid binding or changed original top");
  for (auto name : {"hostClock", "hostReset", "tracerv_stream", "tracerv_stream_count", "other"})
    require(wrapper.getPortType(port(wrapper, name)) == old.getPortType(port(old, name)), "lost nonprint port");
  auto target = child(wrapper, old); Interpreter sim(wrapper); std::mt19937_64 rng(15);
  unsigned evaluations = 0;
  for (unsigned slot = 0; slot < 2; ++slot) {
    unsigned d = reverse ? 1 - slot : slot; auto h = child(wrapper, f.hosts[slot]);
    auto prefix = "print_" + std::to_string(slot);
    require(wrapper.getPortType(port(wrapper, prefix + "_ctrl")) == f.hosts[slot].getPortType(11) &&
        wrapper.getPortDirection(port(wrapper, prefix + "_ctrl")) == Direction::In &&
        wrapper.getPortType(port(wrapper, prefix + "_stream_count")) == UIntType::get(&ctx, 13), "host slot port identity");
    auto streamType = cast<BundleType>(wrapper.getPortType(port(wrapper, prefix + "_stream")));
    require(streamType.getElement("bits")->type == UIntType::get(&ctx, 512) && wrapper.getPortDirection(port(wrapper, prefix + "_stream")) == Direction::Out, "stream output shape");
    require(sim.drivers.at(sim.key(h.getResult(0))) == wrapper.getArgument(port(wrapper, "hostClock")) &&
        sim.drivers.at(sim.key(h.getResult(1))) == wrapper.getArgument(port(wrapper, "hostReset")), "host clock/reset binding");
    Value hb = h.getResult(3), reset = sim.field(wrapper, hb, "reset" + std::to_string(d));
    Value record = sim.field(wrapper, hb, "record" + std::to_string(d));
    Value enable = sim.field(wrapper, record, "enable"), argument = sim.field(wrapper, record, "argument");
    require(isa<SIntType>(argument.getType()), "signed argument type lost");
    for (unsigned flags = 0; flags < 8; ++flags) for (unsigned ready = 0; ready < 2; ++ready)
      for (unsigned data = 0; data < 32; ++data) {
        sim.inputs.clear(); sim.inputs[sim.key(h.getResult(4))] = ready;
        for (unsigned field = 0; field < 3; ++field) {
          auto channel = target.getResult(port(old, "source" + std::to_string(d) + "_" + std::to_string(field)));
          sim.inputs[sim.key(sim.field(wrapper, channel, "valid"))] = (flags >> field) & 1;
          sim.inputs[sim.key(sim.field(wrapper, channel, "bits"))] = field == 2 ? data : (rng() & 1);
        }
        require(sim.eval(h.getResult(2)) == (flags == 7), "hValid not conjunction of mapped channels");
        require(sim.eval(argument) == data, "signed argument bits changed");
        for (unsigned field = 0; field < 3; ++field) {
          auto channel = target.getResult(port(old, "source" + std::to_string(d) + "_" + std::to_string(field)));
          unsigned others = 7 ^ (1 << field);
          require(sim.eval(sim.field(wrapper, channel, "ready")) == (ready && (flags & others) == others), "channel ready fails exclusion-of-own-valid join");
        }
        auto c0 = target.getResult(port(old, "source" + std::to_string(d) + "_0"));
        auto c1 = target.getResult(port(old, "source" + std::to_string(d) + "_1"));
        require(sim.eval(reset) == sim.eval(sim.field(wrapper, c0, "bits")) &&
            sim.eval(enable) == sim.eval(sim.field(wrapper, c1, "bits")), "target reset/enable crossed channels");
        ++evaluations;
      }
  }
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned bridges = 0, channels = 0;
  for (auto a : raw) {
    auto dict = cast<DictionaryAttr>(a); auto klass = dict.getAs<StringAttr>("class").getValue();
    if (klass == goldengate::AnnotationClasses::BridgeIO) {
      require(dict.getAs<StringAttr>("target").getValue().contains("hBits"), "bridge target not bound to host payload"); ++bridges;
    }
    if (klass == goldengate::AnnotationClasses::ChannelConnection) {
      auto sources = dict.getAs<ArrayAttr>("sources"), sinks = dict.getAs<ArrayAttr>("sinks");
      require(sources.size() == 1 && sinks.size() == 1 && cast<StringAttr>(sources[0]).getValue().contains("|Top>source") &&
          cast<StringAttr>(sinks[0]).getValue().contains("hBits"), "bound channel annotation identity"); ++channels;
    }
    if (klass == "test.Opaque") require(dict.getAs<StringAttr>("target").getValue() == "~GGPrintBridgeHostWrapper|GGPrintBridgeHostWrapper>other" && dict.getAs<ArrayAttr>("nested").size() == 1, "copied annotations not retargeted/preserved");
  }
  require(bridges == 2 && channels == 6, "binding changed annotation record count");
  if (!output.empty() && !reverse) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec); require(!ec, ec.message()); f.root->print(out);
  }
  auto before = dump(*f.root);
  require(failed(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)) && !error.empty() && dump(*f.root) == before, "repeated bind not atomic");
  require(succeeded(goldengate::mapPrintBridgeCPUStreams(f.circuit,
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream", 6144}},
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream_count", 13}}, error)) && succeeded(verify(*f.root)), error);
  auto read = named(f.circuit, "GGCPUStreamRead");
  auto allocations = read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
  auto mmio = named(f.circuit, "GGCPUStreamCountBank")->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(allocations.size() == 3 && mmio.size() == 3 &&
      read->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt() == 19,
      "Print CPU allocation lost shared 512KiB stream windows");
  for (unsigned i = 0; i < 3; ++i) {
    std::string name = i == 0 ? "TRACERVBRIDGEMODULE_0_to_cpu_stream" :
        "PRINTBRIDGEMODULE_" + std::to_string(i - 1) + "_to_cpu_stream";
    auto allocation = cast<DictionaryAttr>(allocations[i]), row = cast<DictionaryAttr>(mmio[i]);
    require(allocation.getAs<StringAttr>("name").getValue() == name &&
        allocation.getAs<IntegerAttr>("index").getInt() == i &&
        allocation.getAs<IntegerAttr>("bufferBaseAddress").getInt() == i * 524288 &&
        row.getAs<StringAttr>("name").getValue() == name + "_count" &&
        row.getAs<IntegerAttr>("offset").getInt() == i * 4 &&
        row.getAs<BoolAttr>("readable").getValue() && !row.getAs<BoolAttr>("writeable").getValue(),
        "Print stream/count allocation order or permission differs");
  }
  SmallVector<goldengate::ControlMMIOWidget> widgets;
  std::string names[]{"PrintBridgeModule_0", "PrintBridgeModule_1"};
  for (unsigned i = 0; i < 2; ++i) {
    auto info = f.hosts[i]->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    goldengate::ControlMMIOWidget widget;
    require(succeeded(goldengate::deriveControlMMIOWidget(f.circuit, names[i],
        info.getAs<StringAttr>("mcrModule").getValue(),
        {info.getAs<StringAttr>("configModule").getValue()}, widget, error)) && widget.registerCount == 6, error);
    widgets.push_back(widget);
  }
  SmallVector<goldengate::ControlMMIORegion> regions;
  require(succeeded(goldengate::allocateControlMMIORegions(25, widgets, regions, error)) &&
      regions.size() == 2 && regions[0].start == 0 && regions[1].start == 32 &&
      regions[0].size == 32 && regions[1].size == 32, "Print six-word banks must allocate 32-byte regions");
  require(succeeded(goldengate::mapPrintBridgeControlDispatch(f.circuit, error)) && succeeded(verify(*f.root)), error);
  require(f.circuit.getName() == "GGControlReadDispatchWrapper", "Print MMIO composition stopped before AR binding");
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto allocated = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto writeBindings = named(f.circuit, "GGControlWidgetWriteWrapper")->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
  auto readBindings = named(f.circuit, "GGControlReadDispatchWrapper")->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings");
  require(allocated.size() == 3 && writeBindings == readBindings && readBindings.size() == 3,
      "Print and occupancy banks need matching request binding identities");
  for (unsigned i = 0; i < 3; ++i) {
    auto row = cast<DictionaryAttr>(allocated[i]), binding = cast<DictionaryAttr>(readBindings[i]);
    std::string name = i < 2 ? names[i] : "CPUManagedStreamEngine_0";
    std::string port = i < 2 ? "print_" + std::to_string(i) + "_ctrl" : "cpuStream_ctrl";
    require(row.getAs<StringAttr>("name").getValue() == name &&
        row.getAs<IntegerAttr>("start").getInt() == i * 32 &&
        row.getAs<IntegerAttr>("size").getInt() == (i < 2 ? 32 : 16) &&
        binding.getAs<StringAttr>("name").getValue() == name &&
        binding.getAs<StringAttr>("port").getValue() == port &&
        binding.getAs<IntegerAttr>("slave").getInt() == i,
        "Print MMIO allocation or request wiring lost registration identity");
    bool found = false;
    for (auto p : named(f.circuit, f.circuit.getName()).getPorts()) if (p.name == port) {
      auto type = cast<BundleType>(p.type); found = true;
      require(type.getNumElements() == 2 && type.getElement("b") && type.getElement("r"),
          "consumed Print MMIO request channel escaped the dispatcher");
    }
    require(found, "Print response boundary missing");
  }
  // Inspect actual operations, not just the catalog: each bank's AW/W
  // scalars and flipped AR bundle must connect to its allocated dispatcher.
  auto key = [&](Value value) -> std::string {
    std::string suffix;
    while (auto field = value.getDefiningOp<SubfieldOp>()) {
      suffix = "." + field.getFieldName().str() + suffix; value = field.getInput();
    }
    auto inst = value.getDefiningOp<InstanceOp>();
    require(bool(inst), "request binding is not an instance port");
    return inst.getName().str() + "." + inst.getPortNameStr(cast<OpResult>(value).getResultNumber()).str() + suffix;
  };
  std::map<std::string, std::string> writeConnections, readConnections;
  for (auto connect : named(f.circuit, "GGControlWidgetWriteWrapper").getOps<StrictConnectOp>()) {
    // Metadata sources are top arguments; request valid/data use instance results.
    if (!isa<BlockArgument>(connect.getSrc())) writeConnections.emplace(key(connect.getDest()), key(connect.getSrc()));
  }
  for (auto connect : named(f.circuit, "GGControlReadDispatchWrapper").getOps<ConnectOp>())
    if (connect.getDest().getDefiningOp<SubfieldOp>() && connect.getSrc().getDefiningOp<InstanceOp>())
      readConnections.emplace(key(connect.getDest()), key(connect.getSrc()));
  for (unsigned i = 0; i < 3; ++i) {
    std::string bank = "sim." + std::string(i < 2 ? "print_" + std::to_string(i) + "_ctrl" : "cpuStream_ctrl");
    std::string slave = "sim.ctrl_write_dispatch_slave_" + std::to_string(i);
    for (auto channel : {"aw", "w"}) {
      require(writeConnections.at(bank + "." + channel + ".valid") == slave + "_" + channel + "_valid" &&
          writeConnections.at(slave + "_" + channel + "_ready") == bank + "." + channel + ".ready",
          "Print request valid/readiness detached from selected slave");
    }
    require(writeConnections.at(bank + ".aw.bits.addr") == slave + "_aw_bits_addr" &&
        writeConnections.at(bank + ".w.bits.data") == slave + "_w_bits_data" &&
        readConnections.at(bank + ".ar") == "controlReadDispatch.slave_" + std::to_string(i) + "_ar",
        "Print request payload routed to the wrong allocated bank");
  }
  if (!output.empty() && !reverse) {
    std::error_code ec; llvm::raw_fd_ostream out((output + ".control.mlir").str(), ec);
    require(!ec, "cannot write Print MMIO composition fixture"); f.root->print(out);
  }
  auto dispatched = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeControlDispatch(f.circuit, error)) &&
      dump(*f.root) == dispatched, "repeat Print MMIO dispatch must be atomic");
  require(succeeded(goldengate::mapPrintBridgeControlResponses(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  require(f.circuit.getName() == "GGControlWriteTrackerWrapper", "Print response composition stopped before AW tracking");
  for (auto p : named(f.circuit, f.circuit.getName()).getPorts()) {
    auto name = p.name.getValue();
    require(name != "print_0_ctrl" && name != "print_1_ctrl" && name != "cpuStream_ctrl" &&
        name != "ctrl_read_dispatch_tracker_ready" && name != "ctrl_write_route_aw_tracker_ready" &&
        !name.starts_with("ctrl_error_b_") && !name.starts_with("ctrl_error_r_"),
        "selected bank response or tracker readiness escaped composition");
  }
  for (auto name : {"GGControlReadTracker", "GGControlWriteTracker"}) {
    auto tracker = named(f.circuit, name);
    require(tracker->getAttrOfType<IntegerAttr>("goldengate.trackerSlots").getInt() == 64 &&
        tracker->getAttrOfType<IntegerAttr>("goldengate.trackerTagWidth").getInt() == 12 &&
        tracker->getAttrOfType<IntegerAttr>("goldengate.trackerDequeuePorts").getInt() == 4 &&
        tracker->getAttrOfType<IntegerAttr>("goldengate.trackerRouteWidth").getInt() == 2,
        "Print response tracker differs from three banks plus error");
  }
  // Check all actual response nets, including the unmapped-address source.
  for (auto channel : {"r", "b"}) {
    bool read = StringRef(channel) == "r";
    std::map<std::string, std::string> connections;
    auto wrapper = named(f.circuit, read ? "GGControlReadArbiterWrapper" : "GGControlWriteArbiterWrapper");
    for (auto connect : wrapper.getOps<StrictConnectOp>())
      if (!isa<BlockArgument>(connect.getSrc()) && !isa<BlockArgument>(connect.getDest()) &&
          !connect.getSrc().getDefiningOp<AndPrimOp>())
        connections.emplace(key(connect.getDest()), key(connect.getSrc()));
    std::string arb = read ? "readArbiter" : "writeArbiter";
    for (unsigned i = 0; i < 4; ++i) {
      auto prefix = arb + ".in_" + std::to_string(i) + "_";
      auto bank = "sim." + std::string(i < 2 ? "print_" + std::to_string(i) + "_ctrl" : "cpuStream_ctrl") + "." + channel;
      auto errorPrefix = "sim.ctrl_error_" + std::string(channel) + "_";
      require(connections.at(i < 3 ? bank + ".ready" : errorPrefix + "ready") == prefix + "ready" &&
          connections.at(prefix + "valid") == (i < 3 ? bank + ".valid" : errorPrefix + "valid"),
          "Print response valid/readiness detached from arbiter");
      for (auto field : {"resp", "id", "user"})
        require(connections.at(prefix + "bits_" + field) ==
            (i < 3 ? bank + ".bits." + field : errorPrefix + "bits_" + field), "response metadata detached");
      if (read) for (auto field : {"data", "last"})
        require(connections.at(prefix + "bits_" + field) ==
            (i < 3 ? bank + ".bits." + field : errorPrefix + "bits_" + field), "read payload detached");
    }
  }
  if (!output.empty() && !reverse) {
    std::error_code ec; llvm::raw_fd_ostream out((output + ".responses.mlir").str(), ec);
    require(!ec, "cannot write Print response composition fixture"); f.root->print(out);
  }
  auto responded = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeControlResponses(f.circuit, error)) && dump(*f.root) == responded,
      "repeat Print response composition must be atomic");
  llvm::outs() << "PASS binding " << (reverse ? "reversed" : "ordered") << " " << evaluations << " handshake/data cases and three-stream CPU composition\n";
}

void allocationRejections(MLIRContext &ctx) {
  for (unsigned bad = 0; bad < 8; ++bad) {
    Fixture f(ctx); std::string error; OpBuilder b(&ctx);
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
    auto top = named(f.circuit, f.circuit.getName());
    auto rows = top->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
    SmallVector<Attribute> bindings(rows.begin(), rows.end());
    NamedAttrList binding(cast<DictionaryAttr>(bindings[1]));
    if (bad == 0) binding.set("countPort", b.getStringAttr("missingCount")); // read builder succeeds first
    if (bad == 1) binding.set("streamPort", b.getStringAttr("missingStream"));
    if (bad == 2) binding.set("widgetName", b.getStringAttr("unboundWidget"));
    if (bad == 3) binding.set("controlPort", b.getStringAttr("other"));
    bindings[1] = binding.getDictionary(&ctx);
    if (bad == 4) bindings[1] = bindings[0];
    top->setAttr("goldengate.printHostBindings", b.getArrayAttr(bindings));
    auto info = f.hosts[1]->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    auto config = named(f.circuit, info.getAs<StringAttr>("configModule").getValue());
    if (bad == 5) config->removeAttr("goldengate.mmioRegisters");
    if (bad == 6) {
      auto registers = config->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
      SmallVector<Attribute> fewer(registers.begin(), registers.end() - 1);
      config->setAttr("goldengate.mmioRegisters", b.getArrayAttr(fewer));
    }
    SmallVector<goldengate::CPUStreamSourcePort> sources{{"tracerv", "tracerv_stream", 6144}};
    SmallVector<goldengate::CPUStreamCountPort> counts{{bad == 7 ? "wrong" : "tracerv", "tracerv_stream_count", 13}};
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeCPUStreams(f.circuit, sources, counts, error)) &&
        !error.empty() && dump(*f.root) == before,
        "Print CPU allocation accepted or mutated malformed boundary " + std::to_string(bad));
  }
  llvm::outs() << "PASS eight atomic Print CPU allocation rejections including failure after read materialization\n";
}

void controlRejections(MLIRContext &ctx) {
  for (unsigned bad = 0; bad < 11; ++bad) {
    Fixture f(ctx); std::string error; OpBuilder b(&ctx);
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
    require(succeeded(goldengate::mapPrintBridgeCPUStreams(f.circuit, {}, {}, error)), error);
    auto bound = named(f.circuit, "GGPrintBridgeHostWrapper");
    auto registry = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
    SmallVector<Attribute> rows(registry.begin(), registry.end());
    NamedAttrList row(cast<DictionaryAttr>(rows[1]));
    if (bad == 0) bound->removeAttr("goldengate.printHostBindings");
    if (bad == 1) rows[1] = rows[0];
    if (bad == 2) row.set("widgetName", b.getStringAttr("absent"));
    if (bad == 3) row.set("controlPort", b.getStringAttr("other"));
    if (bad == 4) row.erase("hostModule");
    if (bad >= 2 && bad <= 4) rows[1] = row.getDictionary(&ctx);
    if (bad >= 1 && bad <= 4) bound->setAttr("goldengate.printHostBindings", b.getArrayAttr(rows));
    auto counts = named(f.circuit, "GGCPUStreamCountBank");
    if (bad == 5) counts->removeAttr("goldengate.mmioRegisters"); // adapter already created in staging
    if (bad == 6) {
      auto regs = counts->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
      counts->setAttr("goldengate.mmioRegisters", b.getArrayAttr({regs[0]}));
    }
    if (bad == 7) f.circuit.setName("GGPrintBridgeHostWrapper");
    if (bad == 8) f.circuit->removeAttr("rawAnnotations");
    if (bad == 9) {
      auto top = named(f.circuit, f.circuit.getName());
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[0] = b.getStringAttr("notHostClock"); top.setPortNames(names);
    }
    if (bad == 10) {
      auto info = f.hosts[1]->getAttrOfType<DictionaryAttr>("goldengate.printHost");
      named(f.circuit, info.getAs<StringAttr>("configModule").getValue())->removeAttr("goldengate.mmioRegisters");
    }
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeControlDispatch(f.circuit, error)) && !error.empty() &&
        dump(*f.root) == before, "Print MMIO failure mutated circuit " + std::to_string(bad));
  }
  llvm::outs() << "PASS eleven atomic Print MMIO composition rejections including failures after adapter creation\n";
}

void responseRejections(MLIRContext &ctx) {
  for (unsigned bad = 0; bad < 12; ++bad) {
    Fixture f(ctx); std::string error; OpBuilder b(&ctx);
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)) &&
        succeeded(goldengate::mapPrintBridgeCPUStreams(f.circuit, {}, {}, error)) &&
        succeeded(goldengate::mapPrintBridgeControlDispatch(f.circuit, error)), error);
    auto top = named(f.circuit, f.circuit.getName());
    auto bindings = top->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings");
    SmallVector<Attribute> rows(bindings.begin(), bindings.end());
    NamedAttrList row(cast<DictionaryAttr>(rows[0]));
    if (bad == 0) named(f.circuit, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    if (bad == 1) rows.pop_back();
    if (bad == 2) rows[1] = rows[0];
    if (bad == 3) row.set("port", b.getStringAttr("other"));
    if (bad == 4) row.set("name", b.getStringAttr("absent"));
    if (bad == 3 || bad == 4) rows[0] = row.getDictionary(&ctx);
    top->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
    if (bad == 5) named(f.circuit, "GGControlAddressDecode")->removeAttr("goldengate.controlRegions");
    if (bad >= 6 && bad <= 9) {
      const char *collision[] = {"GGControlReadTracker", "GGControlReadArbiter", "GGControlWriteArbiter", "GGControlWriteTracker"};
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(collision[bad - 6]),
          ConventionAttr::get(&ctx, Convention::Internal), SmallVector<PortInfo>{});
    }
    if (bad == 10) {
      // Read tracker and both arbiters have already succeeded in staging.
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      for (auto &name : names) if (cast<StringAttr>(name).getValue() == "ctrl_write_route_aw_track_valid")
        name = b.getStringAttr("absentAWAcceptance");
      top.setPortNames(names);
    }
    if (bad == 11) {
      auto decoder = named(f.circuit, "GGControlAddressDecode");
      auto catalog = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
      SmallVector<Attribute> extra(catalog.begin(), catalog.end());
      NamedAttrList unbound(cast<DictionaryAttr>(catalog[0]));
      unbound.set("name", b.getStringAttr("unboundBank"));
      unbound.set("slave", b.getI32IntegerAttr(catalog.size()));
      extra.push_back(unbound.getDictionary(&ctx));
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(extra));
    }
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeControlResponses(f.circuit, error)) && !error.empty() &&
        dump(*f.root) == before, "Print response failure mutated circuit " + std::to_string(bad));
  }
  llvm::outs() << "PASS twelve atomic Print response rejections including failure after both arbiters\n";
}

void rejections(MLIRContext &ctx) {
  constexpr unsigned cases = 22;
  for (unsigned bad = 0; bad < cases; ++bad) {
    Fixture f(ctx), other(ctx); OpBuilder b(&ctx); auto h = f.hosts[1];
    auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> annos(raw.begin(), raw.end());
    auto change = [&](unsigned i, StringRef key, Attribute value) {
      NamedAttrList fields(cast<DictionaryAttr>(annos[i])); if (value) fields.set(key, value); else fields.erase(key);
      annos[i] = fields.getDictionary(&ctx);
    };
    auto metadata = [&](StringRef key, Attribute value) {
      NamedAttrList fields(h->getAttrOfType<DictionaryAttr>("goldengate.printHost")); if (value) fields.set(key, value); else fields.erase(key);
      h->setAttr("goldengate.printHost", fields.getDictionary(&ctx));
    };
    if (bad == 0) f.hosts[1] = {};
    if (bad == 1) f.hosts[1] = other.hosts[0];
    if (bad == 2) f.hosts[1] = f.hosts[0];
    if (bad == 3) h->removeAttr("goldengate.printHost");
    if (bad == 4) metadata("resetPortName", b.getStringAttr("absent"));
    if (bad == 5) metadata("bridgeTarget", b.getStringAttr("~Top|Top>different"));
    if (bad == 6) metadata("queueDepth", b.getI64IntegerAttr(0));
    if (bad == 7) change(2, "channelMapping", b.getStringAttr("malformed"));
    if (bad == 8) change(2, "channelMapping", b.getDictionaryAttr({}));
    if (bad == 9) change(3, "sources", b.getArrayAttr({}));
    if (bad == 10) change(3, "sources", b.getArrayAttr({b.getStringAttr("~Top|Top>source1_2.bits"), b.getStringAttr("~Top|Top>source0_2.bits")}));
    if (bad == 11) change(3, "sources", b.getArrayAttr({b.getStringAttr("~Top|Top>source1_2.valid")}));
    if (bad == 12) change(3, "sources", b.getArrayAttr({b.getStringAttr("~Top|Top>missing.bits")}));
    if (bad == 13) change(3, "sinks", b.getArrayAttr({b.getStringAttr("~Top|Top>other")}));
    if (bad == 14) change(3, "channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)), b.getNamedAttr("latency", b.getI64IntegerAttr(1))}));
    if (bad == 15) change(3, "globalName", b.getStringAttr("global1_1"));
    if (bad == 16) {
      auto top = named(f.circuit, "Top"); SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end()); names[0] = b.getStringAttr("targetClock"); top.setPortNames(names);
    }
    if (bad == 17) {
      auto top = named(f.circuit, "Top"); SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end()); types[1] = TypeAttr::get(UIntType::get(&ctx, 2)); top.setPortTypes(types);
    }
    if (bad == 18) {
      auto top = named(f.circuit, "Top"); SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end()); names[10] = b.getStringAttr("print_1_stream"); top.setPortNames(names);
    }
    if (bad == 19) {
      auto top = named(f.circuit, "Top"); b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(top.getLoc(), top, "illegalRecursiveUse");
    }
    if (bad == 20) {
      // This domain is completely annotated, so rejection specifically tests
      // incompatible clocks rather than mixed present/absent clock metadata.
      for (unsigned i = 3; i < 6; ++i)
        change(i, "clock", b.getStringAttr("~Top|Top>domainClock"));
      change(3, "clock", b.getStringAttr("~Top|Top>otherClock"));
    }
    if (bad == 21) {
      auto info = h->getAttrOfType<DictionaryAttr>("goldengate.printHost");
      auto queue = named(f.circuit, info.getAs<StringAttr>("queueModule").getValue());
      auto ram = *queue.getOps<MemOp>().begin();
      ram->setAttr("depth", b.getI64IntegerAttr(6143));
    }
    f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos)); auto before = dump(*f.root), foreign = dump(*other.root); std::string error;
    require(failed(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), "accepted malformed binding " + std::to_string(bad));
    require(!error.empty() && dump(*f.root) == before && dump(*other.root) == foreign, "binding rejection mutated circuit " + std::to_string(bad));
  }
  Fixture f(ctx); auto before = dump(*f.root); std::string error;
  require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, {}, error)) && dump(*f.root) == before, "empty binding must be no-op");
  llvm::outs() << "PASS " << cases << " atomic binding rejections and empty no-op\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    checkBinding(ctx, false, argc > 1 ? argv[1] : ""); checkBinding(ctx, true, ""); rejections(ctx); allocationRejections(ctx); controlRejections(ctx); responseRejections(ctx);
    for (bool reverse : {false, true}) {
      Fixture f(ctx, true, reverse); std::string error, header;
      require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
      require(succeeded(goldengate::preparePrintBridgeDecoderHeader(f.circuit, f.hosts, header, error)), error);
      require(header.find(std::string(reverse ? "{5U" : "{1U") + ", \"signed=%d") != std::string::npos &&
          header.find(std::string(reverse ? "{1U" : "{7U") + ", \"quote=") != std::string::npos &&
          header.find("2U, 65534U, stream_index, 6144U") != std::string::npos,
          "decoder record permutation offsets or token geometry differ");
      if (argc > 1) {
        std::error_code ec;
        llvm::raw_fd_ostream file(std::string(argv[1]) + (reverse ? ".reverse.h" : ".records.h"), ec);
        require(!ec, "cannot write ordered decoder fixture"); file << header;
      }
      auto rejectWithoutMutation = [&](const std::string &why) {
        auto before = dump(*f.root); std::string candidate = "preserve";
        require(failed(goldengate::preparePrintBridgeDecoderHeader(f.circuit, f.hosts, candidate, error)) &&
            !error.empty() && candidate == "preserve" && dump(*f.root) == before, why);
      };
      std::swap(f.hosts[0], f.hosts[1]); rejectWithoutMutation("accepted wrong host order");
      std::swap(f.hosts[0], f.hosts[1]);
      auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
      SmallVector<Attribute> bad(raw.begin(), raw.end());
      NamedAttrList bridge(cast<DictionaryAttr>(bad[2]));
      auto clock = bridge.get("clockInfo"); NamedAttrList badClock(cast<DictionaryAttr>(clock));
      badClock.set("divisor", IntegerAttr::get(IntegerType::get(&ctx, 64), 0));
      bridge.set("clockInfo", badClock.getDictionary(&ctx)); bad[2] = bridge.getDictionary(&ctx);
      f.circuit->setAttr("rawAnnotations", ArrayAttr::get(&ctx, bad));
      rejectWithoutMutation("accepted zero clock divisor"); f.circuit->setAttr("rawAnnotations", raw);
    }
    llvm::outs() << "PASS Print decoder signed/mixed records, both record orders and transactional rejection\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << "FAIL " << e.what() << "\n"; return 1; }
}
