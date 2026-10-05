// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/PrintBridgePayload.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
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
  SmallVector<FModuleOp> payloads, stages, controls, configs;
  Fixture(MLIRContext &context, unsigned count = 2, unsigned bits = 8) {
    root = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" {
        firrtl.module @Top() {}
        firrtl.module @GGPrintBridgeAXI() {}
        firrtl.module @GGPrintBridgeMCRFile() {}
      }
    })mlir", &context);
    require(bool(root), "AXI fixture parse failed");
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
    require(succeeded(goldengate::materializePrintBridgeConfigs(circuit, controls, configs, error)), error);
  }
  void addAnnotation(MLIRContext &context, StringRef target) {
    OpBuilder b(&context); auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> annotations(raw.begin(), raw.end());
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Nested")),
        b.getNamedAttr("nested", b.getArrayAttr({b.getDictionaryAttr({
            b.getNamedAttr("target", b.getStringAttr(target))})}))}));
    circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  }
};

void checkNasti(MLIRContext &context, FIRRTLBaseType base, unsigned addressBits, unsigned idBits) {
  auto type = cast<BundleType>(base);
  require(type.getNumElements() == 5, "wrong Nasti channel count");
  const char *names[] = {"aw", "w", "b", "ar", "r"};
  for (unsigned i = 0; i < 5; ++i) {
    auto channel = type.getElement(i);
    require(channel.name == names[i] && channel.isFlip == (i == 2 || i == 4), "wrong Nasti channel flip/order");
    auto token = cast<BundleType>(channel.type);
    require(token.getNumElements() == 3 && token.getElement(0).name == "ready" && token.getElement(0).isFlip &&
        token.getElement(1).name == "valid" && !token.getElement(1).isFlip &&
        token.getElement(2).name == "bits" && !token.getElement(2).isFlip &&
        token.getElement(0).type == UIntType::get(&context, 1) &&
        token.getElement(1).type == UIntType::get(&context, 1), "wrong Nasti decoupled fields");
    auto payload = cast<BundleType>(token.getElement(2).type);
    auto check = [&](StringRef name, unsigned width) {
      auto field = payload.getElement(name);
      require(field && !field->isFlip && field->type == UIntType::get(&context, width), "wrong Nasti field " + name.str());
    };
    check("id", idBits); check("user", 1);
    if (i == 0 || i == 3) {
      require(payload.getNumElements() == 11, "address metadata missing");
      check("addr", addressBits); check("len", 8); check("size", 3); check("burst", 2);
      check("lock", 1); check("cache", 4); check("prot", 3); check("qos", 4); check("region", 4);
    } else if (i == 1) {
      require(payload.getNumElements() == 5, "write metadata missing");
      check("data", 32); check("last", 1); check("strb", 4);
    } else {
      check("resp", 2);
      require(payload.getNumElements() == (i == 2 ? 3u : 5u), "response metadata missing");
      if (i == 4) { check("data", 32); check("last", 1); }
    }
  }
}

