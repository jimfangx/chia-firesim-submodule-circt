// See LICENSE for license details.
// Composition regression for the actual selected no-Print functional path.
// The initial channel fabric/target is a typed, constant-output scaffold copied
// from FASEDTokenEngineTest. Every FASED stage below is production hardware;
// no WriteEgress or timing/admission boundary is supplied by this test.
// This checks pass composition and geometry, not target behavior or SFC parity.
#include "circt/Dialect/HW/HWDialect.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/FASEDFRFCFSBridge.h"
#include "goldengate/FASEDFRFCFSBridgeMMIO.h"
#include "goldengate/FASEDFRFCFSMMIO.h"
#include "goldengate/FASEDHostOutstanding.h"
#include "goldengate/FASEDIngressARQueue.h"
#include "goldengate/FASEDIngressAWQueue.h"
#include "goldengate/FASEDIngressCredits.h"
#include "goldengate/FASEDIngressDeadlock.h"
#include "goldengate/FASEDIngressIssue.h"
#include "goldengate/FASEDIngressOrder.h"
#include "goldengate/FASEDIngressWQueue.h"
#include "goldengate/FASEDReadBuffer.h"
#include "goldengate/FASEDReadScheduler.h"
#include "goldengate/FASEDRequestLimits.h"
#include "goldengate/FASEDTokenEngine.h"
#include "goldengate/FASEDWriteEgress.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <stdexcept>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok)
    throw std::runtime_error(why);
}
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m : c.getOps<FModuleOp>())
    if (m.getName() == name)
      return m;
  throw std::runtime_error("missing module " + name.str());
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned flight) {
  const bool normalized = true, selected = false;
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"GGBlockDevResponseSchedulerWrapper\" { "
      "firrtl.module @GGBlockDevResponseSchedulerWrapper() {} } }",
      &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  if (selected)
    c.setName("GGSelectedPrintCPUStreamPlatformWrapper");
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  auto bit = uint(1);
  const llvm::StringRef locals[]{"axi4_aw_fwd",
                                 "axi4_w_fwd",
                                 "axi4_b_rev",
                                 "axi4_ar_fwd",
                                 "axi4_r_rev",
                                 "reset",
                                 "axi4_aw_rev",
                                 "axi4_w_rev",
                                 "axi4_b_fwd",
                                 "axi4_ar_rev",
                                 "axi4_r_fwd"};
  const std::vector<std::string> fields[]{
      {"bits_user",
       "bits_id",
       "bits_region",
       "bits_qos",
       "bits_prot",
       "bits_cache",
       "bits_lock",
       "bits_burst",
       "bits_size",
       "bits_len",
       "bits_addr",
       "valid"},
      {"bits_user", "bits_strb", "bits_id", "bits_last", "bits_data", "valid"},
      {""},
      {"bits_user",
       "bits_id",
       "bits_region",
       "bits_qos",
       "bits_prot",
       "bits_cache",
       "bits_lock",
       "bits_burst",
       "bits_size",
       "bits_len",
       "bits_addr",
       "valid"},
      {""},
      {""},
      {""},
      {""},
      {"bits_user", "bits_id", "bits_resp", "valid"},
      {""},
      {"bits_user", "bits_id", "bits_last", "bits_data", "bits_resp", "valid"}};
  const unsigned pairs[]{6, 7, 8, 9, 10, 5, 0, 1, 2, 3, 4};
  SmallVector<PortInfo> ports{
      {b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 11; ++j) {
    FIRRTLBaseType payload = FIRRTLBaseType(bit);
    if (j == 0 || j == 1 || j == 3 || j == 8 || j == 10) {
      SmallVector<BundleType::BundleElement> elements;
      for (auto &name : fields[j]) {
        unsigned w = name == "bits_data"                         ? 64
                     : name == "bits_addr"                       ? 35
                     : name == "bits_len" || name == "bits_strb" ? 8
                     : name == "bits_id" || name == "bits_region" ||
                             name == "bits_qos" || name == "bits_cache"
                         ? 4
                     : name == "bits_size" || name == "bits_prot"  ? 3
                     : name == "bits_resp" || name == "bits_burst" ? 2
                                                                   : 1;
        if (!normalized || name != "valid")
          elements.push_back(
              {b.getStringAttr(normalized ? name.substr(5) : name),
               false,
               uint(w)});
      }
      payload = BundleType::get(&ctx, elements);
      if (normalized)
        payload = BundleType::get(&ctx,
                                  {{b.getStringAttr("valid"), false, bit},
                                   {b.getStringAttr("bits"), false, payload}});
    }
    auto token = BundleType::get(&ctx,
                                 {{b.getStringAttr("ready"), true, bit},
                                  {b.getStringAttr("valid"), false, bit},
                                  {b.getStringAttr("bits"), false, payload}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)),
                     token,
                     j < 6 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("ep_0_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(),
                      b.getStringAttr(c.getName()),
                      ConventionAttr::get(&ctx, Convention::Internal),
                      ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) {
    return b.getNamedAttr(n, b.getStringAttr(v));
  };
  auto dict = [&](std::initializer_list<NamedAttribute> a) {
    return b.getDictionaryAttr(a);
  };
  auto path = [&](unsigned j, llvm::StringRef suffix) {
    std::string normalizedSuffix = suffix.str();
    if (normalized && suffix.starts_with(".bits.bits_"))
      normalizedSuffix.replace(10, 1, ".");
    return b.getStringAttr("~" + c.getName().str() + "|" + c.getName().str() +
                           ">token" + std::to_string(j) + normalizedSuffix);
  };
  SmallVector<Attribute> raw{dict(
      {str("class", goldengate::AnnotationClasses::BridgeIO),
       str("widgetClass", "midas.models.FASEDMemoryTimingModel"),
       str("target", "~FireSim|FireSim>ep_0"),
       b.getNamedAttr(
           "widgetConstructorKey",
           dict({str("class", "firesim.lib.bridges.CompleteConfig"),
                 b.getNamedAttr(
                     "axi4Edge",
                     dict({b.getNamedAttr("maxFlight",
                                          b.getI64IntegerAttr(flight)),
                           b.getNamedAttr(
                               "idReuse",
                               b.getI64IntegerAttr(flight == 10 ? 1 : 2)),
                           b.getNamedAttr("maxReadTransfer",
                                          b.getI64IntegerAttr(8)),
                           b.getNamedAttr("maxWriteTransfer",
                                          b.getI64IntegerAttr(8))})),
                 b.getNamedAttr(
                     "axi4Widths",
                     dict({b.getNamedAttr("addrBits", b.getI64IntegerAttr(35)),
                           b.getNamedAttr("dataBits", b.getI64IntegerAttr(64)),
                           b.getNamedAttr("idBits",
                                          b.getI64IntegerAttr(4))}))})),
       b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 11; ++j) {
    bool forward = j == 0 || j == 1 || j == 3 || j == 8 || j == 10,
         pipe = j == 5;
    NamedAttrList info;
    info.set("class",
             b.getStringAttr(
                 pipe ? goldengate::AnnotationClasses::PipeChannel
                 : forward
                     ? goldengate::AnnotationClasses::DecoupledForwardChannel
                     : goldengate::AnnotationClasses::DecoupledReverseChannel));
    if (pipe)
      info.set("latency", b.getI64IntegerAttr(1));
    if (forward) {
      info.set(j < 6 ? "validSource" : "validSink", path(j, ".bits.valid"));
      info.set(j < 6 ? "readySink" : "readySource", path(pairs[j], ".bits"));
    }
    SmallVector<Attribute> ends;
    for (auto &f : fields[j])
      ends.push_back(path(j, f.empty() ? ".bits" : ".bits." + f));
    raw.push_back(dict(
        {str("class", goldengate::AnnotationClasses::ChannelConnection),
         str("globalName", "ep_0_" + locals[j].str()),
         b.getNamedAttr("channelInfo", info.getDictionary(&ctx)),
         str("clock", "sameClock"),
         b.getNamedAttr(j < 6 ? "sources" : "sinks", b.getArrayAttr(ends))}));
  }
  raw.push_back(dict(
      {str("class", "test.Annotation"),
       str("target",
           "~" + c.getName().str() + "|" + c.getName().str() + ">other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(raw));
  c->setAttr("goldengate.fasedBaseCapacity", b.getI64IntegerAttr(32));
  return root;
}

// Drive only the scaffold's outward leaves, respecting Decoupled ready flips.
// Channel payload values are arbitrary zeros here; native composition must
// still bind real flow directions and leave no undriven functional inputs.
void initialize(FModuleOp top) {
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->begin());
  auto loc = top.getLoc();
  std::function<void(Value, bool)> drive = [&](Value v, bool output) {
    if (auto t = dyn_cast<BundleType>(v.getType())) {
      for (auto e : t.getElements())
        drive(b.create<SubfieldOp>(loc, v, e.name), output != e.isFlip);
    } else if (output) {
      auto t = cast<UIntType>(v.getType());
      b.create<StrictConnectOp>(
          loc, v, b.create<ConstantOp>(loc, t, APInt(*t.getWidth(), 0)));
    }
  };
  for (auto [i, p] : llvm::enumerate(top.getPorts()))
    if (!isa<ClockType>(p.type))
      drive(top.getBodyBlock()->getArgument(i), p.direction == Direction::Out);
}
void composition(MLIRContext &ctx, unsigned flight, StringRef output = {}) {
  auto root = fixture(ctx, flight);
  auto c = *root->getOps<CircuitOp>().begin();
  initialize(named(c, c.getName()));
  std::string error;
  FModuleOp bank;
  require(succeeded(goldengate::materializeFASEDFRFCFSMMIO(c, bank, error)),
          error);
  auto step = [&](StringRef name, auto transform) {
    require(succeeded(transform(c, error)), name.str() + ": " + error);
    require(succeeded(verify(*root)), name.str() + " produced invalid IR");
    require(bank == named(c, "GGFASEDFRFCFSMMIO"), "early bank handle changed");
  };
  step("FASEDTokenEngine", goldengate::addFASEDTokenEngine);
  step("FASEDHostOutstanding", goldengate::addFASEDHostOutstanding);
  step("FASEDIngressAWQueue", goldengate::addFASEDIngressAWQueue);
  step("FASEDIngressWQueue", goldengate::addFASEDIngressWQueue);
  step("FASEDIngressARQueue", goldengate::addFASEDIngressARQueue);
  step("FASEDIngressCredits", goldengate::addFASEDIngressCredits);
  step("FASEDIngressOrder", goldengate::addFASEDIngressOrder);
  step("FASEDIngressIssue", goldengate::addFASEDIngressIssue);
  step("FASEDIngressDeadlock", goldengate::addFASEDIngressDeadlock);
  step("FASEDReadBuffer", goldengate::addFASEDReadBuffer);
  step("FASEDReadScheduler", goldengate::addFASEDReadScheduler);
  step("FASEDWriteEgress", goldengate::addFASEDWriteEgress);
  step("FASEDFRFCFSBridge", goldengate::addFASEDFRFCFSBridge);
  unsigned limit = 0, countWidth = 0;
  require(succeeded(goldengate::getFASEDRequestLimitProfile(
              c, limit, countWidth, error)) &&
              limit == flight && countWidth == llvm::Log2_64_Ceil(flight + 1),
          "functional platform capacity was lost in the real pass chain");
  auto aw = named(c, "GGFASEDIngressAWQueue" + std::to_string(flight));
  unsigned awMemories = 0;
  for (auto mem : aw.getOps<MemOp>()) {
    ++awMemories;
    require(mem.getDepth() == flight,
            "AW queue still has a ten/sixteen-entry limit");
  }
  require(awMemories == 1, "AW ingress storage missing");
  auto buffer =
      named(c, "GGFASEDReadBuffer16x" + std::to_string(flight == 10 ? 8 : 16));
  unsigned readMemories = 0;
  for (auto mem : buffer.getOps<MemOp>()) {
    ++readMemories;
    require(mem.getDepth() == (flight == 10 ? 128u : 256u),
            "read buffer ignored constructor ID reuse");
  }
  require(readMemories == 1, "read egress storage missing");
  require(succeeded(goldengate::attachFASEDFRFCFSMMIO(c, bank, error)), error);
  require(succeeded(verify(*root)), "MMIO attachment produced invalid IR");
  auto top = named(c, c.getName());
  for (auto p : top.getPorts()) {
    auto n = p.name.getValue();
    if (p.direction == Direction::In)
      require(n == "hostClock" || n == "hostReset" ||
                  n == "fased_host_read_response" ||
                  n == "fased_host_write_response",
              "functional/model input leaked past real composition: " +
                  n.str());
  }
  require(succeeded(goldengate::mapFASEDBridgeControl(c, 32, 12, error)),
          error);
  require(succeeded(verify(*root)), "control attachment produced invalid IR");
  goldengate::ControlMMIOWidget widget;
  require(
      succeeded(goldengate::deriveControlMMIOWidget(c,
                                                    "FASEDMemoryTimingModel_0",
                                                    "GGFASEDMCRFile",
                                                    {bank.getName()},
                                                    widget,
                                                    error)) &&
          widget.registerCount == 68,
      "real functional chain lost FRFCFS register ABI");
  SmallVector<goldengate::ControlMMIORegion> regions;
  require(succeeded(goldengate::allocateControlMMIORegions(
              32, {widget}, regions, error)) &&
              regions.size() == 1 && regions[0].size == 512,
          "FRFCFS control allocation differs");
  if (!output.empty()) {
    std::error_code ec;
    llvm::raw_fd_ostream os(output, ec);
    require(!ec, "cannot export real functional composition fixture");
    // Match emitSimulatorRTL's lowering clone: serialized Golden Gate
    // archive targets were already consumed and must not be replayed by
    // external firtool. Attached operation/port annotations stay intact.
    OwningOpRef<ModuleOp> lowering = cast<ModuleOp>((*root)->clone());
    auto loweringCircuit = *lowering->getOps<CircuitOp>().begin();
    loweringCircuit->removeAttr("rawAnnotations");
    require(c->getAttrOfType<ArrayAttr>("rawAnnotations") &&
                succeeded(verify(*lowering)),
            "fixture export changed source archive or produced invalid IR");
    lowering->print(os);
    os << '\n';
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    if (argc == 4 && StringRef(argv[1]) == "--fixture") {
      unsigned flight = 0;
      require(!StringRef(argv[2]).getAsInteger(10, flight) &&
                  (flight == 10 || flight == 20),
              "fixture requires selected maxFlight 10 or 20");
      composition(ctx, flight, argv[3]);
    } else {
      require(argc == 1, "usage: test [--fixture 10|20 output.mlir]");
      for (unsigned flight : {10u, 20u})
        composition(ctx, flight);
      llvm::outs() << "PASS real FRFCFS no-Print functional pipeline: token "
                      "engine through "
                      "functional queues/egress, native model, 68-word "
                      "MMIO/control; 10/1 and20/2\n";
    }
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
