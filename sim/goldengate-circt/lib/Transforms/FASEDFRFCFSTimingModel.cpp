// See LICENSE for license details.
// Oracle: TimingModel.scala, FRFCFSModel.scala and Util.scala AXI4Releaser.
#include "goldengate/FASEDFRFCFSTimingModel.h"
#include "goldengate/FASEDAXI4Releaser.h"
#include "goldengate/FASEDFRFCFSController.h"
#include "goldengate/FASEDLLCModel.h"
#include "goldengate/FASEDTimingMonitor.h"
#include "mlir/IR/Builders.h"
#include "goldengate/FASEDGeometry.h"
#include "llvm/Support/MathExtras.h"
#include <tuple>
#include "goldengate/FASEDDRAMProfile.h"
using namespace mlir;
using namespace circt::firrtl;

LogicalResult
goldengate::materializeFASEDFRFCFSTimingModel(CircuitOp circuit,
                                              unsigned maxFlight,
                                              FModuleOp &result,
                                              std::string &error) {
  if (maxFlight == 0 || maxFlight > 32) {
    error = "FRFCFS timing model requires selected maxFlight one through "
            "thirty-two";
    return failure();
  }
  auto geometry = getFASEDGeometry(circuit);
  bool useLLC = hasFASEDLLC(circuit);
  std::string suffix = std::to_string(maxFlight);
  std::string name = "GGFASEDFRFCFSTimingModel" + suffix;
  std::string controllerName =
      (isFASEDFCFS(circuit) ? "GGFASEDFCFSController"
                            : "GGFASEDFRFCFSController") +
      suffix;
  std::string frontendName = "GGFASEDUnifiedTransactionScheduler" + suffix;
  std::string monitorName = "GGFASEDTimingMonitor" + suffix;
  SmallVector<StringRef> symbols{name,
                                 controllerName,
                                 frontendName,
                                 monitorName,
                                 "GGFASEDLLCModel",
                                 "GGFASEDLLCMetadata",
                                 "GGFASEDLLCMSHRTable",
                                 "GGFASEDAXI4Releaser",
                                 "GGFASEDDRAMBackend",
                                 isFASEDFCFS(circuit)
                                     ? "GGFASEDFCFSCommandScheduler"
                                     : "GGFASEDFRFCFSCommandScheduler",
                                 "GGFASEDRankStateTracker",
                                 "GGFASEDBankStateTracker",
                                 "GGFASEDRefreshUnit",
                                 "GGFASEDCollapsingBuffer"};
  for (auto module : circuit.getOps<FModuleLike>())
    if (llvm::is_contained(symbols, module.getModuleName())) {
      error = "FRFCFS timing model requires free model and dependency symbols";
      return failure();
    }
  FModuleOp llc, controller, releaser, monitor;
  if ((useLLC && failed(materializeFASEDLLCModel(circuit, llc, error))) ||
      failed(materializeFASEDFRFCFSController(
          circuit, maxFlight, controller, error)) ||
      failed(materializeFASEDAXI4Releaser(circuit, releaser, error)) ||
      failed(materializeFASEDTimingMonitor(circuit, maxFlight, monitor, error)))
    return failure();

  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  SmallVector<PortInfo> ports;
  auto copy = [&](FModuleOp m, StringRef n) {
    for (auto p : m.getPorts())
      if (p.getName() == n) {
        ports.push_back(p);
        return;
      }
    llvm_unreachable("unknown copied timing model port");
  };
  for (auto n : {"clock",
                 "reset",
                 "targetFire",
                 "arValid",
                 "arReady",
                 "arId",
                 "arLen",
                 "arAddr",
                 "awValid",
                 "awReady",
                 "awId",
                 "awLen",
                 "awAddr",
                 "wValid",
                 "wReady",
                 "wLast"})
    copy(useLLC ? llc : controller, n);
  if (useLLC)
    for (auto n : {"wayBits", "setBits", "blockBits", "activeMSHRs"})
      copy(llc, n);
  for (auto n : {"arBurst", "awBurst"})
    ports.push_back({b.getStringAttr(n), u(2), Direction::In});
  for (auto p : releaser.getPorts())
    if (p.getName() != "clock" && p.getName() != "reset" &&
        p.getName() != "targetFire" && !p.getName().starts_with("next"))
      ports.push_back(p);
  for (auto n : {"timings",
                 "schedulerWindowSize",
                 "openPagePolicy",
                 "backendLatency",
                 "bankAddrOffset",
                 "bankAddrMask",
                 "rankAddrOffset",
                 "rankAddrMask",
                 "rowAddrOffset",
                 "rowAddrMask",
                 "targetCycle"})
    if (StringRef(n) != "schedulerWindowSize" || !isFASEDFCFS(circuit))
      copy(controller, n);
  for (auto p : monitor.getPorts())
    if (p.direction == Direction::Out)
      ports.push_back(p);
  // Expose observations without importing oracle state into any native stage.
  for (auto pair :
       {std::pair<FModuleOp, StringRef>{controller, "dram_"}, {llc, "llc_"}})
    if (pair.first)
      for (auto p : pair.first.getPorts())
        if (p.direction == Direction::Out)
          ports.push_back({b.getStringAttr((pair.second + p.getName()).str()),
                           p.type,
                           p.direction});

  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module =
      b.create<FModuleOp>(loc,
                          b.getStringAttr(name),
                          ConventionAttr::get(ctx, Convention::Internal),
                          ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](StringRef n) -> Value {
    for (unsigned i = 0; i < ports.size(); ++i)
      if (ports[i].getName() == n)
        return module.getBodyBlock()->getArgument(i);
    llvm_unreachable("unknown timing model port");
  };
  InstanceOp l;
  if (useLLC)
    l = b.create<InstanceOp>(loc, llc, "llc");
  auto d = b.create<InstanceOp>(loc, controller, "dram");
  auto r = b.create<InstanceOp>(loc, releaser, "release");
  auto m = b.create<InstanceOp>(loc, monitor, "monitor");
  auto pin = [&](InstanceOp inst, FModuleOp helper, StringRef n) -> Value {
    for (unsigned i = 0; i < helper.getNumPorts(); ++i)
      if (helper.getPortName(i) == n)
        return inst.getResult(i);
    llvm_unreachable("unknown timing instance port");
  };
  auto lp = [&](StringRef n) { return pin(l, llc, n); };
  auto dp = [&](StringRef n) { return pin(d, controller, n); };
  auto rp = [&](StringRef n) { return pin(r, releaser, n); };
  auto mp = [&](StringRef n) { return pin(m, monitor, n); };
  auto con = [&](Value dst, Value src) {
    b.create<StrictConnectOp>(loc, dst, src);
  };
  auto constant = [&](unsigned w, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  auto and_ = [&](Value a, Value z) -> Value {
    return b.create<AndPrimOp>(loc, a, z);
  };
  auto or_ = [&](Value a, Value z) -> Value {
    return b.create<OrPrimOp>(loc, a, z);
  };
  auto not_ = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto eq = [&](Value a, Value z) -> Value {
    return b.create<EQPrimOp>(loc, a, z);
  };
  auto field = [&](Value v, StringRef n) -> Value {
    return b.create<SubfieldOp>(loc, v, n);
  };
  auto index = [&](Value v, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, v, i);
  };
  for (auto n : {"clock", "reset", "targetFire"}) {
    if (useLLC)
      con(lp(n), arg(n));
    con(dp(n), arg(n));
    con(rp(n), arg(n));
    con(mp(n), arg(n));
  }
  if (useLLC) {
    for (auto n : {"arValid",
                   "arId",
                   "arLen",
                   "arAddr",
                   "awValid",
                   "awId",
                   "awLen",
                   "awAddr",
                   "wValid",
                   "wLast",
                   "wayBits",
                   "setBits",
                   "blockBits",
                   "activeMSHRs"})
      con(lp(n), arg(n));
    for (auto n : {"arReady", "awReady", "wReady"})
      con(arg(n), lp(n));
  } else {
    for (auto n : {"arValid",
                   "arId",
                   "arLen",
                   "arAddr",
                   "awValid",
                   "awId",
                   "awLen",
                   "awAddr",
                   "wValid",
                   "wLast"})
      con(dp(n), arg(n));
    for (auto n : {"arReady", "awReady", "wReady"})
      con(arg(n), dp(n));
  }
  for (auto p : releaser.getPorts())
    if (p.getName() != "clock" && p.getName() != "reset" &&
        p.getName() != "targetFire" && !p.getName().starts_with("next")) {
      if (p.direction == Direction::In)
        con(rp(p.getName()), arg(p.getName()));
      else
        con(arg(p.getName()), rp(p.getName()));
    }
  for (auto n : {"timings",
                 "schedulerWindowSize",
                 "openPagePolicy",
                 "backendLatency",
                 "bankAddrOffset",
                 "bankAddrMask",
                 "rankAddrOffset",
                 "rankAddrMask",
                 "rowAddrOffset",
                 "rowAddrMask"})
    if (StringRef(n) != "schedulerWindowSize" || !isFASEDFCFS(circuit))
      con(dp(n), arg(n));
  con(arg("targetCycle"), dp("targetCycle"));
  if (useLLC) {
    for (auto ch : {"Ar", "Aw"}) {
      std::string native = StringRef(ch) == "Ar" ? "ar" : "aw";
      std::string mem = std::string("mem") + ch;
      for (auto suffix : {"Valid", "Id", "Len", "Addr"})
        con(dp(native + suffix), lp(mem + suffix));
      con(lp(mem + "Ready"), dp(native + "Ready"));
    }
    con(dp("wValid"), lp("memWValid"));
    con(dp("wLast"), lp("memWLast"));
    con(lp("memWReady"), dp("wReady"));
    con(lp("memRValid"), dp("completedReadValid"));
    con(lp("memRId"), dp("completedReadID"));
    con(dp("completedReadReady"), lp("memRReady"));
    con(lp("memBValid"), dp("completedWriteValid"));
    con(lp("memBId"), dp("completedWriteID"));
    con(dp("completedWriteReady"), lp("memBReady"));
    for (auto ch : {"Read", "Write"}) {
      std::string response = StringRef(ch) == "Read" ? "rResp" : "wResp";
      std::string next = std::string("next") + ch;
      con(rp(next + "Valid"), lp(response + "Valid"));
      con(rp(next + "Id"), lp(response + "Id"));
      con(lp(response + "Ready"), rp(next + "Ready"));
    }
    con(rp("nextReadLen"), lp("rRespLen"));
  } else {
    for (auto ch : {"Read", "Write"}) {
      std::string next = std::string("next") + ch;
      std::string completed = std::string("completed") + ch;
      con(rp(next + "Valid"), dp(completed + "Valid"));
      con(rp(next + "Id"), dp(completed + "ID"));
      con(dp(completed + "Ready"), rp(next + "Ready"));
    }
    con(rp("nextReadLen"), dp("completedReadLen"));
  }
  for (auto ch : {"ar", "aw", "w", "r", "b"})
    con(mp(std::string(ch) + "Fire"),
        and_(arg(std::string(ch) + "Valid"), arg(std::string(ch) + "Ready")));
  con(mp("wLast"), arg("wLast"));
  con(mp("rLast"), arg("rLast"));
  con(dp("pendingAWReq"), mp("pendingAWReq"));
  con(dp("pendingWReq"), mp("pendingWReq"));
  con(mp("selectedCmd"), dp("selectedCmd"));
  con(mp("cmdRank"), dp("cmdRank"));
  Value rankStates = constant(4, 0);
  for (unsigned rank = 0; rank < 4; ++rank) {
    Value state = index(dp("rankStati"), rank);
    Value padded = b.create<PadPrimOp>(loc, field(state, "state"), 4);
    Value shifted = b.create<ShlPrimOp>(loc, padded, rank);
    rankStates = or_(rankStates, b.create<BitsPrimOp>(loc, shifted, 3, 0));
    Value bankMask = constant(8, 0);
    for (unsigned bank = 0; bank < 8; ++bank) {
      Value legal = field(index(field(state, "banks"), bank), "canACT");
      Value widened = b.create<PadPrimOp>(loc, legal, 8);
      Value bit = b.create<ShlPrimOp>(loc, widened, bank);
      bankMask = or_(bankMask, b.create<BitsPrimOp>(loc, bit, 7, 0));
    }
    con(index(mp("bankCanACT"), rank), bankMask);
  }
  con(mp("rankStates"), rankStates);
  for (auto p : monitor.getPorts())
    if (p.direction == Direction::Out)
      con(arg(p.getName()), mp(p.getName()));
  for (auto pair :
       {std::tuple<FModuleOp, InstanceOp, StringRef>{controller, d, "dram_"},
        {llc, l, "llc_"}})
    if (std::get<0>(pair))
      for (auto p : std::get<0>(pair).getPorts())
        if (p.direction == Direction::Out)
          con(arg((std::get<2>(pair) + p.getName()).str()),
              pin(std::get<1>(pair), std::get<0>(pair), p.getName()));

  Value assertionEnable = and_(arg("targetFire"), not_(arg("reset")));
  for (auto ch : {"ar", "aw"}) {
    std::string stem(ch);
    Value valid = arg(stem + "Valid"), len = arg(stem + "Len");
    Value burst = arg(stem + "Burst");
    Value supported =
        or_(eq(burst, constant(2, 1)),
            and_(eq(burst, constant(2, 0)), eq(len, constant(8, 0))));
    b.create<AssertOp>(
        loc,
        arg("clock"),
        or_(not_(valid), supported),
        assertionEnable,
        "Memory model only supports incrementing or fixed (len=0) bursts",
        ValueRange{},
        stem + "BurstProtocol");
    Value accepted = and_(valid, arg(stem + "Ready"));
    Value shortEnough =
        b.create<LEQPrimOp>(loc,
                            len,
                            constant(8,
                                     stem == "ar" ? geometry.readBeats - 1
                                                  : geometry.writeBeats - 1));
    b.create<AssertOp>(
        loc,
        arg("clock"),
        or_(not_(accepted), shortEnough),
        assertionEnable,
        "Burst length exceeds selected memory-model maximum of 8",
        ValueRange{},
        stem + "BurstLength");
  }
  result = module;
  return success();
}
