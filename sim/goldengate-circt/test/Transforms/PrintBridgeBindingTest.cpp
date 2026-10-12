// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/BridgeClockInfo.h"
#include "goldengate/ControlCatalog.h"
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/PrintBridgeHeader.h"
#include "goldengate/RocketDriverHeader.h"
#include "goldengate/LoadMemHeader.h"
#include "goldengate/TracerVHeader.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationEmission.h"
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
#include "llvm/ADT/BitVector.h"
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
// Linking a valid bank may still capture a different child definition. Test
// that closure identity, collisions and incompatible blackboxes fail atomically.
void controlCatalogImport(MLIRContext &ctx) {
  auto source = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "Source" {
    firrtl.module @Source() {}
    firrtl.module @Bank(in %clk: !firrtl.clock) {
      %child = firrtl.instance child @Storage(in clk: !firrtl.clock)
      firrtl.connect %child, %clk : !firrtl.clock, !firrtl.clock
      %gate = firrtl.instance gate @Gate(in clk: !firrtl.clock)
      firrtl.connect %gate, %clk : !firrtl.clock, !firrtl.clock
    }
    firrtl.extmodule @Gate(in clk: !firrtl.clock) attributes {defname = "Gate"}
    firrtl.module @Storage(in %clk: !firrtl.clock) {}
    firrtl.module @Unrelated() {}
  } })", &ctx);
  auto destination = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "Destination" {
    firrtl.module @Destination() {}
    firrtl.extmodule @LocalGate(in clk: !firrtl.clock) attributes {defname = "Gate"}
  } })", &ctx);
  require(bool(source) && bool(destination), "invalid control catalog fixture");
  auto src = *source->getOps<CircuitOp>().begin(), dst = *destination->getOps<CircuitOp>().begin();
  auto original = dump(*source); std::string error;
  auto reject = [&](ArrayRef<StringRef> roots) {
    auto before = dump(*destination);
    require(failed(goldengate::importControlCatalog(dst, src, roots, error)) &&
        !error.empty() && dump(*destination) == before && dump(*source) == original,
        "failed catalog import changed either circuit");
  };
  reject({"Bank", "Missing"}); reject({"Bank", "Bank"}); reject({"Source"});
  auto external = *dst.getOps<FExtModuleOp>().begin();
  external->setAttr("test.differentParameters", UnitAttr::get(&ctx)); reject({"Bank"});
  external->removeAttr("test.differentParameters");
  auto collision = named(src, "Storage")->clone();
  SymbolTable::setSymbolName(collision, "CatalogRocket_Storage");
  collision->setAttr("test.differentStorage", UnitAttr::get(&ctx));
  dst.getBodyBlock()->push_back(collision); reject({"Bank"}); collision->erase();
  auto bank = named(src, "Bank")->clone(), storage = named(src, "Storage")->clone();
  dst.getBodyBlock()->push_back(bank); dst.getBodyBlock()->push_back(storage);
  external->remove();
  auto sourceGate = (*src.getOps<FExtModuleOp>().begin())->clone();
  dst.getBodyBlock()->push_back(sourceGate);
  storage->setAttr("test.differentStorage", UnitAttr::get(&ctx)); reject({"Bank"});
  storage->removeAttr("test.differentStorage");
  auto before = dump(*destination);
  require(succeeded(goldengate::importControlCatalog(dst, src, {"Bank"}, error)) &&
      dump(*destination) == before, "identical shared closure was duplicated");
  bank->erase(); storage->erase(); sourceGate->erase(); dst.getBodyBlock()->push_back(external);
  require(succeeded(goldengate::importControlCatalog(dst, src, {"Bank"}, error)) &&
      succeeded(verify(*destination)) && dump(*source) == original,
      "valid control catalog failed import: " + error);
  require(std::distance(dst.getOps<FModuleLike>().begin(), dst.getOps<FModuleLike>().end()) == 4 &&
      child(named(dst, "Bank"), named(dst, "CatalogRocket_Storage")),
      "catalog imported unrelated modules or lost child identity");
  reject({"Bank"});
  llvm::outs() << "PASS control catalog: isolated closure, shared storage and physical blackbox identity, seven atomic rejections\n";
}
// Small SSA fixture tests the import independently of the large Rocket replay.
void bridgeClockImport(MLIRContext &ctx) {
  auto make = [&](bool source) {
    auto root = parseSourceString<ModuleOp>(source ? R"(module { firrtl.circuit "Source" {
      firrtl.module @Source() {}
      firrtl.module @Engine(in %hPort: !firrtl.uint<1>) {}
    } })" : R"(module { firrtl.circuit "Destination" {
      firrtl.module @Destination() {}
      firrtl.module @Engine(in %hPort: !firrtl.uint<1>) {}
    } })", &ctx);
    require(bool(root), "bridge clock import fixture parse");
    auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    auto clock = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("target")),
        b.getNamedAttr("multiplier", b.getI64IntegerAttr(1)), b.getNamedAttr("divisor", b.getI64IntegerAttr(2))});
    auto key = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Constructor"))});
    auto engine = named(c,"Engine"); engine->setAttr("goldengate.bridgeConstructor", key);
    b.setInsertionPointToStart(named(c,c.getName()).getBodyBlock());
    b.create<InstanceOp>(c.getLoc(),engine,"bridge");
    auto map = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelClockInfo)),
        b.getNamedAttr("infoMap", b.getDictionaryAttr({b.getNamedAttr(source ? "trace" : "print", clock)}))});
    SmallVector<Attribute> raw{map};
    if (source) raw.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
        b.getNamedAttr("widgetClass", b.getStringAttr("test.Bridge")),
        b.getNamedAttr("target", b.getStringAttr("~Source|Engine>hPort")),
        b.getNamedAttr("widgetConstructorKey",key), b.getNamedAttr("clockInfo",clock),
        b.getNamedAttr("channelMapping",b.getDictionaryAttr({b.getNamedAttr("payload",b.getStringAttr("trace"))}))}));
    raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Opaque")),
        b.getNamedAttr("target",b.getStringAttr("~Source|Engine>historical"))}));
    c->setAttr("rawAnnotations",b.getArrayAttr(raw)); return root;
  };
  for (unsigned bad = 0; bad < 12; ++bad) {
    auto src = make(true), dst = make(false); OpBuilder b(&ctx);
    auto sc = *src->getOps<CircuitOp>().begin(), dc = *dst->getOps<CircuitOp>().begin();
    auto rows = llvm::to_vector(sc->getAttrOfType<ArrayAttr>("rawAnnotations"));
    NamedAttrList bridge(cast<DictionaryAttr>(rows[1]));
    if (bad == 0) rows.erase(rows.begin());
    if (bad == 1) {
      NamedAttrList clock(cast<DictionaryAttr>(bridge.get("clockInfo"))); clock.set("divisor", b.getI64IntegerAttr(0));
      bridge.set("clockInfo",clock.getDictionary(&ctx)); rows[1] = bridge.getDictionary(&ctx);
    }
    if (bad == 2) rows.push_back(rows[0]);
    if (bad == 3) rows.push_back(rows[1]);
    if (bad == 4) { bridge.set("target",b.getStringAttr("~Source|Engine>missing")); rows[1] = bridge.getDictionary(&ctx); }
    if (bad == 5) named(dc,"Engine")->removeAttr("goldengate.bridgeConstructor");
    if (bad == 6) (*named(dc,"Destination").getOps<InstanceOp>().begin()).erase();
    if (bad == 7) { b.setInsertionPointToStart(named(dc,"Destination").getBodyBlock()); b.create<InstanceOp>(dc.getLoc(),named(dc,"Engine"),"second"); }
    if (bad == 8) {
      auto raw = llvm::to_vector(dc->getAttrOfType<ArrayAttr>("rawAnnotations"));
      NamedAttrList map(cast<DictionaryAttr>(raw[0]));
      map.set("infoMap",b.getDictionaryAttr({b.getNamedAttr("trace",b.getDictionaryAttr({}))}));
      raw[0] = map.getDictionary(&ctx); dc->setAttr("rawAnnotations",b.getArrayAttr(raw));
    }
    if (bad == 9) {
      bridge.set("channelMapping",b.getDictionaryAttr({b.getNamedAttr("first",b.getStringAttr("trace")),
          b.getNamedAttr("second",b.getStringAttr("trace"))})); rows[1] = bridge.getDictionary(&ctx);
    }
    if (bad == 10) named(sc,"Engine")->removeAttr("goldengate.bridgeConstructor");
    if (bad == 11) named(dc,"Engine").getArgument(0).setType(UIntType::get(&ctx,2));
    sc->setAttr("rawAnnotations",b.getArrayAttr(rows));
    auto sourceBefore = dump(*src), destinationBefore = dump(*dst); std::string error;
    require(failed(goldengate::importBridgeClockInfo(dc,sc,"test.Bridge",error)) && !error.empty() &&
        dump(*src) == sourceBefore && dump(*dst) == destinationBefore,
        "bridge clock import atomic rejection " + std::to_string(bad));
  }
  auto src = make(true), dst = make(false);
  auto sc = *src->getOps<CircuitOp>().begin(), dc = *dst->getOps<CircuitOp>().begin();
  auto old = dc->getAttrOfType<ArrayAttr>("rawAnnotations"); auto sourceBefore = dump(*src); std::string error;
  require(succeeded(goldengate::importBridgeClockInfo(dc,sc,"test.Bridge",error)),error);
  auto raw = dc->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto map = cast<DictionaryAttr>(raw[0]).getAs<DictionaryAttr>("infoMap");
  auto bridge = cast<DictionaryAttr>(raw[raw.size() - 1]);
  require(raw.size() == old.size()+1 && raw[1] == old[1] && map.size() == 2 &&
      map.get("print") == map.get("trace") && bridge.getAs<StringAttr>("target") == "~Destination|Engine>hPort" &&
      dump(*src) == sourceBefore && goldengate::resolveBridgeClockInfo(dc,bridge,error).has_value(),
      "bridge clock import lost destination analysis, opaque archive or source identity");
  auto before = dump(*dst);
  require(failed(goldengate::importBridgeClockInfo(dc,sc,"test.Bridge",error)) && dump(*dst) == before,
      "duplicate bridge import changed IR");
  llvm::outs() << "PASS bridge clock import: twelve atomic rejections, selected channel merge and retained foreign archive\n";
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
    text += ", out %preceding_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>, out %preceding_stream_count: !firrtl.uint<13>, out %other: !firrtl.uint<8>) {} } }";
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
  for (auto name : {"hostClock", "hostReset", "preceding_stream", "preceding_stream_count", "other"})
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
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "preceding_stream", 6144}},
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "preceding_stream_count", 13}}, error)) && succeeded(verify(*f.root)), error);
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

void rocketFASEDWriteLatency(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
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
          bad == 0 ? "GGFASEDWriteLatency10" : "GGFASEDWriteLatencyWrapper"),
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
    auto result = goldengate::mapPrintBridgeRocketFASEDWriteLatency(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED write latency rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDWriteLatency(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDWriteLatencyWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED write latency changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED write latency replaced an existing module operation");
  for (auto p : original.getPorts()) {
    if (p.name.getValue() == "fased_next_write" ||
        p.name.getValue() == "fased_write_release_cycle") continue;
    auto index = port(top, p.name.getValue());
    require(top.getPorts()[index].type == p.type &&
        top.getPorts()[index].direction == p.direction,
        "FASED write latency changed a retained response boundary");
  }
  require(top.getNumPorts() == original.getNumPorts(),
      "FASED write latency must exchange deadline/response ports for completion/capacity ports");
  auto completion = port(top, "fased_write_completion");
  auto capacity = port(top, "fased_write_latency_ready");
  auto bit = UIntType::get(ctx, 1, false);
  auto metadata = BundleType::get(ctx, {{b.getStringAttr("id"), false, UIntType::get(ctx, 4, false)}});
  auto completed = BundleType::get(ctx, {{b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, metadata}});
  require(top.getPorts()[completion].direction == Direction::In &&
      top.getPorts()[completion].type == completed &&
      top.getPorts()[capacity].direction == Direction::Out && top.getPorts()[capacity].type == bit,
      "FASED write latency completion/capacity boundary differs");
  require(bool(child(top, named(f.circuit, "GGFASEDWriteLatency10"))),
      "FASED write latency helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDWriteLatency(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED write latency composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-write-latency-reverse.mlir" : ".rocket-fased-write-latency.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED write latency boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDTimingAWQueue(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
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
          bad == 0 ? "GGFASEDTimingAWQueue10" : "GGFASEDTimingAWQueueWrapper"),
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
    auto result = goldengate::mapPrintBridgeRocketFASEDTimingAWQueue(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED timing AW queue rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDTimingAWQueue(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDTimingAWQueueWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED timing AW queue changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED timing AW queue replaced an existing module operation");
  for (auto p : original.getPorts()) {
    if (p.name.getValue() == "fased_write_completion") continue;
    auto index = port(top, p.name.getValue());
    require(top.getPorts()[index].type == p.type &&
        top.getPorts()[index].direction == p.direction,
        "FASED timing AW queue changed a retained response boundary");
  }
  require(top.getNumPorts() == original.getNumPorts() + 1,
      "FASED timing AW queue must exchange completion metadata for pairing/capacity ports");
  auto paired = port(top, "fased_write_pair_complete");
  auto capacity = port(top, "fased_timing_aw_queue_ready");
  auto bit = UIntType::get(ctx, 1, false);
  require(top.getPorts()[paired].direction == Direction::In &&
      top.getPorts()[paired].type == bit &&
      top.getPorts()[capacity].direction == Direction::Out && top.getPorts()[capacity].type == bit,
      "FASED timing AW queue pairing/capacity boundary differs");
  require(bool(child(top, named(f.circuit, "GGFASEDTimingAWQueue10"))),
      "FASED timing AW queue helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDTimingAWQueue(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED timing AW queue composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-timing-aw-queue-reverse.mlir" : ".rocket-fased-timing-aw-queue.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED timing AW queue boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDWritePairing(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
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
          bad == 0 ? "GGFASEDWritePairing" : "GGFASEDWritePairingWrapper"),
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
    auto result = goldengate::mapPrintBridgeRocketFASEDWritePairing(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED write pairing rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDWritePairing(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDWritePairingWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED write pairing changed bank or expanded allocation");
  for (auto m : existing) require(named(f.circuit, m.getName()) == m,
      "FASED write pairing replaced an existing module operation");
  for (auto p : original.getPorts()) {
    if (p.name.getValue() == "fased_write_pair_complete") continue;
    auto index = port(top, p.name.getValue());
    require(top.getPorts()[index].type == p.type &&
        top.getPorts()[index].direction == p.direction,
        "FASED write pairing changed a retained response boundary");
  }
  require(top.getNumPorts() == original.getNumPorts() + 2,
      "FASED write pairing must exchange the pairing pulse for retirement/maximum/count ports");
  auto retirement = port(top, "fased_target_b_fire");
  auto maximum = port(top, "fased_write_max_reqs");
  auto counts = port(top, "fased_pending_writes");
  auto bit = UIntType::get(ctx, 1, false);
  auto word = UIntType::get(ctx, 4, false);
  auto pending = BundleType::get(ctx, {{b.getStringAttr("awValue"), false, word},
      {b.getStringAttr("wValue"), false, word}, {b.getStringAttr("awFull"), false, bit},
      {b.getStringAttr("wFull"), false, bit}});
  require(top.getPorts()[retirement].direction == Direction::In &&
      top.getPorts()[retirement].type == bit &&
      top.getPorts()[maximum].direction == Direction::In && top.getPorts()[maximum].type == word &&
      top.getPorts()[counts].direction == Direction::Out && top.getPorts()[counts].type == pending,
      "FASED write pairing retirement/maximum/count boundary differs");
  require(bool(child(top, named(f.circuit, "GGFASEDWritePairing"))),
      "FASED write pairing helper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDWritePairing(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED write pairing composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-write-pairing-reverse.mlir" : ".rocket-fased-write-pairing.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED write pairing boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDWriteRetirement(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  const StringRef observed[]{"GGFASEDWritePairingWrapper", "GGFASEDTimingAWQueueWrapper",
      "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
      "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDWriteRetirementWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(),
          named(f.circuit, "GGFASEDTimingAWQueueWrapper"), "duplicate");
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
    auto result = goldengate::mapPrintBridgeRocketFASEDWriteRetirement(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED write retirement rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDWriteRetirement(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDWriteRetirementWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED write retirement changed bank or expanded allocation");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m,
        "FASED write retirement replaced an existing module operation");
    bool appended = llvm::is_contained(observed, m.getName());
    require(m.getNumPorts() == priorPorts[j].size() + unsigned(appended),
        "FASED write retirement changed an unrelated module's ports");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction,
          "FASED write retirement changed an existing port index/type/direction");
    if (appended) require(m.getPortName(m.getNumPorts()-1) == "fased_accepted_b_fire" &&
        m.getPortType(m.getNumPorts()-1) == UIntType::get(ctx, 1, false) &&
        m.getPortDirection(m.getNumPorts()-1) == Direction::Out,
        "FASED write retirement observation differs");
  }
  auto oldPorts = priorPorts[llvm::find(existing, original) - existing.begin()];
  for (auto p : oldPorts) {
    if (p.name.getValue() == "fased_target_b_fire") continue;
    auto index = port(top, p.name.getValue());
    require(top.getPorts()[index].type == p.type && top.getPorts()[index].direction == p.direction,
        "FASED write retirement changed a retained response boundary");
  }
  require(top.getNumPorts() == oldPorts.size() - 1,
      "FASED write retirement must consume the external B retirement input");
  auto sim = child(top, original);
  require(bool(sim), "FASED write retirement wrapper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDWriteRetirement(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED write retirement composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-write-retirement-reverse.mlir" : ".rocket-fased-write-retirement.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED write retirement boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDRequestLimits(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDRequestLimits");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDRequestLimitsWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1 || bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), bad == 1 ? original : bank, "duplicate");
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDRequestLimits(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED request-limit rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDRequestLimits(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDRequestLimitsWrapper", "FASED request-limit top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size(),
        "FASED request-limit composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction, "FASED request-limit composition changed an existing port");
  }
  unsigned copied = 0;
  for (auto p : original.getPorts()) {
    if (p.name == "fased_write_max_reqs" || p.name == "fased_read_max_reqs") continue;
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED request-limit composition changed a retained port"); ++copied;
  }
  require(top.getNumPorts() == copied + 1 && top.getPortName(copied) == "fased_request_limits_mcr" &&
      top.getPortType(copied) == bank.getPortType(3) && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, bank)) && bool(child(top, original)), "FASED request-limit bank attachment differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDRequestLimits(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED request-limit composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-request-limits-reverse.mlir" : ".rocket-fased-request-limits.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED request-limit boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDLatencyRegisters(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDLatencyRegistersWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1 || bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), bad == 1 ? original : bank, "duplicate");
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDLatencyRegisters(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED latency-register rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDLatencyRegisters(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDLatencyRegistersWrapper", "FASED latency-register top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size(),
        "FASED latency-register composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction, "FASED latency-register composition changed an existing port");
  }
  unsigned copied = 0;
  for (auto p : original.getPorts()) {
    if (p.name == "fased_write_latency" || p.name == "fased_read_latency") continue;
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED latency-register composition changed a retained port"); ++copied;
  }
  require(top.getNumPorts() == copied + 1 && top.getPortName(copied) == "fased_latency_mcr" &&
      top.getPortType(copied) == bank.getPortType(3) && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, bank)) && bool(child(top, original)), "FASED latency-register bank attachment differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDLatencyRegisters(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED latency-register composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-latency-registers-reverse.mlir" : ".rocket-fased-latency-registers.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED latency-register boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDFunctionalModelRegister(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDFunctionalModelRegister");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDFunctionalModelRegisterWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1 || bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), bad == 1 ? original : bank, "duplicate");
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDFunctionalModelRegister(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED functional-model register rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDFunctionalModelRegister(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDFunctionalModelRegisterWrapper", "FASED functional-model register top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size(),
        "FASED functional-model register composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction, "FASED functional-model register composition changed an existing port");
  }
  unsigned copied = 0;
  for (auto p : original.getPorts()) {
    if (p.name == "fased_ingress_relaxed") continue;
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED functional-model register composition changed a retained port"); ++copied;
  }
  require(top.getNumPorts() == copied + 1 && top.getPortName(copied) == "fased_functional_model_mcr" &&
      top.getPortType(copied) == bank.getPortType(3) && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, bank)) && bool(child(top, original)), "FASED functional-model register bank attachment differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDFunctionalModelRegister(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED functional-model register composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-functional-model-register-reverse.mlir" : ".rocket-fased-functional-model-register.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED functional-model register boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDResponseErrors(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDResponseErrors");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDResponseErrorsWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1 || bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), bad == 1 ? original : bank, "duplicate");
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDResponseErrors(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED response-error rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDResponseErrors(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDResponseErrorsWrapper", "FASED response-error top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size(),
        "FASED response-error composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction, "FASED response-error composition changed an existing port");
  }
  unsigned copied = 0;
  for (auto p : original.getPorts()) {
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED response-error composition changed a retained port"); ++copied;
  }
  require(top.getNumPorts() == copied + 1 && top.getPortName(copied) == "fased_response_errors_mcr" &&
      top.getPortType(copied) == bank.getPortType(8) && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, bank)) && bool(child(top, original)), "FASED response-error bank attachment differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDResponseErrors(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED response-error composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-response-errors-reverse.mlir" : ".rocket-fased-response-errors.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED response-error boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDControl(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto rows = original->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto raw = f.circuit->getAttr("rawAnnotations");
  auto names = llvm::to_vector(original.getPortNames());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 14; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad < 3) {
      const StringRef collisions[]{"GGFASEDMCRFile", "GGFASEDBridgeControlWrapper", "GGFASEDBridgeBoundWrapper"};
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(collisions[bad]),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 3) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), original, "duplicate");
    }
    if (bad == 4 || bad == 5) {
      SmallVector<Attribute> changed(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1]));
      if (bad == 4) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      changed[1] = row.getDictionary(ctx);
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    if (bad == 8 || bad == 9) {
      auto changed = names;
      changed[port(original, bad == 8 ? "fasedBridge_mcr" : "ctrl_read_dispatch_slave_1_ar")] = b.getStringAttr("missingBoundary");
      original.setPortNames(changed);
    }
    if (bad == 10) original->removeAttr("goldengate.mmioRegisters");
    if (bad == 11) f.circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test.DecodedTarget")),
        b.getNamedAttr("target", b.getStringAttr("~GGFASEDMMIOWrapper|GGFASEDMMIOWrapper>fasedBridge_mcr.read[20].bits"))})}));
    if (bad == 12 || bad == 13) {
      SmallVector<Attribute> changed(rows.begin(), rows.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[0]));
      if (bad == 12) row.set("name", b.getStringAttr("staleName"));
      else row.set("writeable", b.getBoolAttr(false));
      changed[0] = row.getDictionary(ctx);
      original->setAttr("goldengate.mmioRegisters", b.getArrayAttr(changed));
    }
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketFASEDControl(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == before,
        "expanded FASED control rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
    original.setPortNames(names); original->setAttr("goldengate.mmioRegisters", rows);
    decoder->setAttr("goldengate.controlRegions", regions); f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); if (extra) extra.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDControl(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, "GGFASEDBridgeBoundWrapper");
  require(f.circuit.getName() == top.getName() &&
      top->getAttrOfType<IntegerAttr>("goldengate.fasedSlave").getInt() == 1,
      "expanded FASED catalog-selected slave differs");
  auto control = named(f.circuit, "GGFASEDBridgeControlWrapper");
  auto adapter = named(f.circuit, "GGFASEDMCRFile");
  require(bool(child(top, control)) && bool(child(control, original)) && bool(child(control, adapter)),
      "expanded FASED transport hierarchy differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "expanded FASED control changed an existing module identity");
    for (auto [i, arg] : llvm::enumerate(arguments[j]))
      require(m.getArguments()[i] == arg, "expanded FASED control changed a port argument identity");
  }
  for (auto p : top.getPorts())
    require(p.name != "fasedBridge_ctrl" && p.name != "fasedBridge_mcr" &&
        !p.name.getValue().starts_with("ctrl_write_dispatch_slave_1_") &&
        !p.name.getValue().starts_with("ctrl_read_arb_in_1_") &&
        !p.name.getValue().starts_with("ctrl_write_arb_in_1_") &&
        p.name != "ctrl_read_dispatch_slave_1_ar", "expanded FASED control retained a consumed port");
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDControl(f.circuit, error)) &&
      dump(*f.root) == before, "repeated expanded FASED control changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-fased-control-reverse.mlir" : ".rocket-fased-control.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED control boundary"); f.root->print(file); file << '\n';
  }
}

