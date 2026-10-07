// See LICENSE for license details.
// ClockBridge.scala RationalClockTokenGenerator: normalized rational periods,
// minimum countdown, simultaneous reload/subtract, ready hold and sync reset.
// This component creates FIRRTL operations, without consuming annotations or
// changing channel identity. The caller supplies validated clockInfo order.
#include "goldengate/RationalClockTokenGenerator.h"
#include "circt/Support/Namespace.h"
#include <cassert>
#include <limits>
#include <numeric>

using namespace mlir;
using namespace circt::firrtl;

std::optional<goldengate::RationalClockSchedule>
goldengate::analyzeRationalClockSchedule(ArrayRef<RationalClockInfo> clocks,
                                        std::string &error) {
  auto reject = [&](llvm::StringRef reason) -> std::optional<RationalClockSchedule> {
    error = reason.str(); return std::nullopt;
  };
  if (clocks.empty()) return reject("rational clock schedule is empty");
  for (auto &clock : clocks)
    if (!clock.multiplier || !clock.divisor ||
        clock.multiplier > uint64_t(std::numeric_limits<int32_t>::max()) ||
        clock.divisor > uint64_t(std::numeric_limits<int32_t>::max()))
      return reject("rational clock ratios must be positive Scala Int values");

  // Relative period i/0 is (divisor_i * multiplier_0) /
  // (multiplier_i * divisor_0). Reduced denominators must divide period_0.
  // Their LCM gives the smallest integer period_0 and hence exactly the
  // product/GCD-normalized Scala periods, without an unbounded product.
  // Positive Int32 cross-products fit UInt64. Bound the LCM and each period
  // before multiplication, preserving Scala's arbitrary-precision behavior.
  SmallVector<std::pair<uint64_t, uint64_t>> relative;
  uint64_t base = 1;
  constexpr uint64_t maximum = 65535;
  for (auto &clock : clocks) {
    uint64_t num = clock.divisor * clocks.front().multiplier;
    uint64_t den = clock.multiplier * clocks.front().divisor;
    uint64_t gcd = std::gcd(num, den);
    num /= gcd; den /= gcd;
    uint64_t factor = base / std::gcd(base, den);
    if (den > maximum || factor > maximum / den)
      return reject("rational clock countdown exceeds Scala's 16-bit limit");
    base = factor * den;
    relative.emplace_back(num, den);
  }
  RationalClockSchedule schedule;
  unsigned largest = 0;
  for (auto [num, den] : relative) {
    uint64_t scale = base / den;
    if (num > maximum / scale)
      return reject("rational clock countdown exceeds Scala's 16-bit limit");
    unsigned period = scale * num;
    schedule.periods.push_back(period);
    largest = std::max(largest, period);
  }
  schedule.counterWidth = 0;
  for (unsigned n = largest; n; n >>= 1) ++schedule.counterWidth;
  return schedule;
}

SmallVector<Value> goldengate::buildRationalClockTokens(
    OpBuilder &b, Location loc, Value clock, Value reset, Value ready,
    const RationalClockSchedule &schedule) {
  assert(!schedule.periods.empty() && schedule.counterWidth > 0 &&
         schedule.counterWidth <= 16);
  auto bit = UIntType::get(b.getContext(), 1, false);
  if (schedule.periods.size() == 1) {
    assert(schedule.periods.front() == 1);
    return {b.create<ConstantOp>(loc, bit, APInt(1, 1)).getResult()};
  }
  circt::Namespace names;
  auto module = cast<FModuleOp>(b.getInsertionBlock()->getParentOp());
  for (auto port : module.getPorts()) names.newName(port.name.getValue());
  module.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
  });
  unsigned width = schedule.counterWidth;
  auto countType = UIntType::get(b.getContext(), width, false);
  Value zero = b.create<ConstantOp>(loc, countType, APInt(width, 0));
  SmallVector<Value> counts, tokens;
  for (unsigned i = 0; i < schedule.periods.size(); ++i)
    counts.push_back(b.create<RegResetOp>(loc, countType, clock, reset, zero,
        names.newName("timeToNextEdge_" + std::to_string(i))).getResult());
  // Only the final minimum of DensePrefixSum is observed by Scala. A balanced
  // reduction computes the same value while avoiding a linear mux chain.
  SmallVector<Value> level = counts;
  while (level.size() > 1) {
    SmallVector<Value> next;
    for (unsigned i = 0; i < level.size(); i += 2) {
      if (i + 1 == level.size()) { next.push_back(level[i]); continue; }
      Value less = b.create<LTPrimOp>(loc, level[i], level[i + 1]);
      next.push_back(b.create<MuxPrimOp>(loc, less, level[i], level[i + 1]));
    }
    level = std::move(next);
  }
  Value minimum = level.front();
  for (auto [i, count] : llvm::enumerate(counts)) {
    Value firing = b.create<EQPrimOp>(loc, count, minimum);
    tokens.push_back(firing);
    Value period = b.create<ConstantOp>(loc, countType, APInt(width, schedule.periods[i]));
    Value remaining = b.create<SubPrimOp>(loc, count, minimum);
    remaining = b.create<BitsPrimOp>(loc, remaining, width - 1, 0);
    Value advance = b.create<MuxPrimOp>(loc, firing, period, remaining);
    b.create<StrictConnectOp>(loc, count, b.create<MuxPrimOp>(loc, ready, advance, count));
  }
  return tokens;
}

SmallVector<Value> goldengate::buildRationalClockChannel(
    OpBuilder &b, Location loc, Value clock, Value reset, Value channel,
    const RationalClockSchedule &schedule) {
  auto type = cast<BundleType>(channel.getType());
  auto bit = UIntType::get(b.getContext(), 1, false);
  auto vector = cast<FVectorType>(type.getElement("bits")->type);
  assert(type.getElements().size() == 3 &&
         type.getElement("ready")->type == bit &&
         type.getElement("ready")->isFlip &&
         type.getElement("valid")->type == bit &&
         !type.getElement("valid")->isFlip &&
         !type.getElement("bits")->isFlip &&
         vector.getElementType() == bit &&
         vector.getNumElements() == schedule.periods.size());
  Value ready = b.create<SubfieldOp>(loc, channel, "ready");
  Value valid = b.create<SubfieldOp>(loc, channel, "valid");
  Value bits = b.create<SubfieldOp>(loc, channel, "bits");
  b.create<StrictConnectOp>(loc, valid,
      b.create<ConstantOp>(loc, bit, APInt(1, 1)));
  auto tokens = buildRationalClockTokens(b, loc, clock, reset, ready, schedule);
  for (auto [i, token] : llvm::enumerate(tokens))
    b.create<StrictConnectOp>(loc, b.create<SubindexOp>(loc, bits, i), token);
  return tokens;
}
