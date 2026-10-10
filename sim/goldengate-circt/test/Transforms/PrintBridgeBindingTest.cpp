// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/PrintBridgeHeader.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/CPUStreamCountBank.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlReadDispatch.h"
#include "goldengate/SimulationMasterControl.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <algorithm>
#include <random>
#include <stdexcept>
#include <functional>

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
  auto inner = named(f.circuit, f.circuit.getName());
  auto innerBefore = dump(inner);
  // The default full-platform entry must not silently accept a selected host.
  require(failed(goldengate::bindControlMaster(f.circuit, error)) && dump(*f.root) == responded,
      "selected master needs an explicit assembly stage");
  require(succeeded(goldengate::bindControlMaster(f.circuit, "GGControlWriteTrackerWrapper", error)) &&
      succeeded(verify(*f.root)), error);
  auto master = named(f.circuit, "GGControlMasterWrapper");
  require(f.circuit.getName() == master.getName() && dump(inner) == innerBefore,
      "master assembly changed selected host internals");
  require(master.getNumPorts() + 32 == inner.getNumPorts(), "master scalar boundary count differs");
  for (auto p : master.getPorts()) {
    auto name = p.name.getValue();
    require(name != "ctrl_write_route_aw_ready" && name != "ctrl_write_route_aw_valid" &&
        name != "ctrl_write_route_w_ready" && name != "ctrl_write_route_w_valid" &&
        name != "ctrl_write_route_w_last" && !name.starts_with("ctrl_write_dispatch_master_") &&
        name != "ctrl_decode_aw_addr" && !name.starts_with("ctrl_read_dispatch_master_") &&
        !name.starts_with("ctrl_read_arb_out_") && !name.starts_with("ctrl_write_arb_out_"),
        "master request/response scalar remains exposed");
  }
  auto ctrl = cast<BundleType>(master.getPorts()[port(master, "ctrl")].type);
  require(ctrl.getElements().size() == 5 && master.getPorts()[port(master, "ctrl")].direction == Direction::In,
      "master is not one complete flipped Nasti interface");
  unsigned cpuPorts = 0;
  for (auto p : inner.getPorts()) if (p.name.getValue().starts_with("cpu_stream_")) {
    auto copied = master.getPorts()[port(master, p.name.getValue())];
    require(copied.type == p.type && copied.direction == p.direction,
        "master binding changed CPU AXI stream interface");
    ++cpuPorts;
  }
  require(cpuPorts != 0, "master binding lost CPU AXI stream ports");
  unsigned strictConnections = 0, arConnections = 0;
  for (auto connect : master.getOps<StrictConnectOp>()) { (void)connect; ++strictConnections; }
  for (auto connect : master.getOps<ConnectOp>())
    if (auto field = connect.getSrc().getDefiningOp<SubfieldOp>())
      if (field.getInput() == master.getBodyBlock()->getArgument(port(master, "ctrl")) && field.getFieldName() == "ar")
        ++arConnections;
  require(strictConnections == 32 && arConnections == 1, "master lacks complete scalar/AR wiring");
  if (!output.empty() && !reverse) {
    std::error_code ec; llvm::raw_fd_ostream out((output + ".master.mlir").str(), ec);
    require(!ec, "cannot write Print master composition fixture"); f.root->print(out);
  }
  auto mastered = dump(*f.root);
  require(failed(goldengate::bindControlMaster(f.circuit, "GGControlWriteTrackerWrapper", error)) &&
      dump(*f.root) == mastered, "repeat selected master assembly must be atomic");
  llvm::outs() << "PASS binding " << (reverse ? "reversed" : "ordered") << " " << evaluations << " handshake/data cases and three-stream CPU composition\n";
}

// Exercise the production catalog against actual materialized Rocket banks.
// This is a pre-binding allocation boundary: it does not attach Print to the
// complete platform's CPU transport or claim a runnable Print platform.
void platformCatalog(MLIRContext &ctx, StringRef baseline, StringRef output) {
  if (baseline.empty()) return;
  auto root = parseSourceFile<ModuleOp>(baseline, &ctx);
  require(bool(root), "cannot parse materialized Rocket baseline");
  auto circuit = *root->getOps<CircuitOp>().begin(); std::string error; OpBuilder b(&ctx);
  SmallVector<goldengate::ControlMMIOWidget> catalog;
  SmallVector<goldengate::ControlMMIORegion> regions;
  auto before = dump(*root);
  require(succeeded(goldengate::deriveRocketControlMMIOCatalog(circuit, {}, catalog, error)) &&
      succeeded(goldengate::allocateControlMMIORegions(25, catalog, regions, error)) && dump(*root) == before, error);
  auto original = named(circuit, "GGControlAddressDecode")->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  require(regions.size() == 11 && original.size() == 11, "Rocket platform widget count differs");
  for (auto [i, r] : llvm::enumerate(regions)) {
    auto row = cast<DictionaryAttr>(original[i]);
    require(row.getAs<StringAttr>("name") == r.name && row.getAs<IntegerAttr>("start").getInt() == int64_t(r.start) &&
        row.getAs<IntegerAttr>("size").getInt() == int64_t(r.size), "catalog changed baseline decoder allocation");
  }
  auto emitDecoder = [&](StringRef suffix) {
    auto decoded = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGControlErrorWrapper" {
      firrtl.module @GGControlErrorWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>) {}
    } })", &ctx);
    auto c = *decoded->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    c->setAttr("rawAnnotations", b.getArrayAttr({}));
    require(succeeded(goldengate::addControlAddressDecode(c, 25, regions, error)) && succeeded(verify(*decoded)), error);
    if (!output.empty()) {
      std::error_code ec; llvm::raw_fd_ostream file((output + suffix).str(), ec);
      require(!ec, "cannot write platform Print allocation"); decoded->print(file); file << '\n';
    }
  };
  emitDecoder(".platform-baseline.mlir");
  // Import the real two-domain native Print banks, independently of module
  // traversal order. The shared queue definition may already be in Rocket.
  Fixture prints(ctx); SmallVector<FModuleOp> hosts;
  for (auto m : prints.circuit.getOps<FModuleOp>()) {
    if (m.getName() == "Top") continue;
    bool exists = false;
    for (auto prior : circuit.getOps<FModuleOp>()) exists |= prior.getName() == m.getName();
    if (!exists) circuit.getBodyBlock()->push_back(m->clone());
  }
  for (auto h : prints.hosts) hosts.push_back(named(circuit, h.getName()));
  require(succeeded(verify(*root)), "imported Print banks invalid");
  unsigned rejected = 0;
  for (bool reverse : {false, true}) {
    if (reverse) std::reverse(hosts.begin(), hosts.end());
    before = dump(*root);
    require(succeeded(goldengate::deriveRocketControlMMIOCatalog(circuit, hosts, catalog, error)) &&
        succeeded(goldengate::allocateControlMMIORegions(25, catalog, regions, error)) && dump(*root) == before, error);
    require(catalog.size() == 13 && catalog[9].name == "PrintBridgeModule_0" &&
        catalog[10].name == "PrintBridgeModule_1" && catalog[11].name == "LoadMemWidget_0", "Print registration order differs");
    for (auto r : regions) {
      if (r.name == "PrintBridgeModule_0") require(r.start == 544 && r.size == 32, "first platform Print region differs");
      if (r.name == "PrintBridgeModule_1") require(r.start == 576 && r.size == 32, "second platform Print region differs");
      if (r.name == "CPUManagedStreamEngine_0") require(r.start == 632 && r.size == 4, "platform count region differs");
    }
    emitDecoder(reverse ? ".platform-reverse.mlir" : ".platform.mlir");
    // Batch errors preserve both the caller's result and materialized IR.
    auto preserved = catalog;
    auto checkReject = [&](ArrayRef<FModuleOp> bad) {
      auto state = dump(*root);
      require(failed(goldengate::deriveRocketControlMMIOCatalog(circuit, bad, catalog, error)) &&
          dump(*root) == state && catalog.size() == preserved.size(), "catalog failure mutated IR or result");
      for (auto [i, w] : llvm::enumerate(catalog)) require(w.name == preserved[i].name &&
          w.registerCount == preserved[i].registerCount, "catalog failure changed descriptor");
      ++rejected;
    };
    checkReject({hosts[0], hosts[0]}); checkReject({prints.hosts[0]});
    checkReject({}); checkReject({hosts[0]});
    auto info = hosts[1]->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    for (StringRef key : {"configModule", "mcrModule", "queueModule"}) {
      NamedAttrList corrupt(info); corrupt.erase(key);
      hosts[1]->setAttr("goldengate.printHost", corrupt.getDictionary(&ctx));
      checkReject(hosts); hosts[1]->setAttr("goldengate.printHost", info);
    }
    for (StringRef key : {"configModule", "mcrModule"}) {
      NamedAttrList corrupt(info);
      corrupt.set(key, hosts[0]->getAttrOfType<DictionaryAttr>("goldengate.printHost").getAs<StringAttr>(key));
      hosts[1]->setAttr("goldengate.printHost", corrupt.getDictionary(&ctx));
      checkReject(hosts); hosts[1]->setAttr("goldengate.printHost", info);
    }
    auto config = named(circuit, info.getAs<StringAttr>("configModule").getValue());
    auto registers = config->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> rows(registers.begin(), registers.end());
    NamedAttrList badWord(cast<DictionaryAttr>(rows.back())); badWord.set("offset", b.getI64IntegerAttr(0));
    rows.back() = badWord.getDictionary(&ctx); config->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
    checkReject(hosts); config->setAttr("goldengate.mmioRegisters", registers);
  }
  llvm::outs() << "PASS production Rocket catalog: 11 baseline banks, two Print banks at 544/576, count at 632, both orders and "
               << rejected << " atomic rejections (pre-binding allocation only)\n";
}