void rocketControlMaster(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto names = llvm::to_vector(original.getPortNames());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 13; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr("GGControlMasterWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), original, "duplicate");
    }
    if (bad == 2) {
      SmallVector<Attribute> changed(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1])); row.set("start", b.getI64IntegerAttr(256));
      changed[1] = row.getDictionary(ctx);
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    }
    if (bad == 3) f.circuit.setName("WrongTop");
    if (bad == 4) f.circuit->removeAttr("rawAnnotations");
    if (bad == 5 || bad == 6) {
      auto changed = names;
      changed[port(original, bad == 5 ? "ctrl_decode_aw_addr" : "ctrl_read_dispatch_master_ar")] =
          b.getStringAttr("missingBoundary"); original.setPortNames(changed);
    }
    if (bad >= 7) {
      const StringRef exposed[]{"fasedBridge_ctrl", "ctrl_write_dispatch_slave_1_aw_valid",
          "ctrl_read_dispatch_slave_1_ar", "ctrl_write_arb_in_1_valid",
          "ctrl_read_arb_in_1_valid", "print_0_ctrl"};
      auto changed = names; changed[port(original, "other")] = b.getStringAttr(exposed[bad - 7]);
      original.setPortNames(changed);
    }
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketControlMaster(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == before,
        "expanded control master rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
    original.setPortNames(names); decoder->setAttr("goldengate.controlRegions", regions);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); if (extra) extra.erase(); f.circuit.setName(original.getName());
  }
  // Exercise both copied and consumed indexed/nested targets at this boundary.
  SmallVector<Attribute> annotated(raw.begin(), raw.end());
  auto annotation = [&](StringRef name) { return b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.MasterBoundary")),
      b.getNamedAttr("target", b.getStringAttr(name))}); };
  annotated.push_back(annotation("~GGFASEDBridgeBoundWrapper|GGFASEDBridgeBoundWrapper>other"));
  annotated.push_back(annotation("~GGFASEDBridgeBoundWrapper|GGFASEDBridgeBoundWrapper>ctrl_read_dispatch_master_ar.bits.addr"));
  annotated.push_back(annotation("~GGFASEDBridgeBoundWrapper|GGFASEDBridgeBoundWrapper>ctrl_write_dispatch_master_w_bits_data"));
  annotated.push_back(annotation("~GGFASEDBridgeBoundWrapper"));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annotated));
  require(succeeded(goldengate::mapPrintBridgeRocketControlMaster(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, "GGControlMasterWrapper");
  require(f.circuit.getName() == top.getName() && bool(child(top, original)),
      "expanded control master hierarchy differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "expanded control master changed an existing module identity");
    for (auto [i, arg] : llvm::enumerate(arguments[j]))
      require(m.getArguments()[i] == arg, "expanded control master changed a port argument identity");
  }
  require(original.getNumPorts() == top.getNumPorts() + 32 &&
      top.getPortName(top.getNumPorts() - 1) == "ctrl" &&
      top.getPortDirection(top.getNumPorts() - 1) == Direction::In &&
      cast<BundleType>(top.getPortType(top.getNumPorts() - 1)).getNumElements() == 5,
      "expanded control master scalar/aggregate consumption differs");
  auto transferred = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  const StringRef expected[]{"~GGControlMasterWrapper|GGControlMasterWrapper>other",
      "~GGControlMasterWrapper|GGFASEDBridgeBoundWrapper>ctrl_read_dispatch_master_ar.bits.addr",
      "~GGControlMasterWrapper|GGFASEDBridgeBoundWrapper>ctrl_write_dispatch_master_w_bits_data",
      "~GGControlMasterWrapper"};
  for (auto [i, target] : llvm::enumerate(expected))
    require(cast<DictionaryAttr>(transferred[raw.size() + i]).getAs<StringAttr>("target").getValue() == target,
        "expanded control master target transfer differs");
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketControlMaster(f.circuit, error)) &&
      dump(*f.root) == before, "repeated expanded control master changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-control-master-reverse.mlir" : ".rocket-control-master.mlir")).str(), ec);
    require(!ec, "cannot write expanded control master boundary"); f.root->print(file); file << '\n';
  }
}


void rocketDriverHeaders(Fixture &f, StringRef directory, StringRef nativeBoundary) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto collect = [&](CircuitOp c) {
    SmallVector<FModuleOp> hosts;
    for (auto row : named(c, "GGPrintBridgeHostWrapper")->getAttrOfType<ArrayAttr>("goldengate.printHostBindings"))
      hosts.push_back(named(c, cast<DictionaryAttr>(row).getAs<StringAttr>("hostModule").getValue()));
    return hosts;
  };
  // Import only validated retained analysis for the live bridge. This replaces
  // unchecked concatenation of the entire native clock map in the old replay.
  auto native = parseSourceFile<ModuleOp>(nativeBoundary, ctx);
  require(bool(native), "cannot parse native Rocket header oracle boundary");
  auto nc = *native->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::prepareMetasimInterfaceHeader(f.circuit, "FireSim", error)), error);
  auto missing = dump(*f.root);
  require(failed(goldengate::prepareTracerVHeader(f.circuit, error)) && dump(*f.root) == missing,
      "TracerV fabricated missing clock analysis");
  auto foreign = dump(*native);
  require(succeeded(goldengate::importBridgeClockInfo(f.circuit, nc,
      "firechip.goldengateimplementations.TracerVBridgeModule", error)), error);
  require(dump(*native) == foreign, "clock import changed source circuit");
  auto imported = dump(*f.root);
  require(failed(goldengate::importBridgeClockInfo(f.circuit, nc,
      "firechip.goldengateimplementations.TracerVBridgeModule", error)) && dump(*f.root) == imported,
      "repeated clock import changed destination");
  auto original = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto baseline = dump(*f.root);
  for (unsigned bad = 0; bad < 6; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin();
    auto wrapper = named(c,"GGPrintBridgeHostWrapper");
    if (bad == 0) wrapper->removeAttr("goldengate.printHostBindings");
    if (bad == 1) {
      auto rows = llvm::to_vector(wrapper->getAttrOfType<ArrayAttr>("goldengate.printHostBindings"));
      NamedAttrList first(cast<DictionaryAttr>(rows[0])); first.set("hostModule",b.getStringAttr("MissingPrintHost"));
      rows[0] = first.getDictionary(ctx); wrapper->setAttr("goldengate.printHostBindings",b.getArrayAttr(rows));
    }
    if (bad == 2) {
      auto transport = named(c,"GGCPUStreamRead");
      auto rows = llvm::to_vector(transport->getAttrOfType<ArrayAttr>("goldengate.sourceStreams"));
      NamedAttrList first(cast<DictionaryAttr>(rows[1])); first.set("port",b.getStringAttr("wrong_print_stream"));
      rows[1] = first.getDictionary(ctx); transport->setAttr("goldengate.sourceStreams",b.getArrayAttr(rows));
    }
    if (bad == 3) {
      auto rows = llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations"));
      for (auto &row : rows) {
        auto d = cast<DictionaryAttr>(row);
        if (d.getAs<StringAttr>("class") == goldengate::AnnotationClasses::BridgeIO &&
            d.getAs<StringAttr>("widgetClass") == goldengate::AnnotationClasses::PrintBridgeModule) {
          NamedAttrList changed(d); changed.erase("clockInfo"); row = changed.getDictionary(ctx); break;
        }
      }
      c->setAttr("rawAnnotations",b.getArrayAttr(rows));
    }
    if (bad == 4) {
      auto host = collect(c)[0]; auto info = host->getAttrOfType<DictionaryAttr>("goldengate.printHost");
      auto config = named(c,info.getAs<StringAttr>("configModule").getValue());
      auto rows = llvm::to_vector(config->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
      NamedAttrList first(cast<DictionaryAttr>(rows[0])); first.set("offset",b.getI32IntegerAttr(4));
      rows[0] = first.getDictionary(ctx); config->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
    }
    if (bad == 5) named(c,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
    auto saved = dump(*test); error.clear();
    require(failed(goldengate::prepareRocketDriverHeaders(c,error)) && !error.empty() && dump(*test) == saved,
        "expanded driver transaction rejection " + std::to_string(bad));
  }
  for (unsigned bad = 0; bad < 4; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto transport = named(c, "GGCPUStreamRead");
    auto sources = transport->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
    if (bad == 0 || bad == 3) {
      SmallVector<Attribute> rows(sources.begin(), sources.end()); NamedAttrList first(cast<DictionaryAttr>(rows[0]));
      if (bad == 0) first.set("name", b.getStringAttr("PRINTBRIDGEMODULE_0_to_cpu_stream"));
      else first.erase("port");
      rows[0] = first.getDictionary(ctx); transport->setAttr("goldengate.sourceStreams", b.getArrayAttr(rows));
    } else {
      b.setInsertionPointToStart(transport.getBodyBlock());
      for (auto eq : transport.getOps<EQPrimOp>()) {
        auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
        auto selector = eq.getRhs().getDefiningOp<ConstantOp>();
        if (bits && bits.getInput() == transport.getArgument(port(transport, "ar_bits_addr")) &&
            selector && selector.getValue().isOne()) {
          auto invalid = b.create<ConstantOp>(c.getLoc(), cast<UIntType>(selector.getType()), APInt(45, bad == 1 ? 0 : 3));
          eq->setOperand(1, invalid); break;
        }
      }
    }
    auto saved = dump(*test);
    require(failed(goldengate::prepareTracerVHeader(c, error)) && !error.empty() && dump(*test) == saved,
        "expanded TracerV identity/selector rejection " + std::to_string(bad));
  }
  for (unsigned bad = 0; bad < 11; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto engine = named(c,"GGPrintLoadMemEngine");
    InstanceOp pack, writer, requests;
    for (auto i : engine.getOps<InstanceOp>()) {
      if (i.getName() == "write_data") pack = i;
      if (i.getName() == "writer") writer = i;
      if (i.getName() == "read_requests") requests = i;
    }
    require(pack && writer && requests, "direct LoadMem fixture helpers absent");
    if (bad == 0) pack.setNameAttr(b.getStringAttr("missing_write_data"));
    if (bad == 1) { b.setInsertionPointToStart(engine.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),named(c,pack.getModuleName()),"duplicate_pack"); }
    if (bad == 2) named(c,pack.getModuleName())->setAttr("goldengate.inputWidth",b.getI32IntegerAttr(64));
    if (bad == 3 || bad == 5 || bad == 6 || bad == 7)
      for (auto conn : engine.getOps<StrictConnectOp>()) {
        Value destination = bad == 3 ? pack.getResult(3) : bad == 5 ? pack.getResult(0) :
                            bad == 6 ? requests.getResult(12) : writer.getResult(10);
        if (conn.getDest() == destination) conn->setOperand(1,engine.getArgument(bad == 5 ? 1 : 17));
      }
    if (bad == 4) for (auto index : engine.getOps<SubindexOp>()) {
      auto field = index.getInput().getDefiningOp<SubfieldOp>();
      if (field && field.getInput() == engine.getArgument(2) && field.getFieldName() == "write" && index.getIndex() == 0) {
        index->setAttr("index",b.getI32IntegerAttr(1)); break;
      }
    }
    if (bad == 8) {
      auto helper = named(c,pack.getModuleName());
      auto types = llvm::to_vector(helper.getPortTypes()); types.pop_back(); helper.setPortTypes(types);
    }
    if (bad == 9 || bad == 10) {
      auto types = llvm::to_vector(engine.getPortTypes());
      auto mcr = cast<BundleType>(engine.getPortType(2)); auto elements = llvm::to_vector(mcr.getElements());
      elements[0].type = FVectorType::get(cast<FVectorType>(elements[0].type).getElementType(),8);
      types[2] = TypeAttr::get(BundleType::get(ctx,elements)); engine.setPortTypes(types);
      if (bad == 10) engine.getArgument(2).setType(BundleType::get(ctx,elements));
    }
    auto saved = dump(*test);
    require(failed(goldengate::prepareLoadMemHeader(c,error)) && !error.empty() && dump(*test) == saved,
        "direct LoadMem rejection " + std::to_string(bad));
  }
  require(succeeded(goldengate::prepareRocketDriverHeaders(f.circuit, error)), error);
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  f.circuit->setAttr("rawAnnotations", original);
  require(dump(*f.root) == baseline, "expanded driver headers changed hardware");
  f.circuit->setAttr("rawAnnotations", updated);
  for (auto row : original) {
    auto d = cast<DictionaryAttr>(row);
    if (d.getAs<StringAttr>("class") != goldengate::AnnotationClasses::OutputFile)
      require(llvm::is_contained(updated,row),"expanded driver lost semantic/opaque annotation");
  }
  auto saved = dump(*f.root);
  require(failed(goldengate::prepareRocketDriverHeaders(f.circuit,error)) && dump(*f.root) == saved,
      "duplicate expanded driver transaction");
  require(succeeded(goldengate::emitOutputFiles(f.circuit, directory, "FireSim-generated", error)), error);
  require(succeeded(goldengate::emitAllAnnotations(f.circuit, directory.str() + "/bridge-clock-analysis.json", error)), error);
  llvm::outs() << "PASS complete expanded Rocket/Print driver, eleven LoadMem and four TracerV atomic rejections, seven atomic composition rejections and preserved hardware/archive\n";
}

void rocketNativeDriverHeaders(CircuitOp c, ModuleOp root, StringRef directory) {
  auto *ctx = c.getContext(); OpBuilder b(ctx); std::string error;
  // The recorded assembled boundary already includes interface outputs. Replay
  // that same interface generation, retaining the platform defines/archive.
  SmallVector<Attribute> retained;
  for (auto row : c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto d = cast<DictionaryAttr>(row); auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (d.getAs<StringAttr>("class") == goldengate::AnnotationClasses::OutputFile &&
        suffix && (suffix == ".const.h" || suffix == ".const.vh")) continue;
    retained.push_back(row);
  }
  c->setAttr("rawAnnotations", b.getArrayAttr(retained));
  require(succeeded(goldengate::prepareMetasimInterfaceHeader(c, "FireSim", error)), error);
  for (unsigned bad = 0; bad < 7; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(root->clone())); auto nc = *test->getOps<CircuitOp>().begin();
    if (bad == 0) nc.setName("FPGATop");
    else if (bad == 1) named(nc, "GGSingleClockBridge")->removeAttr("goldengate.mmioRegisters");
    else if (bad == 2) named(nc, "GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
    else if (bad == 3) nc->removeAttr("rawAnnotations");
    else {
      auto raw = nc->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> rows;
      for (auto row : raw) {
        auto d = cast<DictionaryAttr>(row);
        auto suffix = d.getAs<StringAttr>("fileSuffix");
        if (suffix && suffix == ".const.h") {
          if (bad == 4) continue;
          if (bad == 5) rows.push_back(row);
          if (bad == 6) {NamedAttrList marked(d); marked.set("goldengate.cpuManagedStreamHeader", b.getBoolAttr(true)); row = marked.getDictionary(ctx);}
        }
        rows.push_back(row);
      }
      nc->setAttr("rawAnnotations", b.getArrayAttr(rows));
    }
    auto saved = dump(*test);
    require(failed(goldengate::prepareRocketDriverHeaders(nc, error)) && !error.empty() && dump(*test) == saved,
        "native driver transaction rejection " + std::to_string(bad));
    if (bad == 2) require(error.find("FASED") != std::string::npos, "expected final-emitter failure");
  }
  auto original = c->getAttrOfType<ArrayAttr>("rawAnnotations"); auto hardware = dump(root);
  require(succeeded(goldengate::prepareRocketDriverHeaders(c, error)), error);
  auto updated = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  c->setAttr("rawAnnotations", original); require(dump(root) == hardware, "composition changed hardware"); c->setAttr("rawAnnotations", updated);
  for (auto row : original) {
    auto d = cast<DictionaryAttr>(row);
    if (d.getAs<StringAttr>("class") != goldengate::AnnotationClasses::OutputFile)
      require(llvm::is_contained(updated, row), "composition lost semantic/opaque annotation");
  }
  auto saved = dump(root);
  require(failed(goldengate::prepareRocketDriverHeaders(c, error)) && dump(root) == saved, "duplicate native header transaction");
  require(succeeded(goldengate::emitOutputFiles(c, directory, "FireSim-generated", error)), error);
  llvm::outs() << "PASS native Rocket transactional driver composition, eight atomic rejections and preserved hardware/archive\n";
}

void rocketAllocatedHeader(Fixture &f, StringRef output) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error, header, decoders;
  auto collect = [&](CircuitOp circuit) {
    SmallVector<FModuleOp> hosts;
    for (auto attr : named(circuit, "GGPrintBridgeHostWrapper")->getAttrOfType<ArrayAttr>("goldengate.printHostBindings"))
      hosts.push_back(named(circuit, cast<DictionaryAttr>(attr).getAs<StringAttr>("hostModule").getValue()));
    return hosts;
  };
  auto hosts = collect(f.circuit); auto before = dump(*f.root);
  require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, hosts, header, error)) &&
      succeeded(goldengate::preparePrintBridgeDecoderHeader(f.circuit, hosts, decoders, error)) &&
      dump(*f.root) == before, error);
  require(hosts.size() == 2, "Rocket header fixture requires two Print hosts");
  for (unsigned slot = 0; slot < 2; ++slot) {
    require(header.find("}, " + std::to_string(slot) + "U, args, " + std::to_string(slot + 1) + "U).release()") != std::string::npos,
        "Rocket Print constructor stream index");
    for (unsigned word = 0; word < 6; ++word)
      require(header.find(std::to_string(544 + slot * 32 + word * 4) + "ULL,") != std::string::npos,
          "Rocket Print constructor address");
  }
  for (unsigned slot = 0; slot < 3; ++slot)
    require(header.find("\"), " + std::to_string(slot * 524288) + "ULL, " +
        std::to_string(624 + 4 * slot) + "ULL, 6144U, 64U)") != std::string::npos,
        "Rocket CPU constructor allocation");
  // Reject stale register allocation, control identity, stream geometry and
  // actual queue/empty-writer wiring. Each failed batch preserves text and IR.
  for (unsigned bad = 0; bad < 9; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto hs = collect(c);
    if (bad == 0) c.setName("FPGATop");
    else if (bad == 1) {
      auto decoder = named(c, "GGControlAddressDecode");
      auto rows = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
      SmallVector<Attribute> values(rows.begin(), rows.end()); NamedAttrList row(cast<DictionaryAttr>(values[8]));
      row.set("start", b.getI64IntegerAttr(548)); values[8] = row.getDictionary(ctx);
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(values));
    } else if (bad == 2) {
      auto config = named(c, hs[0]->getAttrOfType<DictionaryAttr>("goldengate.printHost").getAs<StringAttr>("configModule").getValue());
      config->removeAttr("goldengate.mmioRegisters");
    } else if (bad == 3 || bad == 4) {
      auto writes = named(c, "GGControlWidgetWriteWrapper");
      auto rows = writes->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
      SmallVector<Attribute> values(rows.begin(), rows.end());
      if (bad == 3) values.pop_back(); else values.push_back(values.back());
      writes->setAttr("goldengate.controlWriteBindings", b.getArrayAttr(values));
      named(c, "GGControlReadDispatchWrapper")->setAttr("goldengate.controlReadBindings", b.getArrayAttr(values));
    } else if (bad == 5) {
      auto reader = named(c, "GGCPUStreamRead");
      auto rows = reader->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
      SmallVector<Attribute> values(rows.begin(), rows.end()); NamedAttrList row(cast<DictionaryAttr>(values[1]));
      row.set("bufferBaseAddress", b.getI64IntegerAttr(0)); values[1] = row.getDictionary(ctx);
      reader->setAttr("goldengate.sourceStreams", b.getArrayAttr(values));
    } else if (bad == 6) {
      auto bank = named(c, "GGCPUStreamCountBank"); b.setInsertionPointToStart(bank.getBodyBlock());
      auto zero = b.create<ConstantOp>(c.getLoc(), cast<UIntType>(bank.getPortType(3)), APInt(13, 0));
      for (auto pad : bank.getOps<PadPrimOp>()) if (pad.getInput() == bank.getArgument(3)) pad->setOperand(0, zero);
    } else {
      FModuleOp writer;
      for (auto m : c.getOps<FModuleOp>()) if (m->hasAttr("goldengate.fromHostCPUStreamCount")) writer = m;
      require(bool(writer), "missing catalog writer");
      if (bad == 7) writer->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(1));
      else {
        b.setInsertionPointToStart(writer.getBodyBlock());
        auto one = b.create<ConstantOp>(c.getLoc(), UIntType::get(ctx, 1), APInt(1, 1));
        for (auto conn : writer.getOps<StrictConnectOp>())
          if (conn.getDest() == writer.getArgument(port(writer, "aw_ready"))) conn->setOperand(1, one);
      }
    }
    auto saved = dump(*test); std::string rejected = "preserve";
    require(failed(goldengate::preparePrintBridgeAllocatedHeader(c, hs, rejected, error)) &&
        !error.empty() && rejected == "preserve" && dump(*test) == saved,
        "Rocket header rejection " + std::to_string(bad) + ": " + error);
  }
  // Registry/binding traversal order cannot change constructor order or ABI.
  for (auto [module, key] : {
      std::pair<FModuleOp, StringRef>{named(f.circuit, "GGControlReadDispatchWrapper"), "goldengate.controlReadBindings"},
      {named(f.circuit, "GGControlWidgetWriteWrapper"), "goldengate.controlWriteBindings"}}) {
    auto rows = module->getAttrOfType<ArrayAttr>(key); SmallVector<Attribute> values(rows.begin(), rows.end());
    std::reverse(values.begin(), values.end()); module->setAttr(key, b.getArrayAttr(values));
  }
  std::string reordered;
  require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(f.circuit, hosts, reordered, error)) && reordered == header,
      "Rocket binding order changed header: " + error);
  for (auto [suffix, text] : {std::pair<StringRef, StringRef>{".const.h", header}, {".decoders.h", decoders}}) {
    std::error_code ec; llvm::raw_fd_ostream file((output + suffix).str(), ec);
    require(!ec, "cannot write Rocket Print header"); file << text;
  }
  llvm::outs() << "PASS expanded Rocket Print/count header, nine atomic rejections and binding-order independence\n";
}

