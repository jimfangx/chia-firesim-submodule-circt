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
#include <map>
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
llvm::DenseMap<Value, Value> drivers(FModuleOp module) {
  llvm::DenseMap<Value, Value> result;
  for (auto &op : *module.getBodyBlock()) {
    if (auto c = dyn_cast<ConnectOp>(op)) require(result.try_emplace(c.getDest(), c.getSrc()).second, "multiple host drivers");
    if (auto c = dyn_cast<StrictConnectOp>(op)) require(result.try_emplace(c.getDest(), c.getSrc()).second, "multiple host drivers");
  }
  return result;
}
InstanceOp instanceOf(FModuleOp module, FModuleOp child) {
  InstanceOp found;
  for (auto i : module.getOps<InstanceOp>()) if (i.getModuleName() == child.getName()) {
    require(!found, "duplicate child instance"); found = i;
  }
  require(bool(found), "expected child instance missing"); return found;
}
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  SmallVector<FModuleOp> payloads, stages, controls;
  Fixture(MLIRContext &context, unsigned bits = 8, unsigned count = 2) {
    root = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" {
        firrtl.module @Top() {}
        firrtl.module @GGPrintBridgeHost() {}
        firrtl.module @GGPrintBridgeStream() {}
        firrtl.module @GGPrintBridgeStreamAdapter() {}
        firrtl.module @GGPrintBridgeStreamConfig() {}
        firrtl.module @GGPrintBridgeMCRFile() {}
        firrtl.module @GGPrintBridgeHostMCRFile() {}
      }
    })mlir", &context);
    require(bool(root), "host fixture parse failed");
    circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
    SmallVector<Attribute> raw{b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Opaque")),
        b.getNamedAttr("value", b.getStringAttr("preserve"))})};
    for (unsigned i = 0; i < count; ++i) {
      auto record = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("print")),
          b.getNamedAttr("format", b.getStringAttr("value=%x\n")),
          b.getNamedAttr("ports", b.getArrayAttr({
              b.getDictionaryAttr({b.getNamedAttr("enable", b.getStringAttr("UInt<1>"))}),
              b.getDictionaryAttr({b.getNamedAttr("arg", b.getStringAttr("UInt<" + std::to_string(bits - 3) + ">"))})}))});
      auto key = b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeParameters)),
          b.getNamedAttr("resetPortName", b.getStringAttr("reset" + std::to_string(i))),
          b.getNamedAttr("printPorts", b.getArrayAttr({record}))});
      raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
          b.getNamedAttr("widgetClass", b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeModule)),
          b.getNamedAttr("target", b.getStringAttr("~Top|Top>synthesizedPrintf")), b.getNamedAttr("widgetConstructorKey", key)}));
    }
    circuit->setAttr("rawAnnotations", b.getArrayAttr(raw)); std::string error;
    require(succeeded(goldengate::materializePrintBridgePayloads(circuit, payloads, error)), error);
    require(succeeded(goldengate::materializePrintBridgeTokenStages(circuit, payloads, stages, error)), error);
    require(succeeded(goldengate::materializePrintBridgeControls(circuit, payloads, stages, controls, error)), error);
  }
  void addReference(MLIRContext &context, StringRef target) {
    OpBuilder b(&context); auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> all(raw.begin(), raw.end());
    all.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Nested")),
        b.getNamedAttr("values", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("target", b.getStringAttr(target))})}))}));
    circuit->setAttr("rawAnnotations", b.getArrayAttr(all));
  }
};
void countInstances(CircuitOp circuit, FModuleOp module, std::map<std::string, unsigned> &counts, std::set<std::string> &path) {
  require(path.insert(module.getName().str()).second, "recursive composed host hierarchy");
  for (auto i : module.getOps<InstanceOp>()) {
    ++counts[i.getModuleName().str()]; countInstances(circuit, named(circuit, i.getModuleName()), counts, path);
  }
  path.erase(module.getName().str());
}

