// See LICENSE for license details.
#ifndef GOLDENGATE_TOKEN_IRREVOCABILITY_H
#define GOLDENGATE_TOKEN_IRREVOCABILITY_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/Builders.h"
namespace goldengate {
// midas.core.AssertTokenIrrevocable: previous valid and fire constrain both
// validity and payload until a handshake. Reset suppresses host assertions.
inline void assertTokenIrrevocable(circt::firrtl::CircuitOp circuit,
                                   mlir::OpBuilder &b,
                                   mlir::Location loc,
                                   mlir::Value clock,
                                   mlir::Value reset,
                                   mlir::Value valid,
                                   mlir::Value bits,
                                   mlir::Value ready,
                                   llvm::StringRef name) {
  using namespace circt::firrtl;
  using namespace mlir;
  auto enabled = circuit->getAttrOfType<BoolAttr>(
      "goldengate.tokenIrrevocabilityAssertions");
  if (!enabled || !enabled.getValue())
    return;
  auto bit = UIntType::get(b.getContext(), 1, false);
  Value zero = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 0));
  Value previousValid =
      b.create<RegResetOp>(
           loc, bit, clock, reset, zero, (name + "_validPrev").str())
          .getResult();
  Value previousFire =
      b.create<RegOp>(loc, bit, clock, (name + "_firePrev").str()).getResult();
  auto packedType =
      UIntType::get(b.getContext(),
                    *getBitWidth(cast<FIRRTLBaseType>(bits.getType())),
                    false);
  Value packed = bits.getType() == packedType
                     ? bits
                     : b.create<BitCastOp>(loc, packedType, bits).getResult();
  Value previousBits =
      b.create<RegOp>(loc, packedType, clock, (name + "_bitsPrev").str())
          .getResult();
  b.create<StrictConnectOp>(loc, previousValid, valid);
  b.create<StrictConnectOp>(
      loc, previousFire, b.create<AndPrimOp>(loc, valid, ready));
  b.create<StrictConnectOp>(loc, previousBits, packed);
  Value free = b.create<OrPrimOp>(
      loc, b.create<NotPrimOp>(loc, previousValid), previousFire);
  Value enable = b.create<NotPrimOp>(loc, reset);
  b.create<AssertOp>(
      loc,
      clock,
      b.create<OrPrimOp>(loc, free, valid),
      enable,
      "valid de-asserted without handshake, violating irrevocability",
      ValueRange{},
      (name + "_validStable").str());
  b.create<AssertOp>(
      loc,
      clock,
      b.create<OrPrimOp>(
          loc, free, b.create<EQPrimOp>(loc, previousBits, packed)),
      enable,
      "bits changed without handshake, violating irrevocability",
      ValueRange{},
      (name + "_bitsStable").str());
}
} // namespace goldengate
#endif
