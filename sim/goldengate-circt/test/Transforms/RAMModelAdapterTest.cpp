// See LICENSE for license details.
#include "goldengate/RAMModelAdapter.h"
#include "goldengate/AnnotationClasses.h"
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
const char *fixture = R"fir(circuit Top :
  module Top :
    skip
  module RAM :
    input hostClock : Clock
    input hostReset : UInt<1>
    input read : { flip ready : UInt<1>, valid : UInt<1>, bits : {addr : UInt<3>, en : UInt<1>} }
    output response : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<17> }
    input write : { flip ready : UInt<1>, valid : UInt<1>, bits : {addr : UInt<3>, en : UInt<1>, data : UInt<17>, mask : UInt<1>} }
    wire oldState : UInt<1>
    oldState <= UInt<1>(0)
  module HostRAM :
    input clock : Clock
    input reset : UInt<1>
    output channels : { flip reset : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1>}, flip read_cmds : {flip ready : UInt<1>, valid : UInt<1>, bits : {en : UInt<1>, addr : UInt<3>}}[1], read_resps : {flip ready : UInt<1>, valid : UInt<1>, bits : UInt<17>}[1], flip write_cmds : {flip ready : UInt<1>, valid : UInt<1>, bits : {en : UInt<1>, mask : UInt<1>, addr : UInt<3>, data : UInt<17>}}[1] }
    skip
)fir";
void run(MLIRContext &context) {
  for (unsigned probe = 0; probe != 9; ++probe) {
    std::string input(fixture);
    if (probe == 1) { // A valid but different elaborated implementation ABI.
      auto at = input.rfind("data : UInt<17>");
      input.replace(at, std::string("data : UInt<17>").size(), "data : UInt<18>");
    }
    if (probe == 2 || probe == 3) {
      auto at = input.find("bits : UInt<17>");
      input.replace(at, std::string("bits : UInt<17>").size(), probe == 2 ? "bits : SInt<17>" : "bits : UInt");
    }
    llvm::SourceMgr source;
    source.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(input), llvm::SMLoc());
    DefaultTimingManager timer;
    auto scope = timer.getRootScope();
    auto root = importFIRFile(source, &context, scope, FIRParserOptions());
    require(bool(root), "RAM fixture import failed");
    CircuitOp circuit;
    root->walk([&](CircuitOp c) { circuit = c; });
    FModuleOp wrapper, implementation;
    for (auto m : circuit.getOps<FModuleOp>()) {
      if (m.getName() == "RAM") wrapper = m;
      if (m.getName() == "HostRAM") implementation = m;
    }
    OpBuilder b(&context);
    auto signal = [&](StringRef member, StringRef target) {
      return b.getNamedAttr(member, b.getStringAttr("~Top|RAM>" + target.str()));
    };
    auto read = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(probe == 4 ? goldengate::AnnotationClasses::ModelReadWritePort
                                                        : goldengate::AnnotationClasses::ModelReadPort)),
      signal("addr", probe == 5 ? "read.valid" : probe == 8 ? "write.bits.addr" : "read.bits.addr"),
      signal("en", "read.bits.en"), signal("data", "response.bits")});
    auto write = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ModelWritePort)),
      signal("addr", "write.bits.addr"), signal("en", "write.bits.en"),
      signal("data", "write.bits.data"), signal("mask", "write.bits.mask")});
    SmallVector<Attribute> annos{read, write, read};
    if (probe == 6)
      annos.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test.RetainedBodyReference")),
        signal("target", "oldState")}));
    if (probe == 7) implementation = wrapper;
    auto raw = b.getArrayAttr(annos);
    circuit->setAttr("rawAnnotations", raw);
    auto before = dump(*root);
    goldengate::RAMModelParameters parameters;
    std::string error;
    auto result = goldengate::wrapRAMModel(circuit, wrapper, implementation, parameters, error);
    if (probe) {
      require(failed(result) && !error.empty(), "unsupported RAM boundary accepted");
      require(dump(*root) == before && parameters.reads == 0, "RAM rejection mutated the circuit");
    } else {
      require(succeeded(result), "aggregate RAM adapter failed");
      require(parameters.reads == 1 && parameters.writes == 1 &&
              parameters.addressWidth == 3 && parameters.dataWidth == 17,
              "RAM parameter resolution or duplicate annotation handling failed");
      require(circuit->getAttr("rawAnnotations") == raw, "RAM adapter changed annotations");
      require(succeeded(verify(*root)), "RAM adapter IR verification failed");
      auto instances = wrapper.getOps<InstanceOp>();
      require(std::distance(instances.begin(), instances.end()) == 1 &&
              wrapper.getOps<WireOp>().empty(), "RAM body was not replaced once");
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
  llvm::outs() << "PASS aggregate RAM adapter, duplicate annotations and eight atomic rejection boundaries\n";
}