void checkHosts(MLIRContext &context, unsigned bits) {
  Fixture f(context, bits);
  for (auto control : f.controls) for (auto field : {"tokenData", "bufferReady", "hBits.print.arg"})
    f.addReference(context, "~Top|" + control.getName().str() + ">" + field);
  f.addReference(context, "~Top|GGPrintBridgeStreamConfig_2>mcrOther");
  auto raw = f.circuit->getAttr("rawAnnotations"); auto name = f.circuit.getNameAttr();
  std::map<Operation *, std::string> originals;
  for (auto &op : *f.circuit.getBodyBlock()) originals[&op] = dump(&op);
  SmallVector<FModuleOp> hosts; std::string error;
  require(succeeded(goldengate::materializePrintBridgeHosts(f.circuit, f.controls, 25, 12, hosts, error)), error);
  require(hosts.size() == 2 && hosts[0].getName() != "GGPrintBridgeHost" && hosts[0].getName() != hosts[1].getName(), "host naming/domain collision");
  const char *portNames[] = {"hostClock", "hostReset", "hValid", "hBits", "hReady", "fromHostValid",
      "currentCycle", "enable", "streamReady", "streamValid", "streamData", "ctrl"};
  const char *registerNames[] = {"startCycleL", "startCycleH", "endCycleL", "endCycleH", "doneInit", "flushNarrowPacket"};
  for (unsigned n = 0; n < hosts.size(); ++n) {
    auto host = hosts[n]; auto info = host->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    require(info && host.isPublic() && host.getNumPorts() == 12, "host metadata/interface missing");
    for (auto field : f.controls[n]->getAttrOfType<DictionaryAttr>("goldengate.printControl"))
      require(info.get(field.getName()) == field.getValue(), "host changed constructor/control identity");
    require(info.getAs<StringAttr>("controlModule").getValue() == f.controls[n].getName() &&
        info.getAs<IntegerAttr>("addressBits").getInt() == 25 && info.getAs<IntegerAttr>("idBits").getInt() == 12,
        "host original control/MMIO width metadata");
    auto bank = named(f.circuit, info.getAs<StringAttr>("configModule").getValue());
    auto stream = named(f.circuit, info.getAs<StringAttr>("streamModule").getValue());
    auto adapter = named(f.circuit, info.getAs<StringAttr>("adapterModule").getValue());
    auto transport = named(f.circuit, info.getAs<StringAttr>("mcrModule").getValue());
    auto bankInfo = bank->getAttrOfType<DictionaryAttr>("goldengate.printStreamConfig");
    require(bankInfo && bankInfo.getAs<StringAttr>("streamModule").getValue() == stream.getName() &&
        bankInfo.getAs<StringAttr>("controlModule").getValue() == f.controls[n].getName(), "bank lost original control/stream distinction");
    for (auto field : bankInfo) require(info.get(field.getName()) == field.getValue(), "host changed bank/register/stream collateral");
    for (unsigned p = 0; p < 12; ++p) {
      require(host.getPortName(p) == portNames[p] &&
          host.getPortDirection(p) == (p < 4 || p == 8 || p == 11 ? Direction::In : Direction::Out), "host port order/direction");
      if (p < 11) require(host.getPortType(p) == bank.getPortType(p), "host changed copied bank port type");
    }
    require(host.getPortType(11) == transport.getPortType(2) && transport.getPortType(3) == bank.getPortType(11), "AXI/MCR boundary mismatch");
    require(host.getPortType(3) == f.controls[n].getPortType(10) && host.getPortType(6) == UIntType::get(&context, 64) &&
        host.getPortType(10) == UIntType::get(&context, 512), "host payload/counter/stream width mismatch");
    auto hi = instanceOf(host, bank), ti = instanceOf(host, transport);
    auto hd = drivers(host);
    for (unsigned p = 0; p < 11; ++p) {
      Value outer = host.getArgument(p), inner = hi.getResult(p);
      require(hd.lookup(bank.getPortDirection(p) == Direction::In ? inner : outer) ==
          (bank.getPortDirection(p) == Direction::In ? outer : inner), "host copied bank connection");
    }
    require(hd.lookup(ti.getResult(0)) == host.getArgument(0) && hd.lookup(ti.getResult(1)) == host.getArgument(1) &&
        hd.lookup(ti.getResult(2)) == host.getArgument(11) && hd.lookup(ti.getResult(3)) == hi.getResult(11), "host transport wiring");
    auto si = instanceOf(bank, stream); auto bd = drivers(bank);
    auto ci = instanceOf(stream, f.controls[n]), ai = instanceOf(stream, adapter); auto sd = drivers(stream);
    std::map<std::string, Value> registers;
    for (auto r : bank.getOps<RegOp>()) registers[r.getName().str()] = r.getResult();
    for (auto r : bank.getOps<RegResetOp>()) registers[r.getName().str()] = r.getResult();
    for (unsigned word = 0; word < 6; ++word) {
      Value reg = registers.at(registerNames[word]);
      require(bd.lookup(si.getResult(word < 4 ? 5 + word : word == 4 ? 2 : 4)) == reg, "bank programmable input wiring");
    }
    require(sd.lookup(ci.getResult(4)) == stream.getArgument(4) && sd.lookup(ai.getResult(4)) == stream.getArgument(4),
        "control and width adapter do not share registered flush");
    require(sd.lookup(ci.getResult(5)) == ai.getResult(6) && sd.lookup(ai.getResult(2)) == ci.getResult(13) &&
        sd.lookup(ai.getResult(3)) == ci.getResult(14), "stream feedback/token wiring");
    std::map<std::string, unsigned> reachable; std::set<std::string> path;
    countInstances(f.circuit, host, reachable, path);
    require(reachable[f.controls[n].getName().str()] == 1 && reachable[f.stages[n].getName().str()] == 1 &&
        reachable[f.payloads[n].getName().str()] == 1 && reachable[bank.getName().str()] == 1 &&
        reachable[stream.getName().str()] == 1 && reachable[adapter.getName().str()] == 1 &&
        reachable[transport.getName().str()] == 1 && reachable.size() == 7, "composed host duplicates stateful control/payload/stage");
    require(!reachable.count(f.controls[1-n].getName().str()), "host crossed reset/domain identity");
    unsigned counters = 0;
    for (auto [moduleName, count] : reachable) for (auto r : named(f.circuit, moduleName).getOps<RegResetOp>())
      if (r.getName() == "cycleCounter") counters += count;
    require(counters == 1, "composed host has multiple cycle counters");
  }
  for (auto [op, text] : originals) require(op->getParentOp() == f.circuit && dump(op) == text, "transaction committed altered/cloned original modules");
  require(f.circuit->getAttr("rawAnnotations") == raw && f.circuit.getNameAttr() == name && succeeded(verify(*f.root)),
      "host mutated annotations/circuit name or emitted invalid IR");
  auto before = dump(*f.root); auto size = hosts.size();
  require(failed(goldengate::materializePrintBridgeHosts(f.circuit, f.controls, 25, 12, hosts, error)) &&
      !error.empty() && dump(*f.root) == before && hosts.size() == size, "repeated host transaction not atomic");
}

void checkRejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 22; ++bad) {
    Fixture f(context), foreign(context, 8, 1); OpBuilder b(&context); auto c = f.controls[1];
    unsigned address = 25, ids = 12;
    auto metadata = [&](StringRef key, Attribute value) {
      NamedAttrList fields(c->getAttrOfType<DictionaryAttr>("goldengate.printControl"));
      if (value) fields.set(key, value); else fields.erase(key);
      c->setAttr("goldengate.printControl", fields.getDictionary(&context));
    };
    if (bad == 0) address = 4;
    if (bad == 1) ids = 0;
    if (bad == 2) f.controls[1] = {};
    if (bad == 3) f.controls[1] = foreign.controls[0];
    if (bad == 4) c->removeAttr("goldengate.printControl");
    if (bad == 5) metadata("bridgeTarget", b.getStringAttr(""));
    if (bad == 6) metadata("resetPortName", b.getStringAttr(""));
    if (bad == 7) metadata("tokenBits", b.getI64IntegerAttr(12));
    if (bad == 8) metadata("cycleBits", b.getI64IntegerAttr(32));
    if (bad == 9) metadata("roiInclusive", b.getBoolAttr(false));
    if (bad == 10) f.controls[1] = f.controls[0];
    if (bad == 11) metadata("resetPortName", b.getStringAttr("reset0"));
    if (bad == 12) {
      SmallVector<Attribute> names(c.getPortNames().begin(), c.getPortNames().end());
      names[14] = b.getStringAttr("wrong"); c.setPortNames(names);
    }
    if (bad == 13 || bad == 14) {
      SmallVector<Attribute> types(c.getPortTypes().begin(), c.getPortTypes().end());
      types[bad == 13 ? 14 : 10] = TypeAttr::get(UIntType::get(&context, 1)); c.setPortTypes(types);
    }
    if (bad == 15) f.circuit->removeAttr("rawAnnotations");
    if (bad == 16) f.circuit->setAttr("rawAnnotations", b.getStringAttr("malformed"));
    // Failure after streams/banks are created in the staging circuit must
    // still leave the live circuit and caller's output vector untouched.
    if (bad == 17) f.addReference(context, "~Top|GGPrintBridgeStreamConfig_2>mcr.read[0].bits");
    if (bad == 18) metadata("tokenBits", {});
    if (bad == 19) metadata("roiInclusive", b.getStringAttr("true"));
    if (bad == 20) named(f.circuit, "GGPrintBridgeHost")->setAttr("goldengate.printHost",
        f.controls[1]->getAttr("goldengate.printControl"));
    if (bad == 21) {
      SmallVector<FModuleOp> prior; std::string error;
      require(succeeded(goldengate::materializePrintBridgeStreams(f.circuit, {f.controls[1]}, prior, error)), error);
    }
    auto before = dump(*f.root), other = dump(*foreign.root);
    SmallVector<FModuleOp> hosts{*f.circuit.getOps<FModuleOp>().begin()}; std::string error;
    require(failed(goldengate::materializePrintBridgeHosts(f.circuit, f.controls, address, ids, hosts, error)),
        "accepted invalid later host control " + std::to_string(bad));
    require(!error.empty() && hosts.size() == 1 && dump(*f.root) == before && dump(*foreign.root) == other,
        "host transaction failure mutated live circuit/output " + std::to_string(bad));
  }
  Fixture empty(context, 8, 0); auto before = dump(*empty.root);
  SmallVector<FModuleOp> hosts; std::string error;
  require(succeeded(goldengate::materializePrintBridgeHosts(empty.circuit, {}, 25, 12, hosts, error)) &&
      hosts.empty() && dump(*empty.root) == before, "empty host transaction not a no-op");
}

void checkStreamBankRejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 8; ++bad) {
    Fixture f(context); OpBuilder b(&context); SmallVector<FModuleOp> streams;
    std::string error;
    require(succeeded(goldengate::materializePrintBridgeStreams(f.circuit, f.controls, streams, error)), error);
    auto stream = streams[1]; NamedAttrList metadata(stream->getAttrOfType<DictionaryAttr>("goldengate.printStream"));
    if (bad == 0) metadata.set("streamBits", b.getI64IntegerAttr(256));
    if (bad == 1) metadata.set("adapterDepth", b.getI64IntegerAttr(6144));
    if (bad == 2) metadata.set("packingRatio", b.getI64IntegerAttr(1));
    if (bad == 3) metadata.set("lowSliceFirst", b.getBoolAttr(false));
    if (bad == 4) metadata.set("narrowFlushInjection", b.getBoolAttr(false));
    if (bad == 5) metadata.set("controlModule", b.getStringAttr(""));
    if (bad == 6) metadata.erase("adapterModule");
    if (bad == 7) {
      SmallVector<Attribute> names(stream.getPortNames().begin(), stream.getPortNames().end());
      names[16] = b.getStringAttr("wrong"); stream.setPortNames(names);
    }
    stream->setAttr("goldengate.printStream", metadata.getDictionary(&context));
    auto before = dump(*f.root); SmallVector<FModuleOp> banks{*f.circuit.getOps<FModuleOp>().begin()};
    require(failed(goldengate::materializePrintBridgeStreamConfigs(f.circuit, streams, banks, error)) &&
        !error.empty() && banks.size() == 1 && dump(*f.root) == before,
        "stream bank accepted malformed later stream or mutated batch " + std::to_string(bad));
  }
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned bits : {8, 512, 1024}) checkHosts(context, bits);
    checkRejections(context); checkStreamBankRejections(context);
    llvm::outs() << "PASS PrintBridge host composition, shared flush/state, and atomic staging\n"; return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "FAIL: " << error.what() << "\n"; return 1;
  }
}