void checkWrappers(MLIRContext &context, unsigned addressBits, unsigned idBits, unsigned bits) {
  Fixture f(context, 2, bits);
  // Configs need not be the circuit top. No copied/internal target changes
  // are appropriate for this explicit helper boundary.
  f.addAnnotation(context, "~Top|" + f.configs[0].getName().str() + ">hBits.print.arg");
  f.addAnnotation(context, "~Top|" + f.controls[0].getName().str() + ">cycleCounter");
  f.addAnnotation(context, "~Top|" + f.configs[1].getName().str() + ">mcrOther");
  auto raw = f.circuit->getAttr("rawAnnotations"); auto originalName = f.circuit.getNameAttr();
  SmallVector<FModuleOp> wrappers; std::string error;
  require(succeeded(goldengate::materializePrintBridgeAXIControls(f.circuit, f.configs, addressBits, idBits, wrappers, error)), error);
  require(wrappers.size() == 2 && wrappers[0].getName() != "GGPrintBridgeAXI" &&
      wrappers[0].getName() != wrappers[1].getName(), "wrapper name collision");
  StringAttr previousAdapter;
  for (unsigned i = 0; i < wrappers.size(); ++i) {
    auto wrapper = wrappers[i], config = f.configs[i];
    auto info = wrapper->getAttrOfType<DictionaryAttr>("goldengate.printAXI");
    require(bool(info), "Print AXI metadata missing");
    for (auto entry : config->getAttrOfType<DictionaryAttr>("goldengate.printConfig"))
      require(info.get(entry.getName()) == entry.getValue(), "config/constructor/register metadata changed");
    auto adapterName = info.getAs<StringAttr>("mcrModule");
    require(info.getAs<StringAttr>("configModule").getValue() == config.getName() && adapterName &&
        adapterName.getValue() != "GGPrintBridgeMCRFile" && adapterName != previousAdapter &&
        info.getAs<IntegerAttr>("addressBits").getInt() == addressBits &&
        info.getAs<IntegerAttr>("idBits").getInt() == idBits, "AXI module/width collateral");
    previousAdapter = adapterName; auto adapter = named(f.circuit, adapterName.getValue());
    require(wrapper.isPublic() && wrapper.getNumPorts() == config.getNumPorts(), "wrong wrapper visibility/port count");
    for (unsigned p = 0; p < 11; ++p)
      require(wrapper.getPortName(p) == config.getPortName(p) && wrapper.getPortType(p) == config.getPortType(p) &&
          wrapper.getPortDirection(p) == config.getPortDirection(p), "copied port changed");
    require(wrapper.getPortName(11) == "ctrl" && wrapper.getPortDirection(11) == Direction::In, "ctrl not a slave port");
    checkNasti(context, cast<FIRRTLBaseType>(wrapper.getPortType(11)), addressBits, idBits);
    require(adapter.getNumPorts() == 4 && adapter.getPortName(0) == "clock" &&
        adapter.getPortName(1) == "reset" && adapter.getPortName(2) == "nasti" && adapter.getPortName(3) == "mcr",
        "adapter interface names/order");
    for (unsigned p = 0; p < 4; ++p) require(adapter.getPortDirection(p) == Direction::In, "adapter port directions");
    require(adapter.getPortType(0) == wrapper.getPortType(0) && adapter.getPortType(1) == wrapper.getPortType(1) &&
        adapter.getPortType(2) == wrapper.getPortType(11) && adapter.getPortType(3) == config.getPortType(11), "adapter/config boundary types");
    require(std::distance(adapter.getOps<RegResetOp>().begin(), adapter.getOps<RegResetOp>().end()) == 4 &&
        std::distance(adapter.getOps<RegOp>().begin(), adapter.getOps<RegOp>().end()) == 5 &&
        std::distance(adapter.getOps<AssertOp>().begin(), adapter.getOps<AssertOp>().end()) == 2, "shared transport state/assertion count");
    for (auto r : adapter.getOps<RegOp>()) {
      unsigned width = r.getName() == "bId" || r.getName() == "rId" ? idBits : r.getName() == "wData" ? 32 : 3;
      require(r.getResult().getType() == UIntType::get(&context, width), "captured ID/data/index width");
    }
    InstanceOp bank, mcr; unsigned instances = 0;
    for (auto instance : wrapper.getOps<InstanceOp>()) {
      ++instances;
      if (instance.getModuleName() == config.getName()) bank = instance;
      if (instance.getModuleName() == adapter.getName()) mcr = instance;
    }
    require(instances == 2 && bank && mcr, "wrapper instances refer to wrong domain modules");
    llvm::DenseMap<Value, Value> drivers;
    for (auto &op : *wrapper.getBodyBlock()) {
      if (auto c = dyn_cast<ConnectOp>(op)) require(drivers.try_emplace(c.getDest(), c.getSrc()).second, "multiple wrapper drivers");
      if (auto c = dyn_cast<StrictConnectOp>(op)) require(drivers.try_emplace(c.getDest(), c.getSrc()).second, "multiple wrapper drivers");
    }
    for (unsigned p = 0; p < 11; ++p) {
      Value outer = wrapper.getArgument(p), inner = bank.getResult(p);
      require(drivers.lookup(config.getPortDirection(p) == Direction::In ? inner : outer) ==
          (config.getPortDirection(p) == Direction::In ? outer : inner), "copied instance connection wrong");
    }
    require(drivers.lookup(mcr.getResult(0)) == wrapper.getArgument(0) &&
        drivers.lookup(mcr.getResult(1)) == wrapper.getArgument(1) &&
        drivers.lookup(mcr.getResult(2)) == wrapper.getArgument(11) &&
        drivers.lookup(mcr.getResult(3)) == bank.getResult(11), "transport clock/reset/Nasti/MCR wiring");
  }
  require(f.circuit.getNameAttr() == originalName && f.circuit->getAttr("rawAnnotations") == raw &&
      succeeded(verify(*f.root)), "explicit AXI mapping changed circuit/annotations or emitted invalid IR");
  auto before = dump(*f.root); unsigned size = wrappers.size();
  require(failed(goldengate::materializePrintBridgeAXIControls(f.circuit, f.configs, addressBits, idBits, wrappers, error)) &&
      !error.empty() && wrappers.size() == size && dump(*f.root) == before, "repeated materialization was not atomic");
}

void checkRejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 26; ++bad) {
    Fixture f(context), foreign(context, 1); OpBuilder b(&context); auto c = f.configs[1];
    unsigned addressBits = 25, idBits = 12;
    auto metadata = [&](StringRef key, Attribute value) {
      NamedAttrList fields(c->getAttrOfType<DictionaryAttr>("goldengate.printConfig"));
      if (value) fields.set(key, value); else fields.erase(key);
      c->setAttr("goldengate.printConfig", fields.getDictionary(&context));
    };
    if (bad == 0) addressBits = 4;
    if (bad == 1) idBits = 0;
    if (bad == 2) f.configs[1] = {};
    if (bad == 3) f.configs[1] = foreign.configs[0];
    if (bad == 4) c->removeAttr("goldengate.printConfig");
    if (bad == 5) metadata("bridgeTarget", b.getStringAttr(""));
    if (bad == 6) metadata("resetPortName", b.getStringAttr(""));
    if (bad == 7) metadata("tokenBits", b.getI64IntegerAttr(12));
    if (bad == 8) f.configs[1] = f.configs[0];
    if (bad == 9) metadata("resetPortName", b.getStringAttr("reset0"));
    if (bad == 10 || bad == 11) {
      SmallVector<Attribute> names(c.getPortNames().begin(), c.getPortNames().end());
      names[bad == 10 ? 11 : 0] = b.getStringAttr("wrong"); c.setPortNames(names);
    }
    if (bad == 12 || bad == 13 || bad == 14) {
      SmallVector<Attribute> types(c.getPortTypes().begin(), c.getPortTypes().end());
      types[bad == 12 ? 11 : bad == 13 ? 0 : 1] = TypeAttr::get(UIntType::get(&context, 2)); c.setPortTypes(types);
    }
    if (bad == 15) f.circuit->removeAttr("rawAnnotations");
    if (bad == 16 || bad == 17 || bad == 18)
      f.addAnnotation(context, "~Top|" + c.getName().str() + ">mcr" +
          (bad == 16 ? "" : bad == 17 ? ".read[0].bits" : "[0]"));
    if (bad == 19) metadata("cycleBits", b.getI64IntegerAttr(32));
    if (bad == 20) metadata("roiInclusive", b.getBoolAttr(false));
    if (bad == 21) metadata("flushPulseLength", b.getI64IntegerAttr(1)); // width8 requires64
    if (bad == 22) {
      b.setInsertionPointToEnd(named(f.circuit, "Top").getBodyBlock());
      b.create<InstanceOp>(f.circuit.getLoc(), c, "alreadyInstantiated");
    }
    if (bad == 23 || bad == 24) {
      SmallVector<Attribute> types(c.getPortTypes().begin(), c.getPortTypes().end());
      types[bad == 23 ? 4 : 8] = TypeAttr::get(bad == 23 ?
          Type(BundleType::get(&context, {{b.getStringAttr("flipped"), true, UIntType::get(&context, 1)}})) :
          Type(UIntType::get(&context, 16))); c.setPortTypes(types);
    }
    if (bad == 25) f.circuit->setAttr("rawAnnotations", b.getStringAttr("malformed"));
    auto before = dump(*f.root), other = dump(*foreign.root);
    SmallVector<FModuleOp> wrappers{*f.circuit.getOps<FModuleOp>().begin()}; std::string error;
    require(failed(goldengate::materializePrintBridgeAXIControls(f.circuit, f.configs, addressBits, idBits, wrappers, error)),
        "accepted invalid later AXI config " + std::to_string(bad));
    require(!error.empty() && wrappers.size() == 1 && dump(*f.root) == before && dump(*foreign.root) == other,
        "AXI preflight rejection mutated input/output " + std::to_string(bad));
  }
  Fixture empty(context, 0); auto before = dump(*empty.root);
  SmallVector<FModuleOp> wrappers; std::string error;
  require(succeeded(goldengate::materializePrintBridgeAXIControls(empty.circuit, {}, 25, 12, wrappers, error)) &&
      wrappers.empty() && dump(*empty.root) == before, "empty AXI collection is not a no-op");
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    checkWrappers(context, 25, 12, 8); checkWrappers(context, 5, 1, 512);
    checkRejections(context);
    llvm::outs() << "PASS PrintBridge AXI instances, domains, collateral, and atomic preflight\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "FAIL: " << error.what() << "\n"; return 1;
  }
}
