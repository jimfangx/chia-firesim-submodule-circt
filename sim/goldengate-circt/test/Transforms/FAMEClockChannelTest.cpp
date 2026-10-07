// See LICENSE for license details.
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/SingleClockBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

OwningOpRef<ModuleOp> fixture(MLIRContext &context, bool record, unsigned bad = 0) {
  std::string payload = bad == 4 ? "uint<1>" : record
      ? "bundle<_0: clock, _1: clock, _2: clock>" : "clock";
  std::string dataPorts;
  for (unsigned i = 0; i < (record ? 3u : 1u); ++i)
    dataPorts += ", in %data" + std::to_string(i) +
                 ": !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>";
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top(in %ticks: "
      "!firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: " + payload +
      ">, in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>" + dataPorts + ") {} } }", &context);
  auto c = *root->getOps<CircuitOp>().begin();
  auto top = *c.getOps<FModuleOp>().begin();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto wrapper = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGFAMEPipeWrapper"),
                                   top.getConventionAttr(), top.getPorts());
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child = b.create<InstanceOp>(c.getLoc(), top, "target_FAMETop");
  for (unsigned i = 0; i < top.getNumPorts(); ++i)
    b.create<ConnectOp>(c.getLoc(), child.getResult(i), wrapper.getBodyBlock()->getArgument(i));
  if (bad == 7)
    b.create<SubfieldOp>(c.getLoc(), wrapper.getBodyBlock()->getArgument(0), "valid");
  SmallVector<Attribute> sinks, clockInfo, ratios;
  SmallVector<unsigned> order = record ? SmallVector<unsigned>{0, 1, 2} : SmallVector<unsigned>{0};
  if (bad == 8) order = {2, 0, 1};
  for (unsigned i : order) {
    sinks.push_back(b.getStringAttr("~Top|Top>ticks.bits" +
                                  (record ? "._" + std::to_string(i) : "")));
    clockInfo.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr("clock" + std::to_string(i))),
      b.getNamedAttr("multiplier", b.getI64IntegerAttr(1)),
      b.getNamedAttr("divisor", b.getI64IntegerAttr(i + 1))}));
    ratios.push_back(b.getI64IntegerAttr(i + 1));
  }
  if (bad == 2) sinks[1] = sinks.front();
  if (bad == 3) sinks[0] = b.getStringAttr("~Top|Top>ticks.valid");
  if (bad == 6) ratios.clear();
  auto info = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::TargetClockChannel)),
    b.getNamedAttr("clockInfo", b.getArrayAttr(clockInfo)),
    b.getNamedAttr("perClockMFMR", b.getArrayAttr(ratios))});
  NamedAttrList attrs;
  attrs.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection));
  attrs.set("globalName", b.getStringAttr("clockBridge_clocks"));
  attrs.set("channelInfo", info);
  attrs.set("sinks", b.getArrayAttr(sinks));
  if (bad == 5) attrs.set("sources", b.getArrayAttr(sinks));
  auto annotation = attrs.getDictionary(&context);
  SmallVector<Attribute> annotations{annotation};
  // Associated clocks identify target domains, not the simulator's Boolean
  // token interface. Exercise every lane, including a scalar Clock payload.
  for (unsigned i : order) {
    NamedAttrList data;
    data.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection));
    data.set("globalName", b.getStringAttr("data" + std::to_string(i)));
    data.set("channelInfo", b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
      b.getNamedAttr("latency", b.getI64IntegerAttr(0))}));
    data.set("clock", b.getStringAttr("~Top|Top>ticks.bits" +
                                     (record ? "._" + std::to_string(i) : "")));
    data.set("sinks", b.getArrayAttr({b.getStringAttr("~Top|Top>data" + std::to_string(i))}));
    annotations.push_back(data.getDictionary(&context));
  }
  annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("target", b.getStringAttr("~Top|Top>clockBridge")),
    b.getNamedAttr("widgetClass", b.getStringAttr("midas.widgets.ClockBridgeModule")),
    b.getNamedAttr("widgetConstructorKey", b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridges.ClockParameters")),
      b.getNamedAttr("clocks", b.getArrayAttr(clockInfo))})),
    b.getNamedAttr("channelMapping", b.getDictionaryAttr({
      b.getNamedAttr("clocks", b.getStringAttr("clockBridge_clocks"))}))}));
  c->setAttr("rawAnnotations", bad == 1 ? b.getArrayAttr({}) : b.getArrayAttr(annotations));
  return root;
}