// This replay starts from a synthetic enabled-Print handoff plus actual Rocket
// components. Seed the FAME identities explicitly in the TEST; the compiler
// requires them and must not fabricate them from the host input port names.
void rocketF1Shim(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto body = [](FModuleOp m) {
    std::string text; llvm::raw_string_ostream out(text);
    m->print(out, OpPrintingFlags().useLocalScope()); return text;
  };
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  std::map<std::string, std::string> bodies;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts()); bodies[m.getName().str()] = body(m);
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  auto originalArchive = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  for (unsigned bad = 0; bad < 17; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto shell = named(c, "FPGATop");
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> as(raw.begin(), raw.end());
    unsigned source = 0;
    for (auto [i, a] : llvm::enumerate(as))
      if (cast<DictionaryAttr>(a).getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::HostClockSource) source = i;
    if (bad == 0) c.setName("WrongTop");
    else if (bad == 1) c->removeAttr("rawAnnotations");
    else if (bad == 2) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else if (bad == 3) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr("F1Shim"), shell.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 4) {
      b.setInsertionPointToEnd(named(c, "GGPrintCPUStreamPlatformWrapper").getBodyBlock());
      b.create<InstanceOp>(c.getLoc(), shell, "alreadyUsed");
    } else if (bad == 5) {as.erase(as.begin()+source); c->setAttr("rawAnnotations", b.getArrayAttr(as));}
    else if (bad == 6) {as.push_back(as[source]); c->setAttr("rawAnnotations", b.getArrayAttr(as));}
    else if (bad == 7) {
      NamedAttrList a(cast<DictionaryAttr>(as[source])); a.set("target", b.getStringAttr("~FPGATop|Other>clock"));
      as[source] = a.getDictionary(ctx); c->setAttr("rawAnnotations", b.getArrayAttr(as));
    } else if (bad == 8 || bad == 9) {
      as.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(bad == 8 ?
          goldengate::AnnotationClasses::OutputFile : goldengate::AnnotationClasses::XDCPaths)),
          b.getNamedAttr("fileSuffix", b.getStringAttr(".defines.vh"))}));
      c->setAttr("rawAnnotations", b.getArrayAttr(as));
    } else if (bad == 10) {
      auto inst = *shell.getOps<InstanceOp>().begin(); inst.setModuleNameAttr(FlatSymbolRefAttr::get(ctx, "Other"));
    } else if (bad == 11) {auto conn = *shell.getOps<ConnectOp>().begin(); conn.erase();}
    else if (bad == 12) {
      auto conn = *shell.getOps<ConnectOp>().begin(); b.setInsertionPoint(conn); b.clone(*conn);
    } else if (bad == 13) {
      auto conn = *shell.getOps<ConnectOp>().begin(); conn->setOperand(1, shell.getArguments()[1]);
    } else if (bad == 14) {
      SmallVector<bool> dirs(shell.getPortDirections().begin(), shell.getPortDirections().end());
      dirs[port(shell, "ctrl")] = true; shell.setPortDirections(dirs);
    } else if (bad == 15) {as.push_back(b.getStringAttr("malformed")); c->setAttr("rawAnnotations", b.getArrayAttr(as));}
    else {
      SmallVector<Attribute> names(shell.getPortNames().begin(), shell.getPortNames().end());
      names[port(shell, "ctrl")] = b.getStringAttr("renamedCtrl"); shell.setPortNames(names);
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketF1Shim(c, error)) && !error.empty() && dump(*test) == before,
        "Print F1Shim rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketF1Shim(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && body(m) == bodies.at(m.getName().str()), "Print F1Shim changed prior module body/identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "Print F1Shim changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "Print F1Shim replaced prior operations");
  auto shim = named(f.circuit, "F1Shim"); child(shim, named(f.circuit, "FPGATop"));
  require(f.circuit.getName() == "F1Shim" && shim.getNumPorts() == 8 && llvm::range_size(shim.getOps<InstanceOp>()) == 1 &&
      llvm::range_size(shim.getOps<RegResetOp>()) == 2 && shim.getOps<RegOp>().empty() && shim.getOps<MemOp>().empty(), "Print F1Shim geometry");
  for (auto reg : shim.getOps<RegResetOp>())
    require(cast<UIntType>(reg.getResult().getType()).getWidth() == 12 && reg.getClockVal() == shim.getArguments()[0] &&
        reg.getResetSignal() == shim.getArguments()[1] && reg.getResetValue().getDefiningOp<ConstantOp>().getValue().isZero(),
        "Print F1Shim request counter contract");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == originalArchive.size()+2, "Print F1Shim annotation count");
  auto source = llvm::find_if(raw, [](Attribute a) {
    return cast<DictionaryAttr>(a).getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::HostClockSource;
  });
  require(source != raw.end() && cast<DictionaryAttr>(*source).getAs<StringAttr>("target").getValue() == "~F1Shim|FPGATop>clock",
      "Print F1Shim retained host clock source");
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketF1Shim(f.circuit, error)) && dump(*f.root) == before,
      "repeated Print F1Shim changed IR"); ++rejected;
  if (!output.empty()) {
    auto suffix = reverse ? ".rocket-f1-shim-reverse.mlir" : ".rocket-f1-shim.mlir";
    std::error_code ec; llvm::raw_fd_ostream file((output + suffix).str(), ec);
    require(!ec, "cannot write Print F1Shim boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFPGATopShell(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, "GGPrintCPUStreamPlatformWrapper");
  auto missing = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFPGATopShell(f.circuit, error)) &&
      dump(*f.root) == missing, "Print shell accepted missing FAME identities"); ++rejected;
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  for (auto [cls, n] : {std::pair<StringRef, StringRef>{goldengate::AnnotationClasses::HostClock, "hostClock"},
                       {goldengate::AnnotationClasses::HostReset, "hostReset"}})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(cls)),
        b.getNamedAttr("target", b.getStringAttr("~GGPrintCPUStreamPlatformWrapper|GGPrintCPUStreamPlatformWrapper>" + n))}));
  SmallVector<Attribute> targets;
  for (StringRef n : {"hostClock", "hostReset", "ctrl.aw.bits.addr", "mem_0.r.bits.data",
                     "cpu_managed_axi4.r.bits.data", "other", "fased_pending_reads"})
    targets.push_back(b.getStringAttr("~GGPrintCPUStreamPlatformWrapper|GGPrintCPUStreamPlatformWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.FPGATopTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  auto body = [](FModuleOp m) {
    std::string text; llvm::raw_string_ostream out(text);
    m->print(out, OpPrintingFlags().useLocalScope()); return text;
  };
  std::map<std::string, std::string> bodies;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts()); bodies[m.getName().str()] = body(m);
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 15; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> as(raw.begin(), raw.end());
    if (bad == 0) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr("FPGATop"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 1) c->removeAttr("rawAnnotations");
    else if (bad == 2) c.setName("WrongTop");
    else if (bad == 3) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else if (bad == 4) {as.push_back(as[as.size()-3]); c->setAttr("rawAnnotations", b.getArrayAttr(as));}
    else if (bad == 5) {
      NamedAttrList d(cast<DictionaryAttr>(as[as.size()-2])); d.set("target", b.getStringAttr("~Other|Other>reset"));
      as[as.size()-2] = d.getDictionary(ctx); c->setAttr("rawAnnotations", b.getArrayAttr(as));
    } else if (bad == 6) {
      as.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::HostClockSource))}));
      c->setAttr("rawAnnotations", b.getArrayAttr(as));
    } else if (bad == 7) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, "hostClock")] = b.getStringAttr("renamedClock"); top.setPortNames(names);
    } else if (bad == 8) {
      SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[port(top, "cpu_managed_axi4")] = TypeAttr::get(UIntType::get(ctx, 512, false)); top.setPortTypes(types);
    } else if (bad == 9) {
      SmallVector<bool> dirs(top.getPortDirections().begin(), top.getPortDirections().end());
      dirs[port(top, "other")] = false; top.setPortDirections(dirs);
    } else if (bad == 10) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else if (bad == 13 || bad == 14) {
      SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      Type diagnostic = bad == 13 ? Type(BundleType::get(ctx, {{b.getStringAttr("inputLeaf"), true, UIntType::get(ctx, 1, false)}}))
                                  : Type(ClockType::get(ctx));
      types[port(top, "other")] = TypeAttr::get(diagnostic); top.setPortTypes(types);
    } else {
      for (auto conn : top.getOps<ConnectOp>())
        if (conn.getSrc() == top.getArguments()[port(top, "hostClock")]) {
          if (bad == 11) conn.erase();
          else {
            // A competing driver does not use the public hostClock argument.
            b.setInsertionPoint(conn);
            auto clock = b.create<WireOp>(c.getLoc(), ClockType::get(ctx), "competingClock");
            b.create<StrictConnectOp>(conn.getLoc(), conn.getDest(), clock.getResult());
          }
          break;
        }
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketFPGATopShell(c, error)) && !error.empty() && dump(*test) == before,
        "Print FPGATop rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFPGATopShell(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && body(m) == bodies.at(m.getName().str()), "Print shell changed prior module body/identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "Print shell changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "Print shell replaced prior operations");
  auto shell = named(f.circuit, "FPGATop"); child(shell, original);
  require(shell.getNumPorts() == 5 && llvm::range_size(shell.getOps<InstanceOp>()) == 1 &&
      llvm::range_size(shell.getOps<ConnectOp>()) == 5, "Print shell geometry");
  require(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").size() == annos.size()+1, "Print shell annotation count");
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFPGATopShell(f.circuit, error)) && dump(*f.root) == before,
      "repeated Print shell changed IR"); ++rejected;
  if (!output.empty()) {
    auto suffix = reverse ? ".rocket-fpga-top-shell-reverse.mlir" : ".rocket-fpga-top-shell.mlir";
    std::error_code ec; llvm::raw_fd_ostream file((output + suffix).str(), ec);
    require(!ec, "cannot write Print shell boundary"); f.root->print(file); file << '\n';
  }
}

void rocketCPUStreamPort(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGPrintCPUStreamPlatformWrapper", "GGCPUStreamPortAdapter"};
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"cpu_stream_b_valid", "cpu_stream_b_bits_id", "hostClock", "hostReset",
                     "mem_0.aw.bits.id", "cpu_stream_aw_valid", "cpu_stream_ar_bits_addr"})
    targets.push_back(b.getStringAttr("~GGPrintCPUStreamWriteResponseBufferWrapper|GGPrintCPUStreamWriteResponseBufferWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.CPUStreamPortTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 16; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c->removeAttr("rawAnnotations");
    else if (bad == 3) c.setName("WrongTop");
    else if (bad == 4 || bad == 5 || bad == 6) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 4 ? "hostClock" : bad == 5 ? "cpu_stream_b_bits_id" : "other")] =
          b.getStringAttr(bad == 6 ? "cpu_stream_b_bits_user" : "wrongField"); top.setPortNames(names);
    } else if (bad == 7) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else if (bad == 8) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else if (bad == 9) {
      SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[port(top, "cpu_stream_b_bits_id")] = TypeAttr::get(UIntType::get(ctx, 8, false)); top.setPortTypes(types);
    }
    else if (bad == 10 || bad == 11) {
      for (auto conn : top.getOps<ConnectOp>())
        if (conn.getSrc() == top.getArguments()[port(top, "cpu_stream_ar_valid")]) {
          if (bad == 10) conn.erase();
          else { b.setInsertionPoint(conn); b.create<StrictConnectOp>(conn.getLoc(), conn.getDest(), conn.getSrc()); }
          break;
        }
    } else if (bad == 12) {
      for (auto conn : top.getOps<StrictConnectOp>())
        if (conn.getDest() == top.getArguments()[port(top, "cpu_stream_b_bits_id")]) {
          b.setInsertionPoint(conn);
          conn->setOperand(1, b.create<ConstantOp>(conn.getLoc(), UIntType::get(ctx, 16, false), APInt(16, 0)));
          break;
        }
    } else if (bad == 13) named(c, "GGCPUStreamRead")->removeAttr("goldengate.streamAddressSpaceBits");
    else if (bad == 14) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      auto writer = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("DuplicateWriter"), top.getConventionAttr(), ArrayRef<PortInfo>{});
      writer->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(0));
    }
    else {
      auto queue = child(top, named(c, "GGCPUStreamBQueue2"));
      b.setInsertionPointToEnd(top.getBodyBlock());
      auto ready = b.create<SubfieldOp>(c.getLoc(), queue.getResult(3), "ready");
      auto zero = b.create<ConstantOp>(c.getLoc(), UIntType::get(ctx, 1, false), APInt(1, 0));
      b.create<StrictConnectOp>(c.getLoc(), ready, zero);
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketCPUStreamPort(c, error)) && !error.empty() && dump(*test) == before,
        "expanded CPU platform rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketCPUStreamPort(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "CPU platform changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "CPU platform changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "CPU platform replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 98 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 2 &&
      port(wrapper, "cpu_managed_axi4") == 97,
      "CPU platform wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, "GGCPUStreamPortAdapter"));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "CPU platform changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto module = (i < 2 || i >= 5) ? original.getName() : additions[0];
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGPrintCPUStreamPlatformWrapper|" + module + ">" + suffix).str(),
        "CPU platform failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketCPUStreamPort(f.circuit, error)) && dump(*f.root) == before,
      "repeated CPU platform changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-cpu-stream-platform-reverse.mlir" : ".rocket-cpu-stream-platform.mlir")).str(), ec);
    require(!ec, "cannot write expanded CPU platform boundary"); f.root->print(file); file << '\n';
  }
}

void rocketCPUStreamWriteResponseBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGPrintCPUStreamWriteResponseBufferWrapper", "GGCPUStreamBQueue2"};
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"cpu_stream_b_valid", "cpu_stream_b_bits_id", "hostClock", "hostReset",
                     "mem_0.aw.bits.id", "cpu_stream_aw_valid", "cpu_stream_ar_bits_addr"})
    targets.push_back(b.getStringAttr("~GGPrintCPUStreamResponseBufferWrapper|GGPrintCPUStreamResponseBufferWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.CPUStreamWriteResponseBufferTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 10; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c->removeAttr("rawAnnotations");
    else if (bad == 3) c.setName("WrongTop");
    else if (bad == 4 || bad == 5 || bad == 6) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 4 ? "hostClock" : bad == 5 ? "cpu_stream_b_bits_id" : "other")] =
          b.getStringAttr(bad == 6 ? "cpu_stream_b_bits_user" : "wrongField"); top.setPortNames(names);
    } else if (bad == 7) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else if (bad == 8) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else {
      SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[port(top, "cpu_stream_b_bits_id")] = TypeAttr::get(UIntType::get(ctx, 8, false)); top.setPortTypes(types);
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketCPUStreamWriteResponseBuffer(c, error)) && !error.empty() && dump(*test) == before,
        "expanded CPU B buffer rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketCPUStreamWriteResponseBuffer(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "CPU B buffer changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "CPU B buffer changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "CPU B buffer replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 122 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 2 &&
      port(wrapper, "cpu_stream_aw_bits_addr") == 120 && port(wrapper, "cpu_stream_aw_bits_len") == 121,
      "CPU B buffer wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, "GGCPUStreamBQueue2"));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "CPU B buffer changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto module = i < 2 ? original.getName() : additions[0];
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGPrintCPUStreamWriteResponseBufferWrapper|" + module + ">" + suffix).str(),
        "CPU B buffer failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketCPUStreamWriteResponseBuffer(f.circuit, error)) && dump(*f.root) == before,
      "repeated CPU B buffer changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-cpu-stream-write-response-buffer-reverse.mlir" : ".rocket-cpu-stream-write-response-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded CPU B buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketCPUStreamResponseBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGPrintCPUStreamResponseBufferWrapper", "GGCPUStreamRQueue2"};
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"cpu_stream_r_valid", "cpu_stream_r_bits_data", "hostClock", "hostReset",
                     "mem_0.aw.bits.id", "cpu_stream_aw_valid", "cpu_stream_ar_bits_addr"})
    targets.push_back(b.getStringAttr("~GGPrintCPUStreamReadBufferWrapper|GGPrintCPUStreamReadBufferWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.CPUStreamResponseBufferTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 10; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c->removeAttr("rawAnnotations");
    else if (bad == 3) c.setName("WrongTop");
    else if (bad == 4 || bad == 5 || bad == 6) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 4 ? "hostClock" : bad == 5 ? "cpu_stream_r_bits_data" : "other")] =
          b.getStringAttr(bad == 6 ? "cpu_stream_r_bits_user" : "wrongField"); top.setPortNames(names);
    } else if (bad == 7) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else if (bad == 8) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else {
      SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[port(top, "cpu_stream_r_bits_data")] = TypeAttr::get(UIntType::get(ctx, 64, false)); top.setPortTypes(types);
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketCPUStreamResponseBuffer(c, error)) && !error.empty() && dump(*test) == before,
        "expanded CPU R buffer rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketCPUStreamResponseBuffer(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "CPU R buffer changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "CPU R buffer changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "CPU R buffer replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 122 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 2 &&
      port(wrapper, "cpu_stream_aw_bits_addr") == 120 && port(wrapper, "cpu_stream_aw_bits_len") == 121,
      "CPU R buffer wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, "GGCPUStreamRQueue2"));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "CPU R buffer changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto module = i < 2 ? original.getName() : additions[0];
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGPrintCPUStreamResponseBufferWrapper|" + module + ">" + suffix).str(),
        "CPU R buffer failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketCPUStreamResponseBuffer(f.circuit, error)) && dump(*f.root) == before,
      "repeated CPU R buffer changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-cpu-stream-response-buffer-reverse.mlir" : ".rocket-cpu-stream-response-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded CPU R buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketCPUStreamReadBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGPrintCPUStreamReadBufferWrapper"};
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"cpu_stream_ar_valid", "cpu_stream_ar_bits_addr", "hostClock", "hostReset",
                     "mem_0.aw.bits.id", "cpu_stream_aw_valid", "cpu_stream_r_ready"})
    targets.push_back(b.getStringAttr("~GGPrintCPUStreamWriteBufferWrapper|GGPrintCPUStreamWriteBufferWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.CPUStreamReadBufferTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 14; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    auto queue = named(c, "GGCPUStreamAWQueue2");
    if (bad == 0) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[0]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 1) c->removeAttr("rawAnnotations");
    else if (bad == 2) c.setName("WrongTop");
    else if (bad == 3 || bad == 4 || bad == 5) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 3 ? "hostClock" : bad == 4 ? "cpu_stream_ar_bits_addr" : "other")] =
          b.getStringAttr(bad == 5 ? "cpu_stream_ar_bits_user" : "wrongField"); top.setPortNames(names);
    } else if (bad == 6) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else if (bad == 7) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else if (bad == 8) queue->removeAttr("goldengate.queueDepth");
    else if (bad == 9 || bad == 10) queue->setAttr(bad == 9 ? "goldengate.queueFlow" : "goldengate.queuePipe", b.getBoolAttr(true));
    else if (bad == 11) queue.setName("MissingQueue");
    else if (bad == 12) (*queue.getOps<MemOp>().begin())->setAttr("readLatency", b.getI32IntegerAttr(1));
    else {
      SmallVector<Attribute> names(queue.getPortNames().begin(), queue.getPortNames().end());
      names[2] = b.getStringAttr("wrongEnqueue"); queue.setPortNames(names);
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketCPUStreamReadBuffer(c, error)) && !error.empty() && dump(*test) == before,
        "expanded CPU AR buffer rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketCPUStreamReadBuffer(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "CPU AR buffer changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "CPU AR buffer changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "CPU AR buffer replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 122 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 2 &&
      port(wrapper, "cpu_stream_aw_bits_addr") == 120 && port(wrapper, "cpu_stream_aw_bits_len") == 121,
      "CPU AR buffer wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, "GGCPUStreamAWQueue2"));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "CPU AR buffer changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto module = i < 2 ? original.getName() : additions[0];
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGPrintCPUStreamReadBufferWrapper|" + module + ">" + suffix).str(),
        "CPU AR buffer failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketCPUStreamReadBuffer(f.circuit, error)) && dump(*f.root) == before,
      "repeated CPU AR buffer changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-cpu-stream-read-buffer-reverse.mlir" : ".rocket-cpu-stream-read-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded CPU AR buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketCPUStreamWriteBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGPrintCPUStreamWriteBufferWrapper", "GGCPUStreamAWQueue2", "GGCPUStreamWQueue2"};
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"cpu_stream_aw_valid", "cpu_stream_w_bits_strb", "hostClock", "hostReset",
                     "mem_0.aw.bits.id", "cpu_stream_b_bits_id", "cpu_stream_ar_bits_addr"})
    targets.push_back(b.getStringAttr("~GGPrintCPUStreamWriteWrapper|GGPrintCPUStreamWriteWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.CPUStreamWriteBufferTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 10; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    if (bad < 3) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 3) c->removeAttr("rawAnnotations");
    else if (bad == 4) c.setName("WrongTop");
    else if (bad == 5 || bad == 6 || bad == 9) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 5 ? "hostClock" : bad == 6 ? "cpu_stream_w_bits_strb" : "other")] =
          b.getStringAttr(bad == 9 ? "cpu_stream_aw_bits_addr" : "wrongField"); top.setPortNames(names);
    } else if (bad == 7) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketCPUStreamWriteBuffer(c, error)) && !error.empty() && dump(*test) == before,
        "expanded CPU AW/W buffer rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketCPUStreamWriteBuffer(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "CPU AW/W buffer changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "CPU AW/W buffer changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "CPU AW/W buffer replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 122 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 3 &&
      port(wrapper, "cpu_stream_aw_bits_addr") == 120 && port(wrapper, "cpu_stream_aw_bits_len") == 121,
      "CPU AW/W buffer wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, additions[1])); child(wrapper, named(f.circuit, additions[2]));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "CPU AW/W buffer changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto module = i < 2 ? original.getName() : additions[0];
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGPrintCPUStreamWriteBufferWrapper|" + module + ">" + suffix).str(),
        "CPU AW/W buffer failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketCPUStreamWriteBuffer(f.circuit, error)) && dump(*f.root) == before,
      "repeated CPU AW/W buffer changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-cpu-stream-write-buffer-reverse.mlir" : ".rocket-cpu-stream-write-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded CPU AW/W buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketEmptyCPUStreamWrite(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGPrintCPUStreamWriteWrapper"};
  // Probe recursive targets: every copied port moves to the new top.
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"hostClock", "hostReset", "mem_0.aw.bits.id", "cpu_stream_ar_bits_addr", "cpu_stream_r_bits_data"})
    targets.push_back(b.getStringAttr("~GGHostMemoryPlatformWrapper|GGHostMemoryPlatformWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.EmptyCPUStreamWriteTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 12; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    auto engine = named(c, "CatalogRocket_GGEmptyCPUStreamWrite");
    if (bad == 0 || bad == 7) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      auto m = b.create<FModuleOp>(c.getLoc(), b.getStringAttr(bad == 0 ? additions[0] : StringRef("DuplicateWriter")), top.getConventionAttr(), ArrayRef<PortInfo>{});
      if (bad == 7) m->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(0));
    } else if (bad == 1) c->removeAttr("rawAnnotations");
    else if (bad == 2) c.setName("WrongTop");
    else if (bad == 3) engine->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(1));
    else if (bad == 4) engine->removeAttr("goldengate.fromHostCPUStreamCount");
    else if (bad == 5 || bad == 6) {
      SmallVector<Attribute> names(engine.getPortNames().begin(), engine.getPortNames().end());
      names[bad == 5 ? 0 : 8] = b.getStringAttr("wrongField"); engine.setPortNames(names);
    } else if (bad == 8 || bad == 11) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 8 ? "hostClock" : "other")] = b.getStringAttr(bad == 8 ? "wrongClock" : "cpu_stream_aw_valid"); top.setPortNames(names);
    } else if (bad == 9) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "alreadyUsed");
    } else named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketEmptyCPUStreamWrite(c, error)) && !error.empty() && dump(*test) == before,
        "expanded host empty CPU write rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketEmptyCPUStreamWrite(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "empty CPU write changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "empty CPU write changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "empty CPU write replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 120 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 2 &&
      port(wrapper, "cpu_stream_aw_ready") == 109, "empty CPU write wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, "CatalogRocket_GGEmptyCPUStreamWrite"));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "empty CPU write changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto name = additions[0];
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGPrintCPUStreamWriteWrapper|" + name + ">" + suffix).str(),
        "empty CPU write failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketEmptyCPUStreamWrite(f.circuit, error)) && dump(*f.root) == before, "repeated empty CPU write changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-empty-cpu-stream-write-reverse.mlir" : ".rocket-empty-cpu-stream-write.mlir")).str(), ec);
    require(!ec, "cannot write expanded host empty CPU write boundary"); f.root->print(file); file << '\n';
  }
}

