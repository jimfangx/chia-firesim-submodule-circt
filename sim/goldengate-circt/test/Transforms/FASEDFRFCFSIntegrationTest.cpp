// See LICENSE for license details.
#include "FASEDFRFCFSBridgeFixture.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/FASEDFRFCFSBridge.h"
#include "goldengate/FASEDFRFCFSBridgeMMIO.h"
#include "goldengate/FASEDFRFCFSMMIO.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::string print(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m : c.getOps<FModuleOp>())
    if (m.getName() == name)
      return m;
  throw std::runtime_error("missing module " + name.str());
}
std::string path(Value v) {
  if (auto f = v.getDefiningOp<SubfieldOp>())
    return path(f.getInput()) + "." + f.getFieldName().str();
  if (auto a = dyn_cast<BlockArgument>(v))
    return cast<FModuleOp>(a.getOwner()->getParentOp())
        .getPortName(a.getArgNumber())
        .str();
  if (auto i = v.getDefiningOp<InstanceOp>())
    return i.getName().str() + "." +
           i.getPortName(cast<OpResult>(v).getResultNumber()).str();
  return "?";
}
std::map<std::string, std::string> routes(FModuleOp m) {
  std::map<std::string, std::string> out;
  for (auto &op : *m.getBodyBlock()) {
    Value d, s;
    if (auto c = dyn_cast<ConnectOp>(op)) {
      d = c.getDest();
      s = c.getSrc();
    }
    if (auto c = dyn_cast<StrictConnectOp>(op)) {
      d = c.getDest();
      s = c.getSrc();
    }
    if (d)
      require(out.emplace(path(d), path(s)).second, "duplicate wrapper driver");
  }
  return out;
}
// Minimal typed catalogs exercise the actual early compiler allocation API.
// These descriptors test allocation only; they do not model other widgets.
void earlyCatalog(MLIRContext &ctx) {
  for (bool frfcfs : {false, true}) {
    auto root =
        goldengate::test::makeFASEDFRFCFSWriteEgressFixture(ctx, 10, false);
    auto c = *root->getOps<CircuitOp>().begin();
    OpBuilder b(&ctx);
    auto u = [&](unsigned w) { return UIntType::get(&ctx, w); };
    auto add = [&](StringRef name,
                   unsigned count,
                   Direction dir,
                   unsigned first = 0) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      auto token = BundleType::get(&ctx,
                                   {{b.getStringAttr("ready"), true, u(1)},
                                    {b.getStringAttr("valid"), false, u(1)},
                                    {b.getStringAttr("bits"), false, u(32)}});
      auto lanes = FVectorType::get(token, count);
      auto mcr = BundleType::get(&ctx,
                                 {{b.getStringAttr("read"), false, lanes},
                                  {b.getStringAttr("write"), true, lanes},
                                  {b.getStringAttr("wstrb"), true, u(4)}});
      SmallVector<PortInfo> ports{{b.getStringAttr("mcr"), mcr, dir}};
      auto bank =
          b.create<FModuleOp>(c.getLoc(),
                              b.getStringAttr(name),
                              ConventionAttr::get(&ctx, Convention::Internal),
                              ports);
      SmallVector<Attribute> rows;
      for (unsigned i = 0; i < count; ++i)
        rows.push_back(b.getDictionaryAttr(
            {b.getNamedAttr("name",
                            b.getStringAttr(name.str() + std::to_string(i))),
             b.getNamedAttr("offset", b.getI32IntegerAttr(4 * (first + i))),
             b.getNamedAttr("readable", b.getBoolAttr(true)),
             b.getNamedAttr("writeable", b.getBoolAttr(false))}));
      bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
      return bank;
    };
    for (auto [name, n] :
         {std::pair<StringRef, unsigned>{"GGSimulationMasterBank", 3},
          {"GGBlockDevMMIOBank", 26},
          {"GGTSIMMIOBank", 9}})
      add(name, n, Direction::Out);
    for (auto [reg, adapter, n] :
         {std::tuple<StringRef, StringRef, unsigned>{
              "GGPeekPokeMMIOBank", "GGPeekPokeMCRFile", 7},
          {"GGResetPulseBridge", "GGResetPulseBridgeMCRFile", 2},
          {"GGUARTMMIOBank", "GGUARTMCRFile", 6},
          {"GGTracerVTriggerConfig", "GGTracerVMCRFile", 15},
          {"GGSingleClockBridge", "GGClockBridgeMCRFile", 6},
          {"GGCPUStreamCountBank", "GGCPUStreamMCRFile", 1}}) {
      add(reg, n, Direction::Out);
      add(adapter, n, Direction::In);
    }
    add("GGLoadMemWriteMMIOBank", 6, Direction::Out);
    add("GGLoadMemWriteDataWrapper", 1, Direction::Out, 6);
    add("GGLoadMemReadRequestWrapper", 1, Direction::Out, 7);
    add("GGLoadMemReadDataWrapper", 1, Direction::Out, 8);
    add("GGLoadMemMCRFile", 9, Direction::In);
    std::string error;
    if (frfcfs) {
      FModuleOp bank;
      require(succeeded(goldengate::materializeFASEDFRFCFSMMIO(c, bank, error)),
              error);
    } else {
      c->removeAttr("goldengate.fasedBaseCapacity");
      unsigned offset = 0;
      for (auto [name, n] :
           {std::pair<StringRef, unsigned>{"GGFASEDLatencyRegisters", 2},
            {"GGFASEDRequestLimits", 2},
            {"GGFASEDHistograms", 5},
            {"GGFASEDStatistics", 9},
            {"GGFASEDFunctionalModelRegister", 1},
            {"GGFASEDResponseErrors", 2}}) {
        add(name, n, Direction::Out, offset);
        offset += n;
      }
    }
    SmallVector<goldengate::ControlMMIOWidget> widgets;
    auto before = print(c);
    require(succeeded(goldengate::deriveRocketControlMMIOCatalog(
                c, {}, widgets, error)),
            error);
    require(before == print(c) && widgets.size() == 11 &&
                widgets[5].name == "FASEDMemoryTimingModel_0" &&
                widgets[5].registerCount == (frfcfs ? 68u : 21u),
            "early profile catalog differs or mutates IR");
    SmallVector<goldengate::ControlMMIORegion> regions;
    require(succeeded(goldengate::allocateControlMMIORegions(
                32, widgets, regions, error)),
            error);
    auto region = llvm::find_if(
        regions, [](auto &r) { return r.name == "FASEDMemoryTimingModel_0"; });
    require(region != regions.end() && region->size == (frfcfs ? 512u : 128u),
            "early FASED region size differs");
    for (unsigned mode = 0; mode < 4; ++mode) {
      OwningOpRef<CircuitOp> bad = cast<CircuitOp>(c->clone());
      if (mode == 0)
        (*bad)->setAttr("goldengate.fasedBaseCapacity", b.getStringAttr("32"));
      if (mode == 1)
        (*bad)->setAttr("goldengate.fasedBaseCapacity",
                        b.getI64IntegerAttr(64));
      if (mode == 2)
        (*bad)->setAttr("goldengate.fasedBaseCapacity",
                        b.getI64IntegerAttr(frfcfs ? 16 : 32));
      if (mode == 3) {
        if (frfcfs) {
          b.setInsertionPointToEnd(bad->getBodyBlock());
          b.create<FModuleOp>(c.getLoc(),
                              b.getStringAttr("GGFASEDLatencyRegisters"),
                              ConventionAttr::get(&ctx, Convention::Internal),
                              ArrayRef<PortInfo>{});
        } else {
          (*bad)->setAttr("goldengate.fasedBaseCapacity",
                          b.getI64IntegerAttr(-16));
        }
      }
      auto snapshot = print(*bad);
      SmallVector<goldengate::ControlMMIOWidget> sentinel{{"sentinel", 123}};
      require(failed(goldengate::deriveRocketControlMMIOCatalog(
                  *bad, {}, sentinel, error)) &&
                  sentinel.size() == 1 && sentinel[0].name == "sentinel" &&
                  print(*bad) == snapshot,
              "early malformed/mixed profile accepted or mutated outputs");
    }
  }
}
void checkAttachment(MLIRContext &ctx,
                     unsigned flight,
                     bool selected,
                     StringRef output = {}) {
  auto root = goldengate::test::makeFASEDFRFCFSWriteEgressFixture(
      ctx, flight, selected);
  auto c = *root->getOps<CircuitOp>().begin();
  goldengate::test::initializeFASEDFRFCFSWriteEgressFixture(
      named(c, c.getName()));
  std::string error;
  FModuleOp bank;
  require(succeeded(goldengate::materializeFASEDFRFCFSMMIO(c, bank, error)),
          error);
  auto bankBefore = print(bank);
  // This is the production order: bank exists before control allocation and
  // the functional/timing attachment. Existing bank handles must stay valid.
  require(succeeded(goldengate::addFASEDFRFCFSBridge(c, error)), error);
  require(bank == named(c, "GGFASEDFRFCFSMMIO") && print(bank) == bankBefore,
          "functional attachment invalidated early bank");
  auto inner = named(c, c.getName());
  auto before = print(inner);
  require(succeeded(goldengate::attachFASEDFRFCFSMMIO(c, bank, error)), error);
  require(succeeded(verify(*root)), "integrated FRFCFS bank invalid");
  require(print(inner) == before && print(bank) == bankBefore,
          "attachment changed existing module state");
  auto wrapper = named(c, c.getName());
  auto wiring = routes(wrapper);
  auto route = [&](StringRef dest, StringRef src) {
    require(wiring.count(dest.str()) && wiring.at(dest.str()) == src,
            "FRFCFS MMIO attachment route differs: " + dest.str());
  };
  route("registers.clock", "hostClock");
  route("registers.reset", "hostReset");
  route("sim.fased_ingress_relaxed", "registers.relaxFunctionalModel");
  route("fasedBridge_mcr", "registers.mcr");
  route("registers.rResp", "fased_host_read_response.bits.resp");
  route("registers.bResp", "fased_host_write_response.bits.resp");
  for (auto channel : {"r", "b"})
    for (auto suffix : {"Valid", "Ready"}) {
      std::string response = std::string(channel) == "r" ? "read" : "write";
      route("registers." + std::string(channel) + suffix,
            (StringRef(suffix) == "Ready" ? "sim.fased_host_" : "fased_host_") +
                response + "_response." +
                (StringRef(suffix) == "Valid" ? "valid" : "ready"));
    }
  for (auto p : bank.getPorts()) {
    auto n = p.getName();
    if (n == "clock" || n == "reset" || n == "mcr" ||
        n == "transactionQueueDepth" || n == "relaxFunctionalModel" ||
        n == "rValid" || n == "rReady" || n == "rResp" || n == "bValid" ||
        n == "bReady" || n == "bResp")
      continue;
    std::string model = "sim.fased_frfcfs_" +
                        (n == "llc_peakMSHRsUsed" ? std::string("llc_peakMSHRs")
                                                  : n.str()),
                regs = "registers." + n.str();
    route(p.direction == Direction::In ? regs : model,
          p.direction == Direction::In ? model : regs);
    for (auto w : wrapper.getPorts())
      require(w.getName() != ("fased_frfcfs_" + n).str(),
              "MMIO port left unconnected externally");
  }
  auto rows = wrapper->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(rows && rows.size() == 68, "combined MMIO registry lost");
  unsigned rw = 0;
  for (auto row : rows)
    rw += cast<DictionaryAttr>(row).getAs<BoolAttr>("writeable").getValue();
  require(rw == 32, "FRFCFS software permissions differ");
  goldengate::ControlMMIOWidget widget;
  require(
      succeeded(goldengate::deriveControlMMIOWidget(c,
                                                    "FASEDMemoryTimingModel_0",
                                                    bank.getName(),
                                                    {bank.getName()},
                                                    widget,
                                                    error,
                                                    Direction::Out)),
      error);
  SmallVector<goldengate::ControlMMIORegion> allocated;
  require(succeeded(goldengate::allocateControlMMIORegions(
              32, {widget}, allocated, error)) &&
              allocated.size() == 1 && allocated[0].size == 512,
          "FRFCFS bank must allocate 512 bytes");
  require(succeeded(goldengate::mapFASEDBridgeControl(c, 32, 12, error)),
          error);
  require(succeeded(verify(*root)), "FRFCFS control transport invalid");
  auto adapter = named(c, "GGFASEDMCRFile");
  require(
      succeeded(goldengate::deriveControlMMIOWidget(c,
                                                    "FASEDMemoryTimingModel_0",
                                                    adapter.getName(),
                                                    {bank.getName()},
                                                    widget,
                                                    error)) &&
          widget.registerCount == 68,
      error);
  unsigned selectors = 0;
  for (auto bits : adapter.getOps<BitsPrimOp>()) {
    auto addr = bits.getInput().getDefiningOp<SubfieldOp>();
    if (addr && addr.getFieldName() == "addr") {
      require(bits.getHi() == 8 && bits.getLo() == 2,
              "FRFCFS local address selector truncated");
      ++selectors;
    }
  }
  require(selectors == 2, "expected independent 7-bit AW/AR word selectors");
  if (!output.empty()) {
    std::error_code ec;
    llvm::raw_fd_ostream os(output, ec);
    require(!ec, "cannot export integration fixture");
    root->print(os);
    os << '\n';
  }
  // The retained inactive catalogue still uses its original typed bank. A
  // duplicate attach must reject before changing hardware or annotations.
  auto saved = print(c);
  require(failed(goldengate::attachFASEDFRFCFSMMIO(c, bank, error)) &&
              !error.empty() && print(c) == saved,
          "duplicate MMIO attach mutated circuit");
}
void rejection(MLIRContext &ctx) {
  for (unsigned mode = 0; mode < 10; ++mode) {
    auto root =
        goldengate::test::makeFASEDFRFCFSWriteEgressFixture(ctx, 20, false);
    auto c = *root->getOps<CircuitOp>().begin();
    std::string error;
    FModuleOp bank;
    require(succeeded(goldengate::materializeFASEDFRFCFSMMIO(c, bank, error)),
            error);
    require(succeeded(goldengate::addFASEDFRFCFSBridge(c, error)), error);
    OpBuilder b(&ctx);
    auto top = named(c, c.getName());
    if (mode == 0)
      c->removeAttr("rawAnnotations");
    if (mode == 1)
      c->setAttr("goldengate.fasedBaseCapacity", b.getI64IntegerAttr(16));
    if (mode == 2)
      c->setAttr("goldengate.fasedBaseCapacity", b.getStringAttr("32"));
    if (mode == 3)
      bank->removeAttr("goldengate.mmioRegisters");
    if (mode == 4) {
      auto names = top.getPortNames();
      SmallVector<Attribute> altered(names.begin(), names.end());
      for (unsigned i = 0; i < altered.size(); ++i)
        if (cast<StringAttr>(altered[i]).getValue() == "fased_frfcfs_timings")
          altered[i] = b.getStringAttr("missing");
      top->setAttr("portNames", b.getArrayAttr(altered));
    }
    if (mode == 5) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(),
                          b.getStringAttr("GGFASEDRequestLimits"),
                          top.getConventionAttr(),
                          ArrayRef<PortInfo>{});
    }
    if (mode == 6) {
      b.setInsertionPointToStart(top.getBodyBlock());
      b.create<InstanceOp>(c.getLoc(), bank, "used");
    }
    if (mode == 7)
      bank = {};
    if (mode == 8 || mode == 9) {
      auto rows = llvm::to_vector(
          bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
      NamedAttrList row(cast<DictionaryAttr>(rows[0]));
      if (mode == 8)
        row.set("name", b.getStringAttr("stale"));
      else
        row.set("writeable", b.getBoolAttr(false));
      rows[0] = row.getDictionary(&ctx);
      bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
    }
    auto saved = print(c);
    require(failed(goldengate::attachFASEDFRFCFSMMIO(c, bank, error)) &&
                !error.empty() && saved == print(c),
            "invalid MMIO attachment accepted or mutated IR");
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    if (argc == 3 && StringRef(argv[1]) == "--fixture") {
      checkAttachment(ctx, 20, false, argv[2]);
      return 0;
    }
    for (bool selected : {false, true})
      for (unsigned flight : {10u, 20u})
        checkAttachment(ctx, flight, selected);
    rejection(ctx);
    earlyCatalog(ctx);
    llvm::outs()
        << "PASS FRFCFS bank/model/host observation and 68-lane transport: "
           "four wrapper profiles, ten atomic rejections\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
