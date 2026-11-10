// See LICENSE for license details.
// Oracle: TimingModel.scala parent counters/histograms, widgets/Lib.scala
// SatUpDownCounter and DramCommon.scala RankPowerMonitor. Histogram sampling
// uses the old pending count; simultaneous increment/decrement holds occupancy.
#include "goldengate/FASEDTimingMonitor.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDTimingMonitor(CircuitOp circuit,
                                                        unsigned maxFlight,
                                                        FModuleOp &result,
                                                        std::string &error) {
  if (maxFlight != 10 && maxFlight != 20) {
    error = "FASED timing monitor requires selected maxFlight 10 or 20";
    return failure();
  }
  std::string name = "GGFASEDTimingMonitor" + std::to_string(maxFlight);
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name) {
      error = "FASED timing monitor requires a free " + name + " symbol";
      return failure();
    }
  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  const unsigned pendingWidth = maxFlight == 10 ? 4 : 5;
  SmallVector<BundleType::BundleElement> powerFields;
  for (auto n : {"allPreCycles", "numCASR", "numCASW", "numACT"})
    powerFields.push_back({b.getStringAttr(n), false, u(32)});
  SmallVector<PortInfo> ports;
  auto port = [&](StringRef n, Type t, Direction d) {
    ports.push_back({b.getStringAttr(n), t, d});
  };
  port("clock", ClockType::get(ctx), Direction::In);
  for (auto n : {"reset",
                 "targetFire",
                 "arFire",
                 "awFire",
                 "wFire",
                 "wLast",
                 "rFire",
                 "rLast",
                 "bFire"})
    port(n, u(1), Direction::In);
  port("selectedCmd", u(3), Direction::In);
  port("cmdRank", u(2), Direction::In);
  port("rankStates", u(4), Direction::In);
  port("bankCanACT", FVectorType::get(u(8), 4), Direction::In);
  for (auto n : {"pendingReads", "pendingAWReq", "pendingWReq"})
    port(n, u(pendingWidth), Direction::Out);
  for (auto n :
       {"totalReads", "totalWrites", "totalReadBeats", "totalWriteBeats"})
    port(n, u(32), Direction::Out);
  for (auto n : {"readOutstandingHistogram", "writeOutstandingHistogram"})
    port(n, FVectorType::get(u(32), 5), Direction::Out);
  port("rankPower",
       FVectorType::get(BundleType::get(ctx, powerFields), 4),
       Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module =
      b.create<FModuleOp>(loc,
                          b.getStringAttr(name),
                          ConventionAttr::get(ctx, Convention::Internal),
                          ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned i) { return module.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  auto connect = [&](Value dst, Value src) {
    b.create<StrictConnectOp>(loc, dst, src);
  };
  auto and_ = [&](Value a, Value z) -> Value {
    return b.create<AndPrimOp>(loc, a, z);
  };
  auto or_ = [&](Value a, Value z) -> Value {
    return b.create<OrPrimOp>(loc, a, z);
  };
  auto not_ = [&](Value a) -> Value { return b.create<NotPrimOp>(loc, a); };
  auto eq = [&](Value a, Value z) -> Value {
    return b.create<EQPrimOp>(loc, a, z);
  };
  auto mux = [&](Value c, Value a, Value z) -> Value {
    return b.create<MuxPrimOp>(loc, c, a, z);
  };
  auto trim = [&](Value v, unsigned w) -> Value {
    return b.create<BitsPrimOp>(loc, v, w - 1, 0);
  };
  Value enabledReset = and_(arg(1), arg(2));
  auto reg = [&](unsigned w, StringRef n) -> Value {
    return b
        .create<RegResetOp>(loc, u(w), arg(0), enabledReset, constant(w, 0), n)
        .getResult();
  };
  auto drive = [&](Value r, Value next) { connect(r, mux(arg(2), next, r)); };
  auto incr = [&](Value r, unsigned w) -> Value {
    return trim(b.create<AddPrimOp>(loc, r, constant(w, 1)), w);
  };
  auto pending = [&](StringRef n, Value inc, Value dec, unsigned out) -> Value {
    Value r = reg(pendingWidth, n);
    Value canInc =
        and_(and_(inc, not_(dec)),
             b.create<LTPrimOp>(loc, r, constant(pendingWidth, maxFlight)));
    Value canDec =
        and_(and_(not_(inc), dec), not_(eq(r, constant(pendingWidth, 0))));
    Value minus = trim(b.create<SubPrimOp>(loc, r, constant(pendingWidth, 1)),
                       pendingWidth);
    drive(r, mux(canInc, incr(r, pendingWidth), mux(canDec, minus, r)));
    connect(arg(out), r);
    return r;
  };
  Value reads = pending("pendingReads", arg(3), and_(arg(7), arg(8)), 14);
  Value aw = pending("pendingAWReq", arg(4), arg(9), 15);
  pending("pendingWReq", and_(arg(5), arg(6)), arg(9), 16);
  for (auto [out, enable] : {std::pair<unsigned, Value>{17, arg(3)},
                             {18, arg(4)},
                             {19, arg(7)},
                             {20, arg(5)}}) {
    Value r = reg(32, ports[out].name.getValue());
    drive(r, mux(enable, incr(r, 32), r));
    connect(arg(out), r);
  }
  constexpr std::array<unsigned, 4> maximums{0, 2, 4, 8};
  for (auto [out, count] : {std::pair<unsigned, Value>{21, reads}, {22, aw}}) {
    Value incremented = constant(1, 0);
    for (unsigned i = 0; i < 5; ++i) {
      Value r =
          reg(32, ports[out].name.getValue().str() + "_" + std::to_string(i));
      Value next = r;
      if (i < maximums.size()) {
        Value matches = b.create<LEQPrimOp>(
            loc, count, constant(pendingWidth, maximums[i]));
        next = mux(and_(not_(incremented), matches), incr(r, 32), r);
        incremented = or_(incremented, matches);
      }
      drive(r, next);
      connect(b.create<SubindexOp>(loc, arg(out), i), r);
    }
  }
  for (unsigned rank = 0; rank < 4; ++rank) {
    Value stats = b.create<SubindexOp>(loc, arg(23), rank);
    Value usesRank = eq(arg(11), constant(2, rank));
    Value refreshing = b.create<BitsPrimOp>(loc, arg(12), rank, rank);
    Value bankMask = b.create<SubindexOp>(loc, arg(13), rank);
    Value allPre = and_(not_(refreshing), eq(bankMask, constant(8, 255)));
    for (auto [i, command] :
         {std::pair<unsigned, unsigned>{0, 0}, {1, 4}, {2, 3}, {3, 1}}) {
      StringRef field = powerFields[i].name.getValue();
      Value r = reg(32, "rank" + std::to_string(rank) + "_" + field.str());
      Value enable =
          i == 0 ? allPre : and_(usesRank, eq(arg(10), constant(3, command)));
      drive(r, mux(enable, incr(r, 32), r));
      connect(b.create<SubfieldOp>(loc, stats, field), r);
    }
  }
  result = module;
  return success();
}
