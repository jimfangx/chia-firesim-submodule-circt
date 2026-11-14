// See LICENSE for license details.
// Oracle: midas/models/dram/Util.scala AXI4Releaser.
#include "goldengate/FASEDAXI4Releaser.h"
#include "mlir/IR/Builders.h"
#include "goldengate/FASEDGeometry.h"

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDAXI4Releaser(CircuitOp circuit,
                                                       FModuleOp &result,
                                                       std::string &error) {
  auto geometry = getFASEDGeometry(circuit);
  constexpr llvm::StringLiteral name = "GGFASEDAXI4Releaser";
  for (auto module : circuit.getOps<FModuleLike>()) {
    if (module.getModuleName() == name) {
      error = "FASED AXI4 releaser helper already exists";
      return failure();
    }
  }
  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  SmallVector<PortInfo> ports;
  auto port = [&](StringRef n, unsigned width, Direction direction) {
    ports.push_back({b.getStringAttr(n), u(width), direction});
  };
  ports.push_back(
      {b.getStringAttr("clock"), ClockType::get(ctx), Direction::In});
  for (auto n : {"reset", "targetFire", "nextReadValid"})
    port(n, 1, Direction::In);
  port("nextReadReady", 1, Direction::Out);
  port("nextReadId", 4, Direction::In);
  port("nextReadLen", 8, Direction::In);
  port("nextWriteValid", 1, Direction::In);
  port("nextWriteReady", 1, Direction::Out);
  port("nextWriteId", 4, Direction::In);
  port("rReady", 1, Direction::In);
  port("rValid", 1, Direction::Out);
  port("rId", 4, Direction::Out);
  port("rLast", 1, Direction::Out);
  port("rData", geometry.dataBits, Direction::Out);
  port("rResp", 2, Direction::Out);
  port("rUser", 1, Direction::Out);
  port("bReady", 1, Direction::In);
  port("bValid", 1, Direction::Out);
  port("bId", 4, Direction::Out);
  port("bResp", 2, Direction::Out);
  port("bUser", 1, Direction::Out);
  port("egressReadValid", 1, Direction::Out);
  port("egressReadId", 4, Direction::Out);
  port("egressWriteValid", 1, Direction::Out);
  port("egressWriteId", 4, Direction::Out);
  port("egressRId", 4, Direction::In);
  port("egressRLast", 1, Direction::In);
  port("egressRData", geometry.dataBits, Direction::In);
  port("egressRResp", 2, Direction::In);
  port("egressRUser", 1, Direction::In);
  port("egressBId", 4, Direction::In);
  port("egressBResp", 2, Direction::In);
  port("egressBUser", 1, Direction::In);
  port("egressRReady", 1, Direction::Out);
  port("egressBReady", 1, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module =
      b.create<FModuleOp>(loc,
                          b.getStringAttr(name),
                          ConventionAttr::get(ctx, Convention::Internal),
                          ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned index) {
    return module.getBodyBlock()->getArgument(index);
  };
  auto connect = [&](Value dest, Value src) {
    b.create<StrictConnectOp>(loc, dest, src);
  };
  auto both = [&](Value a, Value z) -> Value {
    return b.create<AndPrimOp>(loc, a, z);
  };
  auto either = [&](Value a, Value z) -> Value {
    return b.create<OrPrimOp>(loc, a, z);
  };
  auto neg = [&](Value a) -> Value { return b.create<NotPrimOp>(loc, a); };
  auto zero = b.create<ConstantOp>(loc, u(1), APInt(1, 0)).getResult();
  for (bool read : {true, false}) {
    Value full =
        b.create<RegResetOp>(loc,
                             u(1),
                             arg(0),
                             both(arg(1), arg(2)),
                             zero,
                             read ? "currentRead_full" : "currentWrite_full")
            .getResult();
    Value pop = both(full, arg(read ? 10 : 17));
    if (read)
      pop = both(pop, arg(27));
    Value ready = either(neg(full), pop);
    Value push = both(ready, arg(read ? 3 : 7));
    Value change = b.create<XorPrimOp>(loc, push, pop);
    connect(full, b.create<MuxPrimOp>(loc, both(arg(2), change), push, full));
    connect(arg(read ? 4 : 8), ready);
    connect(arg(read ? 11 : 18), full);
    connect(arg(read ? 22 : 24), push);
    connect(arg(read ? 23 : 25), arg(read ? 5 : 9));
    connect(arg(read ? 34 : 35), arg(read ? 10 : 17));
  }
  // No queue metadata registers: actual AXI responses come from the functional
  // egress. In particular nextReadLen is intentionally not used by this helper.
  for (auto [dest, src] : {std::pair{12u, 26u},
                           {13u, 27u},
                           {14u, 28u},
                           {15u, 29u},
                           {16u, 30u},
                           {19u, 31u},
                           {20u, 32u},
                           {21u, 33u}})
    connect(arg(dest), arg(src));
  result = module;
  return success();
}
