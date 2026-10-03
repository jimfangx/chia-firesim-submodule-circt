// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEOutputChannel.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
unsigned portNamed(FModuleOp module, StringRef name) {
  for (unsigned i = 0; i < module.getNumPorts(); ++i)
    if (module.getPortName(i) == name) return i;
  throw std::runtime_error("expected port absent: " + name.str());
}
void run(MLIRContext &context, bool output, bool grouped, unsigned rejection) {
  std::string dir = output ? "out" : "in";
  auto fixture = "module { firrtl.circuit \"Top\" { firrtl.module @Top("
      "in %hostClock: !firrtl.clock, in %kept: !firrtl.uint<1>, " + dir +
      " %g_data: !firrtl.uint<8>" +
      (grouped ? ", " + dir + " %g_flag: !firrtl.uint<1>" : "") +
      ") {} firrtl.module @Model(in %clock: !firrtl.clock, " + dir +
      " %m_data: !firrtl.uint<8>" +
      (grouped ? ", " + dir + " %m_flag: !firrtl.uint<1>" : "") + ") {} } }";
  auto root = parseSourceString<ModuleOp>(fixture, &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>();
  auto top = *modules.begin();
  auto model = *std::next(modules.begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  b.create<StrictConnectOp>(top.getLoc(), instance.getResult(0),
                           top.getBodyBlock()->getArgument(0));
  goldengate::TopHierarchy hierarchy{top, {}};
  goldengate::ModelPortGroup group{"m_", model,
      output ? Direction::Out : Direction::In, {}, {1}};
  goldengate::ModelChannelBinding binding{"g_", &group, instance, {1}};
  if (grouped) { group.ports.push_back(2); binding.instancePorts.push_back(2); }
  for (unsigned i = 1; i <= (grouped ? 2u : 1u); ++i) {
    auto wrapperValue = top.getBodyBlock()->getArgument(i + 1);
    auto instanceValue = instance.getResult(i);
    b.create<StrictConnectOp>(top.getLoc(), output ? wrapperValue : instanceValue,
                             output ? instanceValue : wrapperValue);
    hierarchy.connections.push_back({i + 1, instance, i});
  }
  // Empty input-only model bodies are valid and need a safe insertion point.
  if (output) {
    b.setInsertionPointToEnd(model.getBodyBlock());
    for (unsigned i : group.ports) {
      auto value = model.getBodyBlock()->getArgument(i);
      auto constant = b.create<ConstantOp>(model.getLoc(), cast<IntType>(value.getType()),
                                          llvm::APInt(i == 1 ? 8 : 1, 0));
      b.create<StrictConnectOp>(model.getLoc(), value, constant);
    }
  }
  auto anno = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firrtl.transforms.DontTouchAnnotation")),
      b.getNamedAttr("test.marker", b.getStringAttr("data"))});
  auto flagAnno = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firrtl.transforms.DontTouchAnnotation")),
      b.getNamedAttr("test.marker", b.getStringAttr("flag"))});
  SmallVector<Attribute> portAnnos(top.getNumPorts(), b.getArrayAttr({}));
  auto keptAnno = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Kept"))});
  portAnnos[1] = b.getArrayAttr({keptAnno});
  SmallVector<NamedAttribute> explicitZero(anno.getValue());
  explicitZero.push_back(b.getNamedAttr("circt.fieldID", b.getI32IntegerAttr(0)));
  // Both root encodings must become the same payload field, without dropping
  // duplicate protections or custom members.
  portAnnos[2] = b.getArrayAttr({anno, b.getDictionaryAttr(explicitZero)});
  if (grouped) portAnnos[3] = b.getArrayAttr({flagAnno});
  unsigned badPort = grouped ? 3 : 2; // Late leaf failures must also be atomic.
  if (rejection && rejection != 5) {
    SmallVector<NamedAttribute> members(anno.getValue());
    if (rejection == 1) members[0] = b.getNamedAttr("class", b.getStringAttr("test.Unknown"));
    if (rejection == 2) members.push_back(b.getNamedAttr("circt.fieldID", b.getI32IntegerAttr(1)));
    if (rejection == 3) members.push_back(b.getNamedAttr("circt.fieldID", b.getStringAttr("bad")));
    if (rejection == 4) members.push_back(b.getNamedAttr("target", b.getStringAttr("~Top|Top>g_data")));
    portAnnos[badPort] = b.getArrayAttr({b.getDictionaryAttr(members)});
  }
  top.setPortAnnotationsAttr(b.getArrayAttr(portAnnos));
  top.setPortSymbolsAttr(1, circt::hw::InnerSymAttr::get(b.getStringAttr("kept_id")));
  if (rejection == 5)
    top.setPortSymbolsAttr(badPort, circt::hw::InnerSymAttr::get(b.getStringAttr("data_id")));
  auto u1 = UIntType::get(&context, 1);
  Type payload = UIntType::get(&context, 8);
  if (grouped) payload = BundleType::get(&context, {
      {b.getStringAttr("data"), false, cast<FIRRTLBaseType>(payload)},
      {b.getStringAttr("flag"), false, u1}});
  auto type = BundleType::get(&context, {
      {b.getStringAttr("ready"), true, u1},
      {b.getStringAttr("valid"), false, u1},
      {b.getStringAttr("bits"), false, cast<FIRRTLBaseType>(payload)}});
  goldengate::FAMETopChannelPort channel{&binding, "g_channel", type};
  std::string before = dump(*root), error;
  auto result = output ? goldengate::rewriteFAMEOutputChannel(hierarchy, channel, error)
                       : goldengate::rewriteFAMEInputChannel(hierarchy, channel, error);
  if (rejection) {
    require(failed(result) && !error.empty(), "unsupported annotation accepted");
    require(dump(*root) == before, "rejected annotation mutated IR");
    return;
  }
  require(succeeded(result) && succeeded(verify(*root)), "channel rewrite failed verification");
  auto annotations = AnnotationSet::forPort(top, portNamed(top, "g_channel"));
  auto dataID = type.getFieldID(2) + (grouped ? cast<BundleType>(payload).getFieldID(0) : 0);
  auto flagID = grouped ? type.getFieldID(2) + cast<BundleType>(payload).getFieldID(1) : 0;
  require(annotations.size() == (grouped ? 3 : 2), "annotations lost");
  unsigned index = 0;
  for (auto annotation : annotations) {
    require(annotation.getFieldID() == (index < 2 ? dataID : flagID), "wrong payload field ID");
    require(annotation.getMember<StringAttr>("test.marker").getValue() ==
                (index < 2 ? "data" : "flag"), "annotation member lost");
    ++index;
  }
  // CIRCT distributes the aggregate annotations onto flattened ground ports.
  PassManager pm(&context);
  pm.addNestedPass<CircuitOp>(createLowerFIRRTLTypesPass());
  require(succeeded(pm.run(*root)), "LowerTypes failed");
  unsigned payloadPorts = 0;
  for (auto module : circuit.getOps<FModuleOp>())
    for (unsigned p = 0; p < module.getNumPorts(); ++p) {
      auto attached = AnnotationSet::forPort(module, p);
      if (module != top) { require(attached.empty(), "wrapper annotations leaked to model"); continue; }
      auto name = module.getPortName(p);
      if (name == "g_channel_bits" || name == "g_channel_bits_data" || name == "g_channel_bits_flag") {
        ++payloadPorts;
        require(attached.size() == (name.ends_with("flag") ? 1 : 2), "lowered payload annotation lost");
        for (auto annotation : attached)
          require(annotation.getFieldID() == 0 &&
                      annotation.isClass("firrtl.transforms.DontTouchAnnotation"), "lowered leaf identity wrong");
      } else if (name == "kept") {
        require(attached.getArrayAttr() == portAnnos[1] && module.getPorts()[p].sym.getSymName() == "kept_id",
                "unrelated metadata changed");
      } else require(attached.empty(), "payload protection leaked onto handshake");
    }
  require(payloadPorts == (grouped ? 2u : 1u), "flattened payload port missing");
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (bool output : {false, true})
      for (bool grouped : {false, true})
        for (unsigned rejection = 0; rejection < 6; ++rejection)
          run(context, output, grouped, rejection);
    llvm::outs() << "Four scalar/grouped input/output payload transfers and LowerTypes checks passed; "
                    "20 unsupported metadata plans rejected without mutation\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
