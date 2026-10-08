// See LICENSE for license details.
#include "goldengate/AsyncRAMModel.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
const char *fixture = R"fir(circuit HostRAM :
  module HostRAM :
    input clock : Clock
    input reset : UInt<1>
    output channels : { flip reset : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}, flip read_cmds : {flip ready : UInt<1>, valid : UInt<1>, bits : {en : UInt<1>, addr : UInt<3>}}[1], read_resps : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<17>}[1], flip write_cmds : {flip ready : UInt<1>, valid : UInt<1>, bits : {en : UInt<1>, mask : UInt<1>, addr : UInt<3>, data : UInt<17>}}[1] }
    wire oldState : UInt<1>
    oldState <= UInt<1>(0)
)fir";
void run(MLIRContext &context) {
  for (unsigned probe = 0; probe != 13; ++probe) {
    std::string input(fixture);
    auto replace = [&](StringRef from, StringRef to) {
      auto pos = input.find(from.str());
      require(pos != std::string::npos, "bad test mutation");
      input.replace(pos, from.size(), to.str());
    };
    if (probe == 1) replace("bits : UInt<17>", "bits : UInt<18>");
    if (probe == 2) replace("addr : UInt<3>", "addr : UInt");
    if (probe == 3) replace("[1], read_resps", "[0], read_resps");
    if (probe == 4) replace("output channels", "input channels");
    if (probe == 8) replace("input reset", "input resetOther");
    if (probe == 9) replace("addr : UInt<3>", "addr : UInt<63>");
    llvm::SourceMgr source;
    source.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(input), llvm::SMLoc());
    DefaultTimingManager timer;
    auto scope = timer.getRootScope();
    auto root = importFIRFile(source, &context, scope, FIRParserOptions());
    require(bool(root), "Async RAM fixture import failed");
    CircuitOp circuit;
    root->walk([&](CircuitOp c) { circuit = c; });
    auto module = *circuit.getOps<FModuleOp>().begin();
    OpBuilder b(&context);
    auto raw = b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.RetainedReference")),
      b.getNamedAttr("target", b.getStringAttr(
        probe == 6 ? "~HostRAM|HostRAM>oldState" :
        probe == 10 ? "~HostRAM|Parent/ram:HostRAM>oldState" :
        probe == 11 ? "~HostRAM|Parent/ram:HostRAM>channels.read_resps[0].bits" :
        probe == 12 ? "~HostRAM|HostRAM/oldInstance:Child>data" :
                      "~HostRAM|HostRAM>channels.read_resps[0].bits"))})});
    circuit->setAttr("rawAnnotations", raw);
    if (probe == 5) module.getBodyBlock()->front().setAttr("annotations", raw);
    auto before = dump(*root);
    auto portTypes = module->getAttr("portTypes");
    auto portNames = module->getAttr("portNames");
    auto directions = module->getAttr("portDirections");
    goldengate::RAMModelParameters parameters;
    std::string error;
    auto result = goldengate::emitAsyncRAMModel(module, parameters, error);
    if (probe != 0 && probe != 7 && probe != 11) {
      require(failed(result) && !error.empty(), "unsupported Async RAM boundary accepted");
      require(dump(*root) == before && parameters.reads == 0, "Async RAM rejection mutated circuit");
    } else {
      require(succeeded(result), "Async RAM emission failed");
      require(parameters.reads == 1 && parameters.writes == 1 &&
              parameters.addressWidth == 3 && parameters.dataWidth == 17, "Async RAM parameters differ");
      require(circuit->getAttr("rawAnnotations") == raw && module->getAttr("portTypes") == portTypes &&
              module->getAttr("portNames") == portNames && module->getAttr("portDirections") == directions,
              "Async RAM emitter changed ABI/annotations");
      require(succeeded(verify(*root)), "Async RAM IR verification failed");
      require(module.getOps<WireOp>().empty() && !module.getOps<MemOp>().empty(), "body was not replaced");
      require(succeeded(goldengate::emitAsyncRAMModel(module, parameters, error)) &&
              succeeded(verify(*root)), "Async RAM emission not repeatable");
    }
  }
}
} // namespace
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { run(context); } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n'; return 1;
  }
  llvm::outs() << "PASS native Async RAM, retained port identities and ten atomic rejection boundaries\n";
}
