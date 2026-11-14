// See LICENSE for license details.
#include "goldengate/FASEDHostWidthAdapter.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
int main(int argc, char **argv) {
  try {
    unsigned width = 256;
    if (argc != 3 || StringRef(argv[1]).getAsInteger(10, width))
      throw std::runtime_error("usage: test target-data-width output.mlir");
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"GGFASEDTokenEngine\" { firrtl.module "
        "@GGFASEDTokenEngine() {} } }",
        &ctx);
    auto circuit = *root->getOps<CircuitOp>().begin();
    OpBuilder b(&ctx);
    auto engine = *circuit.getOps<FModuleOp>().begin();
    engine->setAttr(
        "goldengate.bridgeConstructor",
        b.getDictionaryAttr(
            {b.getNamedAttr("axi4Widths",
                            b.getDictionaryAttr({b.getNamedAttr(
                                "dataBits", b.getI64IntegerAttr(width))})),
             b.getNamedAttr(
                 "axi4Edge",
                 b.getDictionaryAttr(
                     {b.getNamedAttr("maxReadTransfer",
                                     b.getI64IntegerAttr(64 / (width / 8))),
                      b.getNamedAttr("maxWriteTransfer",
                                     b.getI64IntegerAttr(64 / (width / 8))),
                      b.getNamedAttr("idReuse", b.getI64IntegerAttr(15)),
                      b.getNamedAttr("maxFlight",
                                     b.getI64IntegerAttr(232))}))}));
    circuit->setAttr("goldengate.fasedBaseCapacity", b.getI64IntegerAttr(32));
    auto u = [&](unsigned n) { return UIntType::get(&ctx, n, false); };
    auto token =
        [&](std::initializer_list<std::pair<StringRef, unsigned>> fields) {
          SmallVector<BundleType::BundleElement> members;
          for (auto [n, w] : fields)
            members.push_back({b.getStringAttr(n), false, u(w)});
          return BundleType::get(&ctx,
                                 {{b.getStringAttr("ready"), true, u(1)},
                                  {b.getStringAttr("valid"), false, u(1)},
                                  {b.getStringAttr("bits"),
                                   false,
                                   BundleType::get(&ctx, members)}});
        };
    auto bus = [&](unsigned bits) {
      auto address = token({{"id", 4},
                            {"qos", 4},
                            {"prot", 3},
                            {"cache", 4},
                            {"lock", 1},
                            {"burst", 2},
                            {"size", 3},
                            {"len", 8},
                            {"addr", 35}});
      return BundleType::get(
          &ctx,
          {{b.getStringAttr("aw"), false, address},
           {b.getStringAttr("w"),
            false,
            token({{"strb", bits / 8}, {"last", 1}, {"data", bits}})},
           {b.getStringAttr("b"), true, token({{"id", 4}, {"resp", 2}})},
           {b.getStringAttr("ar"), false, address},
           {b.getStringAttr("r"),
            true,
            token({{"id", 4}, {"last", 1}, {"data", bits}, {"resp", 2}})}});
    };
    FModuleOp helper;
    std::string error;
    if (failed(goldengate::materializeFASEDHostWidthAdapter(
            circuit, bus(width), bus(64), helper, error)))
      throw std::runtime_error(error);
    circuit.setName(helper.getName());
    if (failed(verify(*root)))
      throw std::runtime_error("invalid width adapter");
    std::error_code ec;
    llvm::raw_fd_ostream out(argv[2], ec);
    if (ec)
      throw std::runtime_error(ec.message());
    root->print(out);
    out << '\n';
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n';
    return 1;
  }
}