void rocketHostMemoryPort(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  const StringRef additions[]{"GGHostMemoryPlatformWrapper", "GGHostMemoryPortAdapter"};
  // Probe recursive targets: copied clock/reset move; consumed host_mem stays.
  SmallVector<Attribute> annos(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  SmallVector<Attribute> targets;
  for (StringRef n : {"hostClock", "hostReset", "host_mem", "host_mem.aw.bits.id", "host_mem.r.bits.id"})
    targets.push_back(b.getStringAttr("~GGHostMemoryReadResponseWrapper|GGHostMemoryReadResponseWrapper>" + n));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.HostMemoryPortTargets")),
      b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
    for (auto &op : m.getBodyBlock()->getOperations()) operations.push_back(&op);
  }
  for (unsigned bad = 0; bad < 25; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); auto top = named(c, original.getName());
    auto buffer = child(top, named(c, "GGHostMemoryOutputBuffer"));
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c->removeAttr("rawAnnotations");
    else if (bad == 3) c.setName("WrongTop");
    else if (bad == 4) child(top, named(c, "GGHostMemoryReadWrapper")).setName("missingSim");
    else if (bad == 5) child(top, named(c, "GGHostMemoryReadResponseRouter")).setName("missingRouter");
    else if (bad == 6) buffer.setName("missingBuffer");
    else if (bad == 7 || bad == 8) {
      auto parent = bad == 7 ? top : named(c, "GGHostMemoryReadWrapper");
      b.setInsertionPointToEnd(parent.getBodyBlock());
      b.create<InstanceOp>(c.getLoc(), bad == 7 ? named(c, "GGHostMemoryReadWrapper") : top, "extra");
    } else if (bad == 9) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, "host_mem")] = b.getStringAttr("wrongPort"); top.setPortNames(names);
    } else if (bad == 10) {
      auto engine = named(c, "GGFASEDTokenEngine");
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      NamedAttrList k(key), widths(key.getAs<DictionaryAttr>("axi4Widths"));
      widths.set("idBits", b.getI64IntegerAttr(5)); k.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", k.getDictionary(ctx));
    } else if (bad == 11) named(c, "GGPrintBridgeHostWrapper")->removeAttr("goldengate.printHostBindings");
    else if (bad == 23) {
      b.setInsertionPointToStart(top.getBodyBlock());
      b.create<SubfieldOp>(c.getLoc(), top.getArguments()[port(top, "host_mem")], "aw");
    } else if (bad == 24) {
      auto helper = named(c, "GGHostMemoryOutputBuffer");
      SmallVector<Attribute> names(helper.getPortNames().begin(), helper.getPortNames().end());
      names[0] = b.getStringAttr("wrongClock"); helper.setPortNames(names);
    } else {
      unsigned index = (bad == 12 || bad == 14 || bad == 16 || bad == 18 || bad == 20) ? 0 : 1;
      bool changed = false;
      for (auto &op : llvm::make_early_inc_range(top.getBodyBlock()->getOperations())) {
        if (!isa<ConnectOp, StrictConnectOp>(op)) continue;
        auto dest = op.getOperand(0), src = op.getOperand(1);
        if (bad == 22 ? dest != top.getArguments()[port(top, "host_mem")] : dest != buffer.getResult(index)) continue;
        b.setInsertionPoint(&op);
        if (bad == 14 || bad == 15) b.create<ConnectOp>(c.getLoc(), dest, src);
        else if (bad == 16 || bad == 17) b.create<StrictConnectOp>(c.getLoc(), dest, src);
        else if (bad >= 18 && bad <= 21) {
          auto wrong = b.create<WireOp>(c.getLoc(), src.getType(), "wrongDriver").getResult();
          if (bad < 20) b.create<ConnectOp>(c.getLoc(), dest, wrong);
          else { b.create<StrictConnectOp>(c.getLoc(), dest, wrong); op.erase(); }
        } else op.erase();
        changed = true; break;
      }
      require(changed, "host memory port malformed fixture did not change a binding");
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketHostMemoryPort(c, error)) && !error.empty() && dump(*test) == before,
        "expanded host memory port rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryPort(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "memory port changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "memory port changed prior metadata or SSA argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto m : existing) for (auto &op : m.getBodyBlock()->getOperations()) live.push_back(&op);
  require(live == operations, "memory port replaced prior operations");
  auto wrapper = named(f.circuit, additions[0]);
  require(wrapper.getNumPorts() == 109 && llvm::range_size(wrapper.getOps<InstanceOp>()) == 2 &&
      port(wrapper, "mem_0") == 108, "memory port wrapper geometry");
  child(wrapper, original); child(wrapper, named(f.circuit, additions[1]));
  auto updated = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(updated.size() == annos.size(), "memory port changed annotation count");
  auto probe = cast<DictionaryAttr>(updated[updated.size() - 1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
  for (auto [i, target] : llvm::enumerate(probe)) {
    auto name = i < 2 ? additions[0] : original.getName();
    auto suffix = cast<StringAttr>(targets[i]).getValue().split('>').second;
    require(cast<StringAttr>(target).getValue() == ("~GGHostMemoryPlatformWrapper|" + name + ">" + suffix).str(),
        "memory port failed recursive copied/consumed target transfer");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketHostMemoryPort(f.circuit, error)) && dump(*f.root) == before, "repeated memory port changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-host-memory-port-reverse.mlir" : ".rocket-host-memory-port.mlir")).str(), ec);
    require(!ec, "cannot write expanded host memory platform port boundary"); f.root->print(file); file << '\n';
  }
}

void rocketHostMemoryOutputBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto archive = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts; SmallVector<Operation *> operations;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (auto &op : original.getBodyBlock()->getOperations()) operations.push_back(&op);
  const StringRef additions[]{"GGHostMemoryOutputBuffer", "GGHostMemoryAddressQueue2",
      "GGHostMemoryWriteQueue2", "GGHostMemoryAckQueue2", "GGHostMemoryReadQueue2"};
  for (unsigned bad = 0; bad < 24; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(c, original.getName());
    auto sim = child(top, named(c, "GGHostMemoryReadWrapper"));
    auto router = child(top, named(c, "GGHostMemoryReadResponseRouter"));
    if (bad < 5) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 5) c->removeAttr("rawAnnotations");
    else if (bad == 6) c.setName("WrongTop");
    else if (bad == 7) sim.setName("missingSim");
    else if (bad == 8) router.setName("missingRouter");
    else if (bad == 9) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), named(c, "GGHostMemoryReadWrapper"), "extra");
    } else if (bad == 10) {
      auto engine = named(c, "GGFASEDTokenEngine");
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      NamedAttrList k(key), widths(key.getAs<DictionaryAttr>("axi4Widths"));
      widths.set("idBits", b.getI64IntegerAttr(5)); k.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", k.getDictionary(ctx));
    } else if (bad == 11) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, "host_mem")] = b.getStringAttr("wrongPort"); top.setPortNames(names);
    } else if (bad == 12) {
      b.setInsertionPointToStart(top.getBodyBlock());
      b.create<SubfieldOp>(c.getLoc(), top.getArguments()[port(top, "host_mem")], "aw");
    } else {
      bool changed = false;
      for (auto &op : llvm::make_early_inc_range(top.getBodyBlock()->getOperations())) {
        if (!isa<ConnectOp, StrictConnectOp>(op)) continue;
        Value dest = op.getOperand(0), source = op.getOperand(1);
        bool match;
        if (bad < 18) {
          const StringRef channels[]{"aw", "w", "b", "ar", "r"};
          auto field = (bad == 15 || bad == 17 ? source : dest).getDefiningOp<SubfieldOp>();
          match = field && field.getInput() == top.getArguments()[port(top, "host_mem")] && field.getFieldName() == channels[bad - 13];
        } else {
          auto clock = bad == 18 || bad == 20 || bad == 22;
          match = dest == sim.getResult(port(named(c, "GGHostMemoryReadWrapper"), clock ? "hostClock" : "hostReset"));
        }
        if (!match) continue;
        if (bad == 20 || bad == 21) { b.setInsertionPoint(&op); b.create<StrictConnectOp>(c.getLoc(), dest, source); }
        else if (bad >= 22) {
          b.setInsertionPoint(&op); b.create<ConnectOp>(c.getLoc(), dest, b.create<WireOp>(c.getLoc(), source.getType(), "wrongDriver").getResult());
        } else op.erase();
        changed = true; break;
      }
      require(changed, "output buffer malformed fixture did not change a binding");
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketHostMemoryOutputBuffer(c, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded output buffer rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryOutputBuffer(f.circuit, error)) && succeeded(verify(*f.root)), error);
  require(f.circuit->getAttr("rawAnnotations") == archive, "output buffer changed annotation archive");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "output buffer changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "output buffer changed a prior port or argument");
    }
  }
  SmallVector<Operation *> live;
  for (auto &op : original.getBodyBlock()->getOperations()) live.push_back(&op);
  for (auto op : operations) require(llvm::is_contained(live, op), "output buffer replaced prior top operation");
  require(original.getNumPorts() == 109 && llvm::range_size(original.getOps<InstanceOp>()) == 3, "output buffer wrapper geometry");
  child(original, named(f.circuit, additions[0]));
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketHostMemoryOutputBuffer(f.circuit, error)) && dump(*f.root) == before, "repeated output buffer changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-host-memory-output-buffer-reverse.mlir" : ".rocket-host-memory-output-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded output buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketHostMemoryReadResponses(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  const StringRef additions[]{"GGHostMemoryReadResponseWrapper", "GGHostMemoryReadResponseRouter"};
  for (unsigned bad = 0; bad < 24; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(c, original.getName());
    auto sim = child(top, named(c, "GGHostMemoryWriteResponseWrapper"));
    auto router = child(top, named(c, "GGHostMemoryReadArbiter"));
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c.setName("WrongTop");
    else if (bad == 3) c->removeAttr("rawAnnotations");
    else if (bad == 4) sim.setName("missingSim");
    else if (bad == 5) router.setName("missingRouter");
    else if (bad == 6) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "extra");
    } else if (bad == 7 || bad == 8) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 7 ? "loadmem_mem_r_valid" : "host_mem")] = b.getStringAttr("wrongPort"); top.setPortNames(names);
    } else if (bad == 9) {
      auto engine = named(c, "GGFASEDTokenEngine");
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      NamedAttrList k(key), widths(key.getAs<DictionaryAttr>("axi4Widths"));
      widths.set("idBits", b.getI64IntegerAttr(5)); k.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", k.getDictionary(ctx));
    } else {
      bool changed = false;
      for (auto &op : llvm::make_early_inc_range(top.getBodyBlock()->getOperations())) {
        if (!isa<ConnectOp, StrictConnectOp>(op)) continue;
        Value dest = op.getOperand(0), source = op.getOperand(1);
        auto src = source.getDefiningOp<SubfieldOp>();
        auto sm = named(c, "GGHostMemoryWriteResponseWrapper");
        bool match = bad == 10 ? dest == router.getResult(3) :
            bad == 11 ? src && src.getFieldName() == "r" :
            bad == 12 ? dest == sim.getResult(port(sm, "loadmem_mem_r_valid")) :
            bad == 13 ? dest == sim.getResult(port(sm, "loadmem_mem_r_bits_data")) :
            bad == 14 ? source == sim.getResult(port(sm, "loadmem_mem_r_ready")) :
            bad == 15 ? source == router.getResult(4) :
            bad < 20 ? dest == sim.getResult(port(sm, (bad == 16 || bad == 18) ? "hostClock" : "hostReset")) :
            dest == router.getResult((bad == 20 || bad == 22) ? 0 : 1);
        if (!match) continue;
        if (bad == 18 || bad == 19 || bad >= 22) { b.setInsertionPoint(&op); b.create<ConnectOp>(c.getLoc(), dest, source); }
        else op.erase();
        changed = true; break;
      }
      require(changed, "R malformed fixture did not change a binding");
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketHostMemoryReadResponses(c, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded host R rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> archive(raw.begin(), raw.end());
    auto prefix = "~" + original.getName().str() + "|" + original.getName().str() + ">";
    SmallVector<Attribute> targets;
    for (auto n : {"hostReset", "loadmem_mem_r_ready", "loadmem_mem_r_valid", "loadmem_mem_r_bits_data", "fased_host_mem.r",
                   "fased_host_mem.r.bits.id", "fased_host_mem", "host_mem.aw", "host_mem.w", "host_mem.b", "host_mem.ar",
                   "host_mem.aw.bits.id", "host_mem.b.bits.id", "host_mem.ar.bits.addr", "host_mem"})
      targets.push_back(b.getStringAttr(prefix + n));
    archive.push_back(b.getDictionaryAttr({b.getNamedAttr("nested", b.getArrayAttr(targets))}));
    c->setAttr("rawAnnotations", b.getArrayAttr(archive));
    require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryReadResponses(c, error)), error);
    auto text = dump(c.getOperation());
    for (auto n : {"hostReset", "host_mem.aw", "host_mem.w", "host_mem.b", "host_mem.ar", "host_mem.aw.bits.id", "host_mem.b.bits.id", "host_mem.ar.bits.addr"})
      require(text.find("~GGHostMemoryReadResponseWrapper|GGHostMemoryReadResponseWrapper>" + std::string(n) + "\"") != std::string::npos, "copied or remaining R-stage target lost");
    for (auto n : {"loadmem_mem_r_ready", "loadmem_mem_r_valid", "loadmem_mem_r_bits_data", "fased_host_mem.r", "fased_host_mem.r.bits.id", "fased_host_mem", "host_mem"})
      require(text.find("~GGHostMemoryReadResponseWrapper|" + original.getName().str() + ">" + n + "\"") != std::string::npos, "consumed R-stage target lost");
  }
  require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryReadResponses(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "host R changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "host R changed a prior port or argument");
    }
  }
  auto top = named(f.circuit, additions[0]);
  require(top.getNumPorts() == 109 && llvm::range_size(top.getOps<InstanceOp>()) == 2, "host R wrapper geometry");
  child(top, original); child(top, named(f.circuit, additions[1]));
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketHostMemoryReadResponses(f.circuit, error)) && dump(*f.root) == before, "repeated host R changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-host-memory-read-responses-reverse.mlir" : ".rocket-host-memory-read-responses.mlir")).str(), ec);
    require(!ec, "cannot write expanded host R boundary"); f.root->print(file); file << '\n';
  }
}

void rocketHostMemoryReadArbiter(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  const StringRef additions[]{"GGHostMemoryReadWrapper", "GGHostMemoryReadArbiter"};
  for (unsigned bad = 0; bad < 20; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(c, original.getName());
    auto sim = child(top, named(c, "GGHostMemoryWriteWrapper"));
    auto router = child(top, named(c, "GGHostMemoryWriteResponseRouter"));
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c.setName("WrongTop");
    else if (bad == 3) c->removeAttr("rawAnnotations");
    else if (bad == 4) sim.setName("missingSim");
    else if (bad == 5) router.setName("missingRouter");
    else if (bad == 6) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "extra");
    } else if (bad == 7 || bad == 8) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 7 ? "loadmem_mem_ar_valid" : "host_mem_write")] = b.getStringAttr("wrongPort"); top.setPortNames(names);
    } else if (bad == 9) {
      auto engine = named(c, "GGFASEDTokenEngine");
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      NamedAttrList k(key), widths(key.getAs<DictionaryAttr>("axi4Widths"));
      widths.set("idBits", b.getI64IntegerAttr(5)); k.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", k.getDictionary(ctx));
    } else {
      bool changed = false;
      for (auto &op : llvm::make_early_inc_range(top.getBodyBlock()->getOperations())) {
        if (!isa<ConnectOp, StrictConnectOp>(op)) continue;
        Value dest = op.getOperand(0), source = op.getOperand(1);
        auto src = source.getDefiningOp<SubfieldOp>();
        auto dst = dest.getDefiningOp<SubfieldOp>();
        auto sm = named(c, "GGHostMemoryWriteWrapper");
        bool match = bad == 10 ? src && src.getFieldName() == "ar" :
            bad == 11 ? src && src.getFieldName() == "r" :
            bad == 12 ? dest == router.getResult(0) :
            bad == 13 ? dest == sim.getResult(port(sm, "loadmem_mem_b_valid")) :
            bad == 14 ? dst && dst.getInput() == router.getResult(1) && dst.getFieldName() == "ready" :
            bad == 15 ? source == router.getResult(2) :
            dest == sim.getResult(port(sm, (bad == 16 || bad == 18) ? "hostClock" : "hostReset"));
        if (!match) continue;
        if (bad >= 18) { b.setInsertionPoint(&op); b.create<ConnectOp>(c.getLoc(), dest, source); }
        else op.erase();
        changed = true; break;
      }
      require(changed, "AR malformed fixture did not change a binding");
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketHostMemoryReadArbiter(c, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded host AR rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> archive(raw.begin(), raw.end());
    auto prefix = "~" + original.getName().str() + "|" + original.getName().str() + ">";
    SmallVector<Attribute> targets;
    for (auto n : {"hostReset", "loadmem_mem_ar_bits_addr", "fased_host_mem.ar", "fased_host_mem.ar.bits.id", "fased_host_mem",
                   "fased_host_mem.r", "fased_host_mem.r.bits.id", "host_mem_write.aw", "host_mem_write.w", "host_mem_write.b",
                   "host_mem_write.aw.bits.id", "host_mem_write.b.bits.id", "host_mem_write"})
      targets.push_back(b.getStringAttr(prefix + n));
    archive.push_back(b.getDictionaryAttr({b.getNamedAttr("nested", b.getArrayAttr(targets))}));
    c->setAttr("rawAnnotations", b.getArrayAttr(archive));
    require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryReadArbiter(c, error)), error);
    auto text = dump(c.getOperation());
    for (auto n : {"hostReset", "fased_host_mem.r", "fased_host_mem.r.bits.id", "host_mem.aw", "host_mem.w", "host_mem.b", "host_mem.aw.bits.id", "host_mem.b.bits.id"})
      require(text.find("~GGHostMemoryReadWrapper|GGHostMemoryReadWrapper>" + std::string(n) + "\"") != std::string::npos, "copied or remaining AR-stage target lost");
    for (auto n : {"loadmem_mem_ar_bits_addr", "fased_host_mem.ar", "fased_host_mem.ar.bits.id", "fased_host_mem", "host_mem_write"})
      require(text.find("~GGHostMemoryReadWrapper|" + original.getName().str() + ">" + n + "\"") != std::string::npos, "consumed AR-stage target lost");
  }
  require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryReadArbiter(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "host AR changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "host AR changed a prior port or argument");
    }
  }
  auto top = named(f.circuit, additions[0]);
  require(top.getNumPorts() == 113 && llvm::range_size(top.getOps<InstanceOp>()) == 2, "host AR wrapper geometry");
  child(top, original); child(top, named(f.circuit, additions[1]));
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketHostMemoryReadArbiter(f.circuit, error)) && dump(*f.root) == before, "repeated host AR changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-host-memory-read-arbiter-reverse.mlir" : ".rocket-host-memory-read-arbiter.mlir")).str(), ec);
    require(!ec, "cannot write expanded host AR boundary"); f.root->print(file); file << '\n';
  }
}

