// See LICENSE for license details.
#include "goldengate/NICHeader.h"
#include "goldengate/NICMMIOBank.h"
#include "goldengate/CPUStreamQueue.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
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
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s);
  op->print(out,OpPrintingFlags().printGenericOpForm()); return s; }
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGNICCPUStreamResponseBufferWrapper" {
      firrtl.module @GGNICCPUStreamResponseBufferWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %nic_macAddr: !firrtl.uint<48>, in %nic_rlimit_inc: !firrtl.uint<8>,
        in %nic_rlimit_period: !firrtl.uint<8>, in %nic_rlimit_size: !firrtl.uint<8>,
        in %nic_pauser_threshold: !firrtl.uint<16>, in %nic_pauser_quanta: !firrtl.uint<16>,
        in %nic_pauser_refresh: !firrtl.uint<16>, out %nic_done: !firrtl.uint<1>,
        in %other: !firrtl.uint<8>) {}
    } })", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  SmallVector<Attribute> targets;
  targets.push_back(b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper"));
  for (auto p : named(c, c.getName()).getPorts())
    targets.push_back(b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper|GGNICCPUStreamResponseBufferWrapper>" + p.name.str()));
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Nested")), b.getNamedAttr("targets", b.getArrayAttr(targets))})}));
  return root;
}

// The bank is produced by the native hardware transform. Synthetic bindings
// isolate header allocation from the much larger platform; production replay
// below uses the complete independently validated CPU/MMIO transport instead.
OwningOpRef<ModuleOp> allocated(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&ctx); std::string error;
  auto output = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("previous\n")),
    b.getNamedAttr("goldengate.cpuManagedStreamHeader",b.getBoolAttr(true))});
  auto nic = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("widgetClass",b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule"))});
  c->setAttr("rawAnnotations",b.getArrayAttr({output,nic}));
  require(succeeded(goldengate::addNICMMIOBank(c,error)),error);
  auto top = named(c,c.getName());
  auto queue = goldengate::createCPUStreamQueue3072(c,"GGNICCPUStreamQueue3072");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto owner = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGNICCPUStreamQueuesWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  b.setInsertionPointToStart(owner.getBodyBlock());
  for (bool incoming : {false,true}) {
    auto inst = b.create<InstanceOp>(c.getLoc(),queue,incoming ? "incoming" : "outgoing");
    inst->setAttr("goldengate.streamParameters",b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr(incoming ? "SIMPLENICBRIDGEMODULE_3_from_cpu_stream" : "SIMPLENICBRIDGEMODULE_3_to_cpu_stream")),
      b.getNamedAttr("direction",b.getStringAttr(incoming ? "fromHostCPU" : "toHostCPU")),
      b.getNamedAttr("depth",b.getI64IntegerAttr(3072)),b.getNamedAttr("widthBytes",b.getI64IntegerAttr(64))}));
  }
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{
    {b.getStringAttr("addr"),UIntType::get(&ctx,25,false),Direction::In}});
  auto region = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("SimpleNICBridgeModule_3")),
    b.getNamedAttr("slave",b.getI32IntegerAttr(0)),b.getNamedAttr("start",b.getI64IntegerAttr(4096)),b.getNamedAttr("size",b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({region}));
  SmallVector<FModuleOp> helpers{decoder,owner};
  auto binding = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("SimpleNICBridgeModule_3")),
    b.getNamedAttr("port",b.getStringAttr("nicBridge_ctrl")),b.getNamedAttr("slave",b.getI32IntegerAttr(0))});
  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper","goldengate.controlWriteBindings"),
      std::make_pair("GGControlReadDispatchWrapper","goldengate.controlReadBindings")}) {
    auto m = b.create<FModuleOp>(c.getLoc(),b.getStringAttr(pair.first),top.getConventionAttr(),ArrayRef<PortInfo>{});
    m->setAttr(pair.second,b.getArrayAttr({binding})); helpers.push_back(m);
  }
  for (bool incoming : {false,true}) {
    auto m = b.create<FModuleOp>(c.getLoc(),b.getStringAttr(incoming ? "GGCPUStreamWrite" : "GGCPUStreamRead"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    SmallVector<Attribute> streams;
    // NIC indices deliberately differ between directions and are nonzero.
    for (unsigned i = 0; i <= (incoming ? 2U : 1U); ++i) {
      bool last = i == (incoming ? 2U : 1U);
      streams.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name",b.getStringAttr(last ? (incoming ? "SIMPLENICBRIDGEMODULE_3_from_cpu_stream" : "SIMPLENICBRIDGEMODULE_3_to_cpu_stream") : "other")),
        b.getNamedAttr("index",b.getI64IntegerAttr(i)),b.getNamedAttr("depth",b.getI64IntegerAttr(3072)),b.getNamedAttr("widthBytes",b.getI64IntegerAttr(64))}));
    }
    m->setAttr(incoming ? "goldengate.sinkStreams" : "goldengate.sourceStreams",b.getArrayAttr(streams)); helpers.push_back(m);
  }
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) b.create<InstanceOp>(c.getLoc(),m,m.getName());
  return root;
}
void mutate(CircuitOp c, unsigned bad) {
  OpBuilder b(c.getContext()); auto bank = named(c,"GGNICMMIOBank");
  if (bad < 24) {
    unsigned i = bad/4; auto rows = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> changed(rows.begin(),rows.end()); NamedAttrList d(cast<DictionaryAttr>(rows[i]));
    if (bad%4 == 0) d.set("name",b.getStringAttr("wrong"));
    if (bad%4 == 1) d.set("offset",b.getI32IntegerAttr(i*4+4));
    if (bad%4 == 2) d.set("readable",b.getBoolAttr(i != 5));
    if (bad%4 == 3) d.set("writeable",b.getBoolAttr(false));
    changed[i] = d.getDictionary(c.getContext()); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(changed));
  } else if (bad < 30) {
    unsigned index = 0;
    for (auto op : bank.getOps<StrictConnectOp>()) if (isa<RegOp>(op.getDest().getDefiningOp())) {
      if (index++ == bad-24) { op.erase(); break; }
    }
  } else if (bad < 37) {
    for (auto op : bank.getOps<StrictConnectOp>()) if (op.getDest() == bank.getArgument(2+bad-30)) { op.erase(); break; }
  } else if (bad < 42) {
    unsigned index = 0; for (auto a : bank.getOps<AssertOp>()) if (index++ == bad-37) { a.erase(); break; }
  } else if (bad < 48) {
    unsigned index = 0; for (auto r : bank.getOps<RegOp>()) if (index++ == bad-42) { r.setName("wrong"); break; }
  } else if (bad < 54) {
    unsigned index = 0; for (auto op : bank.getOps<StrictConnectOp>()) if (isa<RegOp>(op.getDest().getDefiningOp())) {
      if (index++ != bad-48) continue;
      auto mux = op.getSrc().getDefiningOp<MuxPrimOp>(); mux->setOperand(2,mux.getHigh()); break;
    }
  } else if (bad < 66) {
    bool incoming = bad >= 60; auto m = named(c,incoming ? "GGCPUStreamWrite" : "GGCPUStreamRead");
    StringRef key = incoming ? "goldengate.sinkStreams" : "goldengate.sourceStreams";
    auto rows = m->getAttrOfType<ArrayAttr>(key); SmallVector<Attribute> changed(rows.begin(),rows.end());
    NamedAttrList d(cast<DictionaryAttr>(changed.back()));
    switch ((bad-54)%6) {
      case 0: d.set("depth",b.getI64IntegerAttr(3071)); break;
      case 1: d.set("widthBytes",b.getI64IntegerAttr(32)); break;
      case 2: d.set("index",b.getI64IntegerAttr(99)); break;
      case 3: d.set("name",b.getStringAttr("missing")); break;
      case 4: d.erase("depth"); break;
      case 5: d.erase("index"); break;
    }
    changed.back() = d.getDictionary(c.getContext()); m->setAttr(key,b.getArrayAttr(changed));
  } else if (bad < 71) {
    auto decoder = named(c,"GGControlAddressDecode"); auto rows = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
    NamedAttrList row(cast<DictionaryAttr>(rows[0]));
    if (bad == 66) row.set("start",b.getI64IntegerAttr(4097));
    if (bad == 67) row.set("size",b.getI64IntegerAttr(20));
    if (bad == 68) row.set("start",b.getI64IntegerAttr(1<<25));
    if (bad == 69) row.set("name",b.getStringAttr("Other_0"));
    if (bad == 70) row.set("slave",b.getI32IntegerAttr(1));
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({row.getDictionary(c.getContext())}));
  } else if (bad == 71) named(c,"GGControlReadDispatchWrapper")->removeAttr("goldengate.controlReadBindings");
  else if (bad == 72) { b.setInsertionPointToStart(named(c,c.getName()).getBodyBlock()); b.create<InstanceOp>(c.getLoc(),bank,"duplicateBank"); }
  else if (bad >= 78) {
    auto owner = named(c,"GGNICCPUStreamQueuesWrapper"), queue = named(c,"GGNICCPUStreamQueue3072");
    if (bad == 78) for (auto mem : queue.getOps<MemOp>()) mem.setDepthAttr(b.getI64IntegerAttr(3071));
    if (bad == 79) { auto inst = *owner.getOps<InstanceOp>().begin(); inst->removeAttr("goldengate.streamParameters"); }
    if (bad == 80) { auto inst = *owner.getOps<InstanceOp>().begin(); b.setInsertionPoint(inst); b.clone(*inst); }
    if (bad == 81) { auto inst = *owner.getOps<InstanceOp>().begin();
      NamedAttrList info(inst->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"));
      info.set("direction",b.getStringAttr("wrong")); inst->setAttr("goldengate.streamParameters",info.getDictionary(c.getContext())); }
  } else {
    auto rows = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> changed(rows.begin(),rows.end());
    NamedAttrList d(cast<DictionaryAttr>(changed[0]));
    if (bad == 73) d.erase("goldengate.cpuManagedStreamHeader");
    if (bad == 74) d.set("goldengate.nicHeader",b.getBoolAttr(true));
    if (bad == 75) d.erase("body");
    changed[0] = d.getDictionary(c.getContext());
    if (bad == 76) changed.push_back(changed[0]);
    c->setAttr("rawAnnotations",b.getArrayAttr(changed));
    if (bad == 77) c->removeAttr("rawAnnotations");
  }
}
}
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>(); std::string error;
    if (argc == 3) {
      auto root = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(root),"recorded boundary parse failed");
      require(succeeded(verify(*root)),"recorded input IR invalid"); auto c = *root->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(c,"FireSim",error)),error);
      require(succeeded(goldengate::prepareCPUManagedStreamHeader(c,error)),error);
      auto before = c->getAttr("rawAnnotations");
      SmallVector<std::string> bodies; for (auto m : c.getOps<FModuleOp>()) bodies.push_back(dump(m));
      require(succeeded(goldengate::prepareNICHeader(c,error)),error);
      require(succeeded(verify(*root)),"recorded output IR invalid");
      unsigned i = 0; for (auto m : c.getOps<FModuleOp>()) require(dump(m) == bodies[i++],"header changed hardware");
      require(i == bodies.size(),"module count changed");
      require(succeeded(goldengate::emitAllAnnotations(c,std::string(argv[2])+"/nic-all.json",error)),error);
      require(succeeded(goldengate::emitOutputFiles(c,argv[2],"FireSim-generated",error)),error);
      auto after = c->getAttr("rawAnnotations");
      require(failed(goldengate::prepareNICHeader(c,error)) && c->getAttr("rawAnnotations") == after,"duplicate header mutated IR");
      require(before != after,"header missing");
      llvm::outs() << "PASS recorded NIC header; hardware preserved, verified FIRRTL, atomic duplicate rejection\n";
      return 0;
    }
    auto root = allocated(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(succeeded(goldengate::prepareNICHeader(c,error)),error);
    auto after = c->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(after.size() == before.size() && after[1] == before[1],"unrelated annotations changed");
    auto body = cast<DictionaryAttr>(after[0]).getAs<StringAttr>("body").getValue();
    require(body.contains(".macaddr_upper = 4096") && body.contains(".done = 4116") &&
      body.contains("},\n  3,\n  args,\n  1,\n  3072,\n  2,\n  3072"),"allocation-derived constructor differs");
    for (unsigned bad = 0; bad < 82; ++bad) {
      auto invalid = allocated(ctx); auto ic = *invalid->getOps<CircuitOp>().begin(); mutate(ic,bad);
      auto old = dump(*invalid); require(failed(goldengate::prepareNICHeader(ic,error)),"invalid accepted: " + std::to_string(bad));
      require(dump(*invalid) == old,"rejection mutated IR: " + std::to_string(bad));
    }
    llvm::outs() << "PASS SimpleNIC header with relocated MMIO, nonzero independent stream/widget indices and 82 atomic rejections\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
