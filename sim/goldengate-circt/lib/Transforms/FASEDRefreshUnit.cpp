// See LICENSE for license details.
// Oracle: midas/models/dram/DramCommon.scala RefreshUnit (634-654).
#include "goldengate/FASEDRefreshUnit.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDRefreshUnit(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDRefreshUnit";
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name) {
      error = "FASED refresh unit helper already exists";
      return failure();
    }
  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  SmallVector<PortInfo> ports;
  auto port = [&](StringRef n, unsigned w, Direction d) {
    ports.push_back({b.getStringAttr(n), u(w), d});
  };
  for (auto n : {"wantREF", "canREF", "canPRE"}) port(n, 4, Direction::In);
  for (auto n : {"bankCanPRE_0", "bankCanPRE_1", "bankCanPRE_2", "bankCanPRE_3"})
    port(n, 8, Direction::In);
  port("ranksInUse", 4, Direction::In);
  port("suggestREF", 1, Direction::Out);
  port("refRankAddr", 2, Direction::Out);
  port("suggestPRE", 1, Direction::Out);
  port("preRankAddr", 2, Direction::Out);
  port("preBankAddr", 3, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module = b.create<FModuleOp>(loc, b.getStringAttr(name),
      ConventionAttr::get(ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned i) { return module.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, unsigned n) -> Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  auto bit = [&](Value v, unsigned i) -> Value { return b.create<BitsPrimOp>(loc, v, i, i); };
  auto and_ = [&](Value a, Value z) -> Value { return b.create<AndPrimOp>(loc, a, z); };
  auto mux = [&](Value c, Value a, Value z) -> Value { return b.create<MuxPrimOp>(loc, c, a, z); };
  auto any = [&](Value v) -> Value { return b.create<NEQPrimOp>(loc, v, constant(*cast<UIntType>(v.getType()).getWidth(), 0)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  // Chisel's encoder is PriorityMux(indices): its last value is unconditional.
  auto encoder = [&](Value v, unsigned count, unsigned width) {
    Value selected = constant(width, count - 1);
    for (int i = int(count) - 2; i >= 0; --i)
      selected = mux(bit(v, i), constant(width, i), selected);
    return selected;
  };
  Value refresh = and_(arg(0), and_(arg(1), arg(7)));
  connect(arg(8), any(refresh));
  connect(arg(9), encoder(refresh, 4, 2));
  std::array<Value, 4> preEligible, preBank;
  for (unsigned i = 0; i < 4; ++i) {
    preEligible[i] = and_(and_(bit(arg(0), i), bit(arg(7), i)),
                          and_(bit(arg(2), i), any(arg(3 + i))));
    preBank[i] = encoder(arg(3 + i), 8, 3);
  }
  Value preAny = preEligible[3];
  for (unsigned i = 0; i < 3; ++i)
    preAny = b.create<OrPrimOp>(loc, preEligible[i], preAny);
  Value preRank = constant(2, 3), selectedBank = preBank[3];
  for (int i = 2; i >= 0; --i) {
    preRank = mux(preEligible[i], constant(2, i), preRank);
    selectedBank = mux(preEligible[i], preBank[i], selectedBank);
  }
  connect(arg(10), preAny);
  connect(arg(11), preRank);
  connect(arg(12), selectedBank);
  result = module;
  return success();
}
