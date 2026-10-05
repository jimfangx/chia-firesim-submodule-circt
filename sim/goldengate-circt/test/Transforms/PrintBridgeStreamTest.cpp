// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/PrintBridgePayload.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  SmallVector<FModuleOp> payloads, stages, controls;
  Fixture(MLIRContext &context, unsigned bits = 8, unsigned count = 2) {
    root = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" {
        firrtl.module @Top() {}
        firrtl.module @GGPrintBridgeStream() {}
        firrtl.module @GGPrintBridgeStreamAdapter() {}
      }
    })mlir", &context);
    require(bool(root), "stream fixture parse failed");
    circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
    SmallVector<Attribute> raw{b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test.Opaque")),
        b.getNamedAttr("value", b.getStringAttr("preserve"))})};
    for (unsigned i = 0; i < count; ++i) {
      auto record = b.getDictionaryAttr({
          b.getNamedAttr("name", b.getStringAttr("print")),
          b.getNamedAttr("format", b.getStringAttr("value=%x\n")),
          b.getNamedAttr("ports", b.getArrayAttr({
              b.getDictionaryAttr({b.getNamedAttr("enable", b.getStringAttr("UInt<1>"))}),
              b.getDictionaryAttr({b.getNamedAttr("arg", b.getStringAttr("UInt<" + std::to_string(bits - 3) + ">"))})}))});
      auto key = b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeParameters)),
          b.getNamedAttr("resetPortName", b.getStringAttr("reset" + std::to_string(i))),
          b.getNamedAttr("printPorts", b.getArrayAttr({record}))});
      raw.push_back(b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
          b.getNamedAttr("widgetClass", b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeModule)),
          b.getNamedAttr("target", b.getStringAttr("~Top|Top>synthesizedPrintf")),
          b.getNamedAttr("widgetConstructorKey", key)}));
    }
    circuit->setAttr("rawAnnotations", b.getArrayAttr(raw));
    std::string error;
    require(succeeded(goldengate::materializePrintBridgePayloads(circuit, payloads, error)), error);
    require(succeeded(goldengate::materializePrintBridgeTokenStages(circuit, payloads, stages, error)), error);
    require(succeeded(goldengate::materializePrintBridgeControls(circuit, payloads, stages, controls, error)), error);
  }
};
bool constant(Value value, uint64_t expected) {
  auto c = value.getDefiningOp<ConstantOp>(); return c && c.getValue() == expected;
}
llvm::DenseMap<Value, Value> drivers(FModuleOp m) {
  llvm::DenseMap<Value, Value> result;
  for (auto &op : *m.getBodyBlock()) {
    if (auto c = dyn_cast<ConnectOp>(op)) require(result.try_emplace(c.getDest(), c.getSrc()).second, "duplicate stream driver");
    if (auto c = dyn_cast<StrictConnectOp>(op)) require(result.try_emplace(c.getDest(), c.getSrc()).second, "duplicate stream driver");
  }
  return result;
}

