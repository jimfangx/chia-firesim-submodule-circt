// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/BridgeAnalysis.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

// The NIC bridge's constructor is None. Its ports still have real FIRRTL
// drivers/users, which must be reconnected when its instance is removed.
void run(MLIRContext &context, Attribute constructorKey) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @ClockBridge(out %clocks: !firrtl.clock) {}
      firrtl.module @NICBridge(in %clock: !firrtl.clock,
        in %tx: !firrtl.uint<8>, out %rx: !firrtl.uint<8>) {}
      firrtl.module @Top(in %tx: !firrtl.uint<8>, out %rx: !firrtl.uint<8>) {
        %clocks = firrtl.instance clockBridge @ClockBridge(out clocks: !firrtl.clock)
        %clock, %nicTx, %nicRx = firrtl.instance nic @NICBridge(
          in clock: !firrtl.clock, in tx: !firrtl.uint<8>, out rx: !firrtl.uint<8>)
        firrtl.strictconnect %clock, %clocks : !firrtl.clock
        firrtl.strictconnect %nicTx, %tx : !firrtl.uint<8>
        firrtl.strictconnect %rx, %nicRx : !firrtl.uint<8>
      }
    }
  })mlir", &context);
  require(bool(root), "parse bridge promotion fixture");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto str = [&](StringRef name, StringRef value) {
    return b.getNamedAttr(name, b.getStringAttr(value));
  };
  auto pipe = [&](StringRef name, StringRef port, bool source) {
    return b.getDictionaryAttr({
        str("class", "firesim.lib.bridgeutils.PipeBridgeChannel"),
        str("name", name), str("clock", "~Top|NICBridge>clock"),
        b.getNamedAttr("latency", b.getI64IntegerAttr(1)),
        b.getNamedAttr("sources", b.getArrayAttr(source ? ArrayRef<Attribute>{
            b.getStringAttr(port)} : ArrayRef<Attribute>{})),
        b.getNamedAttr("sinks", b.getArrayAttr(source ? ArrayRef<Attribute>{} :
            ArrayRef<Attribute>{b.getStringAttr(port)}))});
  };
  NamedAttrList nic({
      str("class", goldengate::AnnotationClasses::Bridge),
      str("target", "~Top|NICBridge"),
      str("widgetClass", "firechip.goldengateimplementations.SimpleNICBridgeModule"),
      b.getNamedAttr("bridgeChannels", b.getArrayAttr({
          pipe("tx", "~Top|NICBridge>tx", false),
          pipe("rx", "~Top|NICBridge>rx", true)}))});
  if (constructorKey)
    nic.set("widgetConstructorKey", constructorKey);
  auto clockKey = b.getDictionaryAttr({
      str("class", "firesim.lib.bridges.ClockParameters"),
      b.getNamedAttr("clocks", b.getArrayAttr({b.getDictionaryAttr({
          str("name", "base"), b.getNamedAttr("multiplier", b.getI64IntegerAttr(1)),
          b.getNamedAttr("divisor", b.getI64IntegerAttr(1))})}))});
  auto clock = b.getDictionaryAttr({
      str("class", goldengate::AnnotationClasses::Bridge),
      str("target", "~Top|ClockBridge"),
      str("widgetClass", "midas.widgets.ClockBridgeModule"),
      b.getNamedAttr("widgetConstructorKey", clockKey),
      b.getNamedAttr("bridgeChannels", b.getArrayAttr({}))});
  auto retained = b.getDictionaryAttr({
      str("class", goldengate::AnnotationClasses::DontTouch),
      str("target", "~Top|Top>rx")});
  circuit->setAttr("rawAnnotations", b.getArrayAttr({clock, nic.getDictionary(&context), retained}));

  std::string error;
  require(succeeded(goldengate::promoteBridgePorts(circuit, error, true)), error);
  require(succeeded(verify(*root)), "verify promoted bridge IR");
  FModuleOp top;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == "Top")
      top = module;
  require(top && top.getOps<InstanceOp>().empty(), "bridge instances were not removed");
  auto annotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(annotations[0] == retained, "unrelated annotation changed");
  DictionaryAttr promoted;
  unsigned channels = 0;
  for (Attribute attr : annotations) {
    auto row = cast<DictionaryAttr>(attr);
    auto cls = row.getAs<StringAttr>("class").getValue();
    require(cls != goldengate::AnnotationClasses::Bridge, "source bridge annotation remains");
    if (cls == goldengate::AnnotationClasses::BridgeIO &&
        row.getAs<StringAttr>("widgetClass").getValue().contains("SimpleNIC"))
      promoted = row;
    if (cls == goldengate::AnnotationClasses::BridgeIO &&
        row.getAs<StringAttr>("widgetClass").getValue() == "midas.widgets.ClockBridgeModule")
      require(row.get("widgetConstructorKey") == clockKey, "clock constructor changed");
    if (cls == goldengate::AnnotationClasses::ChannelConnection) {
      ++channels;
      auto target = row.getAs<ArrayAttr>(row.get("sources") ? "sources" : "sinks");
      auto name = row.getAs<StringAttr>("globalName").getValue();
      require(target == b.getArrayAttr({b.getStringAttr(
          name == "nic_tx" ? "~Top|Top>nic.tx" : "~Top|Top>nic.rx")}),
          "channel endpoint was not retargeted");
      require(row.getAs<StringAttr>("clock").getValue() == "~Top|Top>nic.clock",
              "channel clock was not retargeted");
    }
  }
  require(promoted && channels == 2, "missing bridge/channel annotations");
  require(promoted.get("widgetConstructorKey") == constructorKey,
          "optional constructor was changed or fabricated");
  require(promoted.getAs<StringAttr>("target").getValue() == "~Top|Top>nic",
          "promoted bridge target changed");
  require(promoted.getAs<DictionaryAttr>("channelMapping") ==
          b.getDictionaryAttr({str("tx", "nic_tx"), str("rx", "nic_rx")}),
          "channel mapping changed");
  unsigned connected = 0;
  for (auto connect : top.getOps<StrictConnectOp>()) {
    auto dest = connect.getDest().getDefiningOp<SubfieldOp>();
    auto src = connect.getSrc().getDefiningOp<SubfieldOp>();
    if (dest && dest.getFieldName() == "tx") {
      require(connect.getSrc() == top.getArgument(0), "NIC tx driver changed");
      ++connected;
    } else if (src && src.getFieldName() == "rx") {
      require(connect.getDest() == top.getArgument(1), "NIC rx user changed");
      ++connected;
    } else if (dest && dest.getFieldName() == "clock") {
      require(src && src.getFieldName() == "clocks", "NIC clock driver changed");
      ++connected;
    }
  }
  require(connected == 3, "promoted bridge connections missing");
}
} // namespace

int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    OpBuilder b(&context);
    run(context, {});
    run(context, b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("OpaqueKey")),
        b.getNamedAttr("payload", b.getArrayAttr({b.getI64IntegerAttr(7), b.getStringAttr("value")}))}));
    // Scala permits any serializable AnyRef key, rather than only a case class.
    run(context, b.getStringAttr("opaque constructor"));
    llvm::outs() << "optional bridge constructors and FIRRTL promotion: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n';
    return 1;
  }
}