std::string path(Value value) {
  if (auto field = value.getDefiningOp<SubfieldOp>())
    return path(field.getInput()) + "." + field.getFieldName().str();
  if (auto index = value.getDefiningOp<SubindexOp>())
    return path(index.getInput()) + "[" + std::to_string(index.getIndex()) + "]";
  if (isa<BlockArgument>(value)) return "external";
  if (value.getDefiningOp<InstanceOp>()) return "target";
  throw std::runtime_error("unexpected clock connection path");
}
unsigned evaluate(Value value, const std::map<std::string, unsigned> &inputs) {
  if (auto cast = value.getDefiningOp<AsClockPrimOp>())
    return evaluate(cast.getInput(), inputs);
  return inputs.at(path(value));
}
void behavior(MLIRContext &context, bool record) {
  auto root = fixture(context, record);
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto before = circuit->getAttrOfType<ArrayAttr>("rawAnnotations")[0];
  std::string error;
  require(succeeded(goldengate::addFAMEClockChannel(circuit, error)), error.c_str());
  require(succeeded(verify(*root)), "clock conversion failed verification");
  FModuleOp wrapper, target;
  for (auto module : circuit.getOps<FModuleOp>())
    (module.getName() == "Top" ? target : wrapper) = module;
  auto targetType = cast<BundleType>(target.getPortType(0));
  auto vector = dyn_cast<FVectorType>(cast<BundleType>(wrapper.getPortType(0)).getElement("bits")->type);
  require(vector && vector.getNumElements() == (record ? 3 : 1) &&
          vector.getElementType() == UIntType::get(&context, 1, false), "wrong Boolean vector port");
  require(record ? isa<BundleType>(targetType.getElement("bits")->type)
                 : isa<ClockType>(targetType.getElement("bits")->type), "target payload type changed");
  unsigned casts = 0, connects = 0;
  wrapper.walk([&](AsClockPrimOp) { ++casts; });
  for (auto connect : wrapper.getOps<ConnectOp>()) ++connects;
  require(casts == (record ? 3 : 1) && connects == 2 * casts + 4, "wrong clock/handshake connection count");
  for (unsigned bits = 0; bits < (record ? 8u : 2u); ++bits)
    for (unsigned valid = 0; valid < 2; ++valid)
      for (unsigned ready = 0; ready < 2; ++ready) {
        std::map<std::string, unsigned> inputs{{"external.valid", valid}, {"target.ready", ready}};
        for (unsigned i = 0; i < casts; ++i)
          inputs["external.bits[" + std::to_string(i) + "]"] = (bits >> i) & 1;
        std::map<std::string, unsigned> outputs;
        for (auto connect : wrapper.getOps<ConnectOp>()) {
          if (auto arg = dyn_cast<BlockArgument>(connect.getSrc()); arg && arg.getArgNumber() > 0)
            continue; // Host-clock/reset passthrough is outside the token truth table.
          outputs[path(connect.getDest())] = evaluate(connect.getSrc(), inputs);
        }
        require(outputs.at("external.ready") == ready && outputs.at("target.valid") == valid,
                "clock handshake changed");
        SmallVector<unsigned> order = record ? SmallVector<unsigned>{0, 1, 2} : SmallVector<unsigned>{0};
        for (unsigned i = 0; i < casts; ++i)
          require(outputs.at("target.bits" + (record ? "._" + std::to_string(order[i]) : "")) == ((bits >> i) & 1),
                  "clock bit reordered or gated by host valid");
      }
  require(succeeded(goldengate::activateFAMEPipeWrapper(circuit, error)), error.c_str());
  require(succeeded(verify(*root)), "active clock wrapper failed verification");
  auto after = cast<DictionaryAttr>(circuit->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
  require(after.get("channelInfo") == cast<DictionaryAttr>(before).get("channelInfo"), "clock metadata changed");
  for (auto [i, attr] : llvm::enumerate(after.getAs<ArrayAttr>("sinks"))) {
    std::string spelling = "~GGFAMEPipeWrapper|GGFAMEPipeWrapper>ticks.bits[" + std::to_string(i) + "]";
    require(cast<StringAttr>(attr).getValue() == spelling, "clock boundary annotation not transferred");
    auto resolved = goldengate::resolveAnnotationTarget(circuit, spelling, error);
    require(resolved && resolved->module == wrapper && resolved->fieldID, "Boolean token target does not resolve");
  }
  auto annotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  for (unsigned i = 0; i < casts; ++i) {
    auto data = cast<DictionaryAttr>(annotations[i + 1]);
    auto clock = data.getAs<StringAttr>("clock");
    std::string spelling = "~GGFAMEPipeWrapper|Top>ticks.bits" +
                           (record ? "._" + std::to_string(i) : "");
    require(clock && clock.getValue() == spelling, "associated target clock moved to Boolean interface");
    auto resolved = goldengate::resolveAnnotationTarget(circuit, spelling, error);
    uint64_t bitsID = targetType.getFieldID(*targetType.getElementIndex("bits"));
    uint64_t leafID = record ? bitsID + cast<BundleType>(targetType.getElement("bits")->type).getFieldID(i)
                             : bitsID;
    require(resolved && resolved->module == target && resolved->fieldID == leafID,
            "associated target clock identity does not resolve to original leaf");
    auto sink = cast<StringAttr>(data.getAs<ArrayAttr>("sinks")[0]);
    auto endpoint = goldengate::resolveAnnotationTarget(circuit, sink.getValue(), error);
    require(endpoint && endpoint->module == wrapper && endpoint->port == i + 3,
            "data endpoint did not move to simulator wrapper");
  }
  require(succeeded(goldengate::addClockBridge(circuit, error)), error.c_str());
  require(succeeded(verify(*root)), "producer insertion failed verification");
  for (unsigned i = 0; i < casts; ++i) {
    auto data = cast<DictionaryAttr>(circuit->getAttrOfType<ArrayAttr>("rawAnnotations")[i + 1]);
    auto clock = data.getAs<StringAttr>("clock");
    std::string spelling = "~GGClockBridgeWrapper|Top>ticks.bits" +
                           (record ? "._" + std::to_string(i) : "");
    require(clock && clock.getValue() == spelling, "producer insertion lost inner clock identity");
    auto resolved = goldengate::resolveAnnotationTarget(circuit, spelling, error);
    require(resolved && resolved->module == target && resolved->fieldID,
            "producer insertion left an unresolved target clock");
    auto sink = cast<StringAttr>(data.getAs<ArrayAttr>("sinks")[0]);
    auto endpoint = goldengate::resolveAnnotationTarget(circuit, sink.getValue(), error);
    require(endpoint && endpoint->module.getModuleName() == circuit.getName(),
            "producer insertion did not transfer data boundary endpoint");
  }
}
void mapHandoff(MLIRContext &context, const char *input, const char *output) {
  auto root = parseSourceFile<ModuleOp>(input, &context);
  require(bool(root), "cannot parse captured clock-wrapper handoff");
  auto circuit = *root->getOps<CircuitOp>().begin();
  std::string oldName = circuit.getName().str(), error;
  auto before = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (failed(goldengate::activateFAMEPipeWrapper(circuit, error)))
    throw std::runtime_error(error);
  if (failed(goldengate::addClockBridge(circuit, error)))
    throw std::runtime_error(error);
  require(succeeded(verify(*root)), "captured clock-wrapper mapping failed verification");
  auto after = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  unsigned clocks = 0, endpoints = 0;
  require(before.size() == after.size(), "mapping changed annotation count");
  for (auto [i, attr] : llvm::enumerate(before)) {
    auto original = cast<DictionaryAttr>(attr), candidate = cast<DictionaryAttr>(after[i]);
    require(original.get("class") == candidate.get("class"), "mapping changed annotation class");
    if (original.getAs<StringAttr>("class").getValue() != goldengate::AnnotationClasses::ChannelConnection)
      continue;
    auto info = original.getAs<DictionaryAttr>("channelInfo");
    auto mappedInfo = candidate.getAs<DictionaryAttr>("channelInfo");
    require(info && mappedInfo && info.get("class") == mappedInfo.get("class"),
            "mapping changed channel type");
    if (info.getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::TargetClockChannel)
      require(info == mappedInfo, "mapping changed ordered clock metadata");
    if (auto clock = original.getAs<StringAttr>("clock")) {
      std::string expected = "~GGClockBridgeWrapper" + clock.getValue().drop_front(oldName.size() + 1).str();
      auto mapped = candidate.getAs<StringAttr>("clock");
      require(mapped && mapped.getValue() == expected, "captured target domain moved to simulator boundary");
      auto resolved = goldengate::resolveAnnotationTarget(circuit, expected, error);
      require(resolved && resolved->module.getModuleName() == oldName && resolved->port &&
              resolved->fieldID == 0 && isa<ClockType>(resolved->module.getPorts()[*resolved->port].type),
              "captured target domain no longer resolves to its Clock port");
      ++clocks;
    }
    for (auto member : {"sources", "sinks"})
      if (auto values = candidate.getAs<ArrayAttr>(member))
        for (auto value : values) {
          require(bool(goldengate::resolveAnnotationTarget(circuit, cast<StringAttr>(value).getValue(), error)),
                  "captured channel boundary does not resolve");
          ++endpoints;
        }
  }
  std::error_code ec;
  llvm::raw_fd_ostream stream(output, ec);
  require(!ec, "cannot write candidate handoff");
  root->print(stream);
  llvm::outs() << "ClockChannel handoff: " << before.size() << " retained annotations, "
               << clocks << " target clocks, " << endpoints << " channel endpoints verified\n";
}
int main(int argc, char **argv) {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    if (argc == 3) { mapHandoff(context, argv[1], argv[2]); return 0; }
    behavior(context, false); behavior(context, true);
    for (unsigned bad = 1; bad <= 8; ++bad) {
      auto root = fixture(context, bad == 2 || bad == 8, bad);
      auto c = *root->getOps<CircuitOp>().begin();
      std::string original, changed;
      llvm::raw_string_ostream before(original); root->print(before);
      std::string error;
      require(failed(goldengate::addFAMEClockChannel(c, error)) && !error.empty(), "malformed clock accepted");
      llvm::raw_string_ostream after(changed); root->print(after);
      require(original == changed, "failed clock conversion mutated IR");
    }
  } catch (const std::exception &e) {
    llvm::errs() << "ClockChannel: " << e.what() << '\n'; return 1;
  }
  llvm::outs() << "ClockChannel: scalar/vector token truth tables, inner clock identities through producer insertion and eight rejection cases passed\n";
}
