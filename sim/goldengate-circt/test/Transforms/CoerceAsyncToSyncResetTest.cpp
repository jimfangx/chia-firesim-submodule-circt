// See LICENSE for license details.
#include "goldengate/CoerceAsyncToSyncReset.h"
#include "goldengate/SimulatorRTL.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, llvm::StringRef message) {
  if (!condition) throw std::runtime_error(message.str());
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
void check(CircuitOp circuit) {
  circuit.walk([](Operation *op) {
    require(!isa<AsAsyncResetPrimOp>(op), "asAsyncReset survived");
    for (auto result : op->getResults())
      require(!isa<ResetType, AsyncResetType>(result.getType()), "reset result survived");
    if (auto mod = dyn_cast<FModuleLike>(op))
      for (auto port : mod.getPorts())
        require(!isa<ResetType, AsyncResetType>(port.type), "reset port survived");
  });
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2 || argc == 3, "usage: test output.sv [boundary.mlir]");
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::chirrtl::CHIRRTLDialect,
                        circt::hw::HWDialect>();
    auto fixture = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" attributes {rawAnnotations = [{class = "test.Target", target = "~Top|Top>state"}]} {
        firrtl.extmodule private @External(in reset: !firrtl.asyncreset, out resetOut: !firrtl.reset)
        firrtl.module @Top(in %clock: !firrtl.clock, in %reset: !firrtl.asyncreset, in %next: !firrtl.uint<8>, out %value: !firrtl.uint<8>, out %resetOut: !firrtl.reset) {
          %zero = firrtl.constant 0 : !firrtl.const.uint<8>
          %state = firrtl.regreset %clock, %reset, %zero {annotations = [{class = "firrtl.transforms.DontTouchAnnotation"}]} : !firrtl.clock, !firrtl.asyncreset, !firrtl.const.uint<8>, !firrtl.uint<8>
          firrtl.strictconnect %state, %next : !firrtl.uint<8>
          firrtl.strictconnect %value, %state : !firrtl.uint<8>
          %ext:2 = firrtl.instance ext @External(in reset: !firrtl.asyncreset, out resetOut: !firrtl.reset)
          firrtl.strictconnect %ext#0, %reset : !firrtl.asyncreset
          firrtl.strictconnect %resetOut, %ext#1 : !firrtl.reset
          %one = firrtl.constant 1 : !firrtl.const.uint<1>
          %cast = firrtl.asAsyncReset %one : (!firrtl.const.uint<1>) -> !firrtl.const.asyncreset
        }
      }
    })mlir", &context);
    require(bool(fixture), "fixture parse");
    CircuitOp circuit; RegResetOp reg;
    fixture->walk([&](CircuitOp op) { circuit = op; });
    fixture->walk([&](RegResetOp op) { reg = op; });
    auto archive = circuit->getAttr("rawAnnotations");
    auto metadata = reg->getAttrDictionary();
    SmallVector<Value> operands(reg->getOperands());
    goldengate::coerceAsyncToSyncReset(circuit);
    check(circuit);
    require(succeeded(verify(*fixture)), "coerced fixture invalid");
    require(circuit->getAttr("rawAnnotations") == archive &&
            reg->getAttrDictionary() == metadata &&
            llvm::equal(reg->getOperands(), operands),
            "coercion changed identities, register clock/reset/init or annotations");
    auto once = dump(*fixture);
    require(once.find("firrtl.asUInt") != std::string::npos &&
            once.find("!firrtl.const.uint<1>") != std::string::npos,
            "constant reset cast did not preserve Bool constness");
    goldengate::coerceAsyncToSyncReset(circuit);
    require(once == dump(*fixture), "coercion is not idempotent");
    std::string error;
    require(succeeded(goldengate::emitSimulatorRTL(*fixture, "", argv[1], error)), error);
    auto file = llvm::MemoryBuffer::getFile(argv[1]);
    require(bool(file), "cannot read emitted fixture");
    auto rtl = (*file)->getBuffer();
    require(rtl.contains("always @(posedge clock)") &&
            !rtl.contains("or posedge reset") && !rtl.contains("or negedge reset") &&
            rtl.contains("if (reset)") && rtl.contains("state <= 8'h0") &&
            rtl.contains("state <= next"), "register reset is not clocked");
    llvm::outs() << "Reset ports/casts, clock/reset/init identity, annotations, constness, idempotence and synchronous register RTL passed\n";
    if (argc == 3) {
      auto boundary = parseSourceFile<ModuleOp>(argv[2], &context);
      require(bool(boundary), "boundary parse");
      boundary->walk([&](CircuitOp op) { circuit = op; });
      goldengate::coerceAsyncToSyncReset(circuit);
      check(circuit);
      require(succeeded(verify(*boundary)), "coerced boundary invalid");
      require(succeeded(goldengate::emitSimulatorRTL(*boundary, argv[2], argv[1], error)), error);
      llvm::outs() << "Coerced actual candidate boundary and emitted RTL\n";
    }
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n'; return 1;
  }
}
