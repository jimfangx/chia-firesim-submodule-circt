// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
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
  for (unsigned rejection = 0; rejection < 6; ++rejection) {
    auto root = parseSourceString<ModuleOp>(R"mlir(
      module { firrtl.circuit "Top" {
        firrtl.module @Top(in %hostClock: !firrtl.clock,
                          in %hostReset: !firrtl.uint<1>,
                          in %stale_in: !firrtl.clock,
                          out %stale_out: !firrtl.clock,
                          in %unused: !firrtl.clock,
                          in %data_in: !firrtl.uint<8>,
                          out %data_out: !firrtl.uint<8>,
                          in %token: !firrtl.bundle<bits: clock>) {
          %ancillary = firrtl.wire : !firrtl.clock
          firrtl.strictconnect %ancillary, %stale_in : !firrtl.clock
          %host = firrtl.wire : !firrtl.clock
          firrtl.strictconnect %host, %hostClock : !firrtl.clock
          %bits = firrtl.subfield %token[bits] : !firrtl.bundle<bits: clock>
          %channel = firrtl.wire : !firrtl.clock
          firrtl.strictconnect %channel, %bits : !firrtl.clock
          firrtl.strictconnect %data_out, %data_in : !firrtl.uint<8>
        }
        firrtl.module @Parent() {}
      } }
    )mlir", &context);
    require(bool(root), "fixture parse failed");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto top = *circuit.getOps<FModuleOp>().begin();
    OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
    // Exercise both FIRRTL connect operations and a connect between two
    // removed ports: it must be erased exactly once.
    b.create<ConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(3),
                        top.getBodyBlock()->getArgument(2));
    SmallVector<Attribute> annotations(top.getNumPorts(), b.getArrayAttr({}));
    auto anno = b.getDictionaryAttr(
        {b.getNamedAttr("class", b.getStringAttr("test.Retained"))});
    annotations[6] = b.getArrayAttr({anno});
    if (rejection == 2)
      annotations[4] = b.getArrayAttr({anno});
    top.setPortAnnotationsAttr(b.getArrayAttr(annotations));
    top.setPortSymbolsAttr(6, circt::hw::InnerSymAttr::get(
                                 b.getStringAttr("data_id")));
    if (rejection == 1)
      b.create<AsUIntPrimOp>(top.getLoc(), top.getBodyBlock()->getArgument(4));
    if (rejection == 3)
      top.setPortSymbolsAttr(4, circt::hw::InnerSymAttr::get(
                                   b.getStringAttr("clock_id")));
    if (rejection == 4) {
      auto parent = *std::next(circuit.getOps<FModuleOp>().begin());
      b.setInsertionPointToEnd(parent.getBodyBlock());
      b.create<InstanceOp>(parent.getLoc(), top, "child");
    }
    if (rejection == 5) {
      SmallVector<Attribute> names(top.getPortNamesAttr().getValue());
      names[0] = b.getStringAttr("missingHost");
      top.setPortNamesAttr(b.getArrayAttr(names));
    }
    std::string before = dump(*root), error;
    auto result = goldengate::removeFAMEStaleTopClocks(top, error);
    if (rejection) {
      require(failed(result) && !error.empty(), "unsafe clock removal accepted");
      require(dump(*root) == before, "rejected clock removal mutated IR");
      continue;
    }
    require(succeeded(result) && succeeded(verify(*root)),
            "clock removal failed verification");
    SmallVector<StringRef> expected{"hostClock", "hostReset", "data_in",
                                    "data_out", "token"};
    require(top.getNumPorts() == expected.size(), "wrong ports removed");
    for (unsigned i = 0; i < expected.size(); ++i)
      require(top.getPortName(i) == expected[i], "retained port order changed");
    require(top.getPortAnnotationsAttr()[3] == annotations[6] &&
                cast<circt::hw::InnerSymAttr>(top.getPortSymbolsAttr()[3])
                        .getSymName() == "data_id",
            "retained port metadata lost");
    unsigned connects = 0, wires = 0;
    for (auto wire : top.getOps<WireOp>()) {
      ++wires;
      if (wire.getName() == "ancillary")
        require(wire.getResult().use_empty(), "stale clock connect retained");
    }
    auto args = top.getBodyBlock()->getArguments();
    for (auto connect : top.getOps<StrictConnectOp>()) {
      ++connects;
      if (connect.getDest() == args[3])
        require(connect.getSrc() == args[2], "data passthrough changed");
      else if (auto field = connect.getSrc().getDefiningOp<SubfieldOp>())
        require(field.getInput() == args[4], "clock token wiring changed");
      else
        require(connect.getSrc() == args[0], "host clock wiring changed");
    }
    require(connects == 3 && wires == 3 && top.getOps<ConnectOp>().empty(),
            "ancillary/host/channel clock connects or declarations changed");
    before = dump(*root);
    require(succeeded(goldengate::removeFAMEStaleTopClocks(top, error)) &&
                dump(*root) == before, "repeat removal mutated IR");
  }
  llvm::outs() << "Removed stale input/output/unused clocks and ancillary connects; "
                  "preserved host/token clocks, data wiring and metadata; "
                  "rejected five unsafe cases without mutation\n";
}
void runAncillary(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(
    module { firrtl.circuit "Top" {
      firrtl.module @Top(in %hostClock: !firrtl.clock,
                        in %reset: !firrtl.uint<1>,
                        in %targetClock: !firrtl.clock,
                        out %clockOut: !firrtl.clock,
                        in %data: !firrtl.uint<8>,
                        out %dataOut: !firrtl.uint<8>) {
        %a = firrtl.wire : !firrtl.clock
        %b = firrtl.wire : !firrtl.clock
        %derived = firrtl.node %hostClock : !firrtl.clock
        firrtl.strictconnect %a, %derived : !firrtl.clock
        firrtl.when %reset : !firrtl.uint<1> {
          firrtl.strictconnect %b, %a : !firrtl.clock
        }
        firrtl.strictconnect %dataOut, %data : !firrtl.uint<8>
      }
      firrtl.module @Model(in %clock: !firrtl.clock,
                          out %clockOut: !firrtl.clock) {
        firrtl.strictconnect %clockOut, %clock : !firrtl.clock
      }
    } }
  )mlir", &context);
  require(bool(root), "ancillary fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto model = *std::next(circuit.getOps<FModuleOp>().begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  auto args = top.getBodyBlock()->getArguments();
  auto input = b.create<StrictConnectOp>(top.getLoc(), instance.getResult(0), args[2]);
  auto output = b.create<StrictConnectOp>(top.getLoc(), args[3], instance.getResult(1));
  auto wire = *top.getOps<WireOp>().begin();
  b.create<ConnectOp>(top.getLoc(), wire.getResult(), instance.getResult(1));
  std::string error, before = dump(*root), modelBefore = dump(model);
  require(failed(goldengate::removeFAMEAncillaryTopClockConnects(top, {}, error)) &&
              dump(*root) == before, "invalid instance mutated wrapper");
  require(failed(goldengate::removeFAMEAncillaryTopClockConnects(model, instance, error)) &&
              dump(*root) == before, "foreign instance mutated model");
  require(succeeded(goldengate::removeFAMEAncillaryTopClockConnects(top, instance, error)) &&
              succeeded(verify(*root)), "ancillary cleanup failed verification");
  unsigned connects = 0;
  top.walk([&](StrictConnectOp) { ++connects; });
  require(connects == 3 && top.getOps<ConnectOp>().empty() &&
              wire.getResult().use_empty(), "original ancillary connects remain");
  require(input.getSrc() == args[2] && output.getDest() == args[3] &&
              dump(model) == modelBefore && top.getNumPorts() == 6 &&
              std::distance(top.getOps<WireOp>().begin(), top.getOps<WireOp>().end()) == 2,
          "staged port connects, model body or declarations changed");
  before = dump(*root);
  require(succeeded(goldengate::removeFAMEAncillaryTopClockConnects(top, instance, error)) &&
              dump(*root) == before, "repeated ancillary cleanup changed IR");
  // Generated host and token wiring is added after cleanup, as in the CLI.
  b.create<StrictConnectOp>(top.getLoc(), wire.getResult(), args[0]);
  require(succeeded(verify(*root)) && !wire.getResult().use_empty(),
          "later host clock wiring was not preserved");
  llvm::outs() << "Removed original internal/conditional clock connects; retained "
                  "staged port connects, data and model body; later host wiring survives\n";
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    runAncillary(context);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
