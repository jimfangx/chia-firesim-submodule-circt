// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/ErrorHandling.h"
#include <functional>
namespace goldengate::test {
using namespace mlir;
using namespace circt::firrtl;
// Drive only sink leaves of a fresh fixture top. This is test-only syntax/
// initialization scaffolding: it supplies no oracle feedback or behavior.
// Call before production attachments and only on a body without connections.
inline void initializeFASEDFRFCFSWriteEgressFixture(FModuleOp top) {
  OpBuilder b(top.getContext());
  b.setInsertionPointToEnd(top.getBodyBlock());
  auto loc = top.getLoc();
  std::function<void(Value, Type, bool)> initialize = [&](Value v,
                                                          Type type,
                                                          bool sink) {
    if (auto bundle = dyn_cast<BundleType>(type)) {
      for (auto e : bundle.getElements())
        initialize(
            b.create<SubfieldOp>(loc, v, e.name.getValue()), e.type, sink != e.isFlip);
    } else if (auto vector = dyn_cast<FVectorType>(type)) {
      for (unsigned i = 0; i < vector.getNumElements(); ++i)
        initialize(
            b.create<SubindexOp>(loc, v, i), vector.getElementType(), sink);
    } else if (sink) {
      auto u = dyn_cast<UIntType>(type);
      if (!u || !u.getWidth())
        llvm::report_fatal_error("unsupported fixture sink");
      b.create<StrictConnectOp>(
          loc, v, b.create<ConstantOp>(loc, u, llvm::APInt(*u.getWidth(), 0)));
    }
  };
  for (auto [i, p] : llvm::enumerate(top.getPorts()))
    initialize(top.getArgument(i), p.type, p.direction == Direction::Out);
}
inline OwningOpRef<ModuleOp> makeFASEDFRFCFSWriteEgressFixture(
    MLIRContext &ctx, unsigned flight, bool selected, unsigned mode = 0) {
  auto oldRoot = [&]() -> std::string {
    return selected ? "GGSelectedPrintFASEDWriteEgressWrapper"
                    : "GGFASEDWriteEgressWrapper";
  };
  auto newRoot = [&]() -> std::string {
    return selected ? "GGSelectedPrintFASEDFRFCFSBridgeWrapper"
                    : "GGFASEDFRFCFSBridgeWrapper";
  };
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"" + oldRoot() + "\" { firrtl.module @" +
          oldRoot() + "() {} } }",
      &ctx);
  if (!root)
    llvm::report_fatal_error("fixture parse");
  auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  auto *cx = &ctx;
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto loc = c.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(cx, w, false); };
  auto bit = uint(1);
  auto payload =
      [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
        SmallVector<BundleType::BundleElement> es;
        for (auto [n, w] : fields)
          es.push_back({b.getStringAttr(n), false, uint(w)});
        return BundleType::get(cx, es);
      };
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> es) {
    return BundleType::get(cx, es);
  };
  auto decoupled = [&](BundleType bits, bool flipped = false) {
    return bundle({{b.getStringAttr("ready"), !flipped, bit},
                   {b.getStringAttr("valid"), flipped, bit},
                   {b.getStringAttr("bits"), flipped, bits}});
  };
  auto valid = bundle({{b.getStringAttr("valid"), false, bit},
                       {b.getStringAttr("bits"), false, uint(4)}});
  auto address = payload({{"user", 1},
                          {"id", 4},
                          {"region", 4},
                          {"qos", 4},
                          {"prot", 3},
                          {"cache", 4},
                          {"lock", 1},
                          {"burst", 2},
                          {"size", 3},
                          {"len", 8},
                          {"addr", 35}});
  auto data =
      payload({{"user", 1}, {"strb", 8}, {"id", 4}, {"last", 1}, {"data", 64}});
  auto write = payload({{"user", 1}, {"id", 4}, {"resp", 2}});
  auto read =
      payload({{"user", 1}, {"id", 4}, {"last", 1}, {"data", 64}, {"resp", 2}});
  auto response = [&](BundleType bits) {
    return bundle({{b.getStringAttr("tReady"), true, bit},
                   {b.getStringAttr("hValid"), false, bit},
                   {b.getStringAttr("tBits"), false, bits}});
  };
  auto requests = bundle({{b.getStringAttr("aw"), false, decoupled(address)},
                          {b.getStringAttr("w"), false, decoupled(data)},
                          {b.getStringAttr("ar"), false, decoupled(address)}});
  auto timing = bundle({{b.getStringAttr("aw"), false, decoupled(address)},
                        {b.getStringAttr("w"), false, decoupled(data)},
                        {b.getStringAttr("b"), false, decoupled(write, true)},
                        {b.getStringAttr("ar"), false, decoupled(address)},
                        {b.getStringAttr("r"), false, decoupled(read, true)}});
  const llvm::StringRef names[]{"hostClock",
                                "fased_model_reset",
                                "fased_tfire",
                                "fased_timing",
                                "fased_read_egress_req",
                                "fased_read_egress_resp",
                                "fased_write_egress_req",
                                "fased_write_egress_resp"};
  const Type types[]{ClockType::get(cx),
                     bit,
                     bit,
                     timing,
                     valid,
                     response(read),
                     valid,
                     response(write)};
  const Direction dirs[]{Direction::In,
                         Direction::Out,
                         Direction::Out,
                         Direction::Out,
                         Direction::In,
                         Direction::Out,
                         Direction::In,
                         Direction::Out};

  SmallVector<PortInfo> ps;
  for (unsigned i = 0; i < 8; ++i)
    ps.push_back({b.getStringAttr(mode == i + 1 ? "missing" : names[i]),
                  types[i],
                  dirs[i]});
  ps.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  ps.push_back({b.getStringAttr("hostReset"), bit, Direction::In});
  ps.push_back({b.getStringAttr("fased_ingress_relaxed"), bit, Direction::In});
  auto readToken = bundle({{b.getStringAttr("ready"), true, bit},
                           {b.getStringAttr("valid"), false, bit},
                           {b.getStringAttr("bits"), false, read}});
  auto writeToken = bundle({{b.getStringAttr("ready"), true, bit},
                            {b.getStringAttr("valid"), false, bit},
                            {b.getStringAttr("bits"), false, write}});
  ps.push_back(
      {b.getStringAttr("fased_host_read_response"), readToken, Direction::In});
  ps.push_back({b.getStringAttr("fased_host_write_response"),
                writeToken,
                Direction::In});
  if (mode == 9)
    ps[3].type = bit;
  if (mode == 10)
    ps[5].direction = Direction::In;
  if (mode >= 19 && mode <= 21)
    ps[8].name = b.getStringAttr(mode == 19   ? "fased_frfcfs_timings"
                                 : mode == 20 ? "fased_frfcfs_targetCycle"
                                              : "fased_frfcfs_unknown");
  auto top = b.create<FModuleOp>(loc,
                                 b.getStringAttr(c.getName()),
                                 ConventionAttr::get(cx, Convention::Internal),
                                 ps);
  if (mode != 11) {
    auto engine = b.create<FModuleOp>(loc,
                                      b.getStringAttr("GGFASEDTokenEngine"),
                                      top.getConventionAttr(),
                                      ArrayRef<PortInfo>{});
    if (mode != 18)
      engine->setAttr(
          "goldengate.bridgeConstructor",
          b.getDictionaryAttr(
              {b.getNamedAttr("axi4Edge",
                              b.getDictionaryAttr(
                                  {b.getNamedAttr("maxFlight",
                                                  b.getI64IntegerAttr(flight)),
                                   b.getNamedAttr("idReuse",
                                                  b.getI64IntegerAttr(
                                                      flight == 10 ? 1 : 2)),
                                   b.getNamedAttr("maxReadTransfer",
                                                  b.getI64IntegerAttr(8)),
                                   b.getNamedAttr("maxWriteTransfer",
                                                  b.getI64IntegerAttr(8))})),
               b.getNamedAttr(
                   "axi4Widths",
                   b.getDictionaryAttr(
                       {b.getNamedAttr("addrBits", b.getI64IntegerAttr(35)),
                        b.getNamedAttr(
                            "dataBits",
                            b.getI64IntegerAttr(mode == 17 ? 32 : 64)),
                        b.getNamedAttr("idBits", b.getI64IntegerAttr(4))}))}));
  }
  if (mode == 13 || mode == 14)
    b.create<FModuleOp>(loc,
                        b.getStringAttr(mode == 13
                                            ? std::string("GGFASEDAXI4Releaser")
                                            : newRoot()),
                        top.getConventionAttr(),
                        ArrayRef<PortInfo>{});
  if (mode == 15) {
    b.setInsertionPointToStart(top.getBodyBlock());
    b.create<InstanceOp>(loc, top, "used");
  }
  if (mode == 16)
    c.setName("wrong");
  SmallVector<Attribute> raw;
  for (auto n : {"other",
                 "fased_timing.aw.bits.id",
                 "fased_timing.w.ready",
                 "fased_timing.ar.valid",
                 "fased_timing.r.bits.data",
                 "fased_read_egress_req.valid",
                 "fased_write_egress_resp.tBits.id"})
    raw.push_back(b.getDictionaryAttr(
        {b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
         b.getNamedAttr("target",
                        b.getStringAttr("~" + oldRoot() + "|" + oldRoot() +
                                        ">" + std::string(n)))}));
  if (mode != 12)
    c->setAttr("rawAnnotations", b.getArrayAttr(raw));
  c->setAttr("goldengate.fasedBaseCapacity", b.getI64IntegerAttr(32));
  return root;
}
} // namespace goldengate::test
