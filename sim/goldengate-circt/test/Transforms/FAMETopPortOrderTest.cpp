// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void run(MLIRContext &context) {
  for (unsigned rejection = 0; rejection < 8; ++rejection) {
    auto root = parseSourceString<ModuleOp>(R"mlir(
      module { firrtl.circuit "Top" {
        firrtl.module @Top(in %hostClock: !firrtl.clock,
                          in %hostReset: !firrtl.uint<1>,
                          in %bypass_in: !firrtl.uint<8>,
                          in %sink: !firrtl.uint<8>,
                          out %source: !firrtl.uint<8>,
                          out %bypass_out: !firrtl.uint<8>,
                          out %aggregate: !firrtl.bundle<x: uint<8>>) {
          firrtl.strictconnect %bypass_out, %bypass_in : !firrtl.uint<8>
          firrtl.strictconnect %source, %sink : !firrtl.uint<8>
          %x = firrtl.subfield %aggregate[x] : !firrtl.bundle<x: uint<8>>
          firrtl.strictconnect %x, %bypass_in : !firrtl.uint<8>
        }
      } }
    )mlir", &context);
    require(bool(root), "fixture parse failed");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto top = *circuit.getOps<FModuleOp>().begin();
    OpBuilder b(&context);
    SmallVector<Attribute> annotations(top.getNumPorts(), b.getArrayAttr({}));
    auto anno = b.getDictionaryAttr(
        {b.getNamedAttr("class", b.getStringAttr("test.Passthrough"))});
    annotations[5] = b.getArrayAttr({anno});
    top.setPortAnnotationsAttr(b.getArrayAttr(annotations));
    top.setPortSymbolsAttr(5, circt::hw::InnerSymAttr::get(
                                 b.getStringAttr("bypass_id")));
    // Original non-stale order before channel grouping: output before input.
    SmallVector<StringRef> retained{"bypass_out", "bypass_in", "hostClock",
                                    "hostReset", "aggregate"};
    SmallVector<StringRef> channels{"sink", "source"};
    if (rejection == 1)
      retained[0] = "absent";
    if (rejection == 2)
      channels[0] = "absent";
    if (rejection == 3)
      retained.push_back("bypass_out");
    if (rejection == 4)
      channels.push_back("sink");
    if (rejection == 5)
      channels.push_back("bypass_out");
    if (rejection == 6)
      retained.erase(retained.begin() + 2);
    if (rejection == 7)
      retained.pop_back();
    std::string before = dump(*root), error;
    auto result = goldengate::orderFAMETopPorts(top, retained, channels, error);
    if (rejection) {
      require(failed(result) && !error.empty(), "incomplete ordering accepted");
      require(dump(*root) == before, "rejected ordering mutated IR");
      continue;
    }
    require(succeeded(result) && succeeded(verify(*root)),
            "port ordering failed verification");
    SmallVector<StringRef> expected(retained);
    expected.append(channels.begin(), channels.end());
    require(top.getNumPorts() == expected.size(), "port dropped");
    for (unsigned i = 0; i < expected.size(); ++i)
      require(top.getPortName(i) == expected[i], "original retained order lost");
    require(top.getPortAnnotationsAttr()[0] == annotations[5] &&
                cast<circt::hw::InnerSymAttr>(top.getPortSymbolsAttr()[0])
                        .getSymName() == "bypass_id",
            "retained port annotation/symbol lost");
    auto args = top.getBodyBlock()->getArguments();
    require(top.getPortDirection(0) == Direction::Out &&
                top.getPortDirection(1) == Direction::In &&
                isa<BundleType>(args[4].getType()),
            "retained port direction/type changed");
    unsigned connects = 0;
    for (auto connect : top.getOps<StrictConnectOp>()) {
      ++connects;
      if (connect.getDest() == args[0])
        require(connect.getSrc() == args[1], "passthrough SSA route changed");
      else if (connect.getDest() == args[6])
        require(connect.getSrc() == args[5], "channel SSA route changed");
      else {
        auto field = connect.getDest().getDefiningOp<SubfieldOp>();
        require(field && field.getInput() == args[4] &&
                    connect.getSrc() == args[1], "aggregate SSA route changed");
      }
    }
    require(connects == 3, "body connections lost");
    before = dump(*root);
    require(succeeded(goldengate::orderFAMETopPorts(top, retained, channels, error)) &&
                dump(*root) == before, "identity ordering mutated IR");
  }
  llvm::outs() << "Preserved retained top port order, SSA, aggregates and metadata; "
                  "rejected seven incomplete plans without mutation\n";
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
