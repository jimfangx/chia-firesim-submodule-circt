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
  for (unsigned probe = 0; probe != 26; ++probe) {
    std::string input(fixture);
    if (probe == 1) { // A valid but different elaborated implementation ABI.
      auto at = input.rfind("data : UInt<17>");
      input.replace(at, std::string("data : UInt<17>").size(), "data : UInt<18>");
    }
    if (probe == 2 || probe == 3) {
      auto at = input.find("bits : UInt<17>");
      input.replace(at, std::string("bits : UInt<17>").size(), probe == 2 ? "bits : SInt<17>" : "bits : UInt");
    }
    if (probe >= 9) {
      input.erase(input.find("  module HostRAM :")); // Native creation needs no host ABI.
      if (probe == 10 || probe == 11)
        input += "  extmodule RamModel :\n    defname = ExistingExternalRAM\n";
      if (probe == 11)
        input += "  module RamModel_1 :\n    skip\n";
      if (probe == 12) {
        size_t at = 0;
        while ((at = input.find("UInt<3>", at)) != std::string::npos) {
          input.replace(at, 7, "UInt<63>"); at += 8;
        }
      }
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
    if (probe == 13 || probe == 14 || probe == 15 || probe == 16) {
      std::string target = probe == 13 ? "~Top|RAM>oldState" :
          probe == 14 ? "~Top|Top/ram:RAM>read.bits.addr" :
          probe == 15 ? "~Top|Top/ram:RAM>oldState" : "~Top|RAM/child:Child>port";
      annos.push_back(b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr("test.RetainedReference")),
          b.getNamedAttr("target", b.getStringAttr(target))}));
    }
    if (probe == 20 || probe == 21) {
      // Constructor/format text is not a reference, even if it looks exactly
      // like a deleted body target (including a nested member named target).
      annos.push_back(b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
          b.getNamedAttr("target", b.getStringAttr("~Top|Top")),
          b.getNamedAttr("widgetConstructorKey", b.getDictionaryAttr({
              b.getNamedAttr("format", b.getStringAttr("~Top|RAM>oldState")),
              b.getNamedAttr("target", b.getStringAttr("~Top|RAM>oldState"))})),
          b.getNamedAttr("description", b.getStringAttr("~Top|RAM/child:Child>data"))}));
    }
    if (probe >= 22) {
      const char *klass = probe == 22 ? goldengate::AnnotationClasses::InternalAutoCounter.data() :
          probe == 23 ? goldengate::AnnotationClasses::CombinationalPath.data() :
          probe == 24 ? goldengate::AnnotationClasses::InternalXDC.data() :
                        goldengate::AnnotationClasses::ChannelConnection.data();
      NamedAttrList extra;
      extra.append("class", b.getStringAttr(klass));
      if (probe == 22) extra.append("reset", b.getStringAttr("~Top|RAM>oldState"));
      if (probe == 23) extra.append("sources", b.getArrayAttr({b.getStringAttr("~Top|RAM>oldState")}));
      if (probe == 24) extra.append("argumentList", b.getArrayAttr({b.getStringAttr("~Top|RAM>oldState")}));
      if (probe == 25) extra.append("channelInfo", b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DecoupledForwardChannel)),
          b.getNamedAttr("readySink", b.getStringAttr("~Top|RAM>oldState"))}));
      annos.push_back(extra.getDictionary(&context));
    }
    if (probe == 7) implementation = wrapper;
    if (probe == 17)
      wrapper.getBodyBlock()->front().setAttr("annotations", b.getArrayAttr({read}));
    if (probe == 18)
      wrapper.getBodyBlock()->front().setAttr("inner_sym", circt::hw::InnerSymAttr::get(b.getStringAttr("oldState")));
    if (probe == 19) wrapper = {};
    auto raw = b.getArrayAttr(annos);
    circuit->setAttr("rawAnnotations", raw);
    auto before = dump(*root);
    SmallVector<std::pair<Operation *, std::string>> unchanged;
    for (auto &op : circuit.getBodyBlock()->getOperations())
      if (&op != wrapper) unchanged.emplace_back(&op, dump(&op));
    goldengate::RAMModelParameters parameters;
    std::string error;
    auto result = probe < 9
        ? goldengate::wrapRAMModel(circuit, wrapper, implementation, parameters, error)
        : goldengate::materializeRAMModel(circuit, wrapper, implementation, parameters, error);
    bool accepted = probe == 0 || probe == 8 || probe == 9 || probe == 10 || probe == 11 || probe == 14 || probe == 20 || probe == 21;
    if (!accepted) {
      require(failed(result) && !error.empty(), "unsupported RAM boundary accepted");
      require(dump(*root) == before && parameters.reads == 0, "RAM rejection mutated the circuit");
    } else {
      require(succeeded(result), "aggregate RAM adapter failed");
      require(parameters.reads == 1 && parameters.writes == 1 &&
              parameters.addressWidth == 3 && parameters.dataWidth == 17,
              "RAM parameter resolution or duplicate annotation handling failed");
      if (probe >= 9) {
        require(implementation && implementation.getName() ==
                (probe == 10 ? "RamModel_1" : probe == 11 ? "RamModel_2" : "RamModel"),
                "RAM host module namespace allocation failed");
        require(std::distance(implementation.getOps<MemOp>().begin(),
                              implementation.getOps<MemOp>().end()) == 1,
                "native RAM host storage was not emitted");
        require((*wrapper.getOps<InstanceOp>().begin()).getModuleName() == implementation.getName(),
                "wrapper does not instantiate the newly created host");
        if (probe == 9) {
          auto firstHost = dump(implementation);
          FModuleOp second;
          require(succeeded(goldengate::materializeRAMModel(circuit, wrapper, second, parameters, error)) &&
                  second.getName() == "RamModel_1" && dump(implementation) == firstHost,
                  "multiple materializations reused or changed another host's storage");
        }
      }
      for (const auto &[op, text] : unchanged)
        require(dump(op) == text, "RAM creation changed another module");
      require(circuit->getAttr("rawAnnotations") == raw, "RAM adapter changed annotations");
      require(succeeded(verify(*root)), "RAM adapter IR verification failed");
      auto instances = wrapper.getOps<InstanceOp>();
      require(std::distance(instances.begin(), instances.end()) == 1 &&
              wrapper.getOps<WireOp>().empty(), "RAM body was not replaced once");
    }
  }
}