void checkStreams(MLIRContext &context, unsigned bits) {
  Fixture f(context, bits); OpBuilder b(&context);
  auto original = f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> annotated(original.begin(), original.end());
  SmallVector<Attribute> targets;
  for (auto control : f.controls)
    for (auto port : {"bufferReady", "tokenValid", "tokenData", "hBits.print.arg"})
      targets.push_back(b.getStringAttr("~Top|" + control.getName().str() + ">" + port));
  annotated.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Targets")),
      b.getNamedAttr("targets", b.getArrayAttr(targets))}));
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr(annotated));
  // These ports are internally connected in the new wrapper, but remain on
  // their original control module. Preserve their annotation identities.
  auto raw = f.circuit->getAttr("rawAnnotations"); auto circuitName = f.circuit.getNameAttr();
  std::string beforeControls[2] = {dump(f.controls[0]), dump(f.controls[1])};
  SmallVector<FModuleOp> wrappers; std::string error;
  require(succeeded(goldengate::materializePrintBridgeStreams(f.circuit, f.controls, wrappers, error)), error);
  require(wrappers.size() == 2 && wrappers[0].getName() != "GGPrintBridgeStream" &&
      wrappers[0].getName() != wrappers[1].getName(), "stream wrapper naming collision");
  const unsigned copied[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 15, 16};
  const char *names[] = {"hostClock", "hostReset", "doneInit", "hValid", "flushNarrowPacket", "startCycleL", "startCycleH",
      "endCycleL", "endCycleH", "hBits", "hReady", "fromHostValid", "currentCycle", "enable", "streamReady", "streamValid", "streamData"};
  const char *adapterNames[] = {"hostClock", "hostReset", "tokenValid", "tokenData", "flushNarrowPacket", "streamReady",
      "bufferReady", "streamValid", "streamData", "count"};
  unsigned ratio = bits < 512 ? 512 / bits : bits / 512, depth = bits < 512 ? 1 : bits / 512;
  StringAttr previousAdapter;
  for (unsigned i = 0; i < wrappers.size(); ++i) {
    auto wrapper = wrappers[i], control = f.controls[i];
    require(wrapper.isPublic() && wrapper.getNumPorts() == 17, "stream wrapper visibility/port count");
    for (unsigned p = 0; p < 17; ++p) {
      require(wrapper.getPortName(p) == names[p] &&
          wrapper.getPortDirection(p) == (p < 10 || p == 14 ? Direction::In : Direction::Out), "stream wrapper port order/direction");
      if (p < 14) require(wrapper.getPortType(p) == control.getPortType(copied[p]), "copied control port type changed");
      else require(wrapper.getPortType(p) == UIntType::get(&context, p == 16 ? 512 : 1), "stream external port width");
    }
    auto info = wrapper->getAttrOfType<DictionaryAttr>("goldengate.printStream");
    require(bool(info), "stream metadata absent");
    for (auto field : control->getAttrOfType<DictionaryAttr>("goldengate.printControl"))
      require(info.get(field.getName()) == field.getValue(), "control constructor/layout metadata changed");
    auto adapterName = info.getAs<StringAttr>("adapterModule");
    require(info.getAs<StringAttr>("controlModule").getValue() == control.getName() && adapterName &&
        adapterName.getValue() != "GGPrintBridgeStreamAdapter" && adapterName != previousAdapter &&
        info.getAs<IntegerAttr>("streamBits").getInt() == 512 &&
        info.getAs<IntegerAttr>("adapterDepth").getInt() == depth &&
        info.getAs<IntegerAttr>("packingRatio").getInt() == (bits < 512 ? ratio : 1) &&
        info.getAs<BoolAttr>("lowSliceFirst").getValue() &&
        info.getAs<BoolAttr>("narrowFlushInjection").getValue() == (bits < 512), "stream metadata contract");
    previousAdapter = adapterName; auto adapter = named(f.circuit, adapterName.getValue());
    require(adapter->getAttr("goldengate.printStreamAdapter") == info, "adapter and wrapper collateral diverged");
    require(adapter.isPublic() && adapter.getNumPorts() == 10, "stream adapter visibility/interface");
    for (unsigned p = 0; p < 10; ++p) {
      Type expected = p == 0 ? Type(ClockType::get(&context)) : Type(UIntType::get(&context,
          p == 3 ? bits : p == 8 ? 512 : p == 9 ? llvm::Log2_64_Ceil(depth + 1) : 1));
      require(adapter.getPortName(p) == adapterNames[p] && adapter.getPortType(p) == expected &&
          adapter.getPortDirection(p) == (p < 6 ? Direction::In : Direction::Out), "adapter port schema");
    }
    InstanceOp inner, fifo; unsigned instances = 0;
    for (auto instance : wrapper.getOps<InstanceOp>()) {
      ++instances;
      if (instance.getModuleName() == control.getName()) inner = instance;
      if (instance.getModuleName() == adapter.getName()) fifo = instance;
    }
    require(instances == 2 && inner && fifo, "stream wrapper instances refer to wrong domain");
    auto d = drivers(wrapper);
    for (unsigned p = 0; p < 14; ++p) {
      Value outer = wrapper.getArgument(p), inside = inner.getResult(copied[p]);
      require(d.lookup(control.getPortDirection(copied[p]) == Direction::In ? inside : outer) ==
          (control.getPortDirection(copied[p]) == Direction::In ? outer : inside), "copied stream instance wiring");
    }
    require(d.lookup(fifo.getResult(0)) == wrapper.getArgument(0) &&
        d.lookup(fifo.getResult(1)) == wrapper.getArgument(1) &&
        d.lookup(fifo.getResult(2)) == inner.getResult(13) &&
        d.lookup(fifo.getResult(3)) == inner.getResult(14) &&
        d.lookup(fifo.getResult(4)) == wrapper.getArgument(4) &&
        d.lookup(fifo.getResult(5)) == wrapper.getArgument(14) &&
        d.lookup(inner.getResult(5)) == fifo.getResult(6) &&
        d.lookup(wrapper.getArgument(15)) == fifo.getResult(7) &&
        d.lookup(wrapper.getArgument(16)) == fifo.getResult(8), "adapter/control ready-valid/data wiring");
    unsigned storage = 0, reset = 0; std::map<unsigned, unsigned> resetWidths;
    for (auto reg : adapter.getOps<RegOp>()) {
      ++storage; require(reg.getClockVal() == adapter.getArgument(0) &&
          reg.getResult().getType() == UIntType::get(&context, bits), "storage width/clock/reset mismatch");
    }
    for (auto reg : adapter.getOps<RegResetOp>()) {
      ++reset; ++resetWidths[cast<UIntType>(reg.getResult().getType()).getWidthOrSentinel()];
      require(reg.getClockVal() == adapter.getArgument(0) && reg.getResetSignal() == adapter.getArgument(1) &&
          constant(reg.getResetValue(), 0), "adapter position/size reset mismatch");
    }
    if (bits == 512) {
      auto a = drivers(adapter);
      require(storage == 0 && reset == 0 && a.lookup(adapter.getArgument(6)) == adapter.getArgument(5) &&
          a.lookup(adapter.getArgument(7)) == adapter.getArgument(2) && a.lookup(adapter.getArgument(8)) == adapter.getArgument(3) &&
          constant(a.lookup(adapter.getArgument(9)), 0), "512-bit equality must bypass without FIFO state/flush");
    } else {
      require(storage == (bits < 512 ? ratio : 1) && reset == 2 &&
          resetWidths[llvm::Log2_64_Ceil(ratio)] == 1 && resetWidths[llvm::Log2_64_Ceil(ratio + 1)] == 1,
          "single-group adapter register count/position/occupancy widths");
    }
    require(dump(control) == beforeControls[i], "stream wrapping mutated source control");
  }
  require(f.circuit->getAttr("rawAnnotations") == raw && f.circuit.getNameAttr() == circuitName &&
      succeeded(verify(*f.root)), "stream emitted invalid IR or changed annotation/circuit identity");
  auto before = dump(*f.root); unsigned size = wrappers.size();
  require(failed(goldengate::materializePrintBridgeStreams(f.circuit, f.controls, wrappers, error)) &&
      !error.empty() && dump(*f.root) == before && wrappers.size() == size, "repeated stream materialization not atomic");
}

void checkRejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 18; ++bad) {
    Fixture f(context), foreign(context, 8, 1); OpBuilder b(&context); auto c = f.controls[1];
    auto metadata = [&](StringRef key, Attribute value) {
      NamedAttrList fields(c->getAttrOfType<DictionaryAttr>("goldengate.printControl"));
      if (value) fields.set(key, value); else fields.erase(key);
      c->setAttr("goldengate.printControl", fields.getDictionary(&context));
    };
    if (bad == 0) f.controls[1] = {};
    if (bad == 1) f.controls[1] = foreign.controls[0];
    if (bad == 2) c->removeAttr("goldengate.printControl");
    if (bad == 3) metadata("bridgeTarget", b.getStringAttr(""));
    if (bad == 4) metadata("resetPortName", b.getStringAttr(""));
    if (bad == 5) metadata("tokenBits", b.getI64IntegerAttr(7));
    if (bad == 6) metadata("tokenBits", b.getI64IntegerAttr(12));
    if (bad == 7) metadata("cycleBits", b.getI64IntegerAttr(32));
    if (bad == 8) metadata("roiInclusive", b.getBoolAttr(false));
    if (bad == 9) f.controls[1] = f.controls[0];
    if (bad == 10) metadata("resetPortName", b.getStringAttr("reset0"));
    if (bad == 11) {
      SmallVector<Attribute> names(c.getPortNames().begin(), c.getPortNames().end());
      names[13] = b.getStringAttr("wrong"); c.setPortNames(names);
    }
    if (bad >= 12 && bad <= 14) {
      SmallVector<Attribute> types(c.getPortTypes().begin(), c.getPortTypes().end());
      unsigned p = bad == 12 ? 14 : 10;
      types[p] = TypeAttr::get(bad == 14 ?
          Type(BundleType::get(&context, {{b.getStringAttr("flipped"), true, UIntType::get(&context, 1)}})) :
          Type(UIntType::get(&context, 1))); c.setPortTypes(types);
    }
    if (bad == 15) metadata("tokenBits", {});
    if (bad == 16) metadata("tokenBits", b.getI64IntegerAttr(1LL << 31));
    if (bad == 17) metadata("roiInclusive", b.getStringAttr("true"));
    auto before = dump(*f.root), other = dump(*foreign.root);
    SmallVector<FModuleOp> wrappers{*f.circuit.getOps<FModuleOp>().begin()}; std::string error;
    require(failed(goldengate::materializePrintBridgeStreams(f.circuit, f.controls, wrappers, error)),
        "accepted invalid later stream control " + std::to_string(bad));
    require(!error.empty() && wrappers.size() == 1 && dump(*f.root) == before && dump(*foreign.root) == other,
        "stream rejection mutated first candidate/output or foreign circuit " + std::to_string(bad));
  }
  Fixture empty(context, 8, 0); auto before = dump(*empty.root);
  SmallVector<FModuleOp> wrappers; std::string error;
  require(succeeded(goldengate::materializePrintBridgeStreams(empty.circuit, {}, wrappers, error)) &&
      wrappers.empty() && dump(*empty.root) == before, "empty stream list is not a no-op");
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned bits : {8, 16, 512, 1024, 2048}) checkStreams(context, bits);
    checkRejections(context);
    llvm::outs() << "PASS PrintBridge stream adapter structure, wiring, collateral, and atomic preflight\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "FAIL: " << error.what() << "\n"; return 1;
  }
}
