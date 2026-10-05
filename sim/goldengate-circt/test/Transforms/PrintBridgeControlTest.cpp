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
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <random>
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
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  SmallVector<FModuleOp> payloads, stages;
  Fixture(MLIRContext &context, unsigned count = 2) {
    root = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" {
        firrtl.module @Top() {}
        firrtl.module @GGPrintBridgeControl() {}
      }
    })mlir", &context);
    require(bool(root), "control fixture parse failed");
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
              b.getDictionaryAttr({b.getNamedAttr("arg", b.getStringAttr("UInt<4>"))})}))});
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
  }
};

// Interpret only the wrapper's actual counter/ROI expressions. The token
// stage's hReady is evaluated independently at the instance boundary; full
// stage state and RTL comparison are covered by the token-stage tests.
struct Counter {
  FModuleOp module;
  InstanceOp payload, tokens;
  RegResetOp reg;
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, APInt> inputs;
  APInt state{64, 0};
  Counter(FModuleOp m, FModuleOp pack, FModuleOp stage) : module(m) {
    for (auto connect : m.getOps<StrictConnectOp>())
      require(drivers.try_emplace(connect.getDest(), connect.getSrc()).second,
              "multiple wrapper drivers");
    unsigned instances = 0, regs = 0;
    for (auto instance : m.getOps<InstanceOp>()) {
      ++instances;
      if (instance.getName() == "payload" && instance.getModuleName() == pack.getName()) payload = instance;
      if (instance.getName() == "tokens" && instance.getModuleName() == stage.getName()) tokens = instance;
    }
    for (auto candidate : m.getOps<RegResetOp>()) { ++regs; reg = candidate; }
    require(instances == 2 && payload && tokens, "wrong payload/stage instance binding");
    require(regs == 1 && reg.getResult().getType() == UIntType::get(m.getContext(), 64) &&
            reg.getClockVal() == arg(0) && reg.getResetSignal() == arg(1), "wrong counter state/reset");
    auto registerName = reg->getAttrOfType<StringAttr>("name");
    for (auto port : m.getPorts())
      require(registerName != port.name, "counter name collides with exported FIRRTL port");
    require(drivers.lookup(payload.getResult(0)) == arg(10), "hBits is not connected to payload");
    for (unsigned p = 0; p < 4; ++p)
      require(drivers.lookup(tokens.getResult(p)) == arg(p), "host control instance wiring mismatch");
    require(drivers.lookup(tokens.getResult(5)) == arg(4) &&
            drivers.lookup(tokens.getResult(6)) == arg(5), "flush/ready wiring mismatch");
    require(drivers.lookup(tokens.getResult(7)) == payload.getResult(1) &&
            drivers.lookup(tokens.getResult(8)) == payload.getResult(2), "payload output wiring mismatch");
    require(drivers.lookup(arg(11)) == tokens.getResult(9) &&
            drivers.lookup(arg(13)) == tokens.getResult(10) &&
            drivers.lookup(arg(14)) == tokens.getResult(11), "token output wiring mismatch");
    require(drivers.lookup(arg(15)) == reg.getResult() &&
            drivers.lookup(tokens.getResult(4)) == drivers.lookup(arg(16)), "counter/ROI output wiring mismatch");
  }
  Value arg(unsigned i) { return module.getArgument(i); }
  APInt eval(Value value) {
    if (value == reg.getResult()) return state;
    if (auto i = inputs.find(value); i != inputs.end()) return i->second;
    if (auto d = drivers.find(value); d != drivers.end()) return eval(d->second);
    auto *op = value.getDefiningOp(); require(op != nullptr, "unprovided wrapper input");
    unsigned width = cast<UIntType>(value.getType()).getWidthOrSentinel();
    auto a = [&](unsigned i) { return eval(op->getOperand(i)); };
    if (auto k = dyn_cast<ConstantOp>(op)) return k.getValue();
    if (isa<CatPrimOp>(op)) return a(0).zext(width).shl(a(1).getBitWidth()) | a(1).zext(width);
    if (isa<LEQPrimOp>(op)) return APInt(1, a(0).ule(a(1)));
    if (isa<AndPrimOp>(op)) return a(0) & a(1);
    if (isa<AddPrimOp>(op)) return a(0).zextOrTrunc(width) + a(1).zextOrTrunc(width);
    if (auto bits = dyn_cast<BitsPrimOp>(op)) return a(0).lshr(bits.getLo()).trunc(width);
    if (isa<MuxPrimOp>(op)) return a(a(0).getBoolValue() ? 1 : 2);
    throw std::runtime_error("unexpected wrapper expression: " + op->getName().getStringRef().str());
  }
  void cycle(uint64_t start, uint64_t end, bool reset, bool done, bool valid,
             bool flush, bool ready) {
    uint64_t before = state.getZExtValue();
    bool roi = start <= before && before <= end;
    bool accepted = done && valid && !flush && (!roi || ready);
    inputs.clear();
    for (auto [p, value] : {std::pair<unsigned, bool>{1, reset}, {2, done}, {3, valid}, {4, flush}, {5, ready}})
      inputs[arg(p)] = APInt(1, value);
    inputs[arg(6)] = APInt(32, start); inputs[arg(7)] = APInt(32, start >> 32);
    inputs[arg(8)] = APInt(32, end); inputs[arg(9)] = APInt(32, end >> 32);
    inputs[tokens.getResult(9)] = APInt(1, accepted);
    require(eval(arg(15)).getZExtValue() == before, "counter changed before accepted edge");
    require(eval(arg(16)).getBoolValue() == roi, "ROI does not use inclusive unsigned old counter");
    require(eval(arg(11)).getBoolValue() == accepted, "hReady does not forward accepted cycle");
    require(eval(arg(12)).isOne(), "fromHostValid is not unconditionally true");
    APInt next = eval(drivers.lookup(reg.getResult()));
    state = reset ? eval(reg.getResetValue()) : next;
    require(state.getZExtValue() == (reset ? 0 : before + uint64_t(accepted)), "accepted-cycle update/reset/wrap mismatch");
  }
};