void selectedModels(MLIRContext &context) {
  for (unsigned probe = 0; probe != 6; ++probe) {
    llvm::SourceMgr source;
    source.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(fixture), llvm::SMLoc());
    DefaultTimingManager timer;
    auto scope = timer.getRootScope();
    auto root = importFIRFile(source, &context, scope, FIRParserOptions());
    require(bool(root), "selected RAM fixture import failed");
    auto circuit = *root->getOps<CircuitOp>().begin();
    FModuleOp wrapper, top;
    for (auto module : circuit.getOps<FModuleOp>()) {
      if (module.getName() == "RAM") wrapper = module;
      if (module.getName() == "Top") top = module;
    }
    OpBuilder b(&context);
    auto second = cast<FModuleOp>(wrapper->clone());
    second.setName("OtherRAM");
    circuit.getBodyBlock()->push_back(second);
    auto signal = [&](StringRef member, StringRef module, StringRef target) {
      return b.getNamedAttr(member,
          b.getStringAttr("~Top|" + module.str() + ">" + target.str()));
    };
    SmallVector<Attribute> annotations;
    if (probe != 0) for (StringRef name : {StringRef("RAM"), StringRef("OtherRAM")}) {
      bool unsupported = probe == 2 && name == "OtherRAM";
      auto read = b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(unsupported
              ? goldengate::AnnotationClasses::ModelReadWritePort
              : goldengate::AnnotationClasses::ModelReadPort)),
          signal("addr", name, probe == 3 ? "missing.bits" : "read.bits.addr"),
          signal("en", name, "read.bits.en"), signal("data", name, "response.bits")});
      auto write = b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ModelWritePort)),
          signal("addr", name, "write.bits.addr"), signal("en", name, "write.bits.en"),
          signal("data", name, "write.bits.data"), signal("mask", name, "write.bits.mask")});
      annotations.append({read, write, read});
    }
    if (probe == 4) {
      NamedAttrList missing(cast<DictionaryAttr>(annotations.back()));
      missing.erase("addr"); annotations.back() = missing.getDictionary(&context);
    }
    auto raw = b.getArrayAttr(annotations);
    circuit->setAttr("rawAnnotations", raw);
    if (probe == 5) circuit->removeAttr("rawAnnotations");
    auto before = dump(*root), topBefore = dump(top);
    auto originalArgument = wrapper.getArgument(0);
    unsigned count = 999;
    std::string error;
    auto result = goldengate::materializeSelectedRAMModels(circuit, count, error);
    if (probe >= 2) {
      require(failed(result) && !error.empty(), "selected RAM invalid boundary accepted");
      require(count == 999 && dump(*root) == before &&
              wrapper.getArgument(0) == originalArgument,
              "late selected RAM rejection mutated IR, SSA or result count");
      continue;
    }
    require(succeeded(result) && count == (probe == 0 ? 0u : 2u),
            "selected RAM definition discovery or count failed");
    require(dump(top) == topBefore && succeeded(verify(*root)),
            "selected RAM materialization changed untouched simulator or invalidated IR");
    require(circuit->getAttr("rawAnnotations") == raw, "selected RAM changed annotations");
    if (probe == 0) {
      require(dump(*root) == before && wrapper.getArgument(0) == originalArgument,
              "empty RAM selection changed IR or SSA");
    } else {
      // Both wrapper operations survive; distinct implementations derive their
      // geometry from the selected channel types and allocate independent RAMs.
      auto firstInstance = *wrapper.getOps<InstanceOp>().begin();
      auto secondInstance = *second.getOps<InstanceOp>().begin();
      require(firstInstance.getModuleName() != secondInstance.getModuleName(),
              "selected RAM definitions shared an implementation");
      unsigned hosts = 0;
      for (auto module : circuit.getOps<FModuleOp>())
        if (module.getName() == firstInstance.getModuleName() ||
            module.getName() == secondInstance.getModuleName()) {
          auto memory = *module.getOps<MemOp>().begin();
          require(memory.getDepth() == 8, "selected RAM depth was not derived from address width");
          ++hosts;
        }
      require(hosts == count, "selected RAM implementations were omitted or duplicated");
    }
  }
}
void responseGroups(MLIRContext &context) {
  for (unsigned probe = 0; probe < 3; ++probe) {
    std::string input(fixture);
    input.erase(input.find("  module HostRAM :"));
    auto at = input.find("    output response");
    input.insert(at, "    input read1 : { flip ready : UInt<1>, valid : UInt<1>, bits : {addr : UInt<3>, en : UInt<1>} }\n");
    at = input.find("bits : UInt<17>");
    input.replace(at, std::string("bits : UInt<17>").size(),
                  "bits : {d0 : UInt<17>, d1 : UInt<17>}");
    llvm::SourceMgr source;
    source.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(input), llvm::SMLoc());
    DefaultTimingManager timer;
    auto scope = timer.getRootScope();
    auto root = importFIRFile(source, &context, scope, FIRParserOptions());
    require(bool(root), "grouped response fixture import failed");
    auto circuit = *root->getOps<CircuitOp>().begin();
    FModuleOp wrapper;
    for (auto module : circuit.getOps<FModuleOp>())
      if (module.getName() == "RAM") wrapper = module;
    OpBuilder b(&context);
    auto member = [&](StringRef key, StringRef target) {
      return b.getNamedAttr(key, b.getStringAttr("~Top|RAM>" + target.str()));
    };
    auto read = [&](StringRef command, StringRef data) {
      return b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ModelReadPort)),
          member("addr", command.str() + ".bits.addr"),
          member("en", command.str() + ".bits.en"), member("data", data)});
    };
    auto r0 = read("read", "response.bits.d0");
    auto r1 = read("read1", probe == 1 ? "response.bits.d0" : "response.bits.d1");
    auto write = b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ModelWritePort)),
        member("addr", "write.bits.addr"), member("en", "write.bits.en"),
        member("data", "write.bits.data"), member("mask", "write.bits.mask")});
    SmallVector<Attribute> annotations{r0, r1, write};
    if (probe == 2) annotations.append({r0, r1});
    auto raw = b.getArrayAttr(annotations);
    circuit->setAttr("rawAnnotations", raw);
    auto before = dump(*root);
    FModuleOp implementation;
    goldengate::RAMModelParameters parameters;
    std::string error;
    auto result = goldengate::materializeRAMModel(circuit, wrapper, implementation, parameters, error);
    if (probe == 1) {
      require(failed(result) && error.find("response payload field") != std::string::npos &&
              dump(*root) == before, "multiply-driven response field did not reject atomically");
    } else {
      require(succeeded(result) && succeeded(verify(*root)) &&
              parameters.reads == 2 && parameters.writes == 1,
              "distinct grouped read response fields or annotation duplicates rejected");
      require(circuit->getAttr("rawAnnotations") == raw,
              "response join changed original annotation order or payload");
    }
  }
}
} // namespace
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { run(context); selectedModels(context); responseGroups(context); } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n'; return 1;
  }
  llvm::outs() << "PASS aggregate RAM adapter, shared input delivery, response joins, opaque annotation payloads, independent storage and atomic rejection boundaries\n";
}
