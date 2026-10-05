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
  SmallVector<FModuleOp> payloads, stages, controls;
  Fixture(MLIRContext &context, unsigned bits = 8, unsigned count = 2) {
    root = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" {
        firrtl.module @Top() {}
        firrtl.module @GGPrintBridgeConfig() {}
      }
    })mlir", &context);
    require(bool(root), "config fixture parse failed");
    circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
    SmallVector<Attribute> raw{b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test.Opaque")),
        b.getNamedAttr("value", b.getStringAttr("preserve"))})};
    for (unsigned i = 0; i < count; ++i) {
      // Record enable + argument + reset + reserved bit fills tokenBits.
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

// Trace aggregate selectors without depending on whether duplicate selectors
// have been commoned. This checks actual register drivers, not only metadata.
std::string path(Value value, Value mcr) {
  if (value == mcr) return "mcr";
  if (auto field = value.getDefiningOp<SubfieldOp>()) {
    auto type = cast<BundleType>(field.getInput().getType());
    return path(field.getInput(), mcr) + "." + type.getElement(field.getFieldIndex()).name.str();
  }
  if (auto index = value.getDefiningOp<SubindexOp>())
    return path(index.getInput(), mcr) + "[" + std::to_string(index.getIndex()) + "]";
  return "";
}
bool constant(Value value, uint64_t expected) {
  auto k = value.getDefiningOp<ConstantOp>(); return k && k.getValue() == expected;
}

void checkConfig(MLIRContext &context, unsigned bits) {
  Fixture f(context, bits); auto raw = f.circuit->getAttr("rawAnnotations");
  SmallVector<FModuleOp> configs; std::string error;
  require(succeeded(goldengate::materializePrintBridgeConfigs(f.circuit, f.controls, configs, error)), error);
  require(configs.size() == 2 && configs[0].getName() != "GGPrintBridgeConfig" &&
      configs[0].getName() != configs[1].getName(), "config module name collision");
  const char *names[] = {"startCycleL", "startCycleH", "endCycleL", "endCycleH", "doneInit", "flushNarrowPacket"};
  const unsigned copied[] = {0, 1, 3, 5, 10, 11, 12, 13, 14, 15, 16};
  unsigned pulse = bits < 512 ? 512 / bits : 1;
  for (unsigned i = 0; i < configs.size(); ++i) {
    auto m = configs[i], control = f.controls[i];
    require(m.isPublic() && m.getNumPorts() == 12, "wrong config visibility/interface");
    for (unsigned p = 0; p < 11; ++p)
      require(m.getPortName(p) == control.getPortName(copied[p]) &&
          m.getPortType(p) == control.getPortType(copied[p]) &&
          m.getPortDirection(p) == control.getPortDirection(copied[p]), "copied control port changed");
    require(m.getPortName(11) == "mcr" && m.getPortDirection(11) == Direction::Out, "missing MCR port");
    auto mcrType = cast<BundleType>(m.getPortType(11));
    require(mcrType.getNumElements() == 3 && mcrType.getElement(0).name == "read" &&
        !mcrType.getElement(0).isFlip && mcrType.getElement(1).name == "write" &&
        mcrType.getElement(1).isFlip && mcrType.getElement(2).name == "wstrb" &&
        mcrType.getElement(2).isFlip && mcrType.getElement(2).type == UIntType::get(&context, 4), "MCR field directions/types");
    auto readType = cast<FVectorType>(mcrType.getElement(0).type);
    require(readType.getNumElements() == 6 && mcrType.getElement(1).type == readType, "MCR word count/type");
    auto decoupled = cast<BundleType>(readType.getElementType());
    require(decoupled.getNumElements() == 3 && decoupled.getElement(0).name == "ready" &&
        decoupled.getElement(0).isFlip && decoupled.getElement(1).name == "valid" &&
        !decoupled.getElement(1).isFlip && decoupled.getElement(2).name == "bits" &&
        !decoupled.getElement(2).isFlip && decoupled.getElement(0).type == UIntType::get(&context, 1) &&
        decoupled.getElement(1).type == UIntType::get(&context, 1) &&
        decoupled.getElement(2).type == UIntType::get(&context, 32), "MCR decoupled field schema");
    auto info = m->getAttrOfType<DictionaryAttr>("goldengate.printConfig");
    for (auto entry : control->getAttrOfType<DictionaryAttr>("goldengate.printControl"))
      require(info.get(entry.getName()) == entry.getValue(), "control collateral changed");
    require(info.getAs<StringAttr>("controlModule").getValue() == control.getName() &&
        info.getAs<IntegerAttr>("flushPulseLength").getInt() == pulse, "config identity/pulse metadata");
    auto registers = info.getAs<ArrayAttr>("registers");
    require(registers.size() == 6, "wrong register map size");
    std::map<std::string, Value> regs;
    unsigned unreset = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) {
      ++unreset; regs[r.getName().str()] = r.getResult();
      require(r.getClockVal() == m.getArgument(0) && r.getResult().getType() == UIntType::get(&context, 32), "ROI register width/clock/reset");
    }
    for (auto r : m.getOps<RegResetOp>()) {
      ++reset; regs[r.getName().str()] = r.getResult();
      require(r.getClockVal() == m.getArgument(0) && r.getResetSignal() == m.getArgument(1) &&
          constant(r.getResetValue(), 0), "register reset/clock mismatch");
    }
    require(unreset == 4 && reset == (pulse > 1 ? 3u : 2u), "wrong number of configuration/pulse registers");
    llvm::DenseMap<Value, Value> drivers;
    std::map<std::string, Value> mcrDrivers;
    Value mcr = m.getArgument(11); InstanceOp inner;
    unsigned instances = 0;
    for (auto instance : m.getOps<InstanceOp>()) { ++instances; inner = instance; }
    require(instances == 1 && inner.getName() == "control" && inner.getModuleName() == control.getName(), "wrong control instance");
    for (auto c : m.getOps<StrictConnectOp>()) {
      require(drivers.try_emplace(c.getDest(), c.getSrc()).second, "multiple config drivers");
      auto p = path(c.getDest(), mcr); if (!p.empty()) mcrDrivers[p] = c.getSrc();
    }
    for (unsigned p = 0; p < 11; ++p) {
      Value outside = m.getArgument(p), inside = inner.getResult(copied[p]);
      require(drivers.lookup(control.getPortDirection(copied[p]) == Direction::In ? inside : outside) ==
          (control.getPortDirection(copied[p]) == Direction::In ? outside : inside), "copied instance wiring");
    }
    for (unsigned word = 0; word < 6; ++word) {
      auto entry = cast<DictionaryAttr>(registers[word]); unsigned width = word < 4 ? 32 : 1;
      require(entry.getAs<StringAttr>("name").getValue() == names[word] &&
          entry.getAs<IntegerAttr>("word").getInt() == word &&
          entry.getAs<IntegerAttr>("byteOffset").getInt() == 4 * word &&
          entry.getAs<IntegerAttr>("bits").getInt() == width &&
          entry.getAs<StringAttr>("permissions").getValue() == "ReadWrite" &&
          entry.getAs<StringAttr>("reset").getValue() == (word < 4 ? "none" : "zero"), "register map semantics");
      Value reg = regs.at(names[word]); require(reg.getType() == UIntType::get(&context, width), "register width");
      std::string wr = "mcr.write[" + std::to_string(word) + "]", rd = "mcr.read[" + std::to_string(word) + "]";
      auto next = drivers.lookup(reg).getDefiningOp<MuxPrimOp>();
      require(next && path(next.getSel(), mcr) == wr + ".valid", "write valid must have top priority");
      Value written = next.getHigh();
      if (word >= 4) {
        auto trunc = written.getDefiningOp<BitsPrimOp>();
        require(trunc && trunc.getHi() == 0 && trunc.getLo() == 0, "Bool writes must truncate bit zero");
        written = trunc.getInput();
      }
      require(path(written, mcr) == wr + ".bits", "wrong write data source");
      require(constant(mcrDrivers.at(wr + ".ready"), 1) && constant(mcrDrivers.at(rd + ".valid"), 1), "MCR ready/valid not constant true");
      Value read = mcrDrivers.at(rd + ".bits");
      if (word >= 4) {
        auto pad = read.getDefiningOp<PadPrimOp>(); require(pad && pad.getAmount() == 32, "Bool read must pad to32"); read = pad.getInput();
      }
      require(read == reg, "read data not live register");
      require(drivers.lookup(inner.getResult(word < 4 ? 6 + word : word == 4 ? 2 : 4)) == reg, "configuration word feeds wrong control input");
      if (word < 5) require(next.getLow() == reg, "ordinary register must hold without write");
      else if (pulse == 1) require(constant(next.getLow(), 0), "single-cycle flush does not clear");
      else {
        auto clear = next.getLow().getDefiningOp<MuxPrimOp>();
        require(clear && constant(clear.getHigh(), 0) && clear.getLow() == reg, "flush terminal self clear below write priority");
        auto last = clear.getSel().getDefiningOp<EQPrimOp>();
        require(last && last.getLhs() == regs.at("flushCount") && constant(last.getRhs(), pulse - 1), "wrong pulse terminal test");
      }
    }
    require(mcrDrivers.size() == 18, "unexpected MCR driver/read-ready use");
    for (auto field : m.getOps<SubfieldOp>())
      require(path(field.getResult(), mcr) != "mcr.wstrb", "strobes must be ignored");
    if (pulse > 1) {
      Value count = regs.at("flushCount"); unsigned width = pulse == 64 ? 6 : 5;
      require(count.getType() == UIntType::get(&context, width), "pulse counter width");
      auto terminal = drivers.lookup(count).getDefiningOp<MuxPrimOp>();
      require(terminal && constant(terminal.getHigh(), 0), "counter must clear unconditionally at terminal");
      auto last = terminal.getSel().getDefiningOp<EQPrimOp>();
      require(last && last.getLhs() == count && constant(last.getRhs(), pulse - 1), "counter terminal condition mismatch");
      auto increment = terminal.getLow().getDefiningOp<MuxPrimOp>();
      require(increment && increment.getSel() == regs.at("flushNarrowPacket") && increment.getLow() == count,
          "pulse counter must retain progress while flush is low");
      auto trunc = increment.getHigh().getDefiningOp<BitsPrimOp>();
      auto add = trunc ? trunc.getInput().getDefiningOp<AddPrimOp>() : AddPrimOp();
      require(trunc && trunc.getHi() == width - 1 && trunc.getLo() == 0 && add &&
          add.getLhs() == count && constant(add.getRhs(), 1), "pulse counter increment/truncation");
    }
  }
  require(f.circuit->getAttr("rawAnnotations") == raw && succeeded(verify(*f.root)), "annotations changed or invalid config IR");
  auto before = dump(*f.root); unsigned existing = configs.size();
  require(failed(goldengate::materializePrintBridgeConfigs(f.circuit, f.controls, configs, error)) &&
      !error.empty() && configs.size() == existing && dump(*f.root) == before, "repeated config materialization was not atomic");
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
    if (bad == 7) metadata("tokenBits", b.getI64IntegerAttr(1LL << 31));
    if (bad == 8) metadata("cycleBits", b.getI64IntegerAttr(32));
    if (bad == 9) metadata("roiInclusive", b.getBoolAttr(false));
    if (bad == 10) metadata("tokenBits", {});
    if (bad == 11) {
      SmallVector<Attribute> names(c.getPortNames().begin(), c.getPortNames().end());
      names[16] = b.getStringAttr("wrong"); c.setPortNames(names);
    }
    if (bad == 12 || bad == 13 || bad == 14) {
      SmallVector<Attribute> types(c.getPortTypes().begin(), c.getPortTypes().end());
      unsigned port = bad == 12 ? 14 : 10;
      Type wrong = bad == 14 ? Type(BundleType::get(&context, {{b.getStringAttr("flip"), true, UIntType::get(&context, 1)}})) :
          Type(UIntType::get(&context, 1));
      types[port] = TypeAttr::get(wrong); c.setPortTypes(types);
    }
    if (bad == 15) f.controls[1] = f.controls[0];
    if (bad == 16) metadata("resetPortName", b.getStringAttr("reset0")); // same target, duplicate reset identity
    if (bad == 17) metadata("roiInclusive", b.getStringAttr("true"));
    auto before = dump(*f.root), other = dump(*foreign.root);
    SmallVector<FModuleOp> configs{*f.circuit.getOps<FModuleOp>().begin()}; std::string error;
    require(failed(goldengate::materializePrintBridgeConfigs(f.circuit, f.controls, configs, error)),
        "accepted invalid later config control " + std::to_string(bad));
    require(!error.empty() && configs.size() == 1 && dump(*f.root) == before && dump(*foreign.root) == other,
        "config rejection mutated first candidate/output or foreign circuit " + std::to_string(bad));
  }
  Fixture empty(context, 8, 0); auto before = dump(*empty.root);
  SmallVector<FModuleOp> configs; std::string error;
  require(succeeded(goldengate::materializePrintBridgeConfigs(empty.circuit, {}, configs, error)) &&
      configs.empty() && dump(*empty.root) == before, "empty config batch is not a no-op");
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned bits : {8, 16, 512, 1024}) checkConfig(context, bits);
    checkRejections(context);
    llvm::outs() << "PASS PrintBridge config registers, MCR wiring, Pulsify structure, and atomic preflight\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "FAIL: " << error.what() << "\n"; return 1;
  }
}
