// See LICENSE for license details.
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/CPUStreamCountBank.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <stdexcept>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) {
  // Generic printing avoids expensive FIRRTL SSA name generation for the
  // recorded target while retaining every operand, type and attribute.
  std::string s; llvm::raw_string_ostream out(s);
  op->print(out, OpPrintingFlags().printGenericOpForm()); return s;
}
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
// Use native queue, read, count and empty-write transforms. Only the control
// allocator/binding modules are synthetic; count and stream forwarding are real.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGTracerVBridgeControlWrapper" {
    firrtl.module @GGTracerVBridgeControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>) {}
  } })", &ctx);
  require(bool(root),"fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); std::string error;
  auto output = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("previous body\n"))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("value",b.getI32IntegerAttr(9))});
  c->setAttr("rawAnnotations",b.getArrayAttr({other,output}));
  require(succeeded(goldengate::addTracerVStreamQueue(c,error)),error);
  require(succeeded(goldengate::addCPUStreamRead(c,error)),error);
  require(succeeded(goldengate::addCPUStreamCountBank(c,error)),error);
  auto top = named(c,c.getName()), bank = named(c,"GGCPUStreamCountBank");
  auto queue = named(c,"GGTracerVStreamQueue6144"), read = named(c,"GGCPUStreamRead");
  auto eroot = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGCPUStreamControlWrapper" {
    firrtl.module @GGCPUStreamControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %cpu_stream_r_bits_id: !firrtl.uint<16>) {}
  } })",&ctx);
  auto ec = *eroot->getOps<CircuitOp>().begin(); ec->setAttr("rawAnnotations",b.getArrayAttr({}));
  require(succeeded(goldengate::addEmptyCPUStreamWrite(ec,error)),error);
  b.setInsertionPointToEnd(c.getBodyBlock()); auto empty = cast<FModuleOp>(b.clone(*named(ec,"GGEmptyCPUStreamWrite")));
  auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  auto decoder = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{
      {b.getStringAttr("aw_addr"),uint(25),Direction::In}});
  auto row = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("CPUManagedStreamEngine_2")),
      b.getNamedAttr("slave",b.getI32IntegerAttr(0)),b.getNamedAttr("start",b.getI64IntegerAttr(2048)),b.getNamedAttr("size",b.getI64IntegerAttr(4))});
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("CPUManagedStreamEngine_2")),
      b.getNamedAttr("port",b.getStringAttr("cpuStream_ctrl")),b.getNamedAttr("slave",b.getI32IntegerAttr(0))});
  SmallVector<FModuleOp> helpers{decoder,empty};
  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper","goldengate.controlWriteBindings"),
                   std::make_pair("GGControlReadDispatchWrapper","goldengate.controlReadBindings")}) {
    auto m = b.create<FModuleOp>(c.getLoc(),b.getStringAttr(pair.first),top.getConventionAttr(),ArrayRef<PortInfo>{});
    m->setAttr(pair.second,b.getArrayAttr({binding})); helpers.push_back(m);
  }
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) b.create<InstanceOp>(c.getLoc(),m,m.getName());
  // Vary stream identity independently of widget index and check C++ escaping.
  auto stream = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"); NamedAttrList sp(stream);
  sp.set("name",b.getStringAttr("trace\"stream\\domain"));
  if (bad == 1) sp.set("depth",b.getI64IntegerAttr(6143));
  if (bad == 2) sp.set("widthBytes",b.getI64IntegerAttr(32));
  if (bad == 3) sp.set("index",b.getI64IntegerAttr(1));
  if (bad == 4) sp.set("name",b.getStringAttr(""));
  queue->setAttr("goldengate.streamParameters",sp.getDictionary(&ctx));
  auto sources = read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
  NamedAttrList source(cast<DictionaryAttr>(sources[0])); source.set("name",b.getStringAttr("trace\"stream\\domain"));
  read->setAttr("goldengate.sourceStreams",b.getArrayAttr({source.getDictionary(&ctx)}));
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"); NamedAttrList word(cast<DictionaryAttr>(regs[0]));
  word.set("name",b.getStringAttr("trace\"stream\\domain_count"));
  if (bad == 5) word.set("offset",b.getI32IntegerAttr(4));
  if (bad == 6) word.set("readable",b.getBoolAttr(false));
  if (bad == 7) word.set("writeable",b.getBoolAttr(true));
  if (bad == 8) word.set("name",b.getStringAttr("wrong_count"));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr({word.getDictionary(&ctx)}));
  if (bad == 9) read->setAttr("goldengate.streamAddressSpaceBits",b.getI64IntegerAttr(18));
  if (bad == 10) empty->setAttr("goldengate.fromHostCPUStreamCount",b.getI32IntegerAttr(1));
  if (bad == 11) for (auto mem : queue.getOps<MemOp>()) mem.setDepthAttr(b.getI64IntegerAttr(1024));
  if (bad == 12) for (auto bits : read.getOps<BitsPrimOp>()) if (bits.getHi() == 63) bits.setLoAttr(b.getI32IntegerAttr(18));
  if (bad == 13) for (auto eq : read.getOps<EQPrimOp>()) {
    auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
    if (bits && bits.getHi() == 63) { b.setInsertionPoint(eq); eq->setOperand(1,b.create<ConstantOp>(c.getLoc(),uint(45),APInt(45,1))); }
  }
  if (bad >= 14 && bad <= 18) {
    NamedAttrList r(row);
    if (bad == 14) r.set("start",b.getI64IntegerAttr(2049));
    if (bad == 15) r.set("start",b.getI64IntegerAttr(1<<25));
    if (bad == 16) r.set("size",b.getI64IntegerAttr(0));
    if (bad == 17) r.set("name",b.getStringAttr("Other_2"));
    if (bad == 18) r.set("slave",b.getI32IntegerAttr(1));
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({r.getDictionary(&ctx)}));
  }
  if (bad == 19) { NamedAttrList overlap(row); overlap.set("name",b.getStringAttr("Other_1"));
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({row,overlap.getDictionary(&ctx)})); }
  if (bad == 20) named(c,"GGControlReadDispatchWrapper")->removeAttr("goldengate.controlReadBindings");
  if (bad == 21) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),queue,"duplicateQueue"); }
  if (bad == 22) for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == empty.getName()) { i.erase(); break; }
  if (bad == 23) bank->removeAttr("goldengate.mmioRegisters");
  if (bad == 24) for (auto pad : bank.getOps<PadPrimOp>()) {
    b.setInsertionPoint(pad); pad->setOperand(0,b.create<ConstantOp>(c.getLoc(),uint(13),APInt(13,0))); }
  if (bad == 25) for (auto a : bank.getOps<AssertOp>()) { a.erase(); break; }
  if (bad == 26) for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == bank.getName()) {
    for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == i.getResult(2)) {
      b.setInsertionPoint(conn); conn->setOperand(1,b.create<ConstantOp>(c.getLoc(),uint(13),APInt(13,0))); break; } }
  if (bad == 27) {
    auto wrapper = named(c,"GGCPUStreamReadWrapper");
    for (auto i : wrapper.getOps<InstanceOp>()) if (i.getModuleName() == read.getName())
      for (auto conn : wrapper.getOps<ConnectOp>()) if (conn.getDest() == i.getResult(2)) { conn.erase(); break; }
  }
  if (bad == 28) for (auto conn : empty.getOps<StrictConnectOp>()) if (conn.getDest() == empty.getBodyBlock()->getArgument(2)) {
    b.setInsertionPoint(conn); conn->setOperand(1,b.create<ConstantOp>(c.getLoc(),uint(1),APInt(1,1))); break; }
  if (bad == 29) c->setAttr("rawAnnotations",b.getArrayAttr({other}));
  if (bad == 30) c->setAttr("rawAnnotations",b.getArrayAttr({other,output,output}));
  if (bad == 31) c->removeAttr("rawAnnotations");
  return root;
}
const std::string printName = "print\"stream\\clock\n\t";
const std::string traceName = "trace_stream";
// Two real native queue instances feed both native transports. In shared mode
// the very same queue definition is instantiated twice; optional per-definition
// streamParameters are absent because identity belongs to each instance path.
OwningOpRef<ModuleOp> multiFixture(MLIRContext &ctx, bool shared = false, unsigned bad = 0) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGTracerVBridgeControlWrapper" {
    firrtl.module @GGTracerVBridgeControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>) {}
  } })", &ctx);
  require(bool(root),"multi fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); std::string error;
  auto output = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("previous body\n"))});
  auto untouched = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")), b.getNamedAttr("value",b.getI32IntegerAttr(14))});
  c->setAttr("rawAnnotations",b.getArrayAttr({untouched,output}));
  require(succeeded(goldengate::addTracerVStreamQueue(c,error)),error);
  auto queueA = named(c,"GGTracerVStreamQueue6144"); FModuleOp queueB = queueA;
  if (shared) queueA->removeAttr("goldengate.streamParameters");
  else {
    b.setInsertionPointToEnd(c.getBodyBlock()); queueB = cast<FModuleOp>(b.clone(*queueA)); queueB.setName("PrintQueue6144");
    for (auto [queue,index,name] : {std::make_tuple(queueA,1U,traceName),std::make_tuple(queueB,0U,printName)}) {
      NamedAttrList parameters(queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"));
      parameters.set("index",b.getI64IntegerAttr(index)); parameters.set("name",b.getStringAttr(name));
      queue->setAttr("goldengate.streamParameters",parameters.getDictionary(&ctx));
    }
  }
  auto uint = [&](unsigned bits) { return UIntType::get(&ctx,bits,false); };
  auto token = queueA.getPortType(3);
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
      {b.getStringAttr("hostReset"),uint(1),Direction::In},
      {b.getStringAttr("traceStream"),token,Direction::Out},{b.getStringAttr("printStream"),token,Direction::Out},
      {b.getStringAttr("traceCount"),uint(13),Direction::Out},{b.getStringAttr("printCount"),uint(13),Direction::Out}};
  b.setInsertionPointToEnd(c.getBodyBlock()); auto top = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("MultiQueueTop"),
      ConventionAttr::get(&ctx,Convention::Internal),ports); c.setName("MultiQueueTop"); b.setInsertionPointToStart(top.getBodyBlock());
  auto trace = b.create<InstanceOp>(c.getLoc(),queueA,"traceQueue"), print = b.create<InstanceOp>(c.getLoc(),queueB,"printQueue");
  for (auto queue : {trace,print}) {
    b.create<StrictConnectOp>(c.getLoc(),queue.getResult(0),top.getArgument(0));
    b.create<StrictConnectOp>(c.getLoc(),queue.getResult(1),top.getArgument(1));
    b.create<StrictConnectOp>(c.getLoc(),b.create<SubfieldOp>(c.getLoc(),queue.getResult(2),"valid"),b.create<ConstantOp>(c.getLoc(),uint(1),APInt(1,0)));
    b.create<StrictConnectOp>(c.getLoc(),b.create<SubfieldOp>(c.getLoc(),queue.getResult(2),"bits"),b.create<ConstantOp>(c.getLoc(),uint(512),APInt(512,0)));
  }
  b.create<ConnectOp>(c.getLoc(),top.getArgument(2),trace.getResult(3)); b.create<ConnectOp>(c.getLoc(),top.getArgument(3),print.getResult(3));
  b.create<StrictConnectOp>(c.getLoc(),top.getArgument(4),trace.getResult(4)); b.create<StrictConnectOp>(c.getLoc(),top.getArgument(5),print.getResult(4));
  if (bad == 20) { // Data refers to printQueue while its count still refers to traceQueue.
    for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == top.getArgument(2)) conn->setOperand(1,print.getResult(3));
  }
  if (bad == 21) for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == top.getArgument(5)) conn->setOperand(1,trace.getResult(4));
  if (bad == 28) {
    for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == top.getArgument(2)) conn->setOperand(1,print.getResult(3));
    for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == top.getArgument(4)) conn->setOperand(1,print.getResult(4));
  }
  if (bad == 22) b.create<InstanceOp>(c.getLoc(),queueA,"duplicateLiveQueue");
  require(succeeded(goldengate::addCPUStreamRead(c,{{printName,"printStream",6144},{traceName,"traceStream",6144}},error)),error);
  require(succeeded(goldengate::addCPUStreamCountBank(c,{{printName,"printCount",13},{traceName,"traceCount",13}},error)),error);
  top = named(c,c.getName()); auto bank = named(c,"GGCPUStreamCountBank"), read = named(c,"GGCPUStreamRead");
  // Only the control allocator and binding leaves are synthetic.
  auto eroot = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGCPUStreamControlWrapper" {
    firrtl.module @GGCPUStreamControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %cpu_stream_r_bits_id: !firrtl.uint<16>) {} } })",&ctx);
  auto ec = *eroot->getOps<CircuitOp>().begin(); ec->setAttr("rawAnnotations",b.getArrayAttr({})); require(succeeded(goldengate::addEmptyCPUStreamWrite(ec,error)),error);
  b.setInsertionPointToEnd(c.getBodyBlock()); auto empty = cast<FModuleOp>(b.clone(*named(ec,"GGEmptyCPUStreamWrite")));
  auto decoder = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{
      {b.getStringAttr("aw_addr"),uint(25),Direction::In}});
  auto region = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("CPUManagedStreamEngine_2")), b.getNamedAttr("slave",b.getI32IntegerAttr(0)),
      b.getNamedAttr("start",b.getI64IntegerAttr(2048)),b.getNamedAttr("size",b.getI64IntegerAttr(bad == 23 ? 4 : 8))});
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({region}));
  auto binding = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("CPUManagedStreamEngine_2")),
      b.getNamedAttr("port",b.getStringAttr("cpuStream_ctrl")),b.getNamedAttr("slave",b.getI32IntegerAttr(0))});
  SmallVector<FModuleOp> helpers{decoder,empty};
  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper","goldengate.controlWriteBindings"),
                   std::make_pair("GGControlReadDispatchWrapper","goldengate.controlReadBindings")}) {
    auto module = b.create<FModuleOp>(c.getLoc(),b.getStringAttr(pair.first),top.getConventionAttr(),ArrayRef<PortInfo>{});
    module->setAttr(pair.second,b.getArrayAttr({binding})); helpers.push_back(module);
  }
  b.setInsertionPointToStart(top.getBodyBlock()); for (auto module : helpers) b.create<InstanceOp>(c.getLoc(),module,module.getName());
  auto sources = read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams"); SmallVector<Attribute> rows(sources.begin(),sources.end());
  NamedAttrList first(cast<DictionaryAttr>(rows[0])), second(cast<DictionaryAttr>(rows[1]));
  if (bad == 1) read->removeAttr("goldengate.sourceStreams");
  if (bad == 2) read->setAttr("goldengate.sourceStreams",b.getStringAttr("bad"));
  if (bad == 3) read->setAttr("goldengate.sourceStreams",b.getArrayAttr({}));
  if (bad == 4) first.erase("name");
  if (bad == 5) first.set("name",b.getStringAttr(""));
  if (bad == 6) first.set("name",b.getStringAttr(StringRef("print\0bad",9)));
  if (bad == 7) second.set("name",b.getStringAttr(printName));
  if (bad == 8) second.set("port",b.getStringAttr("printStream"));
  if (bad == 9) second.set("index",b.getI64IntegerAttr(0));
  if (bad == 10) first.set("index",b.getStringAttr("zero"));
  if (bad == 11) second.set("bufferBaseAddress",b.getI64IntegerAttr(0));
  if (bad == 12) second.set("bufferBaseAddress",b.getI64IntegerAttr(524289));
  if (bad == 13) first.set("depth",b.getI64IntegerAttr(6143));
  if (bad == 14) first.set("widthBytes",b.getI64IntegerAttr(32));
  if (bad == 15) first.erase("bufferBaseAddress");
  if (bad == 16) first.set("port",b.getStringAttr(""));
  if (bad == 30) first.set("port",b.getStringAttr("unknownSource"));
  if (bad == 17) read->setAttr("goldengate.streamAddressSpaceBits",b.getI64IntegerAttr(18));
  if ((bad >= 4 && bad <= 16) || bad == 30) { rows[0] = first.getDictionary(&ctx); rows[1] = second.getDictionary(&ctx); read->setAttr("goldengate.sourceStreams",b.getArrayAttr(rows)); }
  if (bad == 18 || bad == 19) {
    auto words = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"); SmallVector<Attribute> updated(words.begin(),words.end()); NamedAttrList word(cast<DictionaryAttr>(words[1]));
    if (bad == 18) word.set("offset",b.getI32IntegerAttr(0)); else word.set("name",b.getStringAttr(printName+"_count"));
    updated[1] = word.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(updated));
  }
  if (bad == 24) {
    auto parameters = queueB->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"); NamedAttrList mismatch(parameters);
    mismatch.set("name",b.getStringAttr("wrong_queue_name")); queueB->setAttr("goldengate.streamParameters",mismatch.getDictionary(&ctx));
  }
  if (bad == 29) queueB->setAttr("goldengate.streamParameters",b.getStringAttr("not-a-dictionary"));
  if (bad == 25) for (auto pad : bank.getOps<PadPrimOp>()) if (pad.getInput() == bank.getArgument(2)) pad->setOperand(0,bank.getArgument(3));
  if (bad == 26 || bad == 27) {
    Value wrongGrant;
    for (auto eq : read.getOps<EQPrimOp>()) {
      auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>(); auto selector = eq.getRhs().getDefiningOp<ConstantOp>();
      if (bits && bits.getInput() == read.getArgument(7) && selector && selector.getValue().isOne()) wrongGrant = eq;
    }
    require(bool(wrongGrant),"second stream grant missing");
    if (bad == 26) for (auto conn : read.getOps<StrictConnectOp>()) {
      auto field = conn.getDest().getDefiningOp<SubfieldOp>();
      if (field && field.getInput() == read.getArgument(2) && field.getFieldName() == "ready")
        conn.getSrc().getDefiningOp<AndPrimOp>()->setOperand(0,wrongGrant);
    }
    if (bad == 27) for (auto mux : read.getOps<MuxPrimOp>()) {
      auto field = mux->getOperand(1).getDefiningOp<SubfieldOp>();
      if (field && field.getInput() == read.getArgument(2) && field.getFieldName() == "bits") mux->setOperand(0,wrongGrant);
    }
  }
  return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules; for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareCPUManagedStreamHeader(c,error)),error);
  auto after = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(before.size() == after.size(),"changed annotation count");
  unsigned changed = 0; std::string body;
  for (unsigned i = 0; i < before.size(); ++i) if (before[i] != after[i]) {
    ++changed; auto old = cast<DictionaryAttr>(before[i]), updated = cast<DictionaryAttr>(after[i]);
    body = updated.getAs<StringAttr>("body").getValue().str();
    require(StringRef(body).starts_with(old.getAs<StringAttr>("body").getValue()),"lost existing header");
    for (auto a : old) if (a.getName() != "body") require(updated.get(a.getName()) == a.getValue(),"changed output metadata");
  }
  require(changed == 1,"changed unrelated annotations"); unsigned i = 0;
  for (auto m : c.getOps<FModuleLike>()) require(modules[i++] == dump(m.getOperation()),"changed hardware");
  return body;
}
}
int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 3,"usage: CPUManagedStreamHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.disableMultithreading();
    ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    require(StringRef(body).contains("  2,\n  args,\n  std::vector<CPUManagedStreams::StreamParameters>{},"),"sink order/widget identity");
    require(StringRef(body).contains("std::string(\"trace\\\"stream\\\\domain\"), 0ULL, 2048ULL, 6144U, 64U)"),"descriptor/escaping differs");
    require(StringRef(body).contains("GET_MANAGED_STREAM_CONSTRUCTOR") && !StringRef(body).contains("_struct"),"constructor guard/substruct differs");
    std::string before = dump(*root), error;
    require(failed(goldengate::prepareCPUManagedStreamHeader(c,error)) && dump(*root) == before,"duplicate constructor mutated IR");
    for (unsigned bad = 1; bad <= 31; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareCPUManagedStreamHeader(nc,error)) && !error.empty() && dump(*negative) == before,
          "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    for (bool shared : {false,true}) {
      auto multi = multiFixture(ctx,shared); auto mc = *multi->getOps<CircuitOp>().begin();
      require(succeeded(verify(*multi)),"multi-queue fixture IR invalid");
      auto multiBody = run(mc);
      std::string first = "std::string(\"print\\\"stream\\\\clock\\012\\011\"), 0ULL, 2048ULL, 6144U, 64U)";
      std::string second = "std::string(\"trace_stream\"), 524288ULL, 2052ULL, 6144U, 64U)";
      require(StringRef(multiBody).contains(first) && StringRef(multiBody).contains(second) &&
          multiBody.find(first) < multiBody.find(second),"ordered outgoing DMA/MMIO descriptors or C++ escaping differs");
      auto beforeMulti = dump(*multi);
      require(failed(goldengate::prepareCPUManagedStreamHeader(mc,error)) && dump(*multi) == beforeMulti,
          "multi-stream duplicate constructor mutated IR");
    }
    for (unsigned bad = 1; bad <= 30; ++bad) {
      auto multi = multiFixture(ctx,false,bad); auto mc = *multi->getOps<CircuitOp>().begin(); auto beforeMulti = dump(*multi);
      require(failed(goldengate::prepareCPUManagedStreamHeader(mc,error)) && !error.empty() && dump(*multi) == beforeMulti,
          "invalid multi-stream boundary accepted or mutated: " + std::to_string(bad));
    }
    // The shared-definition topology also rejects an additional unallocated
    // live instance, rather than requiring each allocated queue's module to
    // have exactly one instance throughout the circuit.
    auto sharedBad = multiFixture(ctx,true,22); auto sc = *sharedBad->getOps<CircuitOp>().begin(); auto sharedBefore = dump(*sharedBad);
    require(failed(goldengate::prepareCPUManagedStreamHeader(sc,error)) && dump(*sharedBad) == sharedBefore,
        "extra shared queue instance escaped allocation validation");
    llvm::outs() << "CPU managed stream header: single/ordered multi-stream descriptors, distinct/shared queue definitions, escaped names; 65 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error); body = run(bc);
      require(StringRef(body).contains("std::string(\"TRACERVBRIDGEMODULE_0_to_cpu_stream\"), 0ULL, 568ULL, 6144U, 64U)"),"recorded U250 descriptor differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: CPU-managed stream constructor emitted\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
