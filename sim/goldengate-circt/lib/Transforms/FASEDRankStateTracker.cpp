// See LICENSE for license details.
// Oracle: models/dram/DramCommon.scala RankStateTracker (470-564), and
// models/dram/Util.scala DownCounter. Preserve source-order last-connect
// priority for refresh expiry and refresh-state exit. The four-entry ACT queue
// has Chisel Queue's default flow=false, pipe=false behavior.
#include "goldengate/FASEDRankStateTracker.h"
#include "goldengate/FASEDBankStateTracker.h"
#include "mlir/IR/Builders.h"
#include <array>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDRankStateTracker(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDRankStateTracker";
  // Check both symbols before invoking the bank materializer, so even a rank
  // name collision cannot leave a new bank helper behind on failure.
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name ||
        module.getModuleName() == "GGFASEDBankStateTracker") {
      error = "FASED rank state tracker requires free rank and bank helper symbols";
      return failure();
    }
  FModuleOp bank;
  if (failed(materializeFASEDBankStateTracker(circuit, bank, error)))
    return failure();

  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = u(1);
  // Reuse the exact timing bundle emitted by the bank helper.
  auto timings = bank.getPortType(7);
  SmallVector<BundleType::BundleElement> bankFields;
  for (auto n : {"canCASW", "canCASR", "canPRE", "canACT", "state"})
    bankFields.push_back({b.getStringAttr(n), false, bit});
  bankFields.push_back({b.getStringAttr("openRow"), false, u(26)});
  SmallVector<PortInfo> ports;
  auto port = [&](StringRef n, Type t, Direction d) {
    ports.push_back({b.getStringAttr(n), t, d});
  };
  port("clock", ClockType::get(ctx), Direction::In);
  for (auto n : {"reset", "targetFire"}) port(n, bit, Direction::In);
  port("selectedCmd", u(3), Direction::In);
  port("cmdUsesThisRank", bit, Direction::In);
  port("cmdBankOH", u(8), Direction::In);
  port("autoPRE", bit, Direction::In);
  port("cmdRow", u(26), Direction::In);
  port("timings", timings, Direction::In);
  port("tCycle", u(7), Direction::In);
  for (auto n : {"canCASW", "canCASR", "canPRE", "canACT", "canREF", "wantREF", "state"})
    port(n, bit, Direction::Out);
  port("banks", FVectorType::get(BundleType::get(ctx, bankFields), 8), Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module = b.create<FModuleOp>(loc, b.getStringAttr(name),
      ConventionAttr::get(ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned i) { return module.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n)->Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto and_ = [&](Value a, Value z)->Value { return b.create<AndPrimOp>(loc, a, z); };
  auto or_ = [&](Value a, Value z)->Value { return b.create<OrPrimOp>(loc, a, z); };
  auto not_ = [&](Value a)->Value { return b.create<NotPrimOp>(loc, a); };
  auto mux = [&](Value c, Value a, Value z)->Value { return b.create<MuxPrimOp>(loc, c, a, z); };
  auto eq = [&](Value a, Value z)->Value { return b.create<EQPrimOp>(loc, a, z); };
  auto trim = [&](Value v, unsigned w)->Value { return b.create<BitsPrimOp>(loc, v, w-1, 0); };
  auto add = [&](Value a, Value z, unsigned w)->Value { return trim(b.create<AddPrimOp>(loc, a, z), w); };
  auto sub = [&](Value a, Value z, unsigned w)->Value { return trim(b.create<SubPrimOp>(loc, a, z), w); };
  auto field = [&](Value v, StringRef n)->Value { return b.create<SubfieldOp>(loc, v, n); };
  auto timing = [&](StringRef n)->Value { return field(arg(8), n); };
  Value enabledReset = and_(arg(1), arg(2));
  auto reg = [&](unsigned w, StringRef n)->Value {
    return b.create<RegResetOp>(loc, u(w), arg(0), enabledReset, constant(w, 0), n).getResult();
  };
  Value pre = reg(7, "nextLegalPRE"), act = reg(10, "nextLegalACT");
  Value casr = reg(7, "nextLegalCASR"), casw = reg(7, "nextLegalCASW");
  Value refi = reg(14, "tREFI"), state = reg(1, "state"), wantREF = reg(1, "wantREF");
  Value count = reg(3, "tFAWCount"), enqPtr = reg(2, "tFAWEnqPtr"), deqPtr = reg(2, "tFAWDeqPtr");
  std::array<Value, 4> expiry;
  for (unsigned i = 0; i < 4; ++i)
    expiry[i] = b.create<RegOp>(loc, u(7), arg(0), "tFAWExpiry" + std::to_string(i)).getResult();
  Value zero7 = constant(7, 0), one7 = constant(7, 1);
  auto command = [&](unsigned n)->Value { return eq(arg(3), constant(3, n)); };
  Value isACT = and_(arg(4), command(1)), isPRE = and_(arg(4), command(2));
  Value isCASW = command(3), isCASR = command(4), isREF = and_(arg(4), command(5));
  Value queueReady = not_(eq(count, constant(3, 4)));
  Value enqueue = and_(isACT, queueReady);
  Value front = expiry[3];
  for (int i = 2; i >= 0; --i)
    front = mux(eq(deqPtr, constant(2, i)), expiry[i], front);
  Value dequeue = and_(not_(eq(count, constant(3, 0))), eq(arg(9), front));
  Value deadline = add(arg(9), timing("tFAW"), 7);
  for (unsigned i = 0; i < 4; ++i)
    connect(expiry[i], mux(and_(arg(2), and_(enqueue, eq(enqPtr, constant(2, i)))), deadline, expiry[i]));
  auto drive = [&](Value r, Value next) { connect(r, mux(arg(2), next, r)); };
  drive(enqPtr, mux(enqueue, add(enqPtr, constant(2, 1), 2), enqPtr));
  drive(deqPtr, mux(dequeue, add(deqPtr, constant(2, 1), 2), deqPtr));
  Value countNext = mux(eq(enqueue, dequeue), count,
      mux(enqueue, add(count, constant(3, 1), 3), sub(count, constant(3, 1), 3)));
  drive(count, countNext);

  Value canCASW = eq(casw, zero7), canCASR = eq(casr, zero7);
  Value canPRE = eq(pre, zero7);
  Value canACT = and_(eq(act, constant(10, 0)), queueReady);
  Value canREF = constant(1, 1);
  for (unsigned i = 0; i < 8; ++i) {
    auto instance = b.create<InstanceOp>(loc, bank, "banks_" + std::to_string(i));
    for (unsigned p = 0; p < 3; ++p) connect(instance.getResult(p), arg(p));
    connect(instance.getResult(3), arg(3));
    connect(instance.getResult(4), and_(arg(4), b.create<BitsPrimOp>(loc, arg(5), i, i)));
    connect(instance.getResult(5), arg(6));
    connect(instance.getResult(6), arg(7));
    connect(instance.getResult(7), arg(8));
    Value out = b.create<SubindexOp>(loc, arg(17), i);
    for (unsigned p = 0; p < bankFields.size(); ++p)
      connect(field(out, bankFields[p].name.getValue()), instance.getResult(8 + p));
    canREF = and_(canREF, instance.getResult(11));
  }
  connect(arg(10), canCASW); connect(arg(11), canCASR); connect(arg(12), canPRE);
  connect(arg(13), canACT); connect(arg(14), canREF); connect(arg(15), wantREF); connect(arg(16), state);

  // UInt subtraction keeps its operand width. tRRD-1 therefore wraps to seven
  // bits before zero-extension into the ten-bit ACT counter; tRFC-1 wraps ten.
  Value actBits7 = sub(timing("tRRD"), one7, 7);
  Value actBits = mux(isACT, b.create<PadPrimOp>(loc, actBits7, 10), sub(timing("tRFC"), constant(10, 1), 10));
  Value casrBits = mux(isCASR,
      sub(add(timing("tCCD"), mux(arg(4), zero7, timing("tRTRS")), 7), one7, 7),
      mux(arg(4), sub(add(add(timing("tCWD"), timing("tCCD"), 7), timing("tWTR"), 7), one7, 7),
          sub(sub(add(add(timing("tCWD"), timing("tCCD"), 7), timing("tRTRS"), 7), timing("tCAS"), 7), one7, 7)));
  Value readToWrite = sub(add(sub(add(timing("tCAS"), timing("tCCD"), 7), timing("tCWD"), 7), timing("tRTRS"), 7), one7, 7);
  Value caswBits = mux(isCASR, readToWrite, sub(timing("tCCD"), one7, 7));
  auto driveCounter = [&](Value counter, unsigned width, Value set, Value bits) {
    Value zero = constant(width, 0), one = constant(width, 1);
    Value accept = and_(set, b.create<GEQPrimOp>(loc, bits, counter));
    drive(counter, mux(accept, bits, mux(eq(counter, zero), zero, sub(counter, one, width))));
  };
  driveCounter(pre, 7, constant(1, 0), zero7);
  driveCounter(act, 10, or_(isACT, isREF), actBits);
  driveCounter(casr, 7, or_(isCASR, isCASW), casrBits);
  driveCounter(casw, 7, or_(isCASR, isCASW), caswBits);
  Value intervalExpired = and_(eq(refi, timing("tREFI")), not_(eq(timing("tREFI"), constant(14, 0))));
  drive(refi, mux(intervalExpired, constant(14, 0), add(refi, constant(14, 1), 14)));
  drive(wantREF, mux(intervalExpired, constant(1, 1), mux(isREF, constant(1, 0), wantREF)));
  Value refreshFinished = and_(state, eq(act, constant(10, 1)));
  drive(state, mux(refreshFinished, constant(1, 0), mux(isREF, constant(1, 1), state)));

  Value assertionEnable = and_(arg(2), not_(arg(1)));
  for (auto [selected, legal, message] : {
      std::tuple<Value, Value, const char *>{isACT, canACT, "Rank Timing Violation: Controller issued ACT command illegally"},
      {isCASR, or_(not_(arg(4)), canCASR), "Rank Timing Violation: Controller issued CASR command illegally"},
      {isCASW, or_(not_(arg(4)), canCASW), "Rank Timing Violation: Controller issued CASW command illegally"},
      {isPRE, canPRE, "Rank Timing Violation: Controller issued PRE command illegally"},
      {isREF, canREF, "Rank Timing Violation: Controller issued REF command illegally"}})
    b.create<AssertOp>(loc, arg(0), legal, and_(assertionEnable, selected), message, ValueRange{}, "");
  result = module;
  return success();
}