// Attach the actual Master.scala bank after expanded Rocket/Print responses.
// Allocation remains derived from the live imported banks and queued hosts.
void rocketMaster(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGSimulationMasterBank");
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto bound = named(f.circuit, "GGPrintBridgeHostWrapper");
  auto hosts = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
  for (unsigned bad = 0; bad < 8; ++bad) {
    SmallVector<Attribute> rows(regions.begin(), regions.end());
    NamedAttrList master(cast<DictionaryAttr>(rows[10]));
    if (bad == 0) master.set("start", b.getI64IntegerAttr(544));
    if (bad == 1) master.set("slave", b.getI32IntegerAttr(8));
    if (bad < 2) { rows[10] = master.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows)); }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> words(registers.begin(), registers.end());
      if (bad == 2) words.pop_back();
      else { NamedAttrList word(cast<DictionaryAttr>(words[0])); word.set("writeable", b.getBoolAttr(false)); words[0] = word.getDictionary(ctx); }
      bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(words));
    }
    FModuleOp collision;
    if (bad == 4 || bad == 5) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(bad == 4 ?
          "GGSimulationMasterMCRFile" : "GGSimulationMasterBoundWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) bound->setAttr("goldengate.printHostBindings", b.getArrayAttr({hosts[1], hosts[0]}));
    auto state = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketSimulationMaster(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == state, "invalid expanded master composition mutated IR");
    ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    bank->setAttr("goldengate.mmioRegisters", registers);
    bound->setAttr("goldengate.printHostBindings", hosts);
    if (collision) collision.erase();
    f.circuit.setName(original.getName());
  }
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  std::function<Attribute(Attribute)> payload = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) if (s.getValue().starts_with("~")) return b.getStringAttr("<target>");
    if (auto rows = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto row : rows) out.push_back(payload(row)); return b.getArrayAttr(out); }
    if (auto row = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto field : row) out.set(field.getName(), payload(field.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  require(succeeded(goldengate::mapPrintBridgeRocketSimulationMaster(f.circuit, error)) && succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGSimulationMasterBoundWrapper" &&
      top->getAttrOfType<IntegerAttr>("goldengate.simulationMasterSlave").getInt() == 10 &&
      decoder->getAttr("goldengate.controlRegions") == regions &&
      bank->getAttr("goldengate.mmioRegisters") == registers, "expanded master identity or allocation changed");
  require(top.getNumPorts() + 22 == original.getNumPorts(), "expanded master consumed port count differs");
  for (auto p : top.getPorts()) {
    auto old = original.getPorts()[port(original, p.name.getValue())];
    require(old.type == p.type && old.direction == p.direction, "master changed a copied port contract");
  }
  std::function<std::string(Value)> key = [&](Value value) -> std::string {
    if (auto field = value.getDefiningOp<SubfieldOp>()) return key(field.getInput()) + "." + field.getFieldName().str();
    if (auto inst = value.getDefiningOp<InstanceOp>())
      return inst.getName().str() + "." + inst.getPortNameStr(cast<OpResult>(value).getResultNumber()).str();
    if (auto arg = dyn_cast<BlockArgument>(value)) {
      auto parent = cast<FModuleOp>(arg.getOwner()->getParentOp());
      return "top." + parent.getPortName(arg.getArgNumber()).str();
    }
    throw std::runtime_error("unexpected expanded master value");
  };
  std::map<std::string, std::string> actual, expected;
  for (auto connect : top.getOps<StrictConnectOp>())
    require(actual.emplace(key(connect.getDest()), key(connect.getSrc())).second, "duplicate expanded master driver");
  const std::string ctrl = "sim.simulationMaster_ctrl";
  const StringRef aw[]{"addr", "len", "size", "burst", "lock", "cache", "prot", "qos", "region", "id", "user"};
  const StringRef w[]{"data", "last", "id", "strb", "user"};
  for (StringRef ch : {"aw", "w"}) {
    auto prefix = "sim.ctrl_write_dispatch_slave_10_" + ch.str();
    auto local = ctrl + "." + ch.str();
    expected[prefix + "_ready"] = local + ".ready";
    expected[local + ".valid"] = prefix + "_valid";
    for (auto field : ch == "aw" ? ArrayRef<StringRef>(aw) : ArrayRef<StringRef>(w)) {
      bool dispatched = ch == "aw" ? field == "addr" || field == "len" || field == "id" : field == "data" || field == "last";
      expected[local + ".bits." + field.str()] = (dispatched ? prefix : "sim.ctrl_write_dispatch_master_" + ch.str()) + "_bits_" + field.str();
    }
  }
  for (StringRef ch : {"r", "b"}) {
    auto prefix = std::string(ch == "r" ? "sim.ctrl_read_arb_in_10" : "sim.ctrl_write_arb_in_10");
    auto local = ctrl + "." + ch.str();
    expected[local + ".ready"] = prefix + "_ready";
    expected[prefix + "_valid"] = local + ".valid";
    for (StringRef field : {"resp", "id", "user"}) expected[prefix + "_bits_" + field.str()] = local + ".bits." + field.str();
    if (ch == "r") for (StringRef field : {"data", "last"}) expected[prefix + "_bits_" + field.str()] = local + ".bits." + field.str();
  }
  require(actual == expected && actual.size() == 32, "expanded master scalar requests/responses differ");
  bool ar = false;
  for (auto connect : top.getOps<ConnectOp>()) if (key(connect.getDest()) == ctrl + ".ar") {
    require(!ar && key(connect.getSrc()) == "sim.ctrl_read_dispatch_slave_10_ar", "expanded master AR slot differs"); ar = true;
  }
  require(ar, "expanded master AR is absent");
  for (auto p : top.getPorts()) {
    auto name = p.name.getValue();
    require(name != "simulationMaster_ctrl" && name != "simulationMaster_mcr" &&
        !name.starts_with("ctrl_write_dispatch_slave_10_") && name != "ctrl_read_dispatch_slave_10_ar" &&
        !name.starts_with("ctrl_read_arb_in_10_") && !name.starts_with("ctrl_write_arb_in_10_"),
        "expanded master consumed port escapes");
  }
  // Host clock/reset connect to the actual bank, independently of Print token resets.
  auto attached = named(f.circuit, "GGSimulationMasterWrapper");
  std::map<std::string, std::string> clockReset;
  for (auto connect : attached.getOps<StrictConnectOp>()) clockReset[key(connect.getDest())] = key(connect.getSrc());
  require(clockReset["simulationMaster.clock"] == "top.hostClock" &&
      clockReset["simulationMaster.reset"] == "top.hostReset", "expanded master host clock/reset differ");
  require(payload(f.circuit->getAttr("rawAnnotations")) == payload(raw), "master changed annotation payloads/order");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketSimulationMaster(f.circuit, error)) &&
      dump(*f.root) == state, "repeated expanded master composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-master-reverse.mlir" : ".rocket-master.mlir")).str(), ec);
    require(!ec, "cannot write expanded master boundary"); f.root->print(file); file << '\n';
  }
}

void rocketTSI(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGTSIMMIOBank");
  auto words = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  for (unsigned bad = 0; bad < 9; ++bad) {
    if (bad < 2) {
      SmallVector<Attribute> rows(regions.begin(), regions.end()); NamedAttrList row(cast<DictionaryAttr>(rows[3]));
      row.set(bad == 0 ? "start" : "slave", b.getI64IntegerAttr(bad == 0 ? 0 : 10));
      rows[3] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 2) {
      SmallVector<Attribute> rows(words.begin(), words.end()); NamedAttrList row(cast<DictionaryAttr>(rows[8]));
      row.set("writeable", b.getBoolAttr(false)); rows[8] = row.getDictionary(ctx);
      bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
    }
    if (bad == 7 || bad == 8) {
      auto source = bad == 7 ? words : regions;
      SmallVector<Attribute> rows(source.begin(), source.end()); NamedAttrList row(cast<DictionaryAttr>(rows[0]));
      row.erase("name"); rows[0] = row.getDictionary(ctx);
      (bad == 7 ? bank : decoder)->setAttr(bad == 7 ? "goldengate.mmioRegisters" : "goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 3) {
      SmallVector<Attribute> rows;
      for (auto attr : raw) { auto row = cast<DictionaryAttr>(attr);
        auto name = row.getAs<StringAttr>("globalName");
        if (!name || name.getValue() != "ep_3_reset") rows.push_back(attr); }
      f.circuit->setAttr("rawAnnotations", b.getArrayAttr(rows));
    }
    FModuleOp collision;
    if (bad == 4 || bad == 5) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(bad == 4 ?
          "GGTSIMCRFile" : "GGTSIBridgeBoundWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    auto state = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketTSI(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == state, "invalid expanded TSI composition mutated IR"); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions); bank->setAttr("goldengate.mmioRegisters", words);
    f.circuit->setAttr("rawAnnotations", raw); if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketTSI(f.circuit, error)) && succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGTSIBridgeBoundWrapper" && top->getAttrOfType<IntegerAttr>("goldengate.tsiSlave").getInt() == 3 &&
      decoder->getAttr("goldengate.controlRegions") == regions && bank->getAttr("goldengate.mmioRegisters") == words,
      "expanded TSI identity or allocation differs");
  require(bool(child(named(f.circuit, "GGTSIMMIOWrapper"), bank)), "TSI register bank is not attached");
  auto queues = named(f.circuit, "GGTSIWordQueuesWrapper"); unsigned instances = 0;
  for (auto inst : queues.getOps<InstanceOp>()) instances += inst.getModuleName() == "GGTSIWordQueue16";
  require(instances == 2, "TSI must instantiate two independent sixteen-word queues");
  for (auto p : top.getPorts()) {
    auto name = p.name.getValue();
    require(name != "tsiBridge_ctrl" && name != "tsiBridge_mcr" && name != "tsi_control" &&
        name != "tsi_in_enq" && name != "tsi_out_deq" && !name.starts_with("FireSim_ep_3_tsi_") &&
        name != "FireSim_ep_3_reset_source" && !name.starts_with("ctrl_write_dispatch_slave_3_") &&
        name != "ctrl_read_dispatch_slave_3_ar" && !name.starts_with("ctrl_read_arb_in_3_") &&
        !name.starts_with("ctrl_write_arb_in_3_"), "consumed TSI boundary escapes");
  }
  unsigned constructors = 0, channels = 0;
  for (auto attr : f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = cast<DictionaryAttr>(attr);
    auto widget = row.getAs<StringAttr>("widgetClass");
    constructors += widget && widget.getValue() == "firechip.goldengateimplementations.TSIBridgeModule";
    auto name = row.getAs<StringAttr>("globalName");
    if (name && name.getValue().starts_with("ep_3_")) {
      ++channels;
      auto sources = row.getAs<ArrayAttr>("sources"), sinks = row.getAs<ArrayAttr>("sinks");
      require(sources && !sources.empty() && sinks && !sinks.empty(), "TSI channel lacks completed host endpoints");
    }
  }
  require(constructors == 1 && channels == 5, "TSI constructor or completed channel retention differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketTSI(f.circuit, error)) && dump(*f.root) == state,
      "repeated expanded TSI composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-tsi-reverse.mlir" : ".rocket-tsi.mlir")).str(), ec);
    require(!ec, "cannot write expanded TSI boundary"); f.root->print(file); file << '\n';
  }
}