void rocketHostMemoryWriteResponses(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  const StringRef additions[]{"GGHostMemoryWriteResponseWrapper", "GGHostMemoryWriteResponseRouter"};
  for (unsigned bad = 0; bad < 18; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(c, original.getName());
    auto sim = child(top, named(c, "GGPrintLoadMemWrapper"));
    auto arb = child(top, named(c, "GGHostMemoryWriteArbiter"));
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) c.setName("WrongTop");
    else if (bad == 3) c->removeAttr("rawAnnotations");
    else if (bad == 4) sim.setName("missingSim");
    else if (bad == 5) arb.setName("missingArbiter");
    else if (bad == 6) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "extra");
    } else if (bad == 7 || bad == 8) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, bad == 7 ? "loadmem_mem_b_valid" : "host_mem_write")] = b.getStringAttr("wrongPort"); top.setPortNames(names);
    } else if (bad == 9) {
      auto engine = named(c, "GGFASEDTokenEngine");
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      NamedAttrList k(key), widths(key.getAs<DictionaryAttr>("axi4Widths"));
      widths.set("idBits", b.getI64IntegerAttr(5)); k.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", k.getDictionary(ctx));
    } else if (bad == 10 || bad == 11 || bad == 12) {
      for (auto x : top.getOps<ConnectOp>()) {
        auto sf = x.getSrc().getDefiningOp<SubfieldOp>();
        if (sf && sf.getFieldName() == (bad == 10 ? "b" : bad == 11 ? "r" : "ar")) { x.erase(); break; }
      }
    } else {
      Value dest = bad == 13 ? sim.getResult(port(named(c, "GGPrintLoadMemWrapper"), "loadmem_mem_b_valid")) :
          bad == 14 ? top.getArgument(port(top, "loadmem_mem_b_ready")) :
          bad == 15 ? sim.getResult(port(named(c, "GGPrintLoadMemWrapper"), "hostClock")) : arb.getResult(1);
      for (auto &op : llvm::make_early_inc_range(top.getBodyBlock()->getOperations()))
        if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == dest) {
          if (bad == 17) { b.setInsertionPoint(&op); b.create<StrictConnectOp>(c.getLoc(), dest, op.getOperand(1)); }
          else op.erase();
          break;
        }
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketHostMemoryWriteResponses(c, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded host B rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> archive(raw.begin(), raw.end());
    auto prefix = "~" + original.getName().str() + "|" + original.getName().str() + ">";
    SmallVector<Attribute> targets;
    for (auto n : {"hostReset", "loadmem_mem_b_valid", "fased_host_mem.b", "fased_host_mem.b.bits.id", "fased_host_mem",
                   "fased_host_mem.ar", "fased_host_mem.r", "fased_host_mem.r.bits.id", "host_mem_write.aw", "host_mem_write.w", "host_mem_write.aw.bits.id", "host_mem_write"})
      targets.push_back(b.getStringAttr(prefix + n));
    archive.push_back(b.getDictionaryAttr({b.getNamedAttr("nested", b.getArrayAttr(targets))}));
    c->setAttr("rawAnnotations", b.getArrayAttr(archive));
    require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryWriteResponses(c, error)), error);
    auto text = dump(c.getOperation());
    for (auto n : {"hostReset", "fased_host_mem.ar", "fased_host_mem.r", "fased_host_mem.r.bits.id", "host_mem_write.aw", "host_mem_write.w", "host_mem_write.aw.bits.id"})
      require(text.find("~GGHostMemoryWriteResponseWrapper|GGHostMemoryWriteResponseWrapper>" + std::string(n)) != std::string::npos, "copied or remaining B-stage target lost");
    for (auto n : {"loadmem_mem_b_valid", "fased_host_mem.b", "fased_host_mem.b.bits.id", "fased_host_mem", "host_mem_write"})
      require(text.find("~GGHostMemoryWriteResponseWrapper|" + original.getName().str() + ">" + n + "\"") != std::string::npos, "consumed B-stage target lost");
  }
  require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryWriteResponses(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "host B changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "host B changed a prior port or argument");
    }
  }
  auto top = named(f.circuit, additions[0]);
  require(top.getNumPorts() == 116 && llvm::range_size(top.getOps<InstanceOp>()) == 2, "host B wrapper geometry");
  child(top, original); child(top, named(f.circuit, additions[1]));
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketHostMemoryWriteResponses(f.circuit, error)) && dump(*f.root) == before, "repeated host B changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-host-memory-write-responses-reverse.mlir" : ".rocket-host-memory-write-responses.mlir")).str(), ec);
    require(!ec, "cannot write expanded host B boundary"); f.root->print(file); file << '\n';
  }
}

void rocketHostMemoryWriteArbiter(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  const StringRef additions[]{"GGHostMemoryWriteWrapper", "GGHostMemoryWriteArbiter", "GGHostMemoryWriteSourceQueue2"};
  for (unsigned bad = 0; bad < 21; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(c, original.getName());
    auto buffered = named(c, "GGFASEDAddressTranslationWrapper");
    auto loadmem = child(top, named(c, "GGPrintLoadMemEngine"));
    auto sim = child(top, buffered);
    if (bad < 3) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(additions[bad]), top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 3) c.setName("WrongTop");
    else if (bad == 4) c->removeAttr("rawAnnotations");
    else if (bad == 5) named(c, "GGPrintLoadMemEngine").setName("MissingEngine");
    else if (bad == 6) loadmem.setName("missingLoadMem");
    else if (bad == 7) sim.setName("missingBufferedMaster");
    else if (bad == 8 || bad == 10) {
      auto dest = bad == 8 ? top.getArgument(port(top, "fased_host_mem")) : sim.getResult(port(buffered, "hostClock"));
      for (auto x : top.getOps<ConnectOp>()) if (x.getDest() == dest) { x.erase(); break; }
    } else if (bad == 9 || bad == 11) {
      auto dest = loadmem.getResult(bad == 9 ? 0 : port(named(c, "GGPrintLoadMemEngine"), "loadmem_mem_aw_ready"));
      for (auto x : top.getOps<StrictConnectOp>()) if (x.getDest() == dest) { x.erase(); break; }
    } else if (bad == 12) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, "loadmem_mem_aw_bits_addr")] = b.getStringAttr("wrongAW"); top.setPortNames(names);
    } else if (bad == 13) {
      for (auto x : buffered.getOps<ConnectOp>()) if (x.getDest() == buffered.getArgument(port(buffered, "fased_host_mem"))) { x.erase(); break; }
    } else if (bad == 14) {
      auto buf = child(buffered, named(c, "GGFASEDHostMemoryBuffer"));
      for (auto x : buffered.getOps<StrictConnectOp>()) if (x.getDest() == buf.getResult(0)) { x.erase(); break; }
    } else if (bad == 15) {
      auto engine = named(c, "GGFASEDTokenEngine");
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      NamedAttrList k(key), widths(key.getAs<DictionaryAttr>("axi4Widths"));
      widths.set("idBits", b.getI64IntegerAttr(5)); k.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", k.getDictionary(ctx));
    } else if (bad == 18) {
      auto dest = sim.getResult(port(buffered, "loadmem_mcr"));
      for (auto x : top.getOps<ConnectOp>()) if (x.getDest() == dest) { x.erase(); break; }
    } else if (bad == 19 || bad == 20) {
      auto dest = bad == 19 ? loadmem.getResult(0) : loadmem.getResult(port(named(c, "GGPrintLoadMemEngine"), "loadmem_mem_r_valid"));
      for (auto x : top.getOps<StrictConnectOp>()) if (x.getDest() == dest) {
        b.setInsertionPoint(x);
        auto zero = b.create<ConstantOp>(c.getLoc(), UIntType::get(ctx, 1, false), APInt(1, 0));
        x.getSrcMutable().assign(zero); break;
      }
    } else {
      b.setInsertionPointToEnd(top.getBodyBlock());
      b.create<InstanceOp>(c.getLoc(), bad == 16 ? named(c, "GGPrintLoadMemEngine") : top, "extra");
    }
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketHostMemoryWriteArbiter(c, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded host write rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> archive(raw.begin(), raw.end());
    auto prefix = "~" + original.getName().str() + "|" + original.getName().str() + ">";
    archive.push_back(b.getDictionaryAttr({b.getNamedAttr("nested", b.getArrayAttr({
        b.getStringAttr(prefix + "hostReset"), b.getStringAttr(prefix + "loadmem_mem_aw_bits_addr"),
        b.getStringAttr(prefix + "fased_host_mem.aw.bits.id"), b.getStringAttr(prefix + "fased_host_mem.r.bits.id"),
        b.getStringAttr(prefix + "fased_host_mem.ar")}))}));
    c->setAttr("rawAnnotations", b.getArrayAttr(archive));
    require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryWriteArbiter(c, error)), error);
    auto text = dump(c.getOperation());
    for (auto target : {"hostReset", "fased_host_mem.r.bits.id", "fased_host_mem.ar"})
      require(text.find("~GGHostMemoryWriteWrapper|GGHostMemoryWriteWrapper>" + std::string(target)) != std::string::npos, "copied or remaining nested target lost");
    for (auto target : {"loadmem_mem_aw_bits_addr", "fased_host_mem.aw.bits.id"})
      require(text.find("~GGHostMemoryWriteWrapper|" + original.getName().str() + ">" + target) != std::string::npos, "consumed nested target lost");
  }
  require(succeeded(goldengate::mapPrintBridgeRocketHostMemoryWriteArbiter(f.circuit, error)) && succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(), "host write changed prior module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type && info.direction == old.direction &&
          info.annotations == old.annotations && info.sym == old.sym, "host write changed a prior port or argument");
    }
  }
  auto top = named(f.circuit, additions[0]);
  require(top.getNumPorts() == 118 && llvm::range_size(top.getOps<InstanceOp>()) == 2, "host write wrapper geometry");
  child(top, original); child(top, named(f.circuit, additions[1]));
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketHostMemoryWriteArbiter(f.circuit, error)) && dump(*f.root) == before, "repeated host write changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-host-memory-write-arbiter-reverse.mlir" : ".rocket-host-memory-write-arbiter.mlir")).str(), ec);
    require(!ec, "cannot write expanded host write boundary"); f.root->print(file); file << '\n';
  }
}

