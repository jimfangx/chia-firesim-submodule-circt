// See LICENSE for license details.
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool b, const std::string &s) { if (!b) throw std::runtime_error(s); }
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
struct Fixture {
  OwningOpRef<ModuleOp> root; CircuitOp circuit;
  Fixture(MLIRContext &ctx, unsigned count=1) {
    root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
    require(bool(root),"parse fixture"); circuit=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    SmallVector<Attribute> annotations;
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Opaque")),
        b.getNamedAttr("value",b.getStringAttr("retain"))}));
    auto event=[&](StringRef name, StringRef label, unsigned bits, StringRef op) {
      return b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.widgets.EventMetadata")),
          b.getNamedAttr("portName",b.getStringAttr(name)),b.getNamedAttr("label",b.getStringAttr(label)),
          b.getNamedAttr("description",b.getStringAttr("fixture event")),b.getNamedAttr("width",b.getI64IntegerAttr(bits)),
          b.getNamedAttr("opType",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(op))}))});
    };
    for (unsigned i=0;i<count;++i) annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
        b.getNamedAttr("target",b.getStringAttr("~Top|Top>autocounter"+std::to_string(i))),
        b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.AutoCounterBridgeModule")),
        b.getNamedAttr("widgetConstructorKey",b.getDictionaryAttr({
            b.getNamedAttr("class",b.getStringAttr("midas.widgets.AutoCounterParameters")),
            b.getNamedAttr("triggerName",b.getStringAttr("trigger")),
            b.getNamedAttr("resetPortName",b.getStringAttr("globalReset")),
            b.getNamedAttr("eventMetadata",b.getArrayAttr({
                event("event0","count",64,goldengate::AnnotationClasses::AutoCounterAccumulate),
                event("event1","value",64,goldengate::AnnotationClasses::AutoCounterIdentity)}))}))}));
    circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));
  }
};
FModuleOp named(CircuitOp circuit,StringRef name) {
  for(auto m:circuit.getOps<FModuleOp>()) if(m.getName()==name)return m;
  throw std::runtime_error("missing module "+name.str());
}
void check(MLIRContext &ctx,unsigned addressBits) {
  Fixture f(ctx,2); auto raw=f.circuit->getAttr("rawAnnotations"); std::string error;
  SmallVector<FModuleOp> hosts;
  require(succeeded(goldengate::materializeAutoCounterBridgeHosts(f.circuit,addressBits,12,hosts,error)),error);
  require(hosts.size()==2 && succeeded(verify(*f.root)),"invalid hosts");
  require(f.circuit->getAttr("rawAnnotations")==raw && f.circuit.getName()=="Top","input metadata changed");
  for(auto host:hosts) {
    auto info=host->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");
    require(info && info.getAs<IntegerAttr>("addressBits").getInt()==addressBits &&
        info.getAs<IntegerAttr>("sampleQueueDepth").getInt()==2,"constructor/queue metadata");
    require(host.getNumPorts()==8 && host.getPortName(7)=="ctrl","host interface");
    auto bank=named(f.circuit,info.getAs<StringAttr>("bankModule").getValue());
    auto rows=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    require(rows.size()==13,"register count");
    const char *names[]={"autocounter_low_local_cycle","autocounter_high_local_cycle", "autocounter_low_count",
        "autocounter_high_count","autocounter_low_value","autocounter_high_value", "cycles_low","cycles_high",
        "readrate_low","readrate_high","init_done","countersready","readdone"};
    for(unsigned i=0;i<rows.size();++i) {
      auto row=cast<DictionaryAttr>(rows[i]);
      require(row.getAs<StringAttr>("name")==names[i] && row.getAs<IntegerAttr>("offset").getInt()==4*i &&
          row.getAs<BoolAttr>("readable").getValue()==!(i>=8&&i<=10) &&
          row.getAs<BoolAttr>("writeable").getValue()==(i>=8&&i<=10||i==12),"SFC MMIO ABI");
    }
  }
  auto before=dump(*f.root); auto count=hosts.size();
  require(failed(goldengate::materializeAutoCounterBridgeHosts(f.circuit,addressBits,12,hosts,error)) &&
      dump(*f.root)==before && hosts.size()==count,"collision rejected atomically");
  Fixture bad(ctx,2); OpBuilder b(&ctx);
  auto annotations=bad.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> changed(annotations.begin(),annotations.end());
  auto bridge=cast<DictionaryAttr>(changed.back()); SmallVector<NamedAttribute> fields(bridge.begin(),bridge.end());
  for(auto &field:fields) if(field.getName()=="widgetConstructorKey") field.setValue(b.getDictionaryAttr({}));
  changed.back()=b.getDictionaryAttr(fields); bad.circuit->setAttr("rawAnnotations",b.getArrayAttr(changed));
  before=dump(*bad.root); hosts.clear();
  require(failed(goldengate::materializeAutoCounterBridgeHosts(bad.circuit,addressBits,12,hosts,error)) &&
      dump(*bad.root)==before && hosts.empty(),"later malformed constructor changed earlier state");
  // The isolated Scala BridgeIO constructor serializer can erase nested
  // singleton types to {} without hints. Never guess Accumulate vs Identity.
  Fixture ambiguous(ctx); auto rawEvents=ambiguous.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> ambiguousAnnos(rawEvents.begin(),rawEvents.end());
  auto ambiguousBridge=cast<DictionaryAttr>(ambiguousAnnos.back());
  auto key=ambiguousBridge.getAs<DictionaryAttr>("widgetConstructorKey");
  auto events=key.getAs<ArrayAttr>("eventMetadata"); SmallVector<Attribute> eventList(events.begin(),events.end());
  auto event=cast<DictionaryAttr>(eventList.front()); SmallVector<NamedAttribute> eventFields(event.begin(),event.end());
  for(auto &field:eventFields) if(field.getName()=="opType") field.setValue(b.getDictionaryAttr({}));
  eventList.front()=b.getDictionaryAttr(eventFields); SmallVector<NamedAttribute> keyFields(key.begin(),key.end());
  for(auto &field:keyFields) if(field.getName()=="eventMetadata") field.setValue(b.getArrayAttr(eventList));
  SmallVector<NamedAttribute> bridgeFields(ambiguousBridge.begin(),ambiguousBridge.end());
  for(auto &field:bridgeFields) if(field.getName()=="widgetConstructorKey") field.setValue(b.getDictionaryAttr(keyFields));
  ambiguousAnnos.back()=b.getDictionaryAttr(bridgeFields);
  ambiguous.circuit->setAttr("rawAnnotations",b.getArrayAttr(ambiguousAnnos)); before=dump(*ambiguous.root);
  require(failed(goldengate::materializeAutoCounterBridgeHosts(ambiguous.circuit,addressBits,12,hosts,error)) &&
      dump(*ambiguous.root)==before && hosts.empty(),"ambiguous op metadata guessed instead of rejected");
  Fixture widths(ctx); before=dump(*widths.root);
  require(failed(goldengate::materializeAutoCounterBridgeHosts(widths.circuit,31,12,hosts,error)) &&
      dump(*widths.root)==before,"unsupported platform accepted");
}
}
int main(int argc,char **argv) {
  try {
    MLIRContext ctx; ctx.getOrLoadDialect<FIRRTLDialect>();ctx.getOrLoadDialect<circt::hw::HWDialect>();
    check(ctx,25);check(ctx,32);
    if(argc==2) {
      Fixture f(ctx); SmallVector<FModuleOp> hosts; std::string error;
      require(succeeded(goldengate::materializeAutoCounterBridgeHosts(f.circuit,32,12,hosts,error)),error);
      OpBuilder b(&ctx); f.circuit.setNameAttr(b.getStringAttr(hosts[0].getName()));
      std::error_code ec; llvm::raw_fd_ostream out(argv[1],ec); require(!ec,"open output"); f.root->print(out);
    }
    llvm::outs()<<"PASS AutoCounter CSV engine materialization, F2/F1 MMIO, multiple widgets and atomic rejection\n";
  }catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
