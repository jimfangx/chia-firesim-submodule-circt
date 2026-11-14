// See LICENSE for license details.
// Oracle: FRFCFSModel.scala, TransactionScheduler.scala and DRAMBackend.scala.
#include "goldengate/FASEDFRFCFSController.h"
#include "goldengate/FASEDFRFCFSCommandScheduler.h"
#include "goldengate/FASEDUnifiedTransactionScheduler.h"
#include "goldengate/FASEDDRAMBackend.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include "goldengate/FASEDDRAMProfile.h"
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDFRFCFSController(
    CircuitOp circuit, unsigned maxWrites, FModuleOp &result, std::string &error) {
  if (maxWrites == 0 || maxWrites > 32) {
    error =
        "FRFCFS controller requires selected maxWrites one through thirty-two";
    return failure();
  }
  std::string name = (isFASEDFCFS(circuit) ? "GGFASEDFCFSController"
                                           : "GGFASEDFRFCFSController") +
                     std::to_string(maxWrites);
  std::string frontendName = "GGFASEDUnifiedTransactionScheduler" + std::to_string(maxWrites);
  SmallVector<StringRef> symbols{name,
                                 frontendName,
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
      error = "FRFCFS controller requires free controller and dependency symbols";
      return failure();
    }
  FModuleOp frontend, scheduler, backend;
  if (failed(materializeFASEDUnifiedTransactionScheduler(circuit, maxWrites, frontend, error)) ||
      failed(materializeFASEDFRFCFSCommandScheduler(circuit, scheduler, error)) ||
      failed(materializeFASEDDRAMBackend(circuit, backend, error))) return failure();
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  SmallVector<PortInfo> ports;
  const auto frontendPorts = frontend.getPorts();
  const auto schedulerPorts = scheduler.getPorts();
  auto port = [&](StringRef n, Type t, Direction d) { ports.push_back({b.getStringAttr(n), t, d}); };
  auto copy = [&](FModuleOp m, StringRef n) {
    for (auto p : m.getPorts()) if (p.getName() == n) { ports.push_back(p); return; }
    llvm_unreachable("unknown copied port");
  };
  for (unsigned i = 0; i < 16; ++i) ports.push_back(frontendPorts[i]);
  for (auto n : {"pendingWReq", "pendingAWReq"})
    port(n, u(llvm::Log2_64_Ceil(maxWrites + 1)), Direction::In);
  for (auto n : {"completedReadValid", "completedReadReady", "completedReadID", "completedReadLen",
                 "completedWriteValid", "completedWriteReady", "completedWriteID"}) copy(backend, n);
  for (auto n :
       {"timings", "schedulerWindowSize", "openPagePolicy", "backendLatency"})
    if (StringRef(n) != "schedulerWindowSize" || !isFASEDFCFS(circuit))
      copy(scheduler, n);
  for (auto stem : {"bank", "rank", "row"}) {
    port(std::string(stem)+"AddrOffset", u(32), Direction::In);
    port(std::string(stem)+"AddrMask", u(StringRef(stem)=="bank" ? 3 : StringRef(stem)=="rank" ? 2 : 26), Direction::In);
  }
  port("targetCycle", u(64), Direction::Out);
  for (unsigned i = 13; i < schedulerPorts.size(); ++i) ports.push_back(schedulerPorts[i]);
  for (auto n : {"readCount", "writeCount"}) copy(backend, n);
  for (auto n : {"backendReadReady", "backendWriteReady"}) port(n, u(1), Direction::Out);
  for (unsigned i = 19; i < frontendPorts.size(); ++i) ports.push_back(frontendPorts[i]);
  port("decoded", scheduler.getPortType(5), Direction::Out);
  port("ranksInUse", u(4), Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module = b.create<FModuleOp>(loc, b.getStringAttr(name), ConventionAttr::get(ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](StringRef n) -> Value {
    for (unsigned i=0; i<ports.size(); ++i) if (ports[i].getName()==n) return module.getBodyBlock()->getArgument(i);
    llvm_unreachable("unknown controller port");
  };
  auto f = b.create<InstanceOp>(loc, frontend, "transactions");
  auto s = b.create<InstanceOp>(loc, scheduler, "commands");
  auto d = b.create<InstanceOp>(loc, backend, "backend");
  auto pin = [&](InstanceOp inst, FModuleOp m, StringRef n) -> Value {
    for (unsigned i=0; i<m.getNumPorts(); ++i) if (m.getPortName(i)==n) return inst.getResult(i);
    llvm_unreachable("unknown instance port");
  };
  auto fp = [&](StringRef n) { return pin(f, frontend, n); };
  auto sp = [&](StringRef n) { return pin(s, scheduler, n); };
  auto dp = [&](StringRef n) { return pin(d, backend, n); };
  auto con = [&](Value dst, Value src) { b.create<StrictConnectOp>(loc, dst, src); };
  auto c = [&](unsigned w, uint64_t v) -> Value { return b.create<ConstantOp>(loc, u(w), APInt(w,v)); };
  auto bits = [&](Value v,unsigned hi,unsigned lo) -> Value { return b.create<BitsPrimOp>(loc,v,hi,lo); };
  auto field = [&](Value v,StringRef n) -> Value { return b.create<SubfieldOp>(loc,v,n); };
  auto mux = [&](Value cond,Value yes,Value no) -> Value { return b.create<MuxPrimOp>(loc,cond,yes,no); };
  for (auto n : {"clock", "reset", "targetFire"}) {
    con(fp(n), arg(n)); con(sp(n), arg(n)); con(dp(n), arg(n));
  }
  for (unsigned i=3;i<16;++i) {
    const auto &p=frontendPorts[i];
    if (p.direction==Direction::In) con(f.getResult(i), arg(p.getName()));
    else con(arg(p.getName()), f.getResult(i));
  }
  for (auto n : {"pendingWReq","pendingAWReq"})
    con(fp(n), b.create<PadPrimOp>(loc,arg(n),maxWrites+1));
  Value enabledReset=b.create<AndPrimOp>(loc,arg("reset"),arg("targetFire"));
  Value cycle=b.create<RegResetOp>(loc,u(64),arg("clock"),enabledReset,c(64,0),"tCycle").getResult();
  Value increment=b.create<AddPrimOp>(loc,cycle,c(64,1));
  con(cycle,mux(arg("targetFire"),bits(increment,63,0),cycle));
  con(arg("targetCycle"),cycle);
  con(sp("tCycle"),bits(cycle,6,0)); con(dp("tCycle"),bits(cycle,11,0));
  con(fp("nextReady"),sp("nextReady")); con(sp("nextValid"),fp("nextValid"));
  for (unsigned i=19;i<frontend.getNumPorts();++i) con(arg(frontend.getPortName(i)),f.getResult(i));
  Value decoded=arg("decoded");
  for (auto pair : {std::pair<StringRef,StringRef>{"id","nextId"},{"len","nextLen"},{"isWrite","nextIsWrite"}})
    con(field(decoded,pair.first),fp(pair.second));
  for (auto stem : {"bank","rank","row"}) {
    unsigned width=StringRef(stem)=="bank"?3:StringRef(stem)=="rank"?2:26;
    Value shifted=b.create<DShrPrimOp>(loc,fp("nextAddr"),arg(std::string(stem)+"AddrOffset"));
    Value masked=b.create<AndPrimOp>(loc,bits(shifted,width-1,0),arg(std::string(stem)+"AddrMask"));
    con(field(decoded,std::string(stem)+"Addr"),masked);
    if (width!=26) {
      Value onehot=b.create<DShlPrimOp>(loc,c(1,1),masked);
      con(field(decoded,std::string(stem)+"AddrOH"),onehot);
    }
  }
  con(sp("nextBits"),decoded);
  Value rankMask=arg("rankAddrMask");
  Value ranks=mux(bits(rankMask,1,1),c(4,15),mux(bits(rankMask,0,0),c(4,3),c(4,1)));
  con(arg("ranksInUse"),ranks); con(sp("ranksInUse"),ranks);
  for (auto n : {"timings", "openPagePolicy", "backendLatency"})
    con(sp(n), arg(n));
  con(sp("schedulerWindowSize"),
      isFASEDFCFS(circuit) ? c(3, 0) : arg("schedulerWindowSize"));
  con(sp("backendReadReady"),dp("newReadReady")); con(sp("backendWriteReady"),dp("newWriteReady"));
  con(arg("backendReadReady"),dp("newReadReady")); con(arg("backendWriteReady"),dp("newWriteReady"));
  for (auto pair : {std::pair<StringRef,StringRef>{"newReadValid","newReadValid"},{"newReadID","newReadId"},
      {"newReadLen","newReadLen"},{"newWriteValid","newWriteValid"},{"newWriteID","newWriteId"},
      {"readLatency","readLatency"},{"writeLatency","writeLatency"}}) con(dp(pair.first),sp(pair.second));
  for (unsigned i=13;i<scheduler.getNumPorts();++i) con(arg(scheduler.getPortName(i)),s.getResult(i));
  for (auto n : {"completedReadReady","completedWriteReady"}) con(dp(n),arg(n));
  for (auto n : {"completedReadValid","completedReadID","completedReadLen","completedWriteValid","completedWriteID","readCount","writeCount"}) con(arg(n),dp(n));
  result=module;
  return success();
}