void checkControls(MLIRContext &context) {
  Fixture f(context); auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> controls; std::string error;
  require(succeeded(goldengate::materializePrintBridgeControls(f.circuit, f.payloads, f.stages, controls, error)), error);
  require(controls.size() == 2 && controls[0].getName() != "GGPrintBridgeControl" &&
          controls[0].getName() != controls[1].getName(), "wrapper name collision");
  const char *names[] = {"hostClock", "hostReset", "doneInit", "hValid", "flushNarrowPacket", "bufferReady",
      "startCycleL", "startCycleH", "endCycleL", "endCycleH", "hBits", "hReady", "fromHostValid",
      "tokenValid", "tokenData", "currentCycle", "enable"};
  for (unsigned i = 0; i < 2; ++i) {
    auto control = controls[i]; require(control.isPublic() && control.getNumPorts() == 17, "wrapper visibility/interface");
    for (unsigned p = 0; p < 17; ++p)
      require(control.getPortName(p) == names[p] &&
          control.getPortDirection(p) == (p < 11 ? Direction::In : Direction::Out), "wrapper port order/direction");
    auto info = control->getAttrOfType<DictionaryAttr>("goldengate.printControl");
    auto layout = f.payloads[i]->getAttrOfType<DictionaryAttr>("goldengate.printPayload");
    for (auto entry : layout) require(info.get(entry.getName()) == entry.getValue(), "payload collateral changed");
    require(info.getAs<StringAttr>("payloadModule").getValue() == f.payloads[i].getName() &&
        info.getAs<StringAttr>("tokenStageModule").getValue() == f.stages[i].getName() &&
        info.getAs<IntegerAttr>("cycleBits").getInt() == 64 && info.getAs<BoolAttr>("roiInclusive").getValue(),
        "wrapper collateral identity/ROI mismatch");
  }
  Counter first(controls[0], f.payloads[0], f.stages[0]);
  Counter second(controls[1], f.payloads[1], f.stages[1]);
  first.cycle(2, 4, false, true, true, false, false); // outside ROI accepts without buffer space
  first.cycle(2, 4, false, true, true, false, false);
  first.cycle(2, 4, false, true, true, false, false); // inclusive start stalls
  first.cycle(2, 4, false, true, true, false, true);
  first.cycle(2, 4, false, false, true, false, true);
  first.cycle(2, 4, false, true, false, false, true);
  first.cycle(2, 4, false, true, true, true, true);
  first.cycle(2, 4, false, true, true, false, true);
  first.cycle(2, 4, false, true, true, false, true); // inclusive end accepted
  first.cycle(2, 4, false, true, true, false, false); // outside end accepts
  first.cycle(8, 1, false, true, true, false, false); // empty ROI remains legal
  second.cycle(0, 0, false, true, true, false, false);
  require(second.state.isZero() && !first.state.isZero(), "domain counter shared state");
  first.state = APInt(64, uint64_t(1) << 63);
  first.cycle(uint64_t(1) << 63, (uint64_t(1) << 63) + 1, false, true, true, false, true);
  first.cycle(0, (uint64_t(1) << 63) - 1, false, true, true, false, false); // unsigned comparison
  first.state = APInt::getAllOnes(64);
  first.cycle(UINT64_MAX, UINT64_MAX, false, true, true, false, true);
  require(first.state.isZero(), "counter failed to wrap to zero");
  first.state = APInt(64, UINT64_MAX);
  first.cycle(0, UINT64_MAX, true, true, true, false, true); // reset priority over accepted edge
  std::mt19937_64 random(0xc1c1e);
  for (unsigned i = 0; i < 2048; ++i) {
    if (i % 13 == 0) first.state = APInt(64, random());
    uint64_t start = i % 3 ? random() : 0, end = i % 3 ? random() : UINT64_MAX;
    first.cycle(start, end, i % 29 == 0, random() & 1, random() & 1, random() & 1, random() & 1);
  }
  require(f.circuit->getAttr("rawAnnotations") == raw && succeeded(verify(*f.root)), "raw annotations or IR changed incorrectly");
  auto before = dump(*f.root); unsigned existing = controls.size();
  require(failed(goldengate::materializePrintBridgeControls(f.circuit, f.payloads, f.stages, controls, error)) &&
      dump(*f.root) == before && controls.size() == existing, "repeated materialization was not atomic");
}

void checkRejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 19; ++bad) {
    Fixture f(context), foreign(context, 1); OpBuilder b(&context);
    auto p = f.payloads[1], s = f.stages[1];
    auto metadata = [&](FModuleOp m, StringRef attr, StringRef field, Attribute value) {
      NamedAttrList fields(m->getAttrOfType<DictionaryAttr>(attr));
      if (value) fields.set(field, value); else fields.erase(field);
      m->setAttr(attr, fields.getDictionary(&context));
    };
    auto rename = [&](FModuleOp m, unsigned index) {
      SmallVector<Attribute> names(m.getPortNames().begin(), m.getPortNames().end());
      names[index] = b.getStringAttr("wrong"); m.setPortNames(names);
    };
    auto retype = [&](FModuleOp m, unsigned index, Type type) {
      SmallVector<Attribute> types(m.getPortTypes().begin(), m.getPortTypes().end());
      types[index] = TypeAttr::get(type); m.setPortTypes(types);
    };
    if (bad == 0) f.payloads[1] = {};
    if (bad == 1) f.stages[1] = {};
    if (bad == 2) f.payloads[1] = foreign.payloads[0];
    if (bad == 3) f.stages[1] = foreign.stages[0];
    if (bad == 4) p->removeAttr("goldengate.printPayload");
    if (bad == 5) s->removeAttr("goldengate.printTokenStage");
    if (bad == 6) metadata(p, "goldengate.printPayload", "bridgeTarget", b.getStringAttr(""));
    if (bad == 7) metadata(s, "goldengate.printTokenStage", "payloadModule", b.getStringAttr(f.payloads[0].getName()));
    if (bad == 8) metadata(s, "goldengate.printTokenStage", "resetPortName", b.getStringAttr("anotherDomain"));
    if (bad == 9) rename(p, 0);
    if (bad == 10) rename(s, 9);
    if (bad == 11) retype(p, 0, UIntType::get(&context, 1));
    if (bad == 12) retype(s, 8, UIntType::get(&context, 16));
    if (bad == 13) retype(p, 0, BundleType::get(&context, {{b.getStringAttr("flip"), true, UIntType::get(&context, 1)}}));
    if (bad == 14) f.stages[1] = f.stages[0]; // same width, different domain identity
    if (bad == 15) { f.payloads.push_back(f.payloads[0]); f.stages.push_back(f.stages[0]); }
    if (bad == 16) f.stages.pop_back();
    if (bad == 17) metadata(p, "goldengate.printPayload", "tokenBits", b.getI64IntegerAttr(7));
    if (bad == 18) metadata(s, "goldengate.printTokenStage", "records", b.getArrayAttr({b.getStringAttr("reordered")}));
    auto before = dump(*f.root), other = dump(*foreign.root);
    SmallVector<FModuleOp> controls{*f.circuit.getOps<FModuleOp>().begin()}; std::string error;
    require(failed(goldengate::materializePrintBridgeControls(f.circuit, f.payloads, f.stages, controls, error)),
            "accepted invalid control pair " + std::to_string(bad));
    require(!error.empty() && controls.size() == 1 && dump(*f.root) == before && dump(*foreign.root) == other,
            "control preflight rejection was not atomic " + std::to_string(bad));
  }
  Fixture empty(context, 0); auto before = dump(*empty.root);
  SmallVector<FModuleOp> controls; std::string error;
  require(succeeded(goldengate::materializePrintBridgeControls(empty.circuit, {}, {}, controls, error)) &&
      controls.empty() && dump(*empty.root) == before, "empty control list is not a no-op");
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    checkControls(context); checkRejections(context);
    llvm::outs() << "PASS PrintBridge control ROI, accepted cycles, wiring, and atomic preflight\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "FAIL: " << error.what() << "\n"; return 1;
  }
}