void rocketLoadMemEngine(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  const StringRef helpers[]{"GGLoadMemWriter", "GGLoadMemRequestQueue2", "GGLoadMemRequestArbiter",
      "GGLoadMemWriteMMIOBank", "GGLoadMemWriteDataFIFO", "GGLoadMemReadRequests", "GGLoadMemReadDataFIFO"};
  auto helper = [&](CircuitOp c, StringRef n) -> FModuleOp {
    for (auto m : c.getOps<FModuleOp>())
      if (m.getName() == n || m.getName().ends_with(("_" + n).str())) return m;
    throw std::runtime_error("missing native LoadMem catalog");
  };
  for (unsigned bad = 0; bad < 18; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(c, original.getName());
    if (bad < 2) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(bad == 0 ? "GGPrintLoadMemWrapper" : "GGPrintLoadMemEngine"),
          top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad < 9) helper(c, helpers[bad - 2]).setName("MissingCatalog");
    else if (bad == 9) c.setName("WrongTop");
    else if (bad == 10) c->removeAttr("rawAnnotations");
    else if (bad == 11) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "usedTop");
    } else if (bad == 12) {
      auto m = helper(c, helpers[0]); SmallVector<Attribute> names(m.getPortNames().begin(), m.getPortNames().end());
      names[4] = b.getStringAttr("wrongPayload"); m.setPortNames(names);
    } else if (bad == 13) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[port(top, "loadmem_mcr")] = b.getStringAttr("missingMCR"); top.setPortNames(names);
    } else if (bad == 14) {
      auto m = helper(c, helpers[0]); auto copy = cast<FModuleOp>(m->clone());
      copy.setName("Duplicate_GGLoadMemWriter"); c.getBodyBlock()->push_back(copy);
    } else if (bad == 15 || bad == 16) {
      auto m = helper(c, helpers[bad == 15 ? 0 : 6]);
      SmallVector<Attribute> types(m.getPortTypes().begin(), m.getPortTypes().end());
      types[4] = TypeAttr::get(UIntType::get(ctx, 32, false)); m.setPortTypes(types);
    } else child(top, named(c, "GGFASEDHostMemoryBuffer")).setName("MissingBuffer");
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketLoadMemEngine(c, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded LoadMem engine rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  // Probe nested target identity independently of the retained real archive.
  {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto c = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto prefix = "~" + original.getName().str();
    auto probe = b.getDictionaryAttr({b.getNamedAttr("nested", b.getArrayAttr({
        b.getStringAttr(prefix + "|" + original.getName().str() + ">hostReset"),
        b.getStringAttr(prefix + "|" + original.getName().str() + ">loadmem_mcr.read[4].bits")}))});
    SmallVector<Attribute> raw(c->getAttrOfType<ArrayAttr>("rawAnnotations").begin(), c->getAttrOfType<ArrayAttr>("rawAnnotations").end());
    raw.push_back(probe); c->setAttr("rawAnnotations", b.getArrayAttr(raw));
    require(succeeded(goldengate::mapPrintBridgeRocketLoadMemEngine(c, error)), error);
    auto text = dump(c.getOperation());
    require(text.find("~GGPrintLoadMemWrapper|GGPrintLoadMemWrapper>hostReset") != std::string::npos &&
        text.find("~GGPrintLoadMemWrapper|" + original.getName().str() + ">loadmem_mcr.read[4].bits") != std::string::npos,
        "LoadMem copied/consumed nested targets differ");
  }
  require(succeeded(goldengate::mapPrintBridgeRocketLoadMemEngine(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "LoadMem engine changed an existing module identity");
    for (auto [p, arg] : llvm::enumerate(arguments[j])) {
      auto info = m.getPorts()[p], old = priorPorts[j][p];
      require(m.getArguments()[p] == arg && info.name == old.name && info.type == old.type &&
          info.direction == old.direction && info.annotations == old.annotations && info.sym == old.sym,
          "LoadMem engine changed a prior port or SSA argument");
    }
  }
  auto top = named(f.circuit, "GGPrintLoadMemWrapper"), engine = named(f.circuit, "GGPrintLoadMemEngine");
  require(top.getNumPorts() == original.getNumPorts() + 15 && engine.getNumPorts() == 19 &&
      llvm::range_size(top.getOps<InstanceOp>()) == 2 && llvm::range_size(engine.getOps<InstanceOp>()) == 7 &&
      engine.getOps<AssertOp>().begin() != engine.getOps<AssertOp>().end(),
      "LoadMem engine wrapper/helper geometry or permission checks differ");
  child(top, original); child(top, engine);
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketLoadMemEngine(f.circuit, error)) &&
      dump(*f.root) == before, "repeated LoadMem engine changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-loadmem-engine-reverse.mlir" : ".rocket-loadmem-engine.mlir")).str(), ec);
    require(!ec, "cannot write expanded LoadMem engine boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDHostMemoryBuffer(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  const StringRef definitions[]{"GGFASEDHostMemoryBuffer", "GGFASEDMemoryAddressQueue2",
      "GGFASEDMemoryWriteQueue2", "GGFASEDMemoryAckQueue2", "GGFASEDMemoryReadQueue2"};
  auto original = named(f.circuit, f.circuit.getName());
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 22; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto circuit = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(circuit, original.getName());
    auto engine = named(circuit, "GGFASEDTokenEngine");
    if (bad < 5) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr(definitions[bad]),
          top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 5) circuit.setName("WrongTop");
    else if (bad == 6) circuit->removeAttr("rawAnnotations");
    else if (bad >= 7 && bad <= 12) {
      const StringRef fields[]{"addrBits", "dataBits", "idBits", "maxReadTransfer", "idReuse", "maxFlight"};
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      StringRef dict = bad < 10 ? "axi4Widths" : "axi4Edge";
      NamedAttrList row(key.getAs<DictionaryAttr>(dict)); row.set(fields[bad - 7], b.getI64IntegerAttr(99));
      NamedAttrList changed(key); changed.set(dict, row.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", changed.getDictionary(ctx));
    } else if (bad == 13) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), top, "duplicateTop");
    } else if (bad == 14) {
      b.setInsertionPointToEnd(top.getBodyBlock());
      b.create<InstanceOp>(circuit.getLoc(), named(circuit, "GGFASEDHostMemoryWrapper"), "duplicateSim");
    } else if (bad == 15) {
      for (auto c : top.getOps<ConnectOp>()) if (auto i = c.getSrc().getDefiningOp<InstanceOp>())
        if (i.getName() == "translation") { c.erase(); break; }
    } else if (bad == 16 || bad == 17) {
      auto translation = child(top, named(circuit, "GGFASEDAddressTranslation"));
      for (auto c : top.getOps<StrictConnectOp>())
        if (c.getDest() == translation.getResult(bad - 16)) { c.erase(); break; }
    } else if (bad == 18) engine->removeAttr("goldengate.bridgeConstructor");
    else if (bad == 19) {
      auto decoder = named(circuit, "GGControlAddressDecode");
      auto rows = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
      SmallVector<Attribute> changed(rows.begin(), rows.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1])); row.set("start", b.getI64IntegerAttr(256));
      changed[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    } else if (bad == 20) {
      child(top, named(circuit, "GGFASEDReadDeinterleaver")).setName("MissingDeinterleaver");
    } else engine.setName("MissingEngine");
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketFASEDHostMemoryBuffer(circuit, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded host memory buffer rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDHostMemoryBuffer(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  require(named(f.circuit, f.circuit.getName()) == original &&
      f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == raw,
      "expanded host memory buffer changed top or annotation identity");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "expanded host memory buffer changed a prior module identity");
    for (auto [i, arg] : llvm::enumerate(arguments[j])) {
      auto p = m.getPorts()[i], old = priorPorts[j][i];
      require(m.getArguments()[i] == arg && p.name == old.name && p.type == old.type &&
          p.direction == old.direction && p.annotations == old.annotations && p.sym == old.sym,
          "expanded host memory buffer changed a prior port or argument identity");
    }
  }
  auto helper = named(f.circuit, "GGFASEDHostMemoryBuffer");
  require(bool(child(original, helper)) && llvm::range_size(helper.getOps<InstanceOp>()) == 5,
      "expanded host memory buffer five-channel queue hierarchy differs");
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDHostMemoryBuffer(f.circuit, error)) &&
      dump(*f.root) == before, "repeated expanded host memory buffer changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-fased-host-memory-buffer-reverse.mlir" : ".rocket-fased-host-memory-buffer.mlir")).str(), ec);
    require(!ec, "cannot write expanded host memory buffer boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDReadDeinterleaver(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 19; ++bad) {
    OwningOpRef<ModuleOp> test(cast<ModuleOp>(f.root->clone()));
    auto circuit = *test->getOps<CircuitOp>().begin(); OpBuilder b(ctx);
    auto top = named(circuit, original.getName());
    auto engine = named(circuit, "GGFASEDTokenEngine");
    if (bad < 2) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr(bad == 0 ?
          "GGFASEDReadDeinterleaver" : "GGFASEDDeinterleaveQueue8"),
          top.getConventionAttr(), ArrayRef<PortInfo>{});
    } else if (bad == 2) circuit.setName("WrongTop");
    else if (bad == 3) circuit->removeAttr("rawAnnotations");
    else if (bad >= 4 && bad <= 9) {
      const StringRef fields[]{"addrBits", "dataBits", "idBits", "maxReadTransfer", "idReuse", "maxFlight"};
      auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      StringRef dict = bad < 7 ? "axi4Widths" : "axi4Edge";
      NamedAttrList row(key.getAs<DictionaryAttr>(dict)); row.set(fields[bad - 4], b.getI64IntegerAttr(99));
      NamedAttrList changed(key); changed.set(dict, row.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", changed.getDictionary(ctx));
    } else if (bad == 10) {
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), top, "duplicateTop");
    } else if (bad == 11) {
      b.setInsertionPointToEnd(top.getBodyBlock());
      b.create<InstanceOp>(circuit.getLoc(), named(circuit, "GGFASEDHostMemoryWrapper"), "duplicateSim");
    } else if (bad == 12) {
      for (auto c : top.getOps<ConnectOp>()) if (auto i = c.getDest().getDefiningOp<InstanceOp>())
        if (i.getName() == "translation") { c.erase(); break; }
    } else if (bad == 13 || bad == 14) {
      auto translation = child(top, named(circuit, "GGFASEDAddressTranslation"));
      for (auto c : top.getOps<StrictConnectOp>())
        if (c.getDest() == translation.getResult(bad - 13)) { c.erase(); break; }
    } else if (bad == 15) engine->removeAttr("goldengate.bridgeConstructor");
    else if (bad == 16) {
      auto decoder = named(circuit, "GGControlAddressDecode");
      auto rows = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
      SmallVector<Attribute> changed(rows.begin(), rows.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1])); row.set("start", b.getI64IntegerAttr(256));
      changed[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    } else if (bad == 17) {
      auto inner = named(circuit, "GGFASEDHostMemoryWrapper");
      auto names = llvm::to_vector(inner.getPortNames());
      names[port(inner, "fased_host_mem")] = b.getStringAttr("MissingMaster"); inner.setPortNames(names);
    } else engine.setName("MissingEngine");
    auto before = dump(*test);
    require(failed(goldengate::mapPrintBridgeRocketFASEDReadDeinterleaver(circuit, error)) &&
        !error.empty() && dump(*test) == before,
        "expanded deinterleaver rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDReadDeinterleaver(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  require(named(f.circuit, f.circuit.getName()) == original &&
      f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == raw,
      "expanded deinterleaver changed top or annotation identity");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "expanded deinterleaver changed a prior module identity");
    for (auto [i, arg] : llvm::enumerate(arguments[j])) {
      auto p = m.getPorts()[i], old = priorPorts[j][i];
      require(m.getArguments()[i] == arg && p.name == old.name && p.type == old.type &&
          p.direction == old.direction && p.annotations == old.annotations && p.sym == old.sym,
          "expanded deinterleaver changed a prior port or argument identity");
    }
  }
  auto helper = named(f.circuit, "GGFASEDReadDeinterleaver");
  require(bool(child(original, helper)) && llvm::range_size(helper.getOps<InstanceOp>()) == 16 &&
      llvm::range_size(helper.getOps<AssertOp>()) == 32,
      "expanded deinterleaver queue hierarchy or assertions differ");
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDReadDeinterleaver(f.circuit, error)) &&
      dump(*f.root) == before, "repeated expanded deinterleaver changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-fased-read-deinterleaver-reverse.mlir" : ".rocket-fased-read-deinterleaver.mlir")).str(), ec);
    require(!ec, "cannot write expanded deinterleaver boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDAddressTranslation(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto names = llvm::to_vector(original.getPortNames());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 18; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad < 2) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(bad == 0 ?
          "GGFASEDAddressTranslation" : "GGFASEDAddressTranslationWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), original, "duplicate");
    }
    if (bad == 3) {
      SmallVector<Attribute> changed(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1])); row.set("start", b.getI64IntegerAttr(256));
      changed[1] = row.getDictionary(ctx);
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    }
    if (bad == 4) f.circuit.setName("WrongTop");
    if (bad == 5) f.circuit->removeAttr("rawAnnotations");
    if (bad >= 6 && bad <= 8) {
      const StringRef missing[]{"fased_host_mem", "hostClock", "hostReset"};
      auto changed = names; changed[port(original, missing[bad - 6])] = b.getStringAttr("missingBoundary");
      original.setPortNames(changed);
    }
    if (bad >= 9 && bad <= 11) {
      NamedAttrList changed(constructor);
      if (bad == 9) changed.set("memoryRegionName", b.getStringAttr("WrongRegion"));
      if (bad == 10) {
        NamedAttrList edge(constructor.getAs<DictionaryAttr>("axi4Edge"));
        auto addresses = cast<ArrayAttr>(edge.get("address"));
        SmallVector<Attribute> rows(addresses.begin(), addresses.end());
        NamedAttrList row(cast<DictionaryAttr>(rows[0])); row.set("mask", b.getI64IntegerAttr(1));
        rows[0] = row.getDictionary(ctx); edge.set("address", b.getArrayAttr(rows));
        changed.set("axi4Edge", edge.getDictionary(ctx));
      }
      if (bad == 11) changed.erase("axi4Edge");
      engine->setAttr("goldengate.bridgeConstructor", changed.getDictionary(ctx));
    }
    if (bad >= 12 && bad <= 14) {
      const StringRef fields[]{"addrBits", "dataBits", "idBits"};
      NamedAttrList widths(constructor.getAs<DictionaryAttr>("axi4Widths"));
      widths.set(fields[bad - 12], b.getI64IntegerAttr(1));
      NamedAttrList changed(constructor); changed.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", changed.getDictionary(ctx));
    }
    if (bad == 15) engine->removeAttr("goldengate.bridgeConstructor");
    if (bad == 16) engine.setName("MissingEngine");
    if (bad == 17) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr("SecondMemoryEngine"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
      collision->setAttr("goldengate.bridgeConstructor", constructor);
    }
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketFASEDAddressTranslation(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == before,
        "expanded translation rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
    original.setPortNames(names); decoder->setAttr("goldengate.controlRegions", regions);
    engine.setName("GGFASEDTokenEngine"); engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); if (extra) extra.erase(); f.circuit.setName(original.getName());
  }
  SmallVector<Attribute> annotated(raw.begin(), raw.end());
  const StringRef probes[]{"ctrl.aw.bits.addr", "hostReset", "fased_host_mem.aw.bits.addr",
      "fased_host_mem.ar.bits.addr", "fased_host_mem.r.bits.data", "fased_host_mem", ""};
  for (auto probe : probes) {
    std::string target = "~GGFASEDHostMemoryWrapper";
    if (!probe.empty()) target += "|GGFASEDHostMemoryWrapper>" + probe.str();
    annotated.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.TranslationBoundary")),
        b.getNamedAttr("target", b.getStringAttr(target))}));
  }
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annotated));
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDAddressTranslation(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, "GGFASEDAddressTranslationWrapper");
  auto helper = named(f.circuit, "GGFASEDAddressTranslation");
  require(f.circuit.getName() == top.getName() && bool(child(top, original)) && bool(child(top, helper)),
      "expanded translation hierarchy differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "expanded translation changed an existing module identity");
    for (auto [i, arg] : llvm::enumerate(arguments[j])) {
      auto p = m.getPorts()[i], old = priorPorts[j][i];
      require(m.getArguments()[i] == arg && p.name == old.name && p.type == old.type &&
          p.direction == old.direction && p.annotations == old.annotations && p.sym == old.sym,
          "expanded translation changed a prior port or argument identity");
    }
  }
  auto addressWidth = [](Type t) {
    auto field = [](Type t, StringRef n) -> Type {
      auto bundle = cast<BundleType>(t); return bundle.getElementType(bundle.getElementIndex(n).value());
    };
    return cast<UIntType>(field(field(field(t, "aw"), "bits"), "addr")).getWidth();
  };
  require(top.getNumPorts() == original.getNumPorts() &&
      addressWidth(original.getPortType(port(original, "fased_host_mem"))) == 35 &&
      addressWidth(top.getPortType(port(top, "fased_host_mem"))) == 34 &&
      llvm::range_size(helper.getOps<AssertOp>()) == 4 &&
      helper.getOps<RegOp>().empty() && helper.getOps<RegResetOp>().empty(),
      "expanded translation address widths, assertions or state differ");
  auto transferred = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  for (auto [i, probe] : llvm::enumerate(probes)) {
    std::string target = "~GGFASEDAddressTranslationWrapper";
    if (!probe.empty()) target += std::string(i < 2 ? "|GGFASEDAddressTranslationWrapper>" :
        "|GGFASEDHostMemoryWrapper>") + probe.str();
    require(cast<DictionaryAttr>(transferred[raw.size() + i]).getAs<StringAttr>("target").getValue() == target,
        "expanded translation target transfer differs");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDAddressTranslation(f.circuit, error)) &&
      dump(*f.root) == before, "repeated expanded translation changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-fased-address-translation-reverse.mlir" : ".rocket-fased-address-translation.mlir")).str(), ec);
    require(!ec, "cannot write expanded translation boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDHostMemory(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto raw = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto names = llvm::to_vector(original.getPortNames());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<Value>> arguments;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); arguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 14; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr("GGFASEDHostMemoryWrapper"),
          original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), original, "duplicate");
    }
    if (bad == 2) {
      SmallVector<Attribute> changed(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1])); row.set("start", b.getI64IntegerAttr(256));
      changed[1] = row.getDictionary(ctx);
      decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    }
    if (bad == 3) f.circuit.setName("WrongTop");
    if (bad == 4) f.circuit->removeAttr("rawAnnotations");
    if (bad >= 5 && bad <= 7) {
      const StringRef missing[]{"fased_host_requests", "fased_host_read_response", "fased_host_write_response"};
      auto changed = names; changed[port(original, missing[bad - 5])] = b.getStringAttr("missingBoundary");
      original.setPortNames(changed);
    }
    if (bad >= 8 && bad <= 10) {
      const StringRef fields[]{"addrBits", "dataBits", "idBits"};
      NamedAttrList widths(constructor.getAs<DictionaryAttr>("axi4Widths"));
      widths.set(fields[bad - 8], b.getI64IntegerAttr(1));
      NamedAttrList changed(constructor); changed.set("axi4Widths", widths.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", changed.getDictionary(ctx));
    }
    if (bad == 11) {
      auto changed = names; changed[port(original, "other")] = b.getStringAttr("fased_host_mem");
      original.setPortNames(changed);
    }
    if (bad == 12) engine->removeAttr("goldengate.bridgeConstructor");
    if (bad == 13) engine.setName("MissingEngine");
    auto before = dump(*f.root);
    require(failed(goldengate::mapPrintBridgeRocketFASEDHostMemory(f.circuit, error)) &&
        !error.empty() && dump(*f.root) == before,
        "expanded host memory rejection " + std::to_string(bad) + " changed IR: " + error); ++rejected;
    original.setPortNames(names); decoder->setAttr("goldengate.controlRegions", regions);
    engine.setName("GGFASEDTokenEngine"); engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (collision) collision.erase(); if (extra) extra.erase(); f.circuit.setName(original.getName());
  }
  SmallVector<Attribute> annotated(raw.begin(), raw.end());
  const StringRef probes[]{"ctrl.aw.bits.addr", "fased_host_requests.aw.bits.addr",
      "fased_host_read_response.bits.data", "fased_host_write_response.ready",
      "fased_host_requests.w.bits.id", "fased_host_requests.aw.bits.user", "fased_host_requests", ""};
  for (auto probe : probes) {
    std::string target = "~GGControlMasterWrapper";
    if (!probe.empty()) target += "|GGControlMasterWrapper>" + probe.str();
    annotated.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.HostMemoryBoundary")),
        b.getNamedAttr("target", b.getStringAttr(target))}));
  }
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annotated));
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDHostMemory(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, "GGFASEDHostMemoryWrapper");
  require(f.circuit.getName() == top.getName() && bool(child(top, original)),
      "expanded host memory hierarchy differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == arguments[j].size(),
        "expanded host memory changed an existing module identity");
    for (auto [i, arg] : llvm::enumerate(arguments[j]))
      require(m.getArguments()[i] == arg, "expanded host memory changed a port argument identity");
  }
  require(original.getNumPorts() == top.getNumPorts() + 2 &&
      top.getPortName(top.getNumPorts() - 1) == "fased_host_mem" &&
      top.getPortDirection(top.getNumPorts() - 1) == Direction::Out &&
      cast<BundleType>(top.getPortType(top.getNumPorts() - 1)).getNumElements() == 5 &&
      top.getPortType(port(top, "ctrl")) == original.getPortType(port(original, "ctrl")),
      "expanded host memory split consumption or control preservation differs");
  auto transferred = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  const StringRef expected[]{"GGFASEDHostMemoryWrapper>ctrl.aw.bits.addr",
      "GGFASEDHostMemoryWrapper>fased_host_mem.aw.bits.addr",
      "GGFASEDHostMemoryWrapper>fased_host_mem.r.bits.data",
      "GGFASEDHostMemoryWrapper>fased_host_mem.b.ready",
      "GGControlMasterWrapper>fased_host_requests.w.bits.id",
      "GGControlMasterWrapper>fased_host_requests.aw.bits.user",
      "GGControlMasterWrapper>fased_host_requests", ""};
  for (auto [i, suffix] : llvm::enumerate(expected)) {
    std::string target = "~GGFASEDHostMemoryWrapper";
    if (!suffix.empty()) target += "|" + suffix.str();
    require(cast<DictionaryAttr>(transferred[raw.size() + i]).getAs<StringAttr>("target").getValue() == target,
        "expanded host memory target transfer differs");
  }
  auto before = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDHostMemory(f.circuit, error)) &&
      dump(*f.root) == before, "repeated expanded host memory changed IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output +
        (reverse ? ".rocket-fased-host-memory-reverse.mlir" : ".rocket-fased-host-memory.mlir")).str(), ec);
    require(!ec, "cannot write expanded host memory boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDMMIOBank(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDHistograms");
  auto rows = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  auto portNames = llvm::to_vector(original.getPortNames());
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  SmallVector<SmallVector<Value>> priorArguments;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    priorArguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 11; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDMMIOWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), original, "duplicate");
    }
    if (bad == 2 || bad == 3) {
      SmallVector<Attribute> changed(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[1]));
      if (bad == 2) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      changed[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(changed));
    }
    if (bad == 4) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 5) f.circuit.setName("WrongTop");
    if (bad == 6) f.circuit->removeAttr("rawAnnotations");
    if (bad == 7) bank.setName("MissingHistogramBank");
    if (bad == 8) {
      auto names = portNames; names[port(original, "fased_histograms_mcr")] = b.getStringAttr("missingFragment");
      original.setPortNames(names);
    }
    if (bad == 9 || bad == 10) {
      SmallVector<Attribute> changed(rows.begin(), rows.end());
      NamedAttrList row(cast<DictionaryAttr>(changed[0]));
      if (bad == 9) row.set("offset", b.getI32IntegerAttr(20)); else row.set("writeable", b.getBoolAttr(true));
      changed[0] = row.getDictionary(ctx); bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(changed));
    }
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDMMIOBank(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED MMIO rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw); bank.setName("GGFASEDHistograms");
    bank->setAttr("goldengate.mmioRegisters", rows); original.setPortNames(portNames);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDMMIOBank(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDMMIOWrapper", "FASED MMIO top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size(),
        "FASED MMIO composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction && m.getArguments()[i] == priorArguments[j][i],
          "FASED MMIO composition changed an existing port or argument identity");
  }
  const StringRef fragments[]{"fased_latency_mcr", "fased_request_limits_mcr", "fased_histograms_mcr",
      "fased_statistics_mcr", "fased_functional_model_mcr", "fased_response_errors_mcr"};
  unsigned copied = 0;
  for (auto p : original.getPorts()) {
    if (llvm::is_contained(fragments, p.name.getValue())) continue;
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED MMIO composition changed a retained port"); ++copied;
  }
  require(original.getNumPorts() == copied + 6 && top.getNumPorts() == copied + 1 &&
      top.getPortName(copied) == "fasedBridge_mcr" && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, original)), "FASED MMIO fragment consumption differs");
  auto mcr = cast<BundleType>(top.getPortType(copied));
  require(cast<FVectorType>(mcr.getElement("read")->type).getNumElements() == 21 &&
      cast<FVectorType>(mcr.getElement("write")->type).getNumElements() == 21,
      "FASED MMIO aggregate lane count differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDMMIOBank(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED MMIO composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-mmio-reverse.mlir" : ".rocket-fased-mmio.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED MMIO boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDHistograms(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDHistograms");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  SmallVector<SmallVector<Value>> priorArguments;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    priorArguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 9; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDHistogramsWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1 || bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), bad == 1 ? original : bank, "duplicate");
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    if (bad == 8) bank.setName("MissingHistogramBank");
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDHistograms(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED histogram rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    bank.setName("GGFASEDHistograms");
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDHistograms(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDHistogramsWrapper", "FASED histogram top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size(),
        "FASED histogram composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction && m.getArguments()[i] == priorArguments[j][i],
          "FASED histogram composition changed an existing port or argument identity");
  }
  unsigned copied = 0;
  for (auto p : original.getPorts()) {
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED histogram composition changed a retained port"); ++copied;
  }
  require(top.getNumPorts() == copied + 1 && top.getPortName(copied) == "fased_histograms_mcr" &&
      top.getPortType(copied) == bank.getPortType(6) && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, bank)) && bool(child(top, original)), "FASED histogram bank attachment differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDHistograms(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED histogram composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-histograms-reverse.mlir" : ".rocket-fased-histograms.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED histogram boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDStatistics(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto bank = named(f.circuit, "GGFASEDStatistics");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  const llvm::StringRef hierarchy[]{"GGFASEDResponseErrorsWrapper", "GGFASEDFunctionalModelRegisterWrapper",
      "GGFASEDLatencyRegistersWrapper", "GGFASEDRequestLimitsWrapper", "GGFASEDReadAdmissionWrapper",
      "GGFASEDWriteAdmissionWrapper", "GGFASEDWriteRetirementWrapper", "GGFASEDWritePairingWrapper",
      "GGFASEDTimingAWQueueWrapper", "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
      "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
  auto observed = [&](FModuleOp m) { return llvm::is_contained(hierarchy, m.getName()); };
  SmallVector<FModuleOp> existing; SmallVector<SmallVector<PortInfo>> priorPorts;
  SmallVector<SmallVector<Value>> priorArguments;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
    priorArguments.emplace_back(m.getArguments().begin(), m.getArguments().end());
  }
  for (unsigned bad = 0; bad < 11; ++bad) {
    FModuleOp collision; InstanceOp extra;
    auto middle = named(f.circuit, hierarchy[6]); auto savedName = middle.getNameAttr();
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDStatisticsWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1 || bad == 2) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), bad == 1 ? original : bank, "duplicate");
    }
    if (bad == 3 || bad == 4) {
      SmallVector<Attribute> rows(regions.begin(), regions.end());
      NamedAttrList row(cast<DictionaryAttr>(rows[1]));
      if (bad == 3) row.set("start", b.getI64IntegerAttr(256)); else row.erase("name");
      rows[1] = row.getDictionary(ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
    }
    if (bad == 5) {
      NamedAttrList key(constructor);
      NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
      edge.set("maxFlight", b.getI64IntegerAttr(9)); key.set("axi4Edge", edge.getDictionary(ctx));
      engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(ctx));
    }
    if (bad == 6) f.circuit.setName("WrongTop");
    if (bad == 7) f.circuit->removeAttr("rawAnnotations");
    if (bad == 8) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(), middle, "duplicateResponse");
    }
    if (bad == 9) middle.setName("WrongResponseHierarchy");
    if (bad == 10) {
      SmallVector<std::pair<unsigned, PortInfo>> added{{middle.getNumPorts(),
          PortInfo(b.getStringAttr("fased_accepted_r_fire"), UIntType::get(ctx, 1), Direction::Out)}};
      middle.insertPorts(added);
    }
    auto state = dump(*f.root);
    auto result = goldengate::mapPrintBridgeRocketFASEDStatistics(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED statistics rejection " + std::to_string(bad) + " changed IR or accepted invalid input: " + error); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    middle.setName(savedName);
    if (bad == 10) {
      llvm::BitVector erased(middle.getNumPorts()); erased.set(middle.getNumPorts() - 1);
      middle.erasePorts(erased);
    }
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDStatistics(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDStatisticsWrapper", "FASED statistics top differs");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m && m.getNumPorts() == priorPorts[j].size() + unsigned(observed(m)),
        "FASED statistics composition changed an existing module identity/port count");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction && m.getArguments()[i] == priorArguments[j][i],
          "FASED statistics composition changed an existing port or argument identity");
  }
  for (auto name : hierarchy) {
    auto m = named(f.circuit, name); unsigned last = m.getNumPorts() - 1;
    require(m.getPortName(last) == "fased_accepted_r_fire" &&
        m.getPortType(last) == UIntType::get(ctx, 1) && m.getPortDirection(last) == Direction::Out,
        "accepted R observation port missing from response hierarchy");
  }
  unsigned copied = 0;
  for (auto p : priorPorts[llvm::find(existing, original) - existing.begin()]) {
    require(top.getPortName(copied) == p.name && top.getPortType(copied) == p.type &&
        top.getPortDirection(copied) == p.direction, "FASED statistics composition changed a retained port"); ++copied;
  }
  require(top.getNumPorts() == copied + 1 && top.getPortName(copied) == "fased_statistics_mcr" &&
      top.getPortType(copied) == bank.getPortType(8) && top.getPortDirection(copied) == Direction::Out &&
      bool(child(top, bank)) && bool(child(top, original)), "FASED statistics bank attachment differs");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDStatistics(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED statistics composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-statistics-reverse.mlir" : ".rocket-fased-statistics.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED statistics boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDReadAdmission(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  const StringRef observed[]{"GGFASEDWriteAdmissionWrapper", "GGFASEDWriteRetirementWrapper", "GGFASEDWritePairingWrapper", "GGFASEDTimingAWQueueWrapper",
      "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
      "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDReadAdmissionWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(),
          named(f.circuit, "GGFASEDTimingAWQueueWrapper"), "duplicate");
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
    auto result = goldengate::mapPrintBridgeRocketFASEDReadAdmission(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED read admission rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDReadAdmission(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDReadAdmissionWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED read admission changed bank or expanded allocation");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m,
        "FASED read admission replaced an existing module operation");
    bool appended = llvm::is_contained(observed, m.getName());
    require(m.getNumPorts() == priorPorts[j].size() + unsigned(appended),
        "FASED read admission changed an unrelated module's ports");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction,
          "FASED read admission changed an existing port index/type/direction");
    if (appended) require(m.getPortName(m.getNumPorts()-1) == "fased_accepted_r_last_fire" &&
        m.getPortType(m.getNumPorts()-1) == UIntType::get(ctx, 1, false) &&
        m.getPortDirection(m.getNumPorts()-1) == Direction::Out,
        "FASED read admission observation differs");
  }
  auto oldPorts = priorPorts[llvm::find(existing, original) - existing.begin()];
  for (auto [i, p] : llvm::enumerate(oldPorts)) {
    auto type = p.type;
    if (p.name.getValue() == "fased_timing_requests") {
      auto requests = cast<BundleType>(type); auto fields = requests.getElements();
      SmallVector<BundleType::BundleElement> rewritten(fields.begin(), fields.end());
      for (auto &ch : rewritten) if (ch.name.getValue() == "ar") {
        auto bundle = cast<BundleType>(ch.type); auto leaves = bundle.getElements();
        SmallVector<BundleType::BundleElement> changed(leaves.begin(), leaves.end());
        for (auto &leaf : changed) if (leaf.name.getValue() == "ready") leaf.isFlip = false;
        ch.type = BundleType::get(ctx, changed);
      }
      type = BundleType::get(ctx, rewritten);
    }
    require(top.getPortName(i) == p.name && top.getPortType(i) == type &&
        top.getPortDirection(i) == p.direction,
        "FASED read admission must change only AR ready and retain every prior port index/type/direction");
  }
  require(top.getNumPorts() == oldPorts.size() + 2 &&
      top.getPortName(oldPorts.size()) == "fased_read_max_reqs" &&
      top.getPortType(oldPorts.size()) == UIntType::get(ctx, 4, false) &&
      top.getPortDirection(oldPorts.size()) == Direction::In &&
      top.getPortName(oldPorts.size()+1) == "fased_pending_reads" &&
      top.getPortDirection(oldPorts.size()+1) == Direction::Out,
      "FASED read admission maximum/count boundary differs");
  auto sim = child(top, original);
  require(bool(sim), "FASED read admission wrapper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDReadAdmission(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED read admission composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-read-admission-reverse.mlir" : ".rocket-fased-read-admission.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED read admission boundary"); f.root->print(file); file << '\n';
  }
}