void rocketBlockDev(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGBlockDevMMIOBank");
  auto words = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  for (unsigned bad = 0; bad < 11; ++bad) {
    if (bad < 2) {
      SmallVector<Attribute> rows(regions.begin(), regions.end()); NamedAttrList row(cast<DictionaryAttr>(rows[0]));
      row.set(bad == 0 ? "start" : "slave", b.getI64IntegerAttr(bad == 0 ? 128 : 10));
      rows[0] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 2) {
      SmallVector<Attribute> rows(words.begin(), words.end()); NamedAttrList row(cast<DictionaryAttr>(rows[25]));
      row.set("writeable", b.getBoolAttr(false)); rows[25] = row.getDictionary(ctx);
      bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
    }
    if (bad == 7 || bad == 8) {
      auto source = bad == 7 ? words : regions;
      SmallVector<Attribute> rows(source.begin(), source.end()); NamedAttrList row(cast<DictionaryAttr>(rows[0]));
      row.erase("name"); rows[0] = row.getDictionary(ctx);
      (bad == 7 ? bank : decoder)->setAttr(bad == 7 ? "goldengate.mmioRegisters" : "goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 3) {
      SmallVector<Attribute> rows;
      for (auto attr : raw) { auto row = cast<DictionaryAttr>(attr);
        auto name = row.getAs<StringAttr>("globalName");
        if (!name || name.getValue() != "ep_reset") rows.push_back(attr); }
      f.circuit->setAttr("rawAnnotations", b.getArrayAttr(rows));
    }
    FModuleOp collision;
    if (bad == 4 || bad == 5 || bad == 9) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(bad == 4 ?
          "GGBlockDevMCRFile" : bad == 5 ? "GGBlockDevBridgeBoundWrapper" : "GGBlockDevResponseScheduler"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 10) {
      SmallVector<Attribute> rows;
      for (auto attr : raw) {
        auto row = cast<DictionaryAttr>(attr); auto name = row.getAs<StringAttr>("globalName");
        if (name && name.getValue() == "ep_reset") {
          NamedAttrList changed(row); changed.set("clock", b.getStringAttr("~Wrong|Clock>clk"));
          attr = changed.getDictionary(ctx);
        }
        rows.push_back(attr);
      }
      f.circuit->setAttr("rawAnnotations", b.getArrayAttr(rows));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    auto state = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketBlockDev(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == state, "invalid expanded BlockDev composition mutated IR"); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions); bank->setAttr("goldengate.mmioRegisters", words);
    f.circuit->setAttr("rawAnnotations", raw); if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketBlockDev(f.circuit, error)) && succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  auto bound = named(f.circuit, "GGBlockDevBridgeBoundWrapper");
  require(top.getName() == "GGBlockDevResponseSchedulerWrapper" &&
      bound->getAttrOfType<IntegerAttr>("goldengate.blockdevSlave").getInt() == 0 &&
      decoder->getAttr("goldengate.controlRegions") == regions && bank->getAttr("goldengate.mmioRegisters") == words,
      "expanded BlockDev identity or allocation differs");
  require(bool(child(named(f.circuit, "GGBlockDevMMIOWrapper"), bank)), "BlockDev register bank is not attached");
  const StringRef queueWrappers[]{"GGBlockDevRequestQueueWrapper", "GGBlockDevDataQueueWrapper",
      "GGBlockDevReadResponseQueueWrapper", "GGBlockDevWriteAckQueueWrapper"};
  const StringRef queues[]{"GGBlockDevRequestQueue10", "GGBlockDevDataQueue32",
      "GGBlockDevDataQueue32", "GGBlockDevWriteAckQueue4"};
  for (unsigned i = 0; i < 4; ++i)
    require(bool(child(named(f.circuit, queueWrappers[i]), named(f.circuit, queues[i]))),
        "BlockDev requires four independent functional queue instances");
  for (auto p : top.getPorts()) {
    auto name = p.name.getValue();
    require(name != "blockdevBridge_ctrl" && name != "blockdevBridge_mcr" && name != "blockdev_control" &&
        name != "blockdev_req_deq" && name != "blockdev_data_deq" && !name.starts_with("FireSim_ep_bdev_") &&
        name != "blockdev_rresp_enq" && name != "blockdev_wack_enq" &&
        name != "blockdev_timing" && name != "blockdev_write_latency_deq" && name != "blockdev_read_latency_deq" &&
        name != "FireSim_ep_reset_source" && !name.starts_with("ctrl_write_dispatch_slave_0_") &&
        name != "ctrl_read_dispatch_slave_0_ar" && !name.starts_with("ctrl_read_arb_in_0_") &&
        !name.starts_with("ctrl_write_arb_in_0_"), "consumed BlockDev boundary escapes");
  }
  unsigned constructors = 0, channels = 0;
  for (auto attr : f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = cast<DictionaryAttr>(attr);
    auto widget = row.getAs<StringAttr>("widgetClass");
    constructors += widget && widget.getValue() == "firechip.goldengateimplementations.BlockDevBridgeModule";
    auto name = row.getAs<StringAttr>("globalName");
    if (name && (name.getValue() == "ep_reset" || name.getValue().starts_with("ep_bdev_"))) {
      ++channels;
      auto sources = row.getAs<ArrayAttr>("sources"), sinks = row.getAs<ArrayAttr>("sinks");
      require(sources && !sources.empty() && sinks && !sinks.empty(), "BlockDev channel lacks completed host endpoints");
    }
  }
  require(constructors == 1 && channels == 9, "BlockDev constructor or completed channel retention differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketBlockDev(f.circuit, error)) && dump(*f.root) == state,
      "repeated expanded BlockDev composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-blockdev-reverse.mlir" : ".rocket-blockdev.mlir")).str(), ec);
    require(!ec, "cannot write expanded BlockDev boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDIngress(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  llvm::StringSet<> channelNames, tokenPorts;
  for (auto attr : raw) {
    auto row = cast<DictionaryAttr>(attr); auto widget = row.getAs<StringAttr>("widgetClass");
    if (widget && widget.getValue() == "midas.models.FASEDMemoryTimingModel")
      for (auto field : row.getAs<DictionaryAttr>("channelMapping"))
        channelNames.insert(cast<StringAttr>(field.getValue()).getValue());
  }
  for (auto attr : raw) {
    auto row = cast<DictionaryAttr>(attr); auto name = row.getAs<StringAttr>("globalName");
    if (!name || !channelNames.count(name.getValue())) continue;
    auto endpoints = row.getAs<ArrayAttr>("sources");
    if (!endpoints || endpoints.empty()) endpoints = row.getAs<ArrayAttr>("sinks");
    for (auto target : endpoints) {
      auto ref = cast<StringAttr>(target).getValue();
      tokenPorts.insert(ref.drop_front(ref.find('>') + 1).split('.').first);
    }
  }
  require(channelNames.size() == 11 && tokenPorts.size() == 11, "FASED input channel catalog differs");
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 3) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      const StringRef names[]{"GGFASEDTokenEngine", "GGFASEDHostOutstanding", "GGFASEDIngressARQueueWrapper"};
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(names[bad]), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end()); NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5 || bad == 6) {
      SmallVector<Attribute> rows;
      for (auto attr : raw) {
        auto row = cast<DictionaryAttr>(attr); auto widget = row.getAs<StringAttr>("widgetClass");
        if (widget && widget.getValue() == "midas.models.FASEDMemoryTimingModel") {
          if (bad == 5) continue;
          NamedAttrList changed(row); changed.erase("widgetConstructorKey"); attr = changed.getDictionary(ctx);
        }
        rows.push_back(attr);
      }
      f.circuit->setAttr("rawAnnotations", b.getArrayAttr(rows));
    }
    if (bad == 7) f.circuit.setName("WrongTop");
    auto state = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketFASEDIngress(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == state, "invalid expanded FASED ingress composition mutated IR"); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions); f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDIngress(f.circuit, error)) && succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDIngressARQueueWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions, "FASED ingress changed expanded allocation");
  for (auto p : top.getPorts())
    require(!tokenPorts.count(p.name.getValue()) &&
        p.name.getValue() != "fased_ingress" && p.name.getValue() != "fased_readiness" &&
        p.name.getValue() != "fased_ingress_w_enq" && p.name.getValue() != "fased_ingress_ar_enq",
        "consumed FASED ingress boundary escapes");
  const StringRef wrappers[]{"GGFASEDIngressAW", "GGFASEDIngressWQueueWrapper", "GGFASEDIngressARQueueWrapper"};
  const StringRef queues[]{"GGFASEDIngressAWQueue10", "GGFASEDIngressWQueue16", "GGFASEDIngressARQueue4"};
  for (unsigned i = 0; i < 3; ++i)
    require(bool(child(named(f.circuit, wrappers[i]), named(f.circuit, queues[i]))), "FASED requires three independent ingress queues");
  unsigned constructors = 0, channels = 0;
  for (auto attr : f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = cast<DictionaryAttr>(attr); auto widget = row.getAs<StringAttr>("widgetClass");
    constructors += widget && widget.getValue() == "midas.models.FASEDMemoryTimingModel";
    auto name = row.getAs<StringAttr>("globalName");
    if (name && channelNames.count(name.getValue())) {
      ++channels; auto sources = row.getAs<ArrayAttr>("sources"), sinks = row.getAs<ArrayAttr>("sinks");
      require(sources && !sources.empty() && sinks && !sinks.empty(), "FASED channel lacks completed host endpoints");
    }
  }
  require(constructors == 1 && channels == 11, "FASED constructor or channel retention differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDIngress(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED ingress composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-ingress-reverse.mlir" : ".rocket-fased-ingress.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED ingress boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDIssue(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttr("goldengate.bridgeConstructor");
  const StringRef updated[]{"GGFASEDIngressARQueueWrapper", "GGFASEDIngressWQueueWrapper",
      "GGFASEDIngressAWWrapper", "GGFASEDIngressAW"};
  SmallVector<FModuleOp> previous;
  for (auto name : updated) previous.push_back(named(f.circuit, name));
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 4) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      const StringRef names[]{"GGFASEDIngressCredits", "GGFASEDIngressOrder20",
          "GGFASEDIngressIssue", "GGFASEDIngressDeadlock"};
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(names[bad]),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 4 || bad == 5) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 4) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 6) {
      NamedAttrList key(cast<DictionaryAttr>(constructor));
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 7) f.circuit.setName("WrongTop");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDIssue(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED issue rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDIssue(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDIngressIssueWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions &&
      named(f.circuit, "GGFASEDLatencyRegisters") == bank && dump(bank.getOperation()) == bankState,
      "FASED issue changed live bank or expanded allocation");
  require(bool(port(top, "fased_host_requests")) && bool(port(top, "fased_host_responses")),
      "FASED issue lacks host request/response boundary");
  for (auto p : top.getPorts())
    require(p.name != "fased_ingress_aw_deq" && p.name != "fased_ingress_w_deq" &&
        p.name != "fased_ingress_ar_deq" && p.name != "fased_host_transactions" &&
        p.name != "fased_ingress_order", "consumed FASED issue port escapes");
  for (auto [i, name] : llvm::enumerate(updated)) {
    auto m = named(f.circuit, name);
    require(m == previous[i] && bool(port(m, "fased_ingress_deadlock_context")),
        "FASED deadlock context did not update existing ingress module identity");
  }
  auto gates = named(f.circuit, "GGFASEDIngressAW");
  require(bool(child(gates, named(f.circuit, "GGFASEDIngressDeadlock"))),
      "FASED enqueue gates lack native deadlock checks");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDIssue(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED issue composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-issue-reverse.mlir" : ".rocket-fased-issue.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED issue boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDReadBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  for (auto m : f.circuit.getOps<FModuleOp>()) existing.push_back(m);
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          bad == 0 ? "GGFASEDReadBuffer16x8" : "GGFASEDReadBufferWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 4 || bad == 5) {
      NamedAttrList key(constructor);
      auto name = bad == 4 ? "axi4Edge" : "axi4Widths";
      NamedAttrList profile(cast<DictionaryAttr>(key.get(name)));
      profile.set(bad == 4 ? "idReuse" : "dataBits", b.getI64IntegerAttr(bad == 4 ? 2 : 32));
      key.set(name, profile.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDReadBuffer(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED read buffer rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDReadBuffer(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDReadBufferWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED read buffer changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED read buffer replaced an existing module operation");
  require(llvm::none_of(top.getPorts(), [](auto p) { return p.name == "fased_host_responses"; }) &&
      bool(port(top, "fased_host_requests")) &&
      bool(port(top, "fased_host_read_response")) && bool(port(top, "fased_host_write_responses")) &&
      bool(port(top, "fased_read_buffer_address")) && bool(port(top, "fased_read_buffer_deq")),
      "FASED read buffer did not consume flat host responses or expose payload/dequeue");
  require(bool(child(top, named(f.circuit, "GGFASEDReadBuffer16x8"))),
      "FASED read buffer helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDReadBuffer(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED read buffer composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-read-buffer-reverse.mlir" : ".rocket-fased-read-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED read buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDReadScheduler(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  for (auto m : f.circuit.getOps<FModuleOp>()) existing.push_back(m);
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          bad == 0 ? "GGFASEDReadScheduler" : "GGFASEDReadSchedulerWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 4 || bad == 5) {
      NamedAttrList key(constructor);
      auto name = bad == 4 ? "axi4Edge" : "axi4Widths";
      NamedAttrList profile(cast<DictionaryAttr>(key.get(name)));
      profile.set(bad == 4 ? "idReuse" : "dataBits", b.getI64IntegerAttr(bad == 4 ? 2 : 32));
      key.set(name, profile.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDReadScheduler(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED read scheduler rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDReadScheduler(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDReadSchedulerWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED read scheduler changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED read scheduler replaced an existing module operation");
  require(llvm::none_of(top.getPorts(), [](auto p) {
        return p.name == "fased_egress_readiness" || p.name == "fased_read_buffer_address" ||
               p.name == "fased_read_buffer_deq";
      }) && bool(port(top, "fased_host_requests")) &&
      bool(port(top, "fased_host_read_response")) && bool(port(top, "fased_host_write_responses")) &&
      bool(port(top, "fased_write_egress_valid")) && bool(port(top, "fased_read_egress_req")) &&
      bool(port(top, "fased_read_egress_resp")),
      "FASED read scheduler did not consume readiness/address/dequeue or expose requests/responses");
  require(bool(child(top, named(f.circuit, "GGFASEDReadScheduler"))),
      "FASED read scheduler helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDReadScheduler(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED read scheduler composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-read-scheduler-reverse.mlir" : ".rocket-fased-read-scheduler.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED read scheduler boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDWriteEgress(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  for (auto m : f.circuit.getOps<FModuleOp>()) existing.push_back(m);
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          bad == 0 ? "GGFASEDWriteEgress" : "GGFASEDWriteEgressWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 4 || bad == 5) {
      NamedAttrList key(constructor);
      auto name = bad == 4 ? "axi4Edge" : "axi4Widths";
      NamedAttrList profile(cast<DictionaryAttr>(key.get(name)));
      profile.set(bad == 4 ? "idReuse" : "dataBits", b.getI64IntegerAttr(bad == 4 ? 2 : 32));
      key.set(name, profile.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDWriteEgress(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED write egress rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDWriteEgress(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDWriteEgressWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED write egress changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED write egress replaced an existing module operation");
  require(llvm::none_of(top.getPorts(), [](auto p) {
        return p.name == "fased_write_egress_valid" || p.name == "fased_host_write_responses";
      }) && bool(port(top, "fased_host_requests")) &&
      bool(port(top, "fased_host_read_response")) && bool(port(top, "fased_host_write_response")) &&
      bool(port(top, "fased_write_egress_req")) && bool(port(top, "fased_write_egress_resp")) &&
      bool(port(top, "fased_read_egress_req")) && bool(port(top, "fased_read_egress_resp")),
      "FASED write egress did not consume flat B/readiness or preserve the read boundary");
  require(bool(child(top, named(f.circuit, "GGFASEDWriteEgress"))),
      "FASED write egress helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDWriteEgress(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED write egress composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-write-egress-reverse.mlir" : ".rocket-fased-write-egress.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED write egress boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDResponseReleaser(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  for (auto m : f.circuit.getOps<FModuleOp>()) existing.push_back(m);
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          bad == 0 ? "GGFASEDResponseReleaser" : "GGFASEDResponseReleaserWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 4 || bad == 5) {
      NamedAttrList key(constructor);
      auto name = "axi4Widths";
      NamedAttrList profile(cast<DictionaryAttr>(key.get(name)));
      profile.set(bad == 4 ? "idBits" : "dataBits", b.getI64IntegerAttr(bad == 4 ? 5 : 32));
      key.set(name, profile.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDResponseReleaser(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED response releaser rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDResponseReleaser(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDResponseReleaserWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED response releaser changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED response releaser replaced an existing module operation");
  require(llvm::none_of(top.getPorts(), [](auto p) {
        return p.name == "fased_timing" || p.name == "fased_read_egress_req" ||
            p.name == "fased_read_egress_resp" || p.name == "fased_write_egress_req" ||
            p.name == "fased_write_egress_resp";
      }) && bool(port(top, "fased_host_requests")) &&
      bool(port(top, "fased_host_read_response")) && bool(port(top, "fased_host_write_response")) &&
      bool(port(top, "fased_timing_requests")) && bool(port(top, "fased_next_read")) &&
      bool(port(top, "fased_next_write")),
      "FASED response releaser did not close timing/egress or preserve host transactions");
  require(bool(child(top, named(f.circuit, "GGFASEDResponseReleaser"))),
      "FASED response releaser helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDResponseReleaser(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED response releaser composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-response-releaser-reverse.mlir" : ".rocket-fased-response-releaser.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED response releaser boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDTimingCycle(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  for (auto m : f.circuit.getOps<FModuleOp>()) existing.push_back(m);
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          bad == 0 ? "GGFASEDTimingCycle" : "GGFASEDTimingCycleWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 4 || bad == 5) {
      NamedAttrList key(constructor);
      auto name = "axi4Widths";
      NamedAttrList profile(cast<DictionaryAttr>(key.get(name)));
      profile.set(bad == 4 ? "idBits" : "dataBits", b.getI64IntegerAttr(bad == 4 ? 5 : 32));
      key.set(name, profile.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDTimingCycle(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED timing cycle rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDTimingCycle(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDTimingCycleWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED timing cycle changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED timing cycle replaced an existing module operation");
  for (auto p : original.getPorts()) {
    auto index = port(top, p.name.getValue());
    require(top.getPorts()[index].type == p.type &&
        top.getPorts()[index].direction == p.direction,
        "FASED timing cycle changed a retained response boundary");
  }
  const StringRef added[]{"fased_timing_cycle", "fased_read_latency", "fased_write_latency",
      "fased_read_release_cycle", "fased_write_release_cycle"};
  for (unsigned i = 0; i < 5; ++i) {
    auto index = port(top, added[i]); bool latency = i == 1 || i == 2;
    require(top.getPorts()[index].type == UIntType::get(ctx, latency ? 32 : 64, false) &&
        top.getPorts()[index].direction == (latency ? Direction::In : Direction::Out),
        "FASED timing cycle added an incompatible latency/cycle boundary");
  }
  require(top.getNumPorts() == original.getNumPorts() + 5,
      "FASED timing cycle port count differs");
  require(bool(child(top, named(f.circuit, "GGFASEDTimingCycle"))),
      "FASED timing cycle helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDTimingCycle(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED timing cycle composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-timing-cycle-reverse.mlir" : ".rocket-fased-timing-cycle.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED timing cycle boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDReadLatency(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  for (auto m : f.circuit.getOps<FModuleOp>()) existing.push_back(m);
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          bad == 0 ? "GGFASEDReadLatency10" : "GGFASEDReadLatencyWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 4 || bad == 5) {
      NamedAttrList key(constructor);
      auto name = "axi4Widths";
      NamedAttrList profile(cast<DictionaryAttr>(key.get(name)));
      profile.set(bad == 4 ? "idBits" : "dataBits", b.getI64IntegerAttr(bad == 4 ? 5 : 32));
      key.set(name, profile.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDReadLatency(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED read latency rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDReadLatency(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDReadLatencyWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED read latency changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED read latency replaced an existing module operation");
  for (auto p : original.getPorts()) {
    if (p.name.getValue() == "fased_next_read" ||
        p.name.getValue() == "fased_read_release_cycle") continue;
    auto index = port(top, p.name.getValue());
    require(top.getPorts()[index].type == p.type &&
        top.getPorts()[index].direction == p.direction,
        "FASED read latency changed a retained response boundary");
  }
  require(top.getNumPorts() == original.getNumPorts() - 2,
      "FASED read latency must consume exactly deadline and completion metadata");
  require(bool(child(top, named(f.circuit, "GGFASEDReadLatency10"))),
      "FASED read latency helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDReadLatency(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED read latency composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-read-latency-reverse.mlir" : ".rocket-fased-read-latency.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED read latency boundary"); f.root->print(file); file << '\n';
  }
}

void rocketResponses(Fixture &f, ArrayAttr reads, StringRef output, bool reverse,
                     unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto top = named(f.circuit, f.circuit.getName());
  auto bound = named(f.circuit, "GGPrintBridgeHostWrapper");
  auto hosts = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto writes = named(f.circuit, "GGControlWidgetWriteWrapper");
  auto bank = named(f.circuit, "GGCPUStreamCountBank");
  auto words = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  for (unsigned bad = 0; bad < 10; ++bad) {
    SmallVector<Attribute> rows(reads.begin(), reads.end());
    if (bad == 0) rows.pop_back();
    if (bad == 1 || bad == 2 || bad == 3) {
      NamedAttrList row(cast<DictionaryAttr>(rows[7]));
      if (bad == 1) row.set("port", cast<DictionaryAttr>(rows[8]).get("port"));
      if (bad == 2) row.set("slave", b.getI32IntegerAttr(9));
      if (bad == 3) row.set("name", b.getStringAttr("SimulationMaster_0"));
      rows[7] = row.getDictionary(ctx);
    }
    top->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
    // Mutate both request catalogs in the wrong-slave/later-bank cases so
    // response preflight cannot succeed merely by comparing AW/W with AR.
    if (bad == 2 || bad == 3) writes->setAttr("goldengate.controlWriteBindings", b.getArrayAttr(rows));
    if (bad == 4) {
      rows.assign(regions.begin(), regions.end()); NamedAttrList row(cast<DictionaryAttr>(rows[8]));
      row.set("start", b.getI64IntegerAttr(548)); rows[8] = row.getDictionary(ctx);
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) { rows.assign(words.begin(), words.end()); rows.pop_back(); bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows)); }
    if (bad == 6) { rows.assign(hosts.begin(), hosts.end()); std::reverse(rows.begin(), rows.end()); bound->setAttr("goldengate.printHostBindings", b.getArrayAttr(rows)); }
    if (bad == 7) bound->removeAttr("goldengate.printHostBindings");
    FModuleOp collision;
    if (bad == 8) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr("GGControlWriteTracker"),
          ConventionAttr::get(ctx, Convention::Internal), SmallVector<PortInfo>{});
    }
    if (bad == 9) {
      // Equal, well-typed stale request/host catalogs must not swap the two
      // actual Print AR connections when the response arbiter is composed.
      rows.assign(reads.begin(), reads.end());
      for (unsigned i : {7U, 8U}) {
        NamedAttrList row(cast<DictionaryAttr>(rows[i]));
        row.set("port", cast<DictionaryAttr>(reads[i == 7 ? 8 : 7]).get("port"));
        rows[i] = row.getDictionary(ctx);
      }
      top->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
      writes->setAttr("goldengate.controlWriteBindings", b.getArrayAttr(rows));
      rows.assign(hosts.begin(), hosts.end());
      for (unsigned i : {0U, 1U}) {
        NamedAttrList row(cast<DictionaryAttr>(rows[i]));
        row.set("controlPort", cast<DictionaryAttr>(hosts[1 - i]).get("controlPort"));
        rows[i] = row.getDictionary(ctx);
      }
      bound->setAttr("goldengate.printHostBindings", b.getArrayAttr(rows));
    }
    auto state = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketControlResponses(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == state, "invalid Rocket response composition mutated IR " + std::to_string(bad));
    ++rejected;
    top->setAttr("goldengate.controlReadBindings", reads);
    writes->setAttr("goldengate.controlWriteBindings", reads);
    decoder->setAttr("goldengate.controlRegions", regions);
    bank->setAttr("goldengate.mmioRegisters", words);
    bound->setAttr("goldengate.printHostBindings", hosts);
    if (collision) collision.erase();
  }
  // Preserve arbitrary annotation payloads/order while allowing target transfer.
  std::function<Attribute(Attribute)> payload = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) if (s.getValue().starts_with("~")) return b.getStringAttr("<target>");
    if (auto rows = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto row : rows) out.push_back(payload(row)); return b.getArrayAttr(out); }
    if (auto row = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto field : row) out.set(field.getName(), payload(field.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  auto annotations = payload(f.circuit->getAttr("rawAnnotations"));
  require(succeeded(goldengate::mapPrintBridgeRocketControlResponses(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  require(f.circuit.getName() == "GGControlWriteTrackerWrapper" &&
      payload(f.circuit->getAttr("rawAnnotations")) == annotations, "Rocket responses changed annotation payloads/order");
  auto final = named(f.circuit, f.circuit.getName());
  for (auto attr : reads) {
    auto control = cast<DictionaryAttr>(attr).getAs<StringAttr>("port");
    for (auto p : final.getPorts()) require(p.name != control, "connected Rocket response bank escaped composition");
  }
  for (StringRef name : {"GGControlReadTracker", "GGControlWriteTracker"}) {
    auto tracker = named(f.circuit, name);
    require(tracker->getAttrOfType<IntegerAttr>("goldengate.trackerSlots").getInt() == 64 &&
        tracker->getAttrOfType<IntegerAttr>("goldengate.trackerTagWidth").getInt() == 12 &&
        tracker->getAttrOfType<IntegerAttr>("goldengate.trackerDequeuePorts").getInt() == 14 &&
        tracker->getAttrOfType<IntegerAttr>("goldengate.trackerRouteWidth").getInt() == 4,
        "expanded Rocket trackers need thirteen slaves plus error");
  }
  auto key = [&](Value v) -> std::string {
    std::string suffix;
    while (auto field = v.getDefiningOp<SubfieldOp>()) { suffix = "." + field.getFieldName().str() + suffix; v = field.getInput(); }
    if (auto inst = v.getDefiningOp<InstanceOp>()) return inst.getName().str() + "." + inst.getPortNameStr(cast<OpResult>(v).getResultNumber()).str() + suffix;
    auto arg = cast<BlockArgument>(v); auto m = cast<FModuleOp>(arg.getOwner()->getParentOp());
    return "top." + m.getPortName(arg.getArgNumber()).str() + suffix;
  };
  auto connections = [&](FModuleOp m) {
    std::map<std::string, Value> result;
    for (auto con : m.getOps<StrictConnectOp>()) require(result.emplace(key(con.getDest()), con.getSrc()).second, "duplicate response driver");
    return result;
  };
  std::function<std::set<std::string>(Value)> terms = [&](Value v) {
    if (auto op = v.getDefiningOp<AndPrimOp>()) {
      auto left = terms(op.getLhs()), right = terms(op.getRhs()); left.insert(right.begin(), right.end()); return left;
    }
    return std::set<std::string>{key(v)};
  };
  auto rt = connections(named(f.circuit, "GGControlReadTrackerWrapper"));
  auto wt = connections(final);
  for (bool read : {true, false}) {
    std::string channel = read ? "r" : "b", arb = read ? "readArbiter" : "writeArbiter";
    auto m = named(f.circuit, read ? "GGControlReadArbiterWrapper" : "GGControlWriteArbiterWrapper");
    auto nets = connections(m);
    require(named(f.circuit, read ? "GGControlReadArbiter" : "GGControlWriteArbiter")
        ->getAttrOfType<IntegerAttr>(read ? "goldengate.readArbiterSources" : "goldengate.writeArbiterSources").getInt() == 14,
        "expanded Rocket arbiter source count differs");
    for (auto attr : reads) {
      auto row = cast<DictionaryAttr>(attr); auto portName = row.getAs<StringAttr>("port").getValue().str();
      auto i = std::to_string(row.getAs<IntegerAttr>("slave").getInt());
      auto ap = arb + ".in_" + i + "_", rp = "sim." + portName + "." + channel;
      require(key(nets.at(rp + ".ready")) == ap + "ready" && key(nets.at(ap + "valid")) == rp + ".valid", "Rocket response handshake routed to wrong bank");
      for (StringRef field : read ? ArrayRef<StringRef>{"resp", "data", "last", "id", "user"} : ArrayRef<StringRef>{"resp", "id", "user"})
        require(key(nets.at(ap + "bits_" + field.str())) == rp + ".bits." + field.str(), "Rocket response payload/ID routed to wrong bank");
      if (read) {
        auto pred = terms(rt.at("controlReadTracker.deq_" + i + "_valid"));
        require(pred == std::set<std::string>{"top." + portName + ".r.ready", rp + ".valid", rp + ".bits.last"} &&
            key(rt.at("controlReadTracker.deq_" + i + "_tag")) == rp + ".bits.id", "Rocket R retirement is not accepted final beat with response ID");
      }
    }
    // B retirement is wired for all thirteen normal sources, including four
    // late attachment boundaries, and the error source at index thirteen.
    for (unsigned i = 0; i < 14; ++i) if (!read) {
      auto p = arb + ".in_" + std::to_string(i) + "_", q = "ctrl_write_tracker_deq_" + std::to_string(i) + "_";
      require(terms(nets.at("top." + q + "valid")) == std::set<std::string>{p + "ready", p + "valid"} &&
          key(nets.at("top." + q + "tag")) == p + "bits_id" &&
          key(wt.at("controlWriteTracker.deq_" + std::to_string(i) + "_valid")) == "sim." + q + "valid" &&
          key(wt.at("controlWriteTracker.deq_" + std::to_string(i) + "_tag")) == "sim." + q + "tag",
          "Rocket B retirement detached from arbiter acceptance or response ID");
    }
    auto ep = arb + ".in_13_", rp = "sim.ctrl_error_" + channel + "_";
    require(key(nets.at(rp + "ready")) == ep + "ready" && key(nets.at(ep + "valid")) == rp + "valid", "Rocket error handshake detached");
    for (StringRef field : read ? ArrayRef<StringRef>{"resp", "data", "last", "id", "user"} : ArrayRef<StringRef>{"resp", "id", "user"})
      require(key(nets.at(ep + "bits_" + field.str())) == rp + "bits_" + field.str(), "Rocket error response payload/ID detached");
  }
  std::set<unsigned> connected;
  for (auto row : reads) connected.insert(cast<DictionaryAttr>(row).getAs<IntegerAttr>("slave").getInt());
  SmallVector<unsigned> late;
  for (unsigned i = 0; i < regions.size(); ++i) if (!connected.count(i)) late.push_back(i);
  require(late.size() == 4, "Rocket response composition must expose exactly four later banks");
  auto ra = connections(named(f.circuit, "GGControlReadArbiterWrapper"));
  for (auto i : late) {
    auto p = "readArbiter.in_" + std::to_string(i) + "_", q = "sim.ctrl_read_tracker_deq_" + std::to_string(i) + "_";
    require(terms(ra.at(q + "valid")) == std::set<std::string>{p + "ready", p + "valid", p + "bits_last"} &&
        key(ra.at(q + "tag")) == p + "bits_id", "late Rocket R retirement detached");
    for (StringRef channel : {"read", "write"}) {
      auto prefix = "ctrl_" + channel.str() + "_arb_in_" + std::to_string(i) + "_";
      for (StringRef field : channel == "read" ? ArrayRef<StringRef>{"valid", "bits_resp", "bits_data", "bits_last", "bits_id", "bits_user"} : ArrayRef<StringRef>{"valid", "bits_resp", "bits_id", "bits_user"})
        require(final.getPorts()[port(final, prefix + field.str())].direction == Direction::In, "late Rocket response boundary missing");
      require(final.getPorts()[port(final, prefix + "ready")].direction == Direction::Out, "late Rocket response readiness missing");
    }
  }
  require(terms(rt.at("controlReadTracker.deq_13_valid")) == std::set<std::string>{"top.ctrl_error_r_ready", "sim.ctrl_error_r_valid", "sim.ctrl_error_r_bits_last"} &&
      key(rt.at("controlReadTracker.deq_13_tag")) == "sim.ctrl_error_r_bits_id", "Rocket error R retirement detached");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-responses-reverse.mlir" : ".rocket-responses.mlir")).str(), ec);
    require(!ec, "cannot write Rocket/Print response boundary"); f.root->print(file); file << '\n';
  }
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketControlResponses(f.circuit, error)) && dump(*f.root) == state, "repeat Rocket responses mutated IR");
  ++rejected;
  llvm::outs() << "PASS Rocket/Print responses: ten connected R/B sources, four late boundaries, fourteen retirements and unchanged annotation payloads\n";
}

void rocketStreams(MLIRContext &ctx, StringRef baseline, StringRef controlBaseline, StringRef output) {
  if (baseline.empty()) return;
  unsigned rejected = 0;
  for (bool reverse : {false, true}) {
    auto rocket = parseSourceFile<ModuleOp>(baseline, &ctx);
    require(bool(rocket), "cannot parse Rocket queue boundary");
    auto c = *rocket->getOps<CircuitOp>().begin(); std::string error;
    SmallVector<goldengate::CPUStreamSourcePort> sources;
    SmallVector<goldengate::CPUStreamCountPort> counts;
    auto before = dump(*rocket);
    require(succeeded(goldengate::deriveRocketCPUStreamPorts(c, sources, counts, error)) &&
        dump(*rocket) == before && sources.size() == 1 && counts.size() == 1, error);
    auto queueTop = named(c, c.getName());
    Fixture f(ctx); OpBuilder b(&ctx);
    for (auto m : c.getOps<FModuleLike>()) {
      bool exists = false;
      for (auto prior : f.circuit.getOps<FModuleLike>()) exists |= prior.getName() == m.getName();
      require(!exists, "unexpected Rocket/Print module collision");
      f.circuit.getBodyBlock()->push_back(m->clone());
    }
    auto original = named(f.circuit, "Top");
    auto importedTop = named(f.circuit, queueTop.getName());
    // Preserve the five live Rocket bridge control endpoints while appending
    // Print and expanding the CPU stream engine. They share the final router.
    for (StringRef name : {"clockBridge_ctrl", "resetBridge_ctrl", "uartBridge_ctrl",
                          "peekPokeBridge_ctrl", "tracerv_ctrl"}) {
      auto p = importedTop.getPorts()[port(importedTop, name)];
      original.insertPorts({{original.getNumPorts(), p}});
    }
    b.setInsertionPointToStart(original.getBodyBlock());
    auto instance = b.create<InstanceOp>(f.circuit.getLoc(), importedTop, "rocket");
    for (StringRef p : {"hostClock", "hostReset"})
      b.create<ConnectOp>(f.circuit.getLoc(), instance.getResult(port(importedTop, p)), original.getArgument(port(original, p)));
    for (StringRef p : {"tracerv_stream", "tracerv_stream_count"})
      b.create<ConnectOp>(f.circuit.getLoc(), original.getArgument(port(original, p)), instance.getResult(port(importedTop, p)));
    for (StringRef p : {"clockBridge_ctrl", "resetBridge_ctrl", "uartBridge_ctrl",
                       "peekPokeBridge_ctrl", "tracerv_ctrl"})
      b.create<ConnectOp>(f.circuit.getLoc(), instance.getResult(port(importedTop, p)), original.getArgument(port(original, p)));
    // Forward actual Rocket TSI, BlockDev and FASED channels, including their nested
    // forward descriptors and constructor metadata, to the active Print top.
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> annotations;
    for (auto attr : f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) annotations.push_back(attr);
    for (StringRef widgetClass : {"firechip.goldengateimplementations.TSIBridgeModule",
                                 "firechip.goldengateimplementations.BlockDevBridgeModule",
                                 "midas.models.FASEDMemoryTimingModel"}) {
      DictionaryAttr bridge;
      for (auto attr : raw) {
        auto row = cast<DictionaryAttr>(attr);
        auto widget = row.getAs<StringAttr>("widgetClass");
        if (widget && widget.getValue() == widgetClass) bridge = row;
      }
      require(bool(bridge), "actual Rocket bridge constructor is absent");
      llvm::StringSet<> channelNames, forwarded;
      for (auto field : bridge.getAs<DictionaryAttr>("channelMapping"))
        channelNames.insert(cast<StringAttr>(field.getValue()).getValue());
      std::function<Attribute(Attribute)> retargetBridge = [&](Attribute attr) -> Attribute {
        if (auto str = dyn_cast<StringAttr>(attr)) {
          auto value = str.getValue();
          std::string prefix = "~" + c.getName().str();
          if (!value.consume_front(prefix)) return attr;
          auto suffix = value.str();
          std::string oldTop = "|" + importedTop.getName().str() + ">";
          if (StringRef(suffix).starts_with(oldTop)) suffix.replace(0, oldTop.size(), "|Top>");
          return b.getStringAttr("~Top" + suffix);
        }
        if (auto rows = dyn_cast<ArrayAttr>(attr)) { SmallVector<Attribute> out; for (auto row : rows) out.push_back(retargetBridge(row)); return b.getArrayAttr(out); }
        if (auto row = dyn_cast<DictionaryAttr>(attr)) { NamedAttrList out; for (auto field : row) out.set(field.getName(), retargetBridge(field.getValue())); return out.getDictionary(&ctx); }
        return attr;
      };
      annotations.push_back(retargetBridge(bridge));
      for (auto attr : raw) {
        auto row = cast<DictionaryAttr>(attr); auto name = row.getAs<StringAttr>("globalName");
        if (!name || !channelNames.count(name.getValue())) continue;
        auto endpoints = row.getAs<ArrayAttr>("sources");
        if (!endpoints || endpoints.empty()) endpoints = row.getAs<ArrayAttr>("sinks");
        require(endpoints && !endpoints.empty(), "actual Rocket channel has no endpoint");
        auto target = cast<StringAttr>(endpoints[0]).getValue();
        auto ref = target.drop_front(target.find('>') + 1).split('.').first;
        if (forwarded.insert(ref).second) {
          auto index = port(importedTop, ref); auto p = importedTop.getPorts()[index];
          original.insertPorts({{original.getNumPorts(), p}});
          b.create<ConnectOp>(f.circuit.getLoc(),
              p.direction == Direction::In ? instance.getResult(index) : original.getArgument(port(original, ref)),
              p.direction == Direction::In ? original.getArgument(port(original, ref)) : instance.getResult(index));
        }
        annotations.push_back(retargetBridge(row));
      }
      require(forwarded.size() == (widgetClass.ends_with("TSIBridgeModule") ? 5 : widgetClass.ends_with("BlockDevBridgeModule") ? 9 : 11),
          "actual Rocket bridge requires distinct live token ports");
    }
    f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    if (reverse) std::reverse(f.hosts.begin(), f.hosts.end());
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)) && succeeded(verify(*f.root)), error);
    before = dump(*f.root);
    require(succeeded(goldengate::deriveRocketCPUStreamPorts(f.circuit, sources, counts, error)) &&
        dump(*f.root) == before, "Rocket stream derivation mutated bound Print circuit");
    auto reject = [&]() {
      auto state = dump(*f.root);
      require(failed(goldengate::mapPrintBridgeRocketCPUStreams(f.circuit, error)) && !error.empty() &&
          dump(*f.root) == state, "invalid Rocket/Print attachment mutated circuit");
      auto savedName = sources[0].streamName, savedCount = counts[0].streamName;
      require(failed(goldengate::deriveRocketCPUStreamPorts(f.circuit, sources, counts, error)) &&
          sources.size() == 1 && counts.size() == 1 && sources[0].streamName == savedName &&
          counts[0].streamName == savedCount, "failed derivation changed caller catalog");
      ++rejected;
    };
    auto queue = named(f.circuit, "GGTracerVStreamQueue6144");
    auto info = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
    for (StringRef key : {"name", "depth", "widthBytes", "index"}) {
      NamedAttrList bad(info); bad.erase(key); queue->setAttr("goldengate.streamParameters", bad.getDictionary(&ctx));
      reject(); queue->setAttr("goldengate.streamParameters", info);
    }
    auto inner = named(f.circuit, "Top");
    ConnectOp countConnect;
    for (auto connect : inner.getOps<ConnectOp>())
      if (connect.getDest() == inner.getArgument(port(inner, "tracerv_stream_count"))) countConnect = connect;
    require(bool(countConnect), "missing Rocket count forwarding");
    auto saved = countConnect.getSrc(); b.setInsertionPoint(countConnect);
    auto zero = b.create<ConstantOp>(f.circuit.getLoc(), UIntType::get(&ctx, 13), APInt(13, 0));
    countConnect->setOperand(1, zero.getResult()); reject(); countConnect->setOperand(1, saved); zero.erase();
    b.setInsertionPoint(countConnect);
    auto duplicate = b.create<ConnectOp>(f.circuit.getLoc(), countConnect.getDest(), saved);
    reject(); duplicate.erase();
    // Two instances of one queue definition still represent distinct storage.
    b.setInsertionPoint(countConnect);
    auto otherRocket = b.create<InstanceOp>(f.circuit.getLoc(), importedTop, "otherRocket");
    countConnect->setOperand(1, otherRocket.getResult(port(importedTop, "tracerv_stream_count")));
    reject(); countConnect->setOperand(1, saved); otherRocket.erase();
    require(succeeded(goldengate::mapPrintBridgeRocketCPUStreams(f.circuit, error)) && succeeded(verify(*f.root)), error);
    auto allocations = named(f.circuit, "GGCPUStreamRead")->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
    auto words = named(f.circuit, "GGCPUStreamCountBank")->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    require(allocations.size() == 3 && words.size() == 3, "Rocket/Print DMA/count catalog differs");
    for (unsigned i = 0; i < 3; ++i) {
      auto stream = cast<DictionaryAttr>(allocations[i]); auto reg = cast<DictionaryAttr>(words[i]);
      std::string name = i == 0 ? "TRACERVBRIDGEMODULE_0_to_cpu_stream" :
          "PRINTBRIDGEMODULE_" + std::to_string(i - 1) + "_to_cpu_stream";
      require(stream.getAs<StringAttr>("name") == name && stream.getAs<IntegerAttr>("index").getInt() == i &&
          stream.getAs<IntegerAttr>("bufferBaseAddress").getInt() == int64_t(i * 524288) &&
          reg.getAs<StringAttr>("name") == name + "_count" && reg.getAs<IntegerAttr>("offset").getInt() == i * 4,
          "Rocket/Print ordered DMA/count allocation differs");
    }
    require(succeeded(goldengate::mapCPUStreamControl(f.circuit, 25, 12, error)) && succeeded(verify(*f.root)), error);
    goldengate::ControlMMIOWidget widget;
    require(succeeded(goldengate::deriveControlMMIOWidget(f.circuit, "CPUManagedStreamEngine_0", "GGCPUStreamMCRFile",
        {"GGCPUStreamCountBank"}, widget, error)) && widget.registerCount == 3, error);
    SmallVector<goldengate::ControlMMIORegion> regions;
    require(succeeded(goldengate::allocateControlMMIORegions(25, {widget}, regions, error)) &&
        regions.size() == 1 && regions[0].size == 16, "three CPU occupancy words require a sixteen-byte region");
    if (!controlBaseline.empty()) {
      auto platform = parseSourceFile<ModuleOp>(controlBaseline, &ctx);
      require(bool(platform), "cannot parse materialized Rocket control banks");
      auto pc = *platform->getOps<CircuitOp>().begin();
      // Import only the actual register banks/adapters and their dependencies.
      // The existing expanded CPU bank remains connected to the live queues;
      // importing a historical platform wrapper would duplicate that transport.
      llvm::StringSet<> present;
      for (auto m : f.circuit.getOps<FModuleLike>()) present.insert(m.getModuleName());
      const StringRef bankNames[]{"GGSimulationMasterBank", "GGPeekPokeMCRFile", "GGPeekPokeMMIOBank",
          "GGResetPulseBridgeMCRFile", "GGResetPulseBridge", "GGBlockDevMMIOBank",
          "GGUARTMCRFile", "GGUARTMMIOBank", "GGFASEDLatencyRegisters", "GGFASEDRequestLimits",
          "GGFASEDHistograms", "GGFASEDStatistics", "GGFASEDFunctionalModelRegister", "GGFASEDResponseErrors",
          "GGTracerVMCRFile", "GGTracerVTriggerConfig", "GGTSIMMIOBank", "GGClockBridgeMCRFile",
          "GGSingleClockBridge", "GGLoadMemMCRFile", "GGLoadMemWriteMMIOBank", "GGLoadMemWriteDataWrapper",
          "GGLoadMemReadRequestWrapper", "GGLoadMemReadDataWrapper"};
      auto importedName = [&](StringRef name) {
        return llvm::is_contained(bankNames, name) ? name.str() : "CatalogRocket_" + name.str();
      };
      std::function<void(StringRef)> importBank = [&](StringRef name) {
        auto symbol = importedName(name);
        if (!present.insert(symbol).second) return;
        FModuleLike found;
        for (auto m : pc.getOps<FModuleLike>()) if (m.getModuleName() == name) found = m;
        require(bool(found), "missing Rocket bank dependency " + name.str());
        auto copy = found->clone(); SymbolTable::setSymbolName(copy, symbol);
        f.circuit.getBodyBlock()->push_back(copy);
        copy->walk([&](InstanceOp i) {
          auto originalName = i.getModuleName().str(); importBank(originalName);
          i.setModuleNameAttr(FlatSymbolRefAttr::get(&ctx, importedName(originalName)));
        });
      };
      for (StringRef name : bankNames) importBank(name);
      require(succeeded(verify(*f.root)), "expanded Rocket platform register banks invalid");
      SmallVector<goldengate::ControlMMIOWidget> catalog;
      before = dump(*f.root);
      require(succeeded(goldengate::allocateRocketControlMMIORegions(f.circuit, 25, f.hosts,
          catalog, regions, error)) && dump(*f.root) == before && catalog.size() == 13, error);
      for (auto r : regions) {
        if (r.name == "PrintBridgeModule_0") require(r.start == 544 && r.size == 32, "full first Print region differs");
        if (r.name == "PrintBridgeModule_1") require(r.start == 576 && r.size == 32, "full second Print region differs");
        if (r.name == "SimulationMaster_0") require(r.start == 608 && r.size == 16, "full master region differs");
        if (r.name == "CPUManagedStreamEngine_0") require(r.start == 624 && r.size == 16, "expanded CPU region differs");
        if (r.name == "ResetPulseBridgeModule_0") require(r.start == 640 && r.size == 8, "reset moves after expanded CPU bank");
      }
      auto preservedCatalog = catalog; auto preservedRegions = regions;
      auto rejectAllocation = [&](unsigned bits = 25) {
        auto state = dump(*f.root);
        require(failed(goldengate::allocateRocketControlMMIORegions(f.circuit, bits, f.hosts, catalog, regions, error)) &&
            !error.empty() && dump(*f.root) == state && catalog.size() == preservedCatalog.size() &&
            regions.size() == preservedRegions.size(), "failed full allocation mutated IR/results");
        for (auto [i, r] : llvm::enumerate(regions)) require(r.name == preservedRegions[i].name &&
            r.start == preservedRegions[i].start && r.size == preservedRegions[i].size, "failure changed regions");
        for (auto [i, w] : llvm::enumerate(catalog)) require(w.name == preservedCatalog[i].name &&
            w.registerCount == preservedCatalog[i].registerCount, "failure changed widgets");
        ++rejected;
      };
      auto reader = named(f.circuit, "GGCPUStreamRead"), bank = named(f.circuit, "GGCPUStreamCountBank");
      for (bool source : {false, true}) {
        auto attrName = source ? "goldengate.sourceStreams" : "goldengate.mmioRegisters";
        auto savedRows = source ? allocations : words;
        SmallVector<Attribute> rows(savedRows.begin(), savedRows.end());
        rows.pop_back(); (source ? reader : bank)->setAttr(attrName, b.getArrayAttr(rows));
        rejectAllocation(); (source ? reader : bank)->setAttr(attrName, savedRows);
        for (StringRef key : source ? ArrayRef<StringRef>{"name", "index"} : ArrayRef<StringRef>{"name", "offset", "readable", "writeable"}) {
          rows.assign(savedRows.begin(), savedRows.end()); NamedAttrList bad(cast<DictionaryAttr>(rows[1]));
          bad.erase(key); rows[1] = bad.getDictionary(&ctx); (source ? reader : bank)->setAttr(attrName, b.getArrayAttr(rows));
          rejectAllocation(); (source ? reader : bank)->setAttr(attrName, savedRows);
        }
        rows.assign(savedRows.begin(), savedRows.end()); std::swap(rows[1], rows[2]);
        (source ? reader : bank)->setAttr(attrName, b.getArrayAttr(rows));
        require(succeeded(goldengate::allocateRocketControlMMIORegions(f.circuit, 25, f.hosts,
            catalog, regions, error)), "metadata row permutation changed full allocation");
        (source ? reader : bank)->setAttr(attrName, savedRows);
        rows.assign(savedRows.begin(), savedRows.end()); NamedAttrList duplicate(cast<DictionaryAttr>(rows[2]));
        auto key = source ? "index" : "offset";
        duplicate.set(key, cast<DictionaryAttr>(rows[1]).get(key)); rows[2] = duplicate.getDictionary(&ctx);
        (source ? reader : bank)->setAttr(attrName, b.getArrayAttr(rows));
        rejectAllocation(); (source ? reader : bank)->setAttr(attrName, savedRows);
      }
      rejectAllocation(1);
      std::reverse(f.hosts.begin(), f.hosts.end()); rejectAllocation();
      std::reverse(f.hosts.begin(), f.hosts.end());
      auto bound = named(f.circuit, "GGPrintBridgeHostWrapper");
      auto bindings = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
      bound->removeAttr("goldengate.printHostBindings"); rejectAllocation();
      bound->setAttr("goldengate.printHostBindings", bindings);
      auto decoded = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGControlErrorWrapper" {
        firrtl.module @GGControlErrorWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>) {}
      } })", &ctx);
      auto dc = *decoded->getOps<CircuitOp>().begin(); dc->setAttr("rawAnnotations", b.getArrayAttr({}));
      require(succeeded(goldengate::addControlAddressDecode(dc, 25, regions, error)) && succeeded(verify(*decoded)), error);
      if (!output.empty()) {
        std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-mmio-reverse.mlir" : ".rocket-mmio.mlir")).str(), ec);
        require(!ec, "cannot write full Rocket/Print decoder"); decoded->print(file); file << '\n';
      }
      // Attach the actual LoadMem AXI adapter. Its MCR-side register fragments
      // remain an explicit boundary here; the five original bridge controls,
      // CPU occupancy bank and both Print banks already reach live storage.
      auto inner = named(f.circuit, f.circuit.getName());
      auto loadmem = named(f.circuit, "GGLoadMemMCRFile");
      auto wrapperPorts = inner.getPorts();
      unsigned ctrl = wrapperPorts.size();
      wrapperPorts.push_back({b.getStringAttr("loadmem_ctrl"), loadmem.getPortType(2), Direction::In});
      unsigned mcr = wrapperPorts.size();
      wrapperPorts.push_back({b.getStringAttr("loadmem_mcr"), loadmem.getPortType(3), Direction::In});
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      auto platformTop = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr("GGRocketPrintControlWrapper"),
          inner.getConventionAttr(), wrapperPorts);
      b.setInsertionPointToStart(platformTop.getBodyBlock());
      auto sim = b.create<InstanceOp>(f.circuit.getLoc(), inner, "sim");
      for (auto [p, info] : llvm::enumerate(inner.getPorts()))
        b.create<ConnectOp>(f.circuit.getLoc(), info.direction == Direction::In ? sim.getResult(p) : platformTop.getArgument(p),
            info.direction == Direction::In ? platformTop.getArgument(p) : sim.getResult(p));
      auto adapter = b.create<InstanceOp>(f.circuit.getLoc(), loadmem, "loadmem");
      for (unsigned p = 0; p < 2; ++p)
        b.create<ConnectOp>(f.circuit.getLoc(), adapter.getResult(p), platformTop.getArgument(p));
      b.create<ConnectOp>(f.circuit.getLoc(), adapter.getResult(2), platformTop.getArgument(ctrl));
      b.create<ConnectOp>(f.circuit.getLoc(), adapter.getResult(3), platformTop.getArgument(mcr));
      auto oldPrefix = "~" + f.circuit.getName().str();
      std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
        if (auto s = dyn_cast<StringAttr>(attr)) {
          auto value = s.getValue();
          if (value == oldPrefix) return b.getStringAttr("~GGRocketPrintControlWrapper");
          if (value.consume_front(oldPrefix + "|")) {
            std::string suffix = "|" + value.str();
            std::string modulePrefix = "|" + inner.getName().str() + ">";
            // Every original port is copied by this LoadMem attachment.
            // Transfer its module identity as well as the circuit prefix.
            if (StringRef(suffix).starts_with(modulePrefix)) {
              auto name = StringRef(suffix).drop_front(modulePrefix.size()).split('.').first.split('[').first;
              for (auto p : inner.getPorts()) if (p.name == name) {
                suffix.replace(0, modulePrefix.size(), "|GGRocketPrintControlWrapper>"); break;
              }
            }
            return b.getStringAttr("~GGRocketPrintControlWrapper" + suffix);
          }
        }
        if (auto rows = dyn_cast<ArrayAttr>(attr)) {
          SmallVector<Attribute> result; for (auto row : rows) result.push_back(retarget(row));
          return b.getArrayAttr(result);
        }
        if (auto row = dyn_cast<DictionaryAttr>(attr)) {
          NamedAttrList result; for (auto field : row) result.set(field.getName(), retarget(field.getValue()));
          return result.getDictionary(&ctx);
        }
        return attr;
      };
      f.circuit->setAttr("rawAnnotations", retarget(f.circuit->getAttr("rawAnnotations")));
      f.circuit.setName("GGRocketPrintControlWrapper");
      require(succeeded(goldengate::addControlErrorSlave(f.circuit, 25, 12, "GGRocketPrintControlWrapper", error)) &&
          succeeded(goldengate::addControlAddressDecode(f.circuit, 25, regions, error)) &&
          succeeded(goldengate::addControlWriteRoute(f.circuit, error)) &&
          succeeded(goldengate::addControlWriteDispatch(f.circuit, error)), error);
      for (unsigned bad = 0; bad < 7; ++bad) {
        auto rows = SmallVector<Attribute>(bindings.begin(), bindings.end());
        auto first = cast<DictionaryAttr>(rows[0]); NamedAttrList modified(first);
        if (bad == 0) rows.pop_back();
        if (bad == 1) std::reverse(rows.begin(), rows.end());
        if (bad == 2) modified.erase("controlPort");
        if (bad == 3) modified.set("hostModule", b.getStringAttr("GGCPUStreamCountBank"));
        if (bad == 4) modified.set("controlPort", b.getStringAttr("cpuStream_ctrl"));
        if (bad == 5) modified.set("widgetName", b.getStringAttr("PrintBridgeModule_2"));
        if (bad == 6) {
          modified.set("controlPort", cast<DictionaryAttr>(bindings[1]).get("controlPort"));
          NamedAttrList second(cast<DictionaryAttr>(rows[1])); second.set("controlPort", first.get("controlPort"));
          rows[1] = second.getDictionary(&ctx);
        }
        if (bad >= 2) rows[0] = modified.getDictionary(&ctx);
        bound->setAttr("goldengate.printHostBindings", b.getArrayAttr(rows));
        auto state = dump(*f.root);
        require(failed(goldengate::bindRocketControlWidgetWrites(f.circuit, error)) && !error.empty() &&
            dump(*f.root) == state, "invalid full-platform Print request binding mutated IR");
        ++rejected; bound->setAttr("goldengate.printHostBindings", bindings);
      }
      require(succeeded(goldengate::bindRocketControlWidgetWrites(f.circuit, error)) &&
          succeeded(goldengate::addControlReadDispatch(f.circuit, error)) && succeeded(verify(*f.root)), error);
      auto writes = named(f.circuit, "GGControlWidgetWriteWrapper");
      std::function<std::string(Value)> key = [&](Value value) -> std::string {
        if (auto field = value.getDefiningOp<SubfieldOp>()) return key(field.getInput()) + "." + field.getFieldName().str();
        if (auto i = value.getDefiningOp<InstanceOp>())
          return "sim." + i.getPortNameStr(cast<OpResult>(value).getResultNumber()).str();
        if (auto arg = dyn_cast<BlockArgument>(value)) return "top." + writes.getPortName(arg.getArgNumber()).str();
        throw std::runtime_error("unexpected full-platform write binding value");
      };
      std::map<std::string, std::string> actual, expected;
      for (auto connect : writes.getOps<StrictConnectOp>())
        require(actual.emplace(key(connect.getDest()), key(connect.getSrc())).second, "duplicate full-platform request driver");
      for (auto attr : writes->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings")) {
        auto row = cast<DictionaryAttr>(attr); auto control = row.getAs<StringAttr>("port").getValue().str();
        auto slave = row.getAs<IntegerAttr>("slave").getInt();
        for (StringRef ch : {"aw", "w"}) {
          auto prefix = "sim.ctrl_write_dispatch_slave_" + std::to_string(slave) + "_" + ch.str();
          auto dest = "sim." + control + "." + ch.str();
          expected[prefix + "_ready"] = dest + ".ready";
          expected[dest + ".valid"] = prefix + "_valid";
          const StringRef aw[]{"addr", "len", "size", "burst", "lock", "cache", "prot", "qos", "region", "id", "user"};
          const StringRef w[]{"data", "last", "id", "strb", "user"};
          for (auto field : ch == "aw" ? ArrayRef<StringRef>(aw) : ArrayRef<StringRef>(w)) {
            bool dispatched = ch == "aw" ? field == "addr" || field == "len" || field == "id" : field == "data" || field == "last";
            expected[dest + ".bits." + field.str()] = dispatched ? prefix + "_bits_" + field.str() :
                "top.ctrl_write_dispatch_master_" + ch.str() + "_bits_" + field.str();
          }
        }
      }
      require(actual == expected && actual.size() == 180, "full-platform AW/W routing or shared metadata differs");
      auto reads = named(f.circuit, f.circuit.getName())->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings");
      require(reads && reads.size() == 9, "full Rocket/Print requests require nine early bank bindings");
      for (auto attr : reads) {
        auto row = cast<DictionaryAttr>(attr); auto name = row.getAs<StringAttr>("name").getValue();
        auto region = llvm::find_if(regions, [&](auto r) { return r.name == name; });
        require(region != regions.end() && row.getAs<IntegerAttr>("slave").getInt() == region - regions.begin(),
            "Rocket/Print request bank differs from its allocated slave");
      }
      if (!output.empty()) {
        std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-requests-reverse.mlir" : ".rocket-requests.mlir")).str(), ec);
        require(!ec, "cannot write Rocket/Print request boundary"); f.root->print(file); file << '\n';
      }
      rocketResponses(f, reads, output, reverse, rejected);
      rocketMaster(f, output, reverse, rejected);
      rocketTSI(f, output, reverse, rejected);
      rocketBlockDev(f, output, reverse, rejected);
      rocketFASEDIngress(f, output, reverse, rejected);
      rocketFASEDIssue(f, output, reverse, rejected);
      rocketFASEDReadBuffer(f, output, reverse, rejected);
      rocketFASEDReadScheduler(f, output, reverse, rejected);
      rocketFASEDWriteEgress(f, output, reverse, rejected);
      rocketFASEDResponseReleaser(f, output, reverse, rejected);
      rocketFASEDTimingCycle(f, output, reverse, rejected);
      rocketFASEDReadLatency(f, output, reverse, rejected);
    }
    if (!output.empty()) {
      std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-streams-reverse.mlir" : ".rocket-streams.mlir")).str(), ec);
      require(!ec, "cannot write Rocket/Print stream boundary"); f.root->print(file); file << '\n';
    }
  }
  llvm::outs() << "PASS actual Rocket queue plus two Print hosts: three live DMA/count bindings, both constructor orders and "
               << rejected << " atomic rejections\n";
}

void allocatedHeaders(MLIRContext &ctx, StringRef output) {
  for (bool reverse : {false, true}) {
    Fixture f(ctx); OpBuilder b(&ctx); std::string error, header;
    // The reversed case has a real preceding stream queue. This makes widget
    // numbers and CPU allocation indices differ, as on a TracerV platform.
    if (reverse) {
      auto original = named(f.circuit, "Top");
      auto queueName = f.hosts[0]->getAttrOfType<DictionaryAttr>("goldengate.printHost").getAs<StringAttr>("queueModule");
      auto queue = named(f.circuit, queueName.getValue());
      b.setInsertionPointToStart(original.getBodyBlock());
      auto q = b.create<InstanceOp>(f.circuit.getLoc(), queue, "precedingTraceQueue");
      for (unsigned p = 0; p < 2; ++p)
        b.create<StrictConnectOp>(f.circuit.getLoc(), q.getResult(p), original.getArgument(p));
      auto enq = q.getResult(2);
      for (auto [name, bits] : {std::make_pair("valid", 1U), std::make_pair("bits", 512U)})
        b.create<StrictConnectOp>(f.circuit.getLoc(), b.create<SubfieldOp>(f.circuit.getLoc(), enq, name),
            b.create<ConstantOp>(f.circuit.getLoc(), UIntType::get(&ctx, bits), APInt(bits, 0)));
      b.create<ConnectOp>(f.circuit.getLoc(), original.getArgument(port(original, "tracerv_stream")), q.getResult(3));
      b.create<StrictConnectOp>(f.circuit.getLoc(), original.getArgument(port(original, "tracerv_stream_count")), q.getResult(4));
    }
    if (reverse) std::reverse(f.hosts.begin(), f.hosts.end());
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
    SmallVector<goldengate::CPUStreamSourcePort> earlier;
    SmallVector<goldengate::CPUStreamCountPort> earlierCounts;
    if (reverse) {
      earlier.push_back({"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream", 6144});
      earlierCounts.push_back({"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream_count", 13});
    }
    require(succeeded(goldengate::mapPrintBridgeCPUStreams(f.circuit, earlier, earlierCounts, error)), error);
    require(succeeded(goldengate::mapPrintBridgeControlDispatch(f.circuit, error)), error);
    require(succeeded(goldengate::mapPrintBridgeControlResponses(f.circuit, error)), error);
    auto before = dump(*f.root); header = "preserve";
    require(failed(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, f.hosts, header, error)) &&
        header == "preserve" && dump(*f.root) == before, "header accepted incomplete master assembly");
    require(succeeded(goldengate::bindControlMaster(f.circuit, "GGControlWriteTrackerWrapper", error)), error);
    before = dump(*f.root);
    require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, f.hosts, header, error)) &&
        dump(*f.root) == before, error);
    // The default full-platform API must still reject this outgoing-only IR.
    OwningOpRef<CircuitOp> complete(cast<CircuitOp>(f.circuit->clone()));
    auto raw = complete->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> annotations(raw.begin(), raw.end());
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
        b.getNamedAttr("fileSuffix", b.getStringAttr(".const.h")), b.getNamedAttr("body", b.getStringAttr("preserve"))}));
    complete->getOperation()->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    auto completeBefore = dump(*complete);
    require(failed(goldengate::prepareCPUManagedStreamHeader(*complete, error)) && dump(*complete) == completeBefore,
        "complete CPU header accepted an absent incoming engine");
    for (unsigned slot = 0; slot < 2; ++slot) {
      unsigned index = slot + reverse;
      require(header.find("make_print_bridge_" + std::to_string(slot)) != std::string::npos &&
          header.find("}, " + std::to_string(slot) + "U, args, " + std::to_string(index) + "U).release()") != std::string::npos,
          "allocated constructor widget/stream index differs");
      for (unsigned word = 0; word < 6; ++word)
        require(header.find(std::to_string(32 * slot + 4 * word) + "ULL,") != std::string::npos,
            "allocated Print register address differs");
      require(header.find("PRINTBRIDGEMODULE_" + std::to_string(slot) +
          "_to_cpu_stream\"), " + std::to_string(index * 524288) + "ULL, " +
          std::to_string(64 + 4 * index) + "ULL, 6144U, 64U)") != std::string::npos,
          "CPU count address/DMA parameters differ from Print allocation");
    }
    if (!output.empty()) {
      std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".allocated-reverse.const.h" : ".allocated.const.h")).str(), ec);
      require(!ec, "cannot write allocated Print header fixture"); file << header;
    }
    auto bound = named(f.circuit, "GGPrintBridgeHostWrapper");
    auto configName = f.hosts[0]->getAttrOfType<DictionaryAttr>("goldengate.printHost").getAs<StringAttr>("configModule");
    auto config = named(f.circuit, configName.getValue());
    // Registry row order cannot change ABI member order or allocated addresses.
    auto registers = config->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> reordered(registers.begin(), registers.end());
    std::reverse(reordered.begin(), reordered.end());
    config->setAttr("goldengate.mmioRegisters", b.getArrayAttr(reordered));
    std::string permuted;
    require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, f.hosts, permuted, error)) && permuted == header,
        "register metadata row order changed allocated header");
    config->setAttr("goldengate.mmioRegisters", registers);
    // Scalar Print payload connections must resolve to the same storage
    // instance as occupancy, not just to a queue with the same definition.
    auto host = f.hosts[0]; ConnectOp payload;
    for (auto c : host.getOps<ConnectOp>())
      if (c.getDest() == host.getArgument(10)) payload = c;
    require(bool(payload), "missing scalar Print payload connection");
    Value savedPayload = payload.getSrc();
    auto queueName = host->getAttrOfType<DictionaryAttr>("goldengate.printHost").getAs<StringAttr>("queueModule");
    b.setInsertionPoint(payload);
    auto otherQueue = b.create<InstanceOp>(host.getLoc(), named(f.circuit, queueName.getValue()), "unrelatedQueue");
    auto otherBits = b.create<SubfieldOp>(host.getLoc(), otherQueue.getResult(3), "bits");
    auto constant = b.create<ConstantOp>(host.getLoc(), UIntType::get(&ctx, 512), APInt(512, 0));
    for (Value wrong : {Value(otherBits.getResult()), Value(constant.getResult())}) {
      payload->setOperand(1, wrong); auto corrupt = dump(*f.root); std::string rejected = "preserve";
      require(failed(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, f.hosts, rejected, error)) &&
          rejected == "preserve" && dump(*f.root) == corrupt, "allocated header accepted detached scalar payload");
    }
    payload->setOperand(1, savedPayload); constant.erase(); otherBits.erase(); otherQueue.erase();
    for (unsigned bad = 0; bad < 17; ++bad) {
      FModuleOp module; StringRef key; StringRef field; Attribute value;
      if (bad < 4) {
        module = named(f.circuit, "GGControlAddressDecode"); key = "goldengate.controlRegions";
        field = bad == 0 ? "start" : bad == 1 ? "size" : bad == 2 ? "slave" : "name";
        value = bad == 3 ? Attribute(b.getStringAttr("wrong_widget")) : Attribute(b.getI64IntegerAttr(bad == 1 ? 16 : 4));
      } else if (bad < 7) {
        module = config; key = "goldengate.mmioRegisters";
        field = bad == 4 ? "name" : bad == 5 ? "offset" : "writeable";
        value = bad == 4 ? Attribute(b.getStringAttr("wrong_register")) : bad == 5 ? Attribute(b.getI64IntegerAttr(4)) : Attribute(b.getBoolAttr(false));
      } else if (bad < 10) {
        module = bound; key = "goldengate.printHostBindings";
        field = bad == 7 ? "widgetName" : bad == 8 ? "controlPort" : "streamPort";
        value = b.getStringAttr("wrong_identity");
      } else if (bad < 15) {
        module = named(f.circuit, "GGCPUStreamRead"); key = "goldengate.sourceStreams";
        field = bad == 10 ? "name" : bad == 11 ? "port" : bad == 12 ? "index" : bad == 13 ? "depth" : "bufferBaseAddress";
        value = bad < 12 ? Attribute(b.getStringAttr("wrong_stream")) : Attribute(b.getI64IntegerAttr(1));
      } else {
        module = named(f.circuit, bad == 15 ? "GGControlReadDispatchWrapper" : "GGCPUStreamCountBank");
        key = bad == 15 ? "goldengate.controlReadBindings" : "goldengate.mmioRegisters";
        field = bad == 15 ? "port" : "name"; value = b.getStringAttr("wrong_count");
      }
      auto original = module->getAttrOfType<ArrayAttr>(key);
      SmallVector<Attribute> rows(original.begin(), original.end()); NamedAttrList row(cast<DictionaryAttr>(rows[0]));
      row.set(field, value); rows[0] = row.getDictionary(&ctx); module->setAttr(key, b.getArrayAttr(rows));
      auto corrupt = dump(*f.root); std::string rejected = "preserve";
      require(failed(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, f.hosts, rejected, error)) &&
          !error.empty() && rejected == "preserve" && dump(*f.root) == corrupt,
          "allocated header accepted corruption or changed IR/text: " + std::to_string(bad));
      module->setAttr(key, original);
    }
    b.setInsertionPointToEnd(f.circuit.getBodyBlock());
    auto unexpected = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr("GGEmptyCPUStreamWrite"),
        f.hosts[0].getConventionAttr(), ArrayRef<PortInfo>{});
    auto unexpectedBefore = dump(*f.root); std::string rejected = "preserve";
    require(failed(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, f.hosts, rejected, error)) &&
        rejected == "preserve" && dump(*f.root) == unexpectedBefore,
        "selected CPU header silently omitted an existing incoming engine");
    unexpected.erase();
  }
  llvm::outs() << "PASS allocated Print/count headers in both host orders, preceding live queue, register permutation and 44 atomic rejections\n";
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
    // Replay both expanded model-cycle boundaries to isolate read deadlines.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-read-latency-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED model-cycle boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDReadLatency(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED read latency boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay expanded response releaser boundaries to isolate model cycle/deadlines.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-timing-cycle-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED response releaser boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDTimingCycle(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED timing cycle boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay expanded write egress boundaries to isolate response retirement.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-response-releaser-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED write egress boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDResponseReleaser(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED response releaser boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay both expanded read scheduler orders to isolate write acknowledgments.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-write-egress-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED read scheduler boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDWriteEgress(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED write egress boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay both real expanded read buffer orders without rebuilding earlier banks.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-read-scheduler-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED read buffer boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDReadScheduler(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED read scheduler boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay both real expanded issue orders without rebuilding earlier banks.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-read-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED issue boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDReadBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED read buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Reuse a recorded expanded ingress boundary to isolate this staged batch.
    if (argc == 4 && StringRef(argv[1]) == "--fased-issue-boundary") {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED ingress boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0; rocketFASEDIssue(f, argv[3], false, rejected);
      llvm::outs() << "PASS expanded FASED issue boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    checkBinding(ctx, false, argc > 1 ? argv[1] : ""); checkBinding(ctx, true, ""); rejections(ctx); allocationRejections(ctx); controlRejections(ctx); responseRejections(ctx);
    allocatedHeaders(ctx, argc > 1 ? argv[1] : "");
    platformCatalog(ctx, argc > 2 ? argv[2] : "", argc > 1 ? argv[1] : "");
    rocketStreams(ctx, argc > 3 ? argv[3] : "", argc > 2 ? argv[2] : "", argc > 1 ? argv[1] : "");
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
