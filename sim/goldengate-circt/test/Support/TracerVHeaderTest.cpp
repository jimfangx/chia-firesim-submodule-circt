// See LICENSE for license details.
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/BlockDevMMIOBank.h"
#include "goldengate/BlockDevHeader.h"
#include "goldengate/TracerVHeader.h"
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/TSIHeader.h"
#include "goldengate/UARTHeader.h"
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/LoadMemHeader.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) {
  std::string s; llvm::raw_string_ostream out(s);
  op->print(out, OpPrintingFlags().useLocalScope()); return s;
}
const llvm::StringRef locals[]{"tiletrace_reset", "tiletrace_trace_retiredinsns_0_valid",
    "tiletrace_trace_retiredinsns_0_iaddr", "tiletrace_trace_retiredinsns_0_insn",
    "tiletrace_trace_retiredinsns_0_priv", "tiletrace_trace_retiredinsns_0_exception",
    "tiletrace_trace_retiredinsns_0_interrupt", "tiletrace_trace_retiredinsns_0_cause",
    "tiletrace_trace_retiredinsns_0_tval", "tiletrace_trace_time", "triggerCredit", "triggerDebit"};
const unsigned widths[]{1, 1, 40, 32, 3, 1, 1, 64, 40, 64, 1, 1};
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> tokenFixture(MLIRContext &ctx, unsigned retire = 1, unsigned latency = 1) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGPeekPokeBridgeControlWrapper" {
    firrtl.module @GGPeekPokeBridgeControlWrapper() {}
  } })", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  auto placeholder = *c.getOps<FModuleOp>().begin(); placeholder.erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
                            {b.getStringAttr("hostReset"), uint(1), Direction::In}};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 12; ++j) {
    auto type = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, uint(1)},
        {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(widths[j])}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)), type, j < 10 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("tracerv_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) { return b.getNamedAttr(n, b.getStringAttr(v)); };
  auto dict = [&](std::initializer_list<NamedAttribute> attrs) { return b.getDictionaryAttr(attrs); };
  auto key = dict({str("class", "firechip.bridgeinterfaces.TraceBundleWidths"),
      b.getNamedAttr("retireWidth", b.getI64IntegerAttr(retire)), b.getNamedAttr("iaddrWidth", b.getI64IntegerAttr(40)),
      b.getNamedAttr("insnWidth", b.getI64IntegerAttr(32)), b.getNamedAttr("causeWidth", b.getI64IntegerAttr(64)),
      b.getNamedAttr("tvalWidth", b.getI64IntegerAttr(40))});
  SmallVector<Attribute> all{dict({str("class", "firesim.lib.bridgeutils.BridgeIOAnnotation"),
      str("widgetClass", "firechip.goldengateimplementations.TracerVBridgeModule"),
      str("target", "~FireSim|FireSim>tracerv"), b.getNamedAttr("widgetConstructorKey", key),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 12; ++j)
    all.push_back(dict({str("class", "midas.passes.fame.FAMEChannelConnectionAnnotation"),
        str("globalName", "tracerv_" + locals[j].str()),
        b.getNamedAttr("channelInfo", dict({str("class", "midas.passes.fame.PipeChannel"),
            b.getNamedAttr("latency", b.getI64IntegerAttr(latency))})),
        b.getNamedAttr(j < 10 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr(
            "~GGPeekPokeBridgeControlWrapper|GGPeekPokeBridgeControlWrapper>token" + std::to_string(j) + ".bits")}))}));
  all.push_back(dict({str("class", "test.Annotation"), str("target", "~GGPeekPokeBridgeControlWrapper|GGPeekPokeBridgeControlWrapper>other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(all)); return root;
}
// Real token, trigger, stream queue and CPU transport transforms; only the
// allocation graph is synthetic. Optional boundary mode exercises the complete compiler IR.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = tokenFixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTracerVTokenEngine(c,error)),error);
  require(succeeded(goldengate::addTracerVTriggerConfig(c,error)),error);
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto top = named(c,c.getName());
  auto bank = named(c,"GGTracerVTriggerConfig"), engine = named(c,"GGTracerVTokenEngine");
  auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  auto qr = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGTracerVBridgeControlWrapper" {
    firrtl.module @GGTracerVBridgeControlWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>) {}
  } })",&ctx);
  auto qc = *qr->getOps<CircuitOp>().begin(); qc->setAttr("rawAnnotations",b.getArrayAttr({}));
  require(succeeded(goldengate::addTracerVStreamQueue(qc,error)),error);
  b.setInsertionPointToEnd(c.getBodyBlock()); auto queue = cast<FModuleOp>(b.clone(*named(qc,"GGTracerVStreamQueue6144")));
  auto stream = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"); NamedAttrList params(stream);
  params.set("name",b.getStringAttr("TRACERVBRIDGEMODULE_2_to_cpu_stream"));
  if (bad == 1) params.set("depth",b.getI64IntegerAttr(1024));
  if (bad == 2) params.set("index",b.getI64IntegerAttr(1));
  if (bad == 3) params.set("widthBytes",b.getI64IntegerAttr(32));
  if (bad == 4) params.set("name",b.getStringAttr("wrong_stream"));
  queue->setAttr("goldengate.streamParameters",params.getDictionary(&ctx));
  require(succeeded(goldengate::addCPUStreamRead(qc,error)),error);
  b.setInsertionPointToEnd(c.getBodyBlock()); auto transport = cast<FModuleOp>(b.clone(*named(qc,"GGCPUStreamRead")));
  if (bad == 5) transport->setAttr("goldengate.streamAddressSpaceBits",b.getI64IntegerAttr(18));
  auto decoder = b.create<FModuleOp>(loc,b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{
    {b.getStringAttr("aw_addr"),uint(25),Direction::In}});
  auto row = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(bad == 6 ? "Other_2" : "TracerVBridgeModule_2")),
    b.getNamedAttr("slave",b.getI32IntegerAttr(0)),b.getNamedAttr("start",b.getI64IntegerAttr(bad == 7 ? 1025 : bad == 8 ? (1<<25)-4 : 1024)),
    b.getNamedAttr("size",b.getI64IntegerAttr(bad == 9 ? 56 : 64))});
  auto overlap = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("Other_0")),b.getNamedAttr("slave",b.getI32IntegerAttr(1)),
    b.getNamedAttr("start",b.getI64IntegerAttr(1028)),b.getNamedAttr("size",b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions",bad == 10 ? b.getArrayAttr({row,overlap}) : b.getArrayAttr({row}));
  auto binding = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("TracerVBridgeModule_2")),
    b.getNamedAttr("port",b.getStringAttr("tracerv_ctrl")),b.getNamedAttr("slave",b.getI32IntegerAttr(bad == 11 ? 1 : 0))});
  SmallVector<FModuleOp> helpers{queue,transport,decoder};
  for (auto pair : {std::make_pair("GGControlWidgetWriteWrapper","goldengate.controlWriteBindings"),
                   std::make_pair("GGControlReadDispatchWrapper","goldengate.controlReadBindings")}) {
    auto m = b.create<FModuleOp>(loc,b.getStringAttr(pair.first),top.getConventionAttr(),ArrayRef<PortInfo>{});
    m->setAttr(pair.second,bad == 12 ? b.getArrayAttr({}) : b.getArrayAttr({binding})); helpers.push_back(m);
  }
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) if (bad != 13 || m != queue) b.create<InstanceOp>(loc,m,m.getName());
  if (bad == 14) b.create<InstanceOp>(loc,engine,"duplicateEngine");
  if (bad == 15) bank->removeAttr("goldengate.mmioRegisters");
  if (bad >= 16 && bad <= 19) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"); SmallVector<Attribute> entries(regs.begin(),regs.end());
    NamedAttrList entry(cast<DictionaryAttr>(entries[2]));
    if (bad == 16) entry.set("name",b.getStringAttr("wrong"));
    if (bad == 17) entry.set("offset",b.getI32IntegerAttr(12));
    if (bad == 18) entry.set("readable",b.getBoolAttr(true));
    if (bad == 19) entry.set("writeable",b.getBoolAttr(false));
    entries[2] = entry.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(entries));
  }
  if (bad == 20) for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "traceEnable") {
    b.setInsertionPoint(r); auto zero = b.create<ConstantOp>(loc,uint(1),APInt(1,0)); r->setOperand(2,zero); break;
  }
  if (bad == 21) { auto conn = *bank.getOps<StrictConnectOp>().begin(); conn.erase(); }
  if (bad == 22) for (auto mux : bank.getOps<MuxPrimOp>()) if (mux.getLow().getDefiningOp<RegResetOp>()) { mux->setOperand(2,mux.getHigh()); break; }
  if (bad == 23) for (auto bits : bank.getOps<BitsPrimOp>()) { bits.setLoAttr(b.getI32IntegerAttr(1)); break; }
  if (bad == 24) for (auto reg : bank.getOps<RegResetOp>()) if (reg.getName() == "initDone") {
    b.setInsertionPoint(reg); auto clock = b.create<WireOp>(loc,ClockType::get(&ctx),"wrongClock"); reg->setOperand(0,clock.getResult()); break;
  }
  if (bad == 25) engine->removeAttr("goldengate.bridgeConstructor");
  if (bad == 26) for (auto mem : queue.getOps<MemOp>()) mem.setDepthAttr(b.getI64IntegerAttr(1024));
  if (bad == 32) for (auto bits : transport.getOps<BitsPrimOp>()) if (bits.getHi() == 63) { bits.setLoAttr(b.getI32IntegerAttr(18)); break; }
  if (bad == 33) for (auto eq : transport.getOps<EQPrimOp>()) {
    auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
    if (bits && bits.getHi() == 63) { b.setInsertionPoint(eq); auto one = b.create<ConstantOp>(loc,uint(45),APInt(45,1)); eq->setOperand(1,one); break; }
  }
  auto clock = b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("test\"clock\\domain")),
    b.getNamedAttr("multiplier",b.getI64IntegerAttr(bad == 27 ? 0 : 3)),b.getNamedAttr("divisor",b.getI64IntegerAttr(2))});
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> annos; NamedAttrList clocks;
  for (auto attr : raw) {
    auto d = cast<DictionaryAttr>(attr);
    if (d.getAs<StringAttr>("class") == goldengate::AnnotationClasses::BridgeIO) {
      NamedAttrList bridge(d); bridge.set("clockInfo",clock);
      if (bad == 28) bridge.set("target",b.getStringAttr("~GGTracerVTriggerWrapper|GGTracerVTriggerConfig>mcr"));
      for (auto mapping : d.getAs<DictionaryAttr>("channelMapping"))
        clocks.set(cast<StringAttr>(mapping.getValue()).getValue(),clock);
      annos.push_back(bridge.getDictionary(&ctx));
    } else annos.push_back(attr);
  }
  if (bad != 29) annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::ChannelClockInfo)),
    b.getNamedAttr("infoMap",clocks.getDictionary(&ctx))}));
  auto output = b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("existing body\n"))});
  if (bad != 30) annos.push_back(output);
  if (bad == 31) annos.push_back(output);
  c->setAttr("rawAnnotations",b.getArrayAttr(annos)); return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules;
  for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareTracerVHeader(c, error)), error);
  auto updated = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == updated.size(), "changed annotation count");
  unsigned changed = 0; std::string result;
  for (unsigned i = 0; i < raw.size(); ++i) {
    if (raw[i] == updated[i]) continue;
    ++changed; auto before = cast<DictionaryAttr>(raw[i]), after = cast<DictionaryAttr>(updated[i]);
    result = after.getAs<StringAttr>("body").getValue().str();
    require(StringRef(result).starts_with(before.getAs<StringAttr>("body").getValue()), "lost existing header body");
    for (auto a : before) if (a.getName() != "body") require(after.get(a.getName()) == a.getValue(), "changed output metadata");
  }
  require(changed == 1, "changed unrelated annotations");
  unsigned i = 0;
  for (auto m : c.getOps<FModuleLike>()) require(modules[i++] == dump(m.getOperation()), "changed hardware semantics");
  return result;
}
}
int main(int argc, char **argv) {
  try {
    require(argc == 1 || argc == 3,"usage: TracerVHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".initDone = 1024", ".traceEnable = 1028", ".triggerSelector = 1080",
        "registry.add_widget(new tracerv_t(","*registry.get_stream_engine()", "},\n  2,\n  args,\n  0,\n  6144,\n  1,\n",
        "ClockInfo{\"test\\\"clock\\\\domain\", 3U, 2U}",
        "sizeof(TRACERVBRIDGEMODULE_struct) == 15 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text),"TracerV allocation/stream/clock ABI differs");
    std::string error, before = dump(*root);
    require(failed(goldengate::prepareTracerVHeader(c,error)) && dump(*root) == before,"duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 33; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareTracerVHeader(nc,error)) && !error.empty() && dump(*negative) == before,
        "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    llvm::outs() << "TracerV header: varied base/index/clock, fifteen words, stream geometry; preserved hardware/annotations; 34 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareResetPulseHeader(bc,error)),error);
      require(succeeded(goldengate::prepareLoadMemHeader(bc,error)),error);
      require(succeeded(goldengate::preparePeekPokeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareUARTHeader(bc,error)),error);
      require(succeeded(goldengate::prepareTSIHeader(bc,error)),error);
      require(succeeded(goldengate::prepareBlockDevHeader(bc,error)),error); body = run(bc);
      for (auto text : {".initDone = 256", ".traceEnable = 260", ".triggerSelector = 312",
          "},\n  0,\n  args,\n  0,\n  6144,\n  1,\n  ClockInfo{\"uart_clock,clock_1000.0MHz,harnessbinder_clock,reference\", 1U, 1U}"})
        require(StringRef(body).contains(text),"recorded U250 TracerV ABI differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: TracerV constructor composed with nine existing header sections\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