void rocketFASEDWriteAdmission(Fixture &f, StringRef output, bool reverse, unsigned &rejected) {
  auto *ctx = f.circuit.getContext(); OpBuilder b(ctx); std::string error;
  auto original = named(f.circuit, f.circuit.getName());
  auto decoder = named(f.circuit, "GGControlAddressDecode");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  auto engine = named(f.circuit, "GGFASEDTokenEngine");
  auto constructor = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> existing;
  SmallVector<SmallVector<PortInfo>> priorPorts;
  for (auto m : f.circuit.getOps<FModuleOp>()) {
    existing.push_back(m); priorPorts.push_back(m.getPorts());
  }
  auto bank = named(f.circuit, "GGFASEDLatencyRegisters");
  auto bankState = dump(bank.getOperation());
  for (unsigned bad = 0; bad < 8; ++bad) {
    FModuleOp collision; InstanceOp extra;
    if (bad == 0) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      collision = b.create<FModuleOp>(f.circuit.getLoc(), b.getStringAttr(
          "GGFASEDWriteAdmissionWrapper"), original.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 1) {
      b.setInsertionPointToEnd(original.getBodyBlock());
      extra = b.create<InstanceOp>(f.circuit.getLoc(),
          original, "duplicate");
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
    auto result = goldengate::mapPrintBridgeRocketFASEDWriteAdmission(f.circuit, error);
    require(failed(result) && !error.empty() && dump(*f.root) == state,
        "expanded FASED write admission rejection " + std::to_string(bad) +
        (succeeded(result) ? " accepted invalid input" : " changed IR or omitted diagnostic: " + error)); ++rejected;
    decoder->setAttr("goldengate.controlRegions", regions);
    engine->setAttr("goldengate.bridgeConstructor", constructor);
    f.circuit->setAttr("rawAnnotations", raw);
    if (extra) extra.erase();
    if (collision) collision.erase(); f.circuit.setName(original.getName());
  }
  require(succeeded(goldengate::mapPrintBridgeRocketFASEDWriteAdmission(f.circuit, error)) &&
      succeeded(verify(*f.root)), error);
  auto top = named(f.circuit, f.circuit.getName());
  require(top.getName() == "GGFASEDWriteAdmissionWrapper" &&
      decoder->getAttr("goldengate.controlRegions") == regions && dump(bank.getOperation()) == bankState,
      "FASED write admission changed bank or expanded allocation");
  for (auto [j, m] : llvm::enumerate(existing)) {
    require(named(f.circuit, m.getName()) == m,
        "FASED write admission replaced an existing module operation");
    require(m.getNumPorts() == priorPorts[j].size(),
        "FASED write admission changed an existing module's ports");
    for (auto [i, p] : llvm::enumerate(priorPorts[j]))
      require(m.getPortName(i) == p.name && m.getPortType(i) == p.type &&
          m.getPortDirection(i) == p.direction,
          "FASED write admission changed an existing port index/type/direction");
  }
  auto oldPorts = original.getPorts();
  require(top.getNumPorts() == oldPorts.size(), "FASED admission changed the outer port count");
  for (auto [i, p] : llvm::enumerate(oldPorts)) {
    auto type = p.type;
    if (p.name.getValue() == "fased_timing_requests") {
      auto requests = cast<BundleType>(type); auto fields = requests.getElements();
      SmallVector<BundleType::BundleElement> rewritten(fields.begin(), fields.end());
      for (auto &ch : rewritten) if (ch.name.getValue() != "ar") {
        auto bundle = cast<BundleType>(ch.type); auto leaves = bundle.getElements();
        SmallVector<BundleType::BundleElement> changed(leaves.begin(), leaves.end());
        for (auto &leaf : changed) if (leaf.name.getValue() == "ready") leaf.isFlip = false;
        ch.type = BundleType::get(ctx, changed);
      }
      type = BundleType::get(ctx, rewritten);
    }
    require(top.getPortName(i) == p.name && top.getPortType(i) == type &&
        top.getPortDirection(i) == p.direction,
        "FASED admission must change only AW/W ready flips and retain AR readiness");
  }
  auto sim = child(top, original);
  require(bool(sim), "FASED write admission wrapper is uninstantiated");
  auto state = dump(*f.root);
  require(failed(goldengate::mapPrintBridgeRocketFASEDWriteAdmission(f.circuit, error)) && dump(*f.root) == state,
      "repeated FASED write admission composition mutated IR"); ++rejected;
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-fased-write-admission-reverse.mlir" : ".rocket-fased-write-admission.mlir")).str(), ec);
    require(!ec, "cannot write expanded FASED write admission boundary"); f.root->print(file); file << '\n';
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

void rocketStreams(MLIRContext &ctx, StringRef baseline, StringRef controlBaseline, StringRef output,
                   bool catalogOnly = false) {
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
    // This case attaches the actual TracerV queue/control pair, so restore
    // its reserved names instead of the generic preceding-stream fixture.
    SmallVector<Attribute> originalPorts(original.getPortNames().begin(), original.getPortNames().end());
    for (auto &name : originalPorts) {
      if (name == b.getStringAttr("preceding_stream")) name = b.getStringAttr("tracerv_stream");
      if (name == b.getStringAttr("preceding_stream_count")) name = b.getStringAttr("tracerv_stream_count");
    }
    original.setPortNames(originalPorts);
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
      require(succeeded(goldengate::importRocketControlCatalog(f.circuit, pc, error)), error);
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
      if (catalogOnly) {
        std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".catalog-reverse.mlir" : ".catalog.mlir")).str(), ec);
        require(!ec, "cannot write allocated Rocket/Print catalog"); f.root->print(file); file << '\n';
        continue;
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
      rocketFASEDWriteLatency(f, output, reverse, rejected);
      rocketFASEDTimingAWQueue(f, output, reverse, rejected);
      rocketFASEDWritePairing(f, output, reverse, rejected);
      rocketFASEDWriteRetirement(f, output, reverse, rejected);
      rocketFASEDWriteAdmission(f, output, reverse, rejected);
      rocketFASEDReadAdmission(f, output, reverse, rejected);
      rocketFASEDRequestLimits(f, output, reverse, rejected);
      rocketFASEDLatencyRegisters(f, output, reverse, rejected);
      rocketFASEDFunctionalModelRegister(f, output, reverse, rejected);
      rocketFASEDResponseErrors(f, output, reverse, rejected);
      rocketFASEDStatistics(f, output, reverse, rejected);
      rocketFASEDHistograms(f, output, reverse, rejected);
      rocketFASEDMMIOBank(f, output, reverse, rejected);
      rocketFASEDControl(f, output, reverse, rejected);
      rocketControlMaster(f, output, reverse, rejected);
      rocketFASEDHostMemory(f, output, reverse, rejected);
      rocketFASEDAddressTranslation(f, output, reverse, rejected);
      rocketFASEDReadDeinterleaver(f, output, reverse, rejected);
      rocketFASEDHostMemoryBuffer(f, output, reverse, rejected);
      rocketLoadMemEngine(f, output, reverse, rejected);
      rocketHostMemoryWriteArbiter(f, output, reverse, rejected);
      rocketHostMemoryWriteResponses(f, output, reverse, rejected);
      rocketHostMemoryReadArbiter(f, output, reverse, rejected);
      rocketHostMemoryReadResponses(f, output, reverse, rejected);
      rocketHostMemoryOutputBuffer(f, output, reverse, rejected);
      rocketHostMemoryPort(f, output, reverse, rejected);
      rocketEmptyCPUStreamWrite(f, output, reverse, rejected);
      rocketCPUStreamWriteBuffer(f, output, reverse, rejected);
      rocketCPUStreamReadBuffer(f, output, reverse, rejected);
      rocketCPUStreamResponseBuffer(f, output, reverse, rejected);
      rocketCPUStreamWriteResponseBuffer(f, output, reverse, rejected);
      rocketCPUStreamPort(f, output, reverse, rejected);
    }
    if (!output.empty()) {
      std::error_code ec; llvm::raw_fd_ostream file((output + (reverse ? ".rocket-streams-reverse.mlir" : ".rocket-streams.mlir")).str(), ec);
      require(!ec, "cannot write Rocket/Print stream boundary"); f.root->print(file); file << '\n';
    }
  }
  if (catalogOnly) {
    llvm::outs() << "PASS imported Rocket/Print catalog allocation: thirteen banks, both constructor orders and "
                 << rejected << " atomic rejections\n";
    return;
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
      b.create<ConnectOp>(f.circuit.getLoc(), original.getArgument(port(original, "preceding_stream")), q.getResult(3));
      b.create<StrictConnectOp>(f.circuit.getLoc(), original.getArgument(port(original, "preceding_stream_count")), q.getResult(4));
    }
    if (reverse) std::reverse(f.hosts.begin(), f.hosts.end());
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
    SmallVector<goldengate::CPUStreamSourcePort> earlier;
    SmallVector<goldengate::CPUStreamCountPort> earlierCounts;
    if (reverse) {
      earlier.push_back({"TRACERVBRIDGEMODULE_0_to_cpu_stream", "preceding_stream", 6144});
      earlierCounts.push_back({"TRACERVBRIDGEMODULE_0_to_cpu_stream", "preceding_stream_count", 13});
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

void nestedCPUAllocation(MLIRContext &ctx) {
  for (int bad = -1; bad < 9; ++bad) {
    Fixture f(ctx); std::string error; OpBuilder b(&ctx);
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
    auto bound = named(f.circuit, f.circuit.getName());
    auto registry = bound->getAttrOfType<ArrayAttr>("goldengate.printHostBindings");
    auto first = cast<DictionaryAttr>(registry[0]), second = cast<DictionaryAttr>(registry[1]);
    auto inner = bound;
    for (unsigned depth = 0; depth < 3; ++depth) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      auto wrapper = b.create<FModuleOp>(inner.getLoc(), b.getStringAttr("NestedPrint" + std::to_string(depth)),
          inner.getConventionAttr(), inner.getPorts());
      b.setInsertionPointToStart(wrapper.getBodyBlock());
      auto sim = b.create<InstanceOp>(inner.getLoc(), inner, "sim");
      for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
        Value external = wrapper.getArgument(i), internal = sim.getResult(i);
        b.create<ConnectOp>(inner.getLoc(), p.direction == Direction::In ? internal : external,
            p.direction == Direction::In ? external : internal);
      }
      inner = wrapper;
    }
    f.circuit.setName(inner.getName());
    auto sim = *inner.getOps<InstanceOp>().begin();
    if (bad >= 0 && bad <= 4) {
      StringRef key = bad == 1 ? "streamPort" : bad == 2 ? "controlPort" : "countPort";
      auto name = bad == 3 ? StringRef("hostReset") : first.getAs<StringAttr>(key).getValue();
      unsigned p = port(inner, name);
      ConnectOp connection;
      for (auto c : inner.getOps<ConnectOp>())
        if (c.getDest() == inner.getArgument(p) || c.getDest() == sim.getResult(p)) connection = c;
      require(bool(connection), "missing nested forwarding connection");
      b.setInsertionPoint(connection);
      if (bad == 3) connection->setOperand(1, b.create<ConstantOp>(inner.getLoc(), UIntType::get(&ctx, 1), APInt(1, 0)));
      else if (bad == 4) b.create<ConnectOp>(inner.getLoc(), connection.getDest(), connection.getSrc());
      else {
        unsigned other = port(inner, second.getAs<StringAttr>(key).getValue());
        connection->setOperand(1, bad == 2 ? inner.getArgument(other) : sim.getResult(other));
      }
    }
    if (bad == 5) inner->setAttr("goldengate.printHostBindings", registry);
    if (bad == 6) sim.setNameAttr(b.getStringAttr("detached"));
    if (bad == 7) sim.setModuleNameAttr(FlatSymbolRefAttr::get(&ctx, inner.getName()));
    if (bad == 8) {
      auto names = llvm::to_vector(inner.getPortNames());
      names[port(inner, first.getAs<StringAttr>("countPort").getValue())] = b.getStringAttr("missingCount");
      inner.setPortNames(names);
    }
    auto before = dump(*f.root), boundBefore = dump(bound);
    auto result = goldengate::mapPrintBridgeCPUStreams(f.circuit, {}, {}, error);
    if (bad >= 0) require(failed(result) && !error.empty() && dump(*f.root) == before,
        "nested Print allocation accepted or mutated corruption " + std::to_string(bad));
    else {
      require(succeeded(result) && succeeded(verify(*f.root)) && dump(bound) == boundBefore, error);
      auto sources = named(f.circuit, "GGCPUStreamRead")->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
      auto counts = named(f.circuit, "GGCPUStreamCountBank")->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
      require(sources.size() == 2 && counts.size() == 2 && f.circuit.getName() == "GGCPUStreamCountWrapper",
          "nested Print allocation lost an ordered queue/count");
    }
  }
  llvm::outs() << "PASS nested Print allocation through three retained wrappers and nine atomic forwarding/identity rejections\n";
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
    SmallVector<goldengate::CPUStreamSourcePort> sources{{"tracerv", "preceding_stream", 6144}};
    SmallVector<goldengate::CPUStreamCountPort> counts{{bad == 7 ? "wrong" : "tracerv", "preceding_stream_count", 13}};
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
    if (argc == 2 && StringRef(argv[1]) == "--nested-cpu-allocation-test") {
      nestedCPUAllocation(ctx); allocationRejections(ctx); return 0;
    }
    if (argc == 3 && StringRef(argv[1]) == "--selected-print-cpu-read-buffer") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source) && succeeded(verify(*source)), "invalid selected Print AW/W boundary");
      auto c = *source->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); std::string error;
      auto hostsFor = [&](CircuitOp circuit) {
        SmallVector<FModuleOp> hosts;
        for (auto attr : named(circuit, "GGPrintBridgeHostWrapper")->getAttrOfType<ArrayAttr>("goldengate.printHostBindings"))
          hosts.push_back(named(circuit, cast<DictionaryAttr>(attr).getAs<StringAttr>("hostModule").getValue()));
        return hosts;
      };
      auto moduleBody = [](FModuleOp m) {
        std::string text; llvm::raw_string_ostream out(text);
        m->print(out, OpPrintingFlags().useLocalScope()); return text;
      };
      for (unsigned bad = 0; bad < 12; ++bad) {
        OwningOpRef<ModuleOp> test(cast<ModuleOp>(source->clone()));
        auto tc = *test->getOps<CircuitOp>().begin(); auto top = named(tc, "GGSelectedPrintCPUStreamWriteBufferWrapper");
        auto hs = hostsFor(tc);
        if (bad == 0) tc.setName("Wrong");
        else if (bad == 1) tc->removeAttr("rawAnnotations");
        else if (bad == 2) hs.clear();
        else if (bad == 3) {
          b.setInsertionPointToEnd(tc.getBodyBlock());
          b.create<FModuleOp>(tc.getLoc(), b.getStringAttr("GGSelectedPrintCPUStreamReadBufferWrapper"),
              top.getConventionAttr(), ArrayRef<PortInfo>{});
        } else if (bad == 4) {
          named(tc, "GGCPUStreamAWQueue2")->setAttr("goldengate.queueDepth", b.getI64IntegerAttr(3));
        } else if (bad == 5) {
          (*named(tc, "GGCPUStreamAWQueue2").getOps<MemOp>().begin())->setAttr("readLatency", b.getI32IntegerAttr(1));
        } else if (bad < 8) {
          SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
          names[port(top, "cpu_stream_ar_bits_addr")] = b.getStringAttr(bad == 6 ?
              "missing_ar_addr" : "cpu_stream_ar_unsupported");
          top.setPortNames(names);
        } else if (bad == 8) {
          named(tc, "GGCPUStreamRead")->removeAttr("goldengate.sourceStreams");
        } else if (bad == 9) {
          named(tc, "GGControlAddressDecode")->removeAttr("goldengate.controlRegions");
        } else if (bad == 10) {
          named(tc, "GGEmptyCPUStreamWrite")->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(1));
        } else {
          auto writer = named(tc, "GGEmptyCPUStreamWrite");
          for (auto conn : writer.getOps<StrictConnectOp>())
            if (conn.getDest() == writer.getBodyBlock()->getArgument(port(writer, "aw_ready"))) {
              b.setInsertionPoint(conn);
              conn->setOperand(1, b.create<ConstantOp>(conn.getLoc(), UIntType::get(&ctx, 1), APInt(1, 1)).getResult());
              break;
            }
        }
        auto before = dump(*test);
        require(failed(goldengate::mapPrintBridgeCPUStreamReadBuffer(tc, hs, error)) &&
            !error.empty() && dump(*test) == before,
            "selected Print CPU AR rejection mutated IR " + std::to_string(bad));
      }
      // AR targets retain the inner identity; other interfaces move outward.
      // Check nested arrays/dictionaries and an aggregate control component.
      auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> as(raw.begin(), raw.end());
      SmallVector<Attribute> targets;
      for (auto n : {"cpu_stream_ar_valid", "cpu_stream_ar_bits_addr", "hostClock", "ctrl.aw.bits.addr", "cpu_stream_aw_valid", "cpu_stream_r_ready"})
        targets.push_back(b.getStringAttr("~GGSelectedPrintCPUStreamWriteBufferWrapper|GGSelectedPrintCPUStreamWriteBufferWrapper>" + std::string(n)));
      as.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.SelectedCPUReadTargets")),
          b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
      c->setAttr("rawAnnotations", b.getArrayAttr(as));
      SmallVector<FModuleOp> modules; SmallVector<Operation *> ops; std::map<std::string, std::string> bodies;
      for (auto m : c.getOps<FModuleOp>()) {
        modules.push_back(m); bodies[m.getName().str()] = moduleBody(m);
        for (auto &op : m.getBodyBlock()->getOperations()) ops.push_back(&op);
      }
      std::string headerBefore, headerAfter;
      require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(c, hostsFor(c), headerBefore, error)), error);
      require(succeeded(goldengate::mapPrintBridgeCPUStreamReadBuffer(c, hostsFor(c), error)) &&
          succeeded(verify(*source)), error);
      for (auto m : modules) require(named(c, m.getName()) == m && moduleBody(m) == bodies.at(m.getName().str()),
          "selected Print CPU AR changed existing module");
      SmallVector<Operation *> after;
      for (auto m : modules) for (auto &op : m.getBodyBlock()->getOperations()) after.push_back(&op);
      require(after == ops, "selected Print CPU AR replaced existing operations");
      auto updated = c->getAttrOfType<ArrayAttr>("rawAnnotations");
      auto probe = cast<DictionaryAttr>(updated[updated.size()-1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
      const StringRef probes[]{"cpu_stream_ar_valid", "cpu_stream_ar_bits_addr", "hostClock", "ctrl.aw.bits.addr", "cpu_stream_aw_valid", "cpu_stream_r_ready"};
      for (auto [i, n] : llvm::enumerate(probes))
        require(cast<StringAttr>(probe[i]).getValue() ==
            "~GGSelectedPrintCPUStreamReadBufferWrapper|" +
            std::string(i < 2 ? "GGSelectedPrintCPUStreamWriteBufferWrapper" : "GGSelectedPrintCPUStreamReadBufferWrapper") + ">" + n.str(),
            "selected Print CPU AR lost recursive copied/consumed target");
      require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(c, hostsFor(c), headerAfter, error)) &&
          headerAfter == headerBefore, "selected Print CPU AR changed allocated driver collateral: " + error);
      auto before = dump(*source);
      require(failed(goldengate::mapPrintBridgeCPUStreamReadBuffer(c, hostsFor(c), error)) &&
          dump(*source) == before, "selected Print CPU AR repeat changed IR");
      llvm::outs() << "PASS selected Print independent AR queue and unchanged allocated headers, 13 atomic rejections, retained SSA/module identities and recursive targets\n";
      return 0;
    }
    if (argc == 3 && StringRef(argv[1]) == "--selected-print-cpu-write-buffer") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source) && succeeded(verify(*source)), "invalid selected Print master");
      auto c = *source->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); std::string error;
      auto hostsFor = [&](CircuitOp circuit) {
        SmallVector<FModuleOp> hosts;
        for (auto attr : named(circuit, "GGPrintBridgeHostWrapper")->getAttrOfType<ArrayAttr>("goldengate.printHostBindings"))
          hosts.push_back(named(circuit, cast<DictionaryAttr>(attr).getAs<StringAttr>("hostModule").getValue()));
        return hosts;
      };
      auto moduleBody = [](FModuleOp m) {
        std::string text; llvm::raw_string_ostream out(text);
        m->print(out, OpPrintingFlags().useLocalScope()); return text;
      };
      for (unsigned bad = 0; bad < 10; ++bad) {
        OwningOpRef<ModuleOp> test(cast<ModuleOp>(source->clone()));
        auto tc = *test->getOps<CircuitOp>().begin(); auto top = named(tc, "GGControlMasterWrapper");
        auto hs = hostsFor(tc);
        if (bad == 0) tc.setName("Wrong");
        else if (bad == 1) tc->removeAttr("rawAnnotations");
        else if (bad == 2) hs.clear();
        else if (bad < 8) {
          const StringRef collisions[]{"GGEmptyCPUStreamWrite", "GGSelectedPrintCPUStreamWriteWrapper",
              "GGCPUStreamAWQueue2", "GGCPUStreamWQueue2", "GGSelectedPrintCPUStreamWriteBufferWrapper"};
          b.setInsertionPointToEnd(tc.getBodyBlock());
          b.create<FModuleOp>(tc.getLoc(), b.getStringAttr(collisions[bad-3]),
              top.getConventionAttr(), ArrayRef<PortInfo>{});
        } else {
          SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
          names[port(top, "cpu_stream_ar_bits_addr")] = b.getStringAttr(bad == 8 ?
              "cpu_stream_aw_bits_addr" : "unsupported_from_cpu_stream");
          top.setPortNames(names);
        }
        auto before = dump(*test);
        require(failed(goldengate::mapPrintBridgeCPUStreamWriteBuffer(tc, hs, error)) &&
            !error.empty() && dump(*test) == before,
            "selected Print CPU AW/W rejection mutated IR " + std::to_string(bad));
      }
      // These nested targets exercise copied and consumed identities through
      // both newly attached wrappers, including an aggregate control component.
      auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> as(raw.begin(), raw.end());
      SmallVector<Attribute> targets;
      for (auto n : {"hostClock", "ctrl.aw.bits.addr", "cpu_stream_r_bits_data"})
        targets.push_back(b.getStringAttr("~GGControlMasterWrapper|GGControlMasterWrapper>" + std::string(n)));
      as.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.SelectedCPUWriteTargets")),
          b.getNamedAttr("nested", b.getDictionaryAttr({b.getNamedAttr("targets", b.getArrayAttr(targets))}))}));
      c->setAttr("rawAnnotations", b.getArrayAttr(as));
      SmallVector<FModuleOp> modules; SmallVector<Operation *> ops; std::map<std::string, std::string> bodies;
      for (auto m : c.getOps<FModuleOp>()) {
        modules.push_back(m); bodies[m.getName().str()] = moduleBody(m);
        for (auto &op : m.getBodyBlock()->getOperations()) ops.push_back(&op);
      }
      require(succeeded(goldengate::mapPrintBridgeCPUStreamWriteBuffer(c, hostsFor(c), error)) &&
          succeeded(verify(*source)), error);
      for (auto m : modules) require(named(c, m.getName()) == m && moduleBody(m) == bodies.at(m.getName().str()),
          "selected Print CPU AW/W changed existing module");
      SmallVector<Operation *> after;
      for (auto m : modules) for (auto &op : m.getBodyBlock()->getOperations()) after.push_back(&op);
      require(after == ops, "selected Print CPU AW/W replaced existing operations");
      auto updated = c->getAttrOfType<ArrayAttr>("rawAnnotations");
      auto probe = cast<DictionaryAttr>(updated[updated.size()-1]).getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
      const StringRef probes[]{"hostClock", "ctrl.aw.bits.addr", "cpu_stream_r_bits_data"};
      for (auto [i, n] : llvm::enumerate(probes))
        require(cast<StringAttr>(probe[i]).getValue() ==
            "~GGSelectedPrintCPUStreamWriteBufferWrapper|GGSelectedPrintCPUStreamWriteBufferWrapper>" + n.str(),
            "selected Print CPU AW/W lost recursive copied target");
      auto before = dump(*source);
      require(failed(goldengate::mapPrintBridgeCPUStreamWriteBuffer(c, hostsFor(c), error)) &&
          dump(*source) == before, "selected Print CPU AW/W repeat changed IR");
      llvm::outs() << "PASS selected Print zero-sink/write queues, 11 atomic rejections, retained SSA/module identities and recursive targets\n";
      return 0;
    }
    if (argc == 3 && StringRef(argv[1]) == "--print-tracerv-allocated-header") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source), "cannot parse joint Print/TracerV response boundary");
      auto c = *source->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); std::string error, header;
      require(succeeded(goldengate::bindControlMaster(c, "GGControlWriteTrackerWrapper", error)) &&
          succeeded(verify(*source)), error);
      auto hostsFor = [&](CircuitOp circuit) {
        SmallVector<FModuleOp> hosts;
        for (auto attr : named(circuit, "GGPrintBridgeHostWrapper")->getAttrOfType<ArrayAttr>("goldengate.printHostBindings"))
          hosts.push_back(named(circuit, cast<DictionaryAttr>(attr).getAs<StringAttr>("hostModule").getValue()));
        return hosts;
      };
      auto before = dump(*source);
      require(succeeded(goldengate::preparePrintBridgeAllocatedHeader(c, hostsFor(c), header, error)) &&
          dump(*source) == before, error);
      for (unsigned word = 0; word < 6; ++word)
        require(header.find(std::to_string(64 + 4 * word) + "ULL,") != std::string::npos,
            "joint Print address differs from live decoder");
      require(header.find("}, 0U, args, 1U).release()") != std::string::npos &&
          header.find("524288ULL, 100ULL, 6144U, 64U)") != std::string::npos,
          "joint Print stream index or count address differs");
      require(header.find("registry.add_widget(new tracerv_t(") != std::string::npos &&
          header.find(".initDone = 0") != std::string::npos &&
          header.find(".triggerSelector = 56") != std::string::npos &&
          header.find("},\n  0,\n  args,\n  0,\n  6144,\n  1,\n") != std::string::npos,
          "joint Trace constructor differs from live MMIO/widget/stream allocation");
      for (unsigned bad = 0; bad < 11; ++bad) {
        OwningOpRef<CircuitOp> copy(cast<CircuitOp>(c->clone())); auto test = *copy;
        if (bad < 3) {
          auto bank = named(test, "GGTracerVTriggerConfig");
          auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
          if (bad == 0) bank->removeAttr("goldengate.mmioRegisters");
          else {
            SmallVector<Attribute> rows(regs.begin(), regs.end());
            if (bad == 1) rows.pop_back();
            else { NamedAttrList row(cast<DictionaryAttr>(rows[0])); row.set("offset", b.getI64IntegerAttr(4)); rows[0] = row.getDictionary(&ctx); }
            bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
          }
        } else if (bad < 5) {
          auto transport = named(test, "GGCPUStreamRead");
          auto regs = transport->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
          SmallVector<Attribute> rows(regs.begin(), regs.end()); NamedAttrList row(cast<DictionaryAttr>(rows[0]));
          row.set(bad == 3 ? "name" : "port", b.getStringAttr("detached_trace")); rows[0] = row.getDictionary(&ctx);
          transport->setAttr("goldengate.sourceStreams", b.getArrayAttr(rows));
        } else if (bad < 8) {
          auto read = named(test, "GGControlReadDispatchWrapper");
          auto regs = read->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings");
          SmallVector<Attribute> rows(regs.begin(), regs.end()); NamedAttrList row(cast<DictionaryAttr>(rows[0]));
          if (bad == 5) rows.erase(rows.begin());
          else { row.set(bad == 6 ? "port" : "slave", bad == 6 ? Attribute(b.getStringAttr("print_0_ctrl")) : Attribute(b.getI32IntegerAttr(1))); rows[0] = row.getDictionary(&ctx); }
          read->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
          named(test, "GGControlWidgetWriteWrapper")->setAttr("goldengate.controlWriteBindings", b.getArrayAttr(rows));
        } else if (bad == 8) {
          // Allocation alone cannot detect changed executable trigger state.
          for (auto reg : named(test, "GGTracerVTriggerConfig").getOps<RegResetOp>())
            if (reg.getName() == "traceEnable") {
              b.setInsertionPoint(reg);
              auto zero = b.create<ConstantOp>(reg.getLoc(), UIntType::get(&ctx, 1), APInt(1, 0));
              reg->setOperand(2, zero.getResult());
              break;
            }
        } else if (bad == 9) {
          named(test, "GGTracerVTokenEngine")->removeAttr("goldengate.bridgeConstructor");
        } else {
          auto annotations = test->getAttrOfType<ArrayAttr>("rawAnnotations");
          SmallVector<Attribute> rows(annotations.begin(), annotations.end());
          bool changed = false;
          for (auto &attr : rows) {
            auto row = dyn_cast<DictionaryAttr>(attr);
            auto widget = row ? row.getAs<StringAttr>("widgetClass") : StringAttr{};
            if (!widget || widget.getValue() != "firechip.goldengateimplementations.TracerVBridgeModule") continue;
            NamedAttrList malformed(row); malformed.erase("clockInfo"); attr = malformed.getDictionary(&ctx); changed = true;
          }
          require(changed, "joint Trace clock annotation missing");
          test->setAttr("rawAnnotations", b.getArrayAttr(rows));
        }
        auto saved = dump(test); std::string rejected = "preserve";
        require(failed(goldengate::preparePrintBridgeAllocatedHeader(test, hostsFor(test), rejected, error)) &&
            !error.empty() && rejected == "preserve" && dump(test) == saved,
            "joint allocated header accepted or mutated corruption " + std::to_string(bad));
      }
      llvm::outs() << "PASS joint Print/TracerV constructors, live addresses/stream index and eleven atomic rejections\n";
      return 0;
    }
    if (argc == 3 && StringRef(argv[1]) == "--print-tracerv-control-responses") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source), "cannot parse selected Print/TracerV request boundary");
      auto original = *source->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
      for (unsigned bad = 0; bad < 12; ++bad) {
        OwningOpRef<CircuitOp> copy(cast<CircuitOp>(original->clone()));
        auto c = *copy; auto top = named(c, c.getName());
        auto bindings = top->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings");
        if (bad <= 2) {
          SmallVector<Attribute> rows(bindings.begin(), bindings.end());
          NamedAttrList row(cast<DictionaryAttr>(rows[0]));
          if (bad == 0) rows.erase(rows.begin());
          if (bad == 1) row.set("port", b.getStringAttr("print_0_ctrl"));
          if (bad == 2) row.set("slave", b.getI32IntegerAttr(1));
          if (bad != 0) rows[0] = row.getDictionary(&ctx);
          auto stale = b.getArrayAttr(rows);
          top->setAttr("goldengate.controlReadBindings", stale);
          named(c, "GGControlWidgetWriteWrapper")->setAttr("goldengate.controlWriteBindings", stale);
        }
        if (bad == 3 || bad == 4) {
          auto decoder = named(c, "GGControlAddressDecode");
          auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
          SmallVector<Attribute> rows(regions.begin(), regions.end());
          NamedAttrList row(cast<DictionaryAttr>(rows[0]));
          row.set(bad == 3 ? "start" : "size", b.getI64IntegerAttr(bad == 3 ? 4 : 32));
          rows[0] = row.getDictionary(&ctx); decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(rows));
        }
        if (bad >= 5 && bad <= 7) {
          ConnectOp trace, print;
          for (auto op : top.getOps<ConnectOp>()) {
            auto field = op.getDest().getDefiningOp<SubfieldOp>();
            auto base = field ? field.getInput().getDefiningOp<InstanceOp>() : InstanceOp{};
            if (!base || base.getName() != "sim" || field.getFieldName() != "ar") continue;
            auto port = base.getPortNameStr(cast<OpResult>(field.getInput()).getResultNumber());
            if (port == "tracerv_ctrl") trace = op;
            if (port == "print_0_ctrl") print = op;
          }
          require(trace && print, "selected AR bank connections absent");
          if (bad == 5) trace->setOperand(1, print.getSrc());
          if (bad == 6) trace.erase();
          if (bad == 7) { b.setInsertionPointAfter(trace); b.clone(*trace); }
        }
        if (bad == 8) named(c, "GGTracerVTriggerConfig")->removeAttr("goldengate.mmioRegisters");
        if (bad == 9) c->removeAttr("rawAnnotations");
        if (bad == 10) {
          b.setInsertionPointToEnd(c.getBodyBlock());
          b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGControlWriteTracker"),
              ConventionAttr::get(&ctx, Convention::Internal), SmallVector<PortInfo>{});
        }
        if (bad == 11) {
          auto bound = named(c, "GGPrintBridgeHostWrapper");
          for (auto i : bound.getOps<InstanceOp>()) if (i.getName() == "PrintBridgeModule_0") {
            i.setModuleNameAttr(FlatSymbolRefAttr::get(&ctx, "MissingPrintHost")); break;
          }
        }
        auto before = dump(c); std::string error;
        require(failed(goldengate::mapPrintBridgeControlResponses(c, error)) && !error.empty() && dump(c) == before,
            "joint response mapping accepted/mutated malformed boundary " + std::to_string(bad));
      }
      auto retained = named(original, "GGTracerVBridgeControlWrapper"); auto before = dump(retained);
      std::string error;
      require(succeeded(goldengate::mapPrintBridgeControlResponses(original, error)) && succeeded(verify(*source)), error);
      require(original.getName() == "GGControlWriteTrackerWrapper" && dump(retained) == before,
          "joint responses changed existing TracerV transport");
      auto arbiter = named(original, "GGControlReadArbiterWrapper");
      require(arbiter->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings").size() == 3,
          "joint responses lost a bank");
      for (auto port : named(original, original.getName()).getPorts())
        require(port.name != "tracerv_ctrl" && port.name != "print_0_ctrl" && port.name != "cpuStream_ctrl",
            "selected B/R response bank remained exposed");
      llvm::outs() << "PASS joint Print/TracerV/count B/R composition and twelve atomic rejections including stale AR wiring\n";
      return 0;
    }
    if (argc == 3 && StringRef(argv[1]) == "--print-tracerv-control-dispatch") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source), "cannot parse allocated Print/TracerV control boundary");
      auto original = *source->getOps<CircuitOp>().begin();
      // Reconstruct the decoded trigger boundary while retaining the imported
      // adapter. Reuse must preserve that existing operation, and a modified
      // catalog implementation must reject without replacing it.
      for (bool altered : {false, true}) {
        OwningOpRef<CircuitOp> copy(cast<CircuitOp>(original->clone()));
        auto c = *copy;
        for (StringRef name : {"GGCPUStreamCountWrapper", "GGCPUStreamReadWrapper",
              "GGPrintTracerVStreamQueueWrapper", "GGTracerVBridgeControlWrapper"}) named(c, name).erase();
        c.setName("GGTracerVTriggerWrapper");
        c->setAttr("rawAnnotations", ArrayAttr::get(&ctx, {}));
        auto adapter = named(c, "GGTracerVMCRFile");
        if (altered) adapter->setAttr("test.alteredTransport", UnitAttr::get(&ctx));
        auto before = dump(c), adapterBefore = dump(adapter); std::string error;
        auto result = goldengate::mapPrintBridgeTracerVControl(c, error);
        if (altered) require(failed(result) && !error.empty() && dump(c) == before,
            "modified imported TracerV adapter was accepted or mutated");
        else require(succeeded(result) && succeeded(verify(c)) && dump(adapter) == adapterBefore &&
            named(c, "GGTracerVMCRFile") == adapter, "catalog adapter reuse changed existing hardware: " + error);
      }
      for (unsigned bad = 0; bad < 9; ++bad) {
        OwningOpRef<CircuitOp> copy(cast<CircuitOp>(original->clone()));
        auto c = *copy; auto top = named(c, c.getName());
        auto transport = named(c, "GGTracerVBridgeControlWrapper");
        auto bank = named(c, "GGTracerVTriggerConfig");
        if (bad == 0) bank->removeAttr("goldengate.mmioRegisters");
        if (bad == 1) c->removeAttr("rawAnnotations");
        if (bad == 2) c.setName("GGPrintBridgeHostWrapper");
        if (bad == 3) {
          for (auto i : transport.getOps<InstanceOp>()) if (i.getName() == "crFile") {
            i.setModuleNameAttr(FlatSymbolRefAttr::get(&ctx, "MissingTracerVAdapter")); break;
          }
        }
        if (bad == 8) named(c, "GGCPUStreamRead")->removeAttr("goldengate.sourceStreams");
        if (bad >= 4 && bad <= 7) {
          auto wrapper = bad == 4 ? top : transport;
          StringRef needle = bad == 4 ? "tracerv_ctrl" : bad == 5 ? "tracerv_mcr" : bad == 6 ? "hostClock" : "hostReset";
          bool removed = false;
          for (auto &op : llvm::make_early_inc_range(*wrapper.getBodyBlock())) if (isa<ConnectOp>(op)) {
            auto text = dump(&op);
            if (StringRef(text).contains(needle)) { op.erase(); removed = true; break; }
          }
          require(removed, "missing rejection connection " + needle.str());
        }
        auto before = dump(c); std::string error;
        require(failed(goldengate::mapPrintBridgeControlDispatch(c, error)) && !error.empty() && dump(c) == before,
            "TracerV control dispatch accepted/mutated malformed boundary " + std::to_string(bad));
      }
      std::string error;
      require(succeeded(goldengate::mapPrintBridgeControlDispatch(original, error)) && succeeded(verify(*source)), error);
      auto decode = named(original, "GGControlAddressDecode");
      auto regions = decode->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
      require(regions && regions.size() == 3, "joint request allocation must have three banks");
      require(cast<DictionaryAttr>(regions[0]).getAs<StringAttr>("name") == "TracerVBridgeModule_0",
          "TracerV registration must precede Print in the selected allocation");
      llvm::outs() << "PASS live Print/TracerV/count request dispatch, catalog reuse and ten atomic rejections\n";
      return 0;
    }
    bridgeClockImport(ctx);
    controlCatalogImport(ctx);
    if (argc == 4 && StringRef(argv[1]) == "--print-tracerv-binding") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source), "cannot parse bound Print TracerV tokens");
      auto c = *source->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
      auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
      unsigned bridgeIndex = raw.size(), mapIndex = raw.size(), channelIndex = raw.size();
      for (auto [i, attr] : llvm::enumerate(raw)) {
        auto row = cast<DictionaryAttr>(attr);
        if (auto widget = row.getAs<StringAttr>("widgetClass"); widget && widget == "firechip.goldengateimplementations.TracerVBridgeModule") bridgeIndex = i;
        if (row.getAs<StringAttr>("class") == goldengate::AnnotationClasses::ChannelClockInfo) mapIndex = i;
        if (auto name = row.getAs<StringAttr>("globalName"); name && name == "tracerv_triggerCredit") channelIndex = i;
      }
      require(bridgeIndex < raw.size() && mapIndex < raw.size() && channelIndex < raw.size(), "missing analyzed TracerV boundary");
      auto bridge = cast<DictionaryAttr>(raw[bridgeIndex]);
      auto clock = bridge.getAs<DictionaryAttr>("clockInfo");
      auto map = cast<DictionaryAttr>(raw[mapIndex]);
      auto info = map.getAs<DictionaryAttr>("infoMap");
      std::string error;
      auto reject = [&](SmallVector<Attribute> attrs) {
        c->setAttr("rawAnnotations", b.getArrayAttr(attrs));
        auto before = dump(*source);
        require(failed(goldengate::bindPrintBridgeTracerV(c, error)) && !error.empty() && dump(*source) == before,
            "invalid Print TracerV boundary mutated IR");
        c->setAttr("rawAnnotations", raw);
      };
      for (StringRef field : {"name", "multiplier", "divisor"}) {
        NamedAttrList bad(clock); bad.erase(field); NamedAttrList row(bridge);
        row.set("clockInfo", bad.getDictionary(&ctx)); SmallVector<Attribute> attrs(raw.begin(), raw.end());
        attrs[bridgeIndex] = row.getDictionary(&ctx); reject(attrs);
      }
      for (auto channel : bridge.getAs<DictionaryAttr>("channelMapping")) {
        NamedAttrList missing(info); missing.erase(cast<StringAttr>(channel.getValue()).getValue());
        NamedAttrList row(map); row.set("infoMap", missing.getDictionary(&ctx));
        SmallVector<Attribute> attrs(raw.begin(), raw.end()); attrs[mapIndex] = row.getDictionary(&ctx); reject(attrs);
      }
      {
        SmallVector<Attribute> attrs(raw.begin(), raw.end()); attrs.push_back(map); reject(attrs);
        attrs.assign(raw.begin(), raw.end()); attrs.push_back(bridge); reject(attrs);
        attrs.assign(raw.begin(), raw.end()); attrs.erase(attrs.begin() + mapIndex); reject(attrs);
        attrs.assign(raw.begin(), raw.end()); attrs.push_back(raw[channelIndex]); reject(attrs);
        NamedAttrList row(cast<DictionaryAttr>(raw[channelIndex]));
        row.set("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
                                                  b.getNamedAttr("latency", b.getI64IntegerAttr(0))}));
        attrs.assign(raw.begin(), raw.end()); attrs[channelIndex] = row.getDictionary(&ctx); reject(attrs);
      }
      auto original = named(c, "GGPrintBridgeHostWrapper"); auto originalBody = dump(original);
      require(succeeded(goldengate::bindPrintBridgeTracerV(c, error)) && succeeded(verify(*source)), error);
      auto newRaw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
      require(newRaw.size() == raw.size() && newRaw[mapIndex] == raw[mapIndex] && dump(original) == originalBody,
          "TracerV binding changed Print hardware or analyzed clocks");
      auto resolved = goldengate::resolveBridgeClockInfo(c, cast<DictionaryAttr>(newRaw[bridgeIndex]), error);
      require(resolved && resolved->module.getModuleName() == "GGTracerVTokenEngine" && resolved->port == 2, error);
      auto before = dump(*source);
      require(failed(goldengate::bindPrintBridgeTracerV(c, error)) && dump(*source) == before, "repeat TracerV binding mutated IR");
      std::error_code ec; llvm::raw_fd_ostream out(argv[3], ec); require(!ec, ec.message());
      source->print(out); out << '\n';
      llvm::outs() << "Print TracerV live clock target, unchanged Print hardware, twenty atomic rejects and repeat rejection passed\n";
      return 0;
    }
    if (argc == 5 && StringRef(argv[1]) == "--rocket-catalog-allocation") {
      rocketStreams(ctx, argv[2], argv[3], argv[4], true);
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]) == "--rocket-control-catalog") {
      auto source = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(source), "cannot parse native Rocket control catalog");
      Fixture f(ctx); std::string error;
      auto c = *source->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit, f.hosts, error)), error);
      auto fragment = named(c, "GGLoadMemReadDataWrapper");
      auto rows = fragment->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
      auto before = dump(*f.root);
      for (StringRef field : {"name", "offset", "readable"}) {
        NamedAttrList bad(cast<DictionaryAttr>(rows[0])); bad.erase(field);
        fragment->setAttr("goldengate.mmioRegisters", ArrayAttr::get(&ctx, {bad.getDictionary(&ctx)}));
        auto sourceBefore = dump(*source);
        require(failed(goldengate::importRocketControlCatalog(f.circuit, c, error)) &&
            !error.empty() && dump(*f.root) == before && dump(*source) == sourceBefore,
            "invalid LoadMem descriptor changed either catalog circuit");
      }
      fragment->setAttr("goldengate.mmioRegisters", rows);
      require(succeeded(goldengate::importRocketControlCatalog(f.circuit, c, error)) &&
          succeeded(verify(*f.root)), error);
      goldengate::ControlMMIOWidget loadmem;
      require(succeeded(goldengate::deriveControlMMIOWidget(f.circuit, "LoadMemWidget_0", "GGLoadMemMCRFile",
          {"GGLoadMemWriteMMIOBank", "CatalogRocket_GGLoadMemWriteDataFIFO",
           "CatalogRocket_GGLoadMemReadRequests", "CatalogRocket_GGLoadMemReadDataFIFO"}, loadmem, error)) &&
          loadmem.registerCount == 9, "imported direct engine descriptors lost the nine-word LoadMem ABI");
      std::error_code ec; llvm::raw_fd_ostream out(argv[3], ec);
      require(!ec, "cannot write imported catalog"); f.root->print(out); out << '\n';
      llvm::outs() << "PASS native Rocket register catalog imported into live Print host boundary, three atomic LoadMem descriptor rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]) == "--rocket-native-driver-headers") {
      auto root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(root), "cannot parse native Rocket F1Shim boundary");
      auto c = *root->getOps<CircuitOp>().begin(); std::string error;
      rocketNativeDriverHeaders(c, *root, argv[3]);
      return 0;
    }
    if (argc == 5 && StringRef(argv[1]) == "--rocket-driver-headers") {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded Rocket F1Shim boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); rocketDriverHeaders(f, argv[3], argv[4]); return 0;
    }
    if (argc == 4 && StringRef(argv[1]) == "--rocket-allocated-header") {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded Rocket F1Shim boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); rocketAllocatedHeader(f, argv[3]); return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-f1-shim-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded Print FPGATop boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketF1Shim(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded Print F1Shim boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-fpga-top-shell-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded CPU platform boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFPGATopShell(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded Print FPGATop boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-cpu-stream-platform-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded CPU B buffer boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketCPUStreamPort(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded CPU platform boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-cpu-stream-write-response-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded CPU R buffer boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketCPUStreamWriteResponseBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded CPU B buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-cpu-stream-response-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded CPU AR buffer boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketCPUStreamResponseBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded CPU R buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-cpu-stream-read-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded CPU AW/W buffer boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketCPUStreamReadBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded CPU AR buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-cpu-stream-write-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded CPU write boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketCPUStreamWriteBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded CPU AW/W buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-empty-cpu-stream-write-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host memory platform boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketEmptyCPUStreamWrite(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded empty CPU stream write boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-host-memory-port-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host output buffer boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketHostMemoryPort(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host memory platform port boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-host-memory-output-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host R-response boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketHostMemoryOutputBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host output buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-host-memory-read-responses-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host AW/W/B/AR boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketHostMemoryReadResponses(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host R response boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-host-memory-read-arbiter-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host AW/W/B boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketHostMemoryReadArbiter(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host AR arbiter boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-host-memory-write-responses-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host AW/W boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketHostMemoryWriteResponses(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host B response boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-host-memory-write-arbiter-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded LoadMem boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketHostMemoryWriteArbiter(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host write arbiter boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-loadmem-engine-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded buffered boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketLoadMemEngine(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded LoadMem engine boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-fased-host-memory-buffer-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded deinterleaver boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDHostMemoryBuffer(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host memory buffer boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-fased-read-deinterleaver-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded translation boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDReadDeinterleaver(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded read deinterleaver boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-fased-address-translation-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded host memory boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDAddressTranslation(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded address translation boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-fased-host-memory-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded control master boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDHostMemory(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded host memory boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--rocket-control-master-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED control binding boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketControlMaster(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded control master boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-control-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED MMIO boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDControl(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED control boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-mmio-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED histogram boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDMMIOBank(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED MMIO boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-histograms-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED statistics boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDHistograms(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED histogram boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-statistics-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED response-error boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDStatistics(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED statistics boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-response-errors-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED functional-model register boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDResponseErrors(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED response-error boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-functional-model-register-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED latency-register boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDFunctionalModelRegister(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED functional-model register boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-latency-registers-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED request-limit boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDLatencyRegisters(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED latency-register boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-request-limits-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED read-admission boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin(); unsigned rejected = 0;
      rocketFASEDRequestLimits(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED request-limit boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay expanded write admission to isolate final-R retirement and AR admission.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-read-admission-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED write-admission boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDReadAdmission(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED read admission boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay the expanded retirement boundary to isolate pending-full admission.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-write-admission-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED retirement boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDWriteAdmission(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED write admission boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay expanded pairing boundaries to isolate target B retirement.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-write-retirement-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED write-pairing boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDWriteRetirement(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED write retirement boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay expanded AW ordering boundaries to isolate AW/W completion pairing.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-write-pairing-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED timing AW queue boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDWritePairing(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED write pairing boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay expanded write-latency boundaries to isolate accepted AW ordering.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-timing-aw-queue-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED write-latency boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDTimingAWQueue(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED timing AW queue boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
    // Replay both expanded read-latency boundaries to isolate write deadlines.
    if (argc == 4 && StringRef(argv[1]).starts_with("--fased-write-latency-boundary")) {
      Fixture f(ctx); f.root = parseSourceFile<ModuleOp>(argv[2], &ctx);
      require(bool(f.root), "cannot parse expanded FASED read-latency boundary");
      f.circuit = *f.root->getOps<CircuitOp>().begin();
      unsigned rejected = 0;
      rocketFASEDWriteLatency(f, argv[3], StringRef(argv[1]).ends_with("-reverse"), rejected);
      llvm::outs() << "PASS expanded FASED write latency boundary and " << rejected << " atomic rejections\n";
      return 0;
    }
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
    checkBinding(ctx, false, argc > 1 ? argv[1] : ""); checkBinding(ctx, true, ""); rejections(ctx); nestedCPUAllocation(ctx); allocationRejections(ctx); controlRejections(ctx); responseRejections(ctx);
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
