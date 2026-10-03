// See LICENSE for license details.
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/TSITokenEngine.h"
#include "goldengate/TSIWordQueues.h"
#include "goldengate/TSIMMIOBank.h"
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
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing fixture module");
}
OwningOpRef<ModuleOp> tokenFixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGSimulationMasterBoundWrapper\" { firrtl.module @GGSimulationMasterBoundWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  auto placeholder = *c.getOps<FModuleOp>().begin(); placeholder.erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  auto bit = uint(1);
  auto forward = BundleType::get(&ctx, {{b.getStringAttr("bits"), false, uint(32)}, {b.getStringAttr("valid"), false, bit}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  const llvm::StringRef locals[]{"tsi_in_rev", "tsi_out_fwd", "reset", "tsi_in_fwd", "tsi_out_rev"};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 5; ++j) {
    auto token = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
        {b.getStringAttr("bits"), false, j == 1 || j == 3 ? FIRRTLBaseType(forward) : FIRRTLBaseType(bit)}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)), token, j < 3 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("ep_3_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) { return b.getNamedAttr(n, b.getStringAttr(v)); };
  auto dict = [&](std::initializer_list<NamedAttribute> a) { return b.getDictionaryAttr(a); };
  auto path = [&](unsigned j, llvm::StringRef suffix) {
    return b.getStringAttr("~GGSimulationMasterBoundWrapper|GGSimulationMasterBoundWrapper>token" + std::to_string(j) + suffix.str());
  };
  SmallVector<Attribute> raw{dict({str("class", goldengate::AnnotationClasses::BridgeIO),
      str("widgetClass", "firechip.goldengateimplementations.TSIBridgeModule"), str("target", "~FireSim|FireSim>ep_3"),
      b.getNamedAttr("widgetConstructorKey", dict({str("class", "firechip.bridgeinterfaces.TSIBridgeParams"), str("memoryRegionNameOpt", "MainMemory_0")})),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 5; ++j) {
    NamedAttrList info;
    info.set("class", b.getStringAttr(j == 2 ? goldengate::AnnotationClasses::PipeChannel : j == 1 || j == 3 ? goldengate::AnnotationClasses::DecoupledForwardChannel : goldengate::AnnotationClasses::DecoupledReverseChannel));
    if (j == 2) info.set("latency", b.getI64IntegerAttr(1));
    if (j == 1 || j == 3) {
      info.set(j == 1 ? "validSource" : "validSink", path(j, ".bits.valid"));
      info.set(j == 1 ? "readySink" : "readySource", path(j == 1 ? 4 : 0, ".bits"));
    }
    SmallVector<Attribute> ends;
    if (j == 1 || j == 3) ends = {path(j, ".bits.bits"), path(j, ".bits.valid")}; else ends = {path(j, ".bits")};
    raw.push_back(dict({str("class", goldengate::AnnotationClasses::ChannelConnection), str("globalName", "ep_3_" + locals[j].str()),
        b.getNamedAttr("channelInfo", info.getDictionary(&ctx)), str("clock", "sameClock"),
        b.getNamedAttr(j < 3 ? "sources" : "sinks", b.getArrayAttr(ends))}));
  }
  raw.push_back(dict({str("class", "test.Annotation"), str("target", "~GGSimulationMasterBoundWrapper|GGSimulationMasterBoundWrapper>other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(raw)); return root;
}
// Exercise the real TSI transforms with a memory offset, base and instance number
// distinct from the U250 reference. The control allocation and typed memory translation graph are synthetic.
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto root = tokenFixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addTSITokenEngine(c, error)), error);
  require(succeeded(goldengate::addTSIWordQueues(c, error)), error);
  require(succeeded(goldengate::addTSIMMIOBank(c, error)), error);
  OpBuilder b(&ctx); auto loc = c.getLoc(); auto top = named(c, "GGTSIMMIOWrapper");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder = b.create<FModuleOp>(loc, b.getStringAttr("GGControlAddressDecode"),
      ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{
        {b.getStringAttr("aw_addr"), UIntType::get(&ctx, 25, false), Direction::In}});
  auto row = b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 1 ? "Other_2" : "TSIBridgeModule_2")),
      b.getNamedAttr("slave", b.getI32IntegerAttr(0)),
      b.getNamedAttr("start", b.getI64IntegerAttr(bad == 2 ? 1025 : bad == 3 ? (1 << 25)-4 : 1024)),
      b.getNamedAttr("size", b.getI64IntegerAttr(bad == 4 ? 32 : 64))});
  auto other = b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("Other_0")),
    b.getNamedAttr("slave", b.getI32IntegerAttr(1)), b.getNamedAttr("start", b.getI64IntegerAttr(1028)),
    b.getNamedAttr("size", b.getI64IntegerAttr(32))});
  decoder->setAttr("goldengate.controlRegions", bad == 5 ? b.getArrayAttr({row,other}) : b.getArrayAttr({row}));
  auto binding = b.create<FModuleOp>(loc,b.getStringAttr("GGTSIBridgeBoundWrapper"),
      ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
  if (bad != 7) binding->setAttr("goldengate.tsiSlave",b.getI32IntegerAttr(bad == 6 ? 1 : 0));
  SmallVector<FModuleOp> helpers{decoder,binding};
  auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  auto master = [&](unsigned width) {
    auto bits = BundleType::get(&ctx,{{b.getStringAttr("addr"),false,uint(width)}});
    auto token = BundleType::get(&ctx,{{b.getStringAttr("bits"),false,bits}});
    return BundleType::get(&ctx,{{b.getStringAttr("aw"),false,token},{b.getStringAttr("ar"),false,token}});
  };
  auto memory = b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDAddressTranslation"),
      ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{
        {b.getStringAttr("clock"),ClockType::get(&ctx),Direction::In},
        {b.getStringAttr("reset"),uint(1),Direction::In},
        {b.getStringAttr("in"),master(35),Direction::In},
        {b.getStringAttr("out"),master(34),Direction::Out}});
  memory->setAttr("goldengate.memoryRegion",b.getDictionaryAttr({
    b.getNamedAttr("name",b.getStringAttr(bad == 30 ? "OtherMemory" : "MainMemory_0")),
    b.getNamedAttr("virtualBase",b.getI64IntegerAttr(128)),
    b.getNamedAttr("virtualBound",b.getI64IntegerAttr(4095)),
    b.getNamedAttr("hostBase",b.getI64IntegerAttr(bad == 34 ? 192 : 64)),
    b.getNamedAttr("offset",b.getI64IntegerAttr(bad == 34 ? 64 : bad == 29 ? -63 : -64))}));
  if (bad == 28) memory->removeAttr("goldengate.memoryRegion");
  b.setInsertionPointToStart(memory.getBodyBlock());
  auto field = [&](Value v,StringRef n)->Value { return b.create<SubfieldOp>(loc,v,n); };
  auto shift = b.create<ConstantOp>(loc,uint(35),APInt(35,bad == 34 ? 64 : (uint64_t(1)<<34)-(bad == 31 ? 63 : 64)));
  for (StringRef ch : {"aw","ar"}) {
    auto x = field(field(field(memory.getBodyBlock()->getArgument(2),ch),"bits"),"addr");
    auto y = field(field(field(memory.getBodyBlock()->getArgument(3),ch),"bits"),"addr");
    auto add = b.create<AddPrimOp>(loc,x,shift);
    auto bits = b.create<BitsPrimOp>(loc,add,33,0);
    if (bad != 33 || ch != "ar") b.create<StrictConnectOp>(loc,y,bits);
  }
  helpers.push_back(memory);
  b.setInsertionPointToStart(top.getBodyBlock());
  for (auto m : helpers) b.create<InstanceOp>(loc, m, m.getName());
  if (bad == 8) b.create<InstanceOp>(loc,binding,"duplicateBinding");
  if (bad == 32) b.create<InstanceOp>(loc,memory,"duplicateMemory");
  auto bank = named(c, "GGTSIMMIOBank");
  if (bad == 9) b.create<InstanceOp>(loc, bank, "duplicateReset");
  if (bad == 10) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    b.create<FModuleOp>(loc, b.getStringAttr("DisconnectedTop"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    c.setNameAttr(b.getStringAttr("DisconnectedTop"));
  }
  if (bad == 11) bank->removeAttr("goldengate.mmioRegisters");
  if (bad == 12 || bad == 13) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> changed(regs.begin(),regs.end()); NamedAttrList d(cast<DictionaryAttr>(changed[0]));
    d.set(bad == 12 ? "offset" : "readable", bad == 12 ? Attribute(b.getI32IntegerAttr(4)) : Attribute(b.getBoolAttr(false)));
    changed[0] = d.getDictionary(&ctx); bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(changed));
  }
  if (bad == 14) for (auto r : bank.getOps<RegOp>()) if (r.getName() == "in_bits") r.setNameAttr(b.getStringAttr("wrong_word"));
  if (bad == 15) for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "in_valid") r.setNameAttr(b.getStringAttr("wrong_pulse"));
  if (bad == 16) for (auto bits : bank.getOps<BitsPrimOp>()) if (bits.getLo() == 0 && bits.getHi() == 0) { bits.setLoAttr(b.getI32IntegerAttr(1)); bits.setHiAttr(b.getI32IntegerAttr(1)); }
  auto engine = named(c, "GGTSITokenEngine");
  if (bad == 19 || bad == 20) {
    NamedAttrList key(engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
    if (bad == 19) key.set("memoryRegionNameOpt", b.getStringAttr("OtherMemory")); else key.erase("class");
    engine->setAttr("goldengate.bridgeConstructor", key.getDictionary(&ctx));
  }
  if (bad == 21) engine->removeAttr("goldengate.bridgeConstructor");
  if (bad == 22) for (auto r : bank.getOps<RegResetOp>()) r->setOperand(2, bank.getBodyBlock()->getArgument(1));
  if (bad == 23) for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    auto slot = field ? field.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    if (field && field.getFieldName() == "bits" && slot && slot.getIndex() == 0)
      for (auto pad : bank.getOps<PadPrimOp>()) {
        auto reg = pad.getInput().getDefiningOp<RegOp>();
        if (reg && reg.getName() == "out_bits") connect->setOperand(1, pad.getResult());
      }
  }
  if (bad == 24) for (auto mux : bank.getOps<MuxPrimOp>()) {
    auto reg = mux.getLow().getDefiningOp<RegOp>();
    if (reg && reg.getName() == "in_bits") mux->setOperand(2, bank.getBodyBlock()->getArgument(1));
  }
  if (bad == 25) for (auto connect : bank.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    if (field && field.getInput() == bank.getBodyBlock()->getArgument(2) && field.getFieldName() == "valid")
      for (auto r : bank.getOps<RegResetOp>()) if (r.getName() == "out_ready") connect->setOperand(1,r.getResult());
  }
  if (bad == 26) {
    auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    SmallVector<Attribute> changed(regs.begin(),regs.end()); NamedAttrList d(cast<DictionaryAttr>(changed[0]));
    d.set("writeable", b.getBoolAttr(false)); changed[0] = d.getDictionary(&ctx);
    bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(changed));
  }
  auto output = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
    b.getNamedAttr("goldengate.loadMemHeader",b.getBoolAttr(bad != 27)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("// retained header\n"))});
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> annotations(raw.begin(),raw.end());
  if (bad != 17) annotations.push_back(output);
  if (bad == 18) annotations.push_back(output);
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return root;
}
std::string run(CircuitOp c) {
  SmallVector<std::string> modules;
  for (auto m : c.getOps<FModuleLike>()) modules.push_back(dump(m.getOperation()));
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::prepareTSIHeader(c, error)), error);
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
    require(argc == 1 || argc == 3, "usage: TSIHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto body = run(c);
    for (auto text : {".in_bits = 1024", ".in_valid = 1028", ".in_ready = 1032",
                     ".out_bits = 1036", ".out_valid = 1040", ".out_ready = 1044",
                     ".step_size = 1048", ".done = 1052", ".start = 1056",
                     "registry.add_widget(new tsibridge_t(", "registry.get_widget<loadmem_t>()",
                     "},\n  2,\n  args,\n  true,\n  -64ULL\n));",
                     "#include \"bridges/tsibridge.h\"", "#ifdef GET_BRIDGE_CONSTRUCTOR",
                     "offsetof(TSIBRIDGEMODULE_struct, start) == 8 * sizeof(uint64_t)",
                     "sizeof(TSIBRIDGEMODULE_struct) == 9 * sizeof(uint64_t)"})
      require(StringRef(body).contains(text), "allocation, widget index or TSI constructor differs");
    auto before = dump(*root); std::string error;
    require(failed(goldengate::prepareTSIHeader(c,error)) && dump(*root) == before, "duplicate constructor changed IR");
    for (unsigned bad = 1; bad <= 33; ++bad) {
      auto negative = fixture(ctx,bad); auto nc = *negative->getOps<CircuitOp>().begin(); before = dump(*negative);
      require(failed(goldengate::prepareTSIHeader(nc,error)) && !error.empty() && dump(*negative) == before,
              "invalid boundary accepted or mutated: " + std::to_string(bad));
    }
    auto positive = fixture(ctx,34);
    require(StringRef(run(*positive->getOps<CircuitOp>().begin())).contains("  true,\n  64ULL\n));"),
            "positive host memory offset differs");
    llvm::outs() << "TSI header: varied base/index and memory offset; nine-address ABI; preserved hardware/annotations; 34 atomic rejections passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[1],&ctx); require(bool(boundary),"boundary parse");
      auto bc = *boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);
      require(succeeded(goldengate::prepareSimulationMasterHeader(bc,error)),error);
      require(succeeded(goldengate::prepareClockBridgeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareResetPulseHeader(bc,error)),error);
      require(succeeded(goldengate::prepareLoadMemHeader(bc,error)),error);
      require(succeeded(goldengate::preparePeekPokeHeader(bc,error)),error);
      require(succeeded(goldengate::prepareUARTHeader(bc,error)),error); body = run(bc);
      for (auto text : {".in_bits = 320", ".in_valid = 324", ".in_ready = 328",
                       ".out_bits = 332", ".out_valid = 336", ".out_ready = 340",
                       ".step_size = 344", ".done = 348", ".start = 352",
                       "},\n  0,\n  args,\n  true,\n  -2147483648ULL\n));"})
        require(StringRef(body).contains(text),"recorded U250 TSI allocation differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs() << "Recorded U250 boundary: TSI constructor composed with seven existing header sections\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
