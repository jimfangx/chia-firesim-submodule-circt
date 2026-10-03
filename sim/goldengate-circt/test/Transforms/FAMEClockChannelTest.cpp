// See LICENSE for license details.
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/FAMEPipeChannel.h"
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
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top(in %ticks: "
      "!firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: " + payload +
      ">) {} } }", &context);
  auto c = *root->getOps<CircuitOp>().begin();
  auto top = *c.getOps<FModuleOp>().begin();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto wrapper = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGFAMEPipeWrapper"),
                                   top.getConventionAttr(), top.getPorts());
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child = b.create<InstanceOp>(c.getLoc(), top, "target_FAMETop");
  b.create<ConnectOp>(c.getLoc(), child.getResult(0), wrapper.getBodyBlock()->getArgument(0));
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
  c->setAttr("rawAnnotations", bad == 1 ? b.getArrayAttr({}) : b.getArrayAttr({annotation}));
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
  require(casts == (record ? 3 : 1) && connects == casts + 2, "wrong clock/handshake connection count");
  for (unsigned bits = 0; bits < (record ? 8u : 2u); ++bits)
    for (unsigned valid = 0; valid < 2; ++valid)
      for (unsigned ready = 0; ready < 2; ++ready) {
        std::map<std::string, unsigned> inputs{{"external.valid", valid}, {"target.ready", ready}};
        for (unsigned i = 0; i < casts; ++i)
          inputs["external.bits[" + std::to_string(i) + "]"] = (bits >> i) & 1;
        std::map<std::string, unsigned> outputs;
        for (auto connect : wrapper.getOps<ConnectOp>())
          outputs[path(connect.getDest())] = evaluate(connect.getSrc(), inputs);
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
}
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
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
  llvm::outs() << "ClockChannel: scalar/vector token truth tables, retargeting and eight rejection cases passed\n";
}
