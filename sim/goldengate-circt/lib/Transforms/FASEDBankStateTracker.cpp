// See LICENSE for license details.
// Oracle: models/dram/DramCommon.scala BankStateTracker (371-449), and
// Util.scala DownCounter (170-187). Selected organization has 26-bit rows.
// DownCounter accepts a set only when it is >= the old value; otherwise it
// decrements, including on a rejected smaller set. Seven-bit timing arithmetic
// wraps exactly as Chisel UInt arithmetic does. No scheduler/platform acceptance
// is implied by this reusable primitive.
#include "goldengate/FASEDBankStateTracker.h"
#include "mlir/IR/Builders.h"
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDBankStateTracker(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDBankStateTracker";
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name) {
      error = "FASED bank state tracker helper already exists";
      return failure();
    }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto u = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = u(1);
  SmallVector<BundleType::BundleElement> fields;
  for (auto n : {"tAL", "tCAS", "tCMD", "tCWD", "tCCD", "tFAW", "tRAS", "tREFI",
                 "tRC", "tRCD", "tRFC", "tRRD", "tRP", "tRTP", "tRTRS", "tWR", "tWTR"})
    fields.push_back({b.getStringAttr(n), false, u(llvm::StringRef(n) == "tREFI" ? 14 :
                                                   llvm::StringRef(n) == "tRFC" ? 10 : 7)});
  SmallVector<PortInfo> ports;
  auto port = [&](StringRef n, Type t, Direction d) { ports.push_back({b.getStringAttr(n), t, d}); };
  port("clock", ClockType::get(ctx), Direction::In);
  for (auto n : {"reset", "targetFire"}) port(n, bit, Direction::In);
  port("selectedCmd", u(3), Direction::In);
  port("cmdUsesThisBank", bit, Direction::In);
  port("autoPRE", bit, Direction::In);
  port("cmdRow", u(26), Direction::In);
  port("timings", BundleType::get(ctx, fields), Direction::In);
  for (auto n : {"canCASW", "canCASR", "canPRE", "canACT", "state"}) port(n, bit, Direction::Out);
  port("openRow", u(26), Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module = b.create<FModuleOp>(loc, b.getStringAttr(name),
      ConventionAttr::get(ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned i) { return module.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n)->Value { return b.create<ConstantOp>(loc, u(w), APInt(w,n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc,d,s); };
  auto and_ = [&](Value a, Value z)->Value { return b.create<AndPrimOp>(loc,a,z); };
  auto not_ = [&](Value a)->Value { return b.create<NotPrimOp>(loc,a); };
  auto mux = [&](Value c, Value a, Value z)->Value { return b.create<MuxPrimOp>(loc,c,a,z); };
  auto eq = [&](Value a, Value z)->Value { return b.create<EQPrimOp>(loc,a,z); };
  auto trim = [&](Value v)->Value { return b.create<BitsPrimOp>(loc,v,6,0); };
  auto add = [&](Value a, Value z)->Value { return trim(b.create<AddPrimOp>(loc,a,z)); };
  auto sub = [&](Value a, Value z)->Value { return trim(b.create<SubPrimOp>(loc,a,z)); };
  auto timing = [&](StringRef n)->Value { return b.create<SubfieldOp>(loc,arg(7),n); };
  Value enabledReset = and_(arg(1),arg(2));
  auto reg = [&](unsigned w, StringRef n)->Value {
    return b.create<RegResetOp>(loc,u(w),arg(0),enabledReset,constant(w,0),n).getResult();
  };
  Value state = reg(1,"state"), pre = reg(7,"nextLegalPRE"), act = reg(7,"nextLegalACT"), cas = reg(7,"nextLegalCAS");
  Value row = b.create<RegOp>(loc,u(26),arg(0),"openRowAddr").getResult();
  Value zero = constant(7,0), one = constant(7,1);
  Value canCAS = and_(state,eq(cas,zero)), canPRE = and_(state,eq(pre,zero));
  Value canACT = and_(not_(state),eq(act,zero));
  connect(arg(8),canCAS); connect(arg(9),canCAS); connect(arg(10),canPRE);
  connect(arg(11),canACT); connect(arg(12),state); connect(arg(13),row);
  auto command = [&](unsigned n)->Value { return and_(arg(4),eq(arg(3),constant(3,n))); };
  Value isACT=command(1), isPRE=command(2), isCASW=command(3), isCASR=command(4);
  Value casAuto=and_(b.create<OrPrimOp>(loc,isCASW,isCASR),arg(5));
  Value nextState=mux(isACT,constant(1,1),mux(b.create<OrPrimOp>(loc,isPRE,casAuto),constant(1,0),state));
  connect(state,mux(arg(2),nextState,state));
  connect(row,mux(and_(arg(2),isACT),arg(6),row));
  // Set values, truncated to the seven-bit DownCounter input. The intermediate
  // truncations preserve the same modular sums/differences as the Scala UInts.
  Value readPRE=sub(add(timing("tRTP"),timing("tAL")),one);
  Value writePRE=sub(add(add(add(timing("tCWD"),timing("tAL")),timing("tWR")),timing("tCCD")),one);
  Value writeACT=add(add(writePRE,timing("tRP")),constant(7,2));
  Value readACT=sub(add(add(timing("tRTP"),timing("tAL")),timing("tRP")),one);
  Value preSet=b.create<OrPrimOp>(loc,isACT,and_(b.create<OrPrimOp>(loc,isCASR,isCASW),not_(arg(5))));
  Value preBits=mux(isACT,sub(timing("tRAS"),one),mux(isCASR,readPRE,writePRE));
  Value actSet=b.create<OrPrimOp>(loc,b.create<OrPrimOp>(loc,isACT,isPRE),casAuto);
  Value actBits=mux(isACT,sub(timing("tRC"),one),mux(isPRE,sub(timing("tRP"),one),mux(isCASR,readACT,writeACT)));
  Value casBits=sub(sub(timing("tRCD"),timing("tAL")),one);
  auto driveCounter = [&](Value counter, Value set, Value bits) {
    Value accept=and_(set,b.create<GEQPrimOp>(loc,bits,counter));
    Value decrement=mux(eq(counter,zero),zero,sub(counter,one));
    connect(counter,mux(arg(2),mux(accept,bits,decrement),counter));
  };
  driveCounter(pre,preSet,preBits); driveCounter(act,actSet,actBits); driveCounter(cas,isACT,casBits);
  Value assertionEnable=and_(arg(2),not_(arg(1)));
  for (auto [selected,legal,message] : {
      std::tuple<Value,Value,const char*>{isACT,canACT,"Bank Timing Violation: Controller issued activate command illegally"},
      {isPRE,canPRE,"Bank Timing Violation: Controller issued PRE command illegally"},
      {isCASR,canCAS,"Bank Timing Violation: Controller issued CASR command illegally"},
      {isCASW,canCAS,"Bank Timing Violation: Controller issued CASW command illegally"}})
    b.create<AssertOp>(loc,arg(0),legal,and_(assertionEnable,selected),message,ValueRange{},"");
  result=module;
  return success();
}
