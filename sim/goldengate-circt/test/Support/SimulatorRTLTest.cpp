// See LICENSE for license details.
#include "goldengate/SimulatorRTL.h"
#include "goldengate/LowerTypes.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/SV/SVDialect.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
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
std::string read(llvm::StringRef path) {
  auto contents = llvm::MemoryBuffer::getFile(path);
  require(bool(contents), "cannot read RTL"); return (*contents)->getBuffer().str();
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 3, "usage: SimulatorRTLTest boundary.mlir output.sv");
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::chirrtl::CHIRRTLDialect, circt::hw::HWDialect>();
    context.printOpOnDiagnostic(false);
    ScopedDiagnosticHandler diagnostics(&context, [](Diagnostic &diagnostic) {
      llvm::errs() << diagnostic.getLocation() << ": " << diagnostic.str() << '\n';
      return success();
    });
    context.loadDialect<circt::comb::CombDialect, circt::sv::SVDialect>();
    auto checkIndex = [&](unsigned wordBits, unsigned bound, bool dynamicBound,
                          bool unboundedIndex, bool initial, unsigned expected) {
      std::string source = "module { hw.module @Indices(in %bound: i7, in %index: i7) {\n"
        "%word = sv.reg : !hw.inout<i" + std::to_string(wordBits) + ">\n"
        "%zero = hw.constant 0 : i7\n%upper = hw.constant " + std::to_string(bound) +
        " : i7\n%step = hw.constant 32 : i7\n" +
        (initial ? "sv.initial {\n" : "sv.alwayscomb {\n") +
        "\"sv.for\"(%zero, " + (dynamicBound ? "%bound" : "%upper") +
        ", %step) ({\n^bb0(%j: i7):\n"
        "%part = sv.indexed_part_select_inout %word[" +
        (unboundedIndex ? "%index" : "%j") + ": 32] : !hw.inout<i" +
        std::to_string(wordBits) + ">, i7\n"
        "%value = hw.constant 0 : i32\nsv.bpassign %part, %value : i32\n"
        "}) {inductionVarName = \"j\"} : (i7, i7, i7) -> ()\n}\nhw.output\n} }";
      auto indices = parseSourceString<ModuleOp>(source, &context);
      require(bool(indices), "initialization index fixture parse");
      circt::sv::ForOp loop; circt::sv::IndexedPartSelectInOutOp part;
      indices->walk([&](circt::sv::ForOp op) { loop = op; });
      indices->walk([&](circt::sv::IndexedPartSelectInOutOp op) { part = op; });
      SmallVector<Value> bounds(loop->getOperands());
      auto iv = loop.getInductionVar();
      auto oldBase = part.getBase();
      auto before = dump(*indices);
      require(goldengate::normalizeInitializationIndices(*indices) == expected,
              "initialization index proof accepted/rejected the wrong case");
      require(succeeded(verify(*indices)), "index normalization invalid");
      require(llvm::equal(bounds, loop->getOperands()) &&
              iv == loop.getInductionVar() && iv.getType().getIntOrFloatBitWidth() == 7,
              "normalization changed loop control or its termination bit");
      if (expected) {
        auto extract = part.getBase().getDefiningOp<circt::comb::ExtractOp>();
        require(extract && extract.getInput() == oldBase && extract.getLowBit() == 0 &&
                extract.getResult().getType().getIntOrFloatBitWidth() == 6,
                "normalized base does not preserve in-range values");
        for (unsigned i = 0; i < bound; i += 32)
          require(i == (i & 63), "normalization changed an executed part-select base");
      } else {
        require(before == dump(*indices), "unproven index was changed");
      }
      require(goldengate::normalizeInitializationIndices(*indices) == 0,
              "index normalization is not idempotent");
    };
    checkIndex(64, 64, false, false, true, 1);
    checkIndex(33, 33, false, false, true, 1);
    checkIndex(64, 65, false, false, true, 0);
    checkIndex(64, 64, true, false, true, 0);
    checkIndex(64, 64, false, true, true, 0);
    checkIndex(64, 64, false, false, false, 0);
    llvm::outs() << "Initialization indices: bounded and partial words narrowed; dynamic/unbounded/out-of-range/non-initial indices retained; loop control unchanged\n";
    auto fixture = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" attributes {rawAnnotations = [{class = "test.Archived", target = "~FormerTop|Removed>port"}]} {
        firrtl.extmodule @BUFGCE(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock) attributes {defname = "BUFGCE"}
        firrtl.module @Top(in %hostClock: !firrtl.clock, in %enabled: !firrtl.uint<1>, in %finishing: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, out %O: !firrtl.clock) {
          %0 = firrtl.and %enabled, %finishing : (!firrtl.uint<1>, !firrtl.uint<1>) -> !firrtl.uint<1>
          %1 = firrtl.not %reset : (!firrtl.uint<1>) -> !firrtl.uint<1>
          %2 = firrtl.and %0, %1 : (!firrtl.uint<1>, !firrtl.uint<1>) -> !firrtl.uint<1>
          %gate:3 = firrtl.instance gate @BUFGCE(in I: !firrtl.clock, in CE: !firrtl.uint<1>, out O: !firrtl.clock)
          firrtl.strictconnect %gate#0, %hostClock : !firrtl.clock
          firrtl.strictconnect %gate#1, %2 : !firrtl.uint<1>
          firrtl.strictconnect %O, %gate#2 : !firrtl.clock
          %probe = firrtl.wire {annotations = [{class = "firrtl.transforms.DontTouchAnnotation"}]} : !firrtl.uint<1>
          firrtl.strictconnect %probe, %enabled : !firrtl.uint<1>
        }
      }
    })mlir", &context);
    require(bool(fixture), "fixture parse");
    std::string error, before = dump(*fixture);
    std::string unitPath = std::string(argv[2]) + ".fixture.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*fixture, argv[1], unitPath, error)), error);
    require(before == dump(*fixture), "backend changed source IR or annotation archive");
    auto rtl = read(unitPath);
    require(rtl.find("BUFGCE gate") != std::string::npos &&
            rtl.find("enabled & finishing & ~reset") != std::string::npos &&
            rtl.find("probe") != std::string::npos, "primitive/control/dont-touch RTL");
    auto invalid = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Incomplete" {
        firrtl.module @Incomplete(out %missing: !firrtl.uint<1>) {}
      }
    })mlir", &context);
    require(bool(invalid), "incomplete fixture parse");
    before = dump(*invalid);
    require(failed(goldengate::emitSimulatorRTL(*invalid, argv[1], unitPath, error)) &&
            before == dump(*invalid) && read(unitPath) == rtl,
            "failed backend replaced source or existing RTL");
    llvm::outs() << "Standard pipeline: BUFGCE/control and attached dont-touch retained; source archive unchanged; incomplete connection fails without replacing RTL\n";
    auto muxIndex = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "MuxIndex" {
        firrtl.module @MuxIndex(in %data: !firrtl.uint<32>, in %select: !firrtl.uint<1>, in %index: !firrtl.uint<3>, out %bit: !firrtl.uint<1>) {
          %zero = firrtl.constant 0 : !firrtl.const.uint<3>
          %choice = firrtl.mux(%select, %index, %zero) : (!firrtl.uint<1>, !firrtl.uint<3>, !firrtl.const.uint<3>) -> !firrtl.uint<3>
          %shift = firrtl.dshr %data, %choice : (!firrtl.uint<32>, !firrtl.uint<3>) -> !firrtl.uint<32>
          %low = firrtl.bits %shift 0 to 0 : (!firrtl.uint<32>) -> !firrtl.uint<1>
          firrtl.strictconnect %bit, %low : !firrtl.uint<1>
        }
      }
    })mlir", &context);
    require(bool(muxIndex), "mux index fixture parse");
    auto muxPath = unitPath + ".mux.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*muxIndex, "", muxPath, error)), error);
    auto muxRTL = read(muxPath);
    auto choice = muxRTL.find("select ? index : 3'h0");
    require(choice != std::string::npos &&
            muxRTL.rfind("wire [2:0]", choice) != std::string::npos &&
            muxRTL.find("data[select ?") == std::string::npos,
            "mux index lost its declared three-bit width");
    llvm::outs() << "Mux index keeps a three-bit temporary before wider expression context\n";
    auto blackboxes = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Wrapped" {
        firrtl.extmodule private @Inline<DEFAULT: ui32 = 0>(out O: !firrtl.uint<1>) attributes {defname = "inline_fixture"}
        firrtl.extmodule private @Inline_1<DEFAULT: ui32 = 1>(out O: !firrtl.uint<1>) attributes {defname = "inline_fixture"}
        firrtl.module @Wrapped(out %O: !firrtl.uint<1>, out %P: !firrtl.uint<1>) {
          %a = firrtl.instance a @Inline(out O: !firrtl.uint<1>)
          %b = firrtl.instance b @Inline_1(out O: !firrtl.uint<1>)
          firrtl.strictconnect %O, %a : !firrtl.uint<1>
          firrtl.strictconnect %P, %b : !firrtl.uint<1>
        }
      }
    })mlir", &context);
    require(bool(blackboxes), "inline blackbox fixture parse");
    auto bc = *blackboxes->getOps<CircuitOp>().begin();
    const std::string body = "module inline_fixture #(parameter DEFAULT=0)(output O); assign O = DEFAULT; endmodule\n";
    auto annotation = [&](llvm::StringRef target, llvm::StringRef text,
                          llvm::StringRef name = "inline_fixture.v") {
      return DictionaryAttr::get(&context, {
          {StringAttr::get(&context, "class"), StringAttr::get(&context, "firrtl.transforms.BlackBoxInlineAnno")},
          {StringAttr::get(&context, "target"), StringAttr::get(&context, target)},
          {StringAttr::get(&context, "name"), StringAttr::get(&context, name)},
          {StringAttr::get(&context, "text"), StringAttr::get(&context, text)}});
    };
    auto first = annotation("FormerTop.Inline", body);
    auto second = annotation("~FormerTop|Inline_1", body);
    bc->setAttr("rawAnnotations", ArrayAttr::get(&context, {first, second}));
    before = dump(*blackboxes);
    auto bbPath = unitPath + ".blackbox.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*blackboxes, argv[1], bbPath, error)), error);
    require(before == dump(*blackboxes), "blackbox backend changed source archive");
    auto bbRTL = read(bbPath);
    llvm::SmallString<256> bbDir(llvm::sys::path::parent_path(bbPath));
    llvm::SmallString<256> bbSource(bbDir), bbList(bbDir);
    llvm::sys::path::append(bbSource, "inline_fixture.v");
    llvm::sys::path::append(bbList, "firrtl_black_box_resource_files.f");
    auto emittedBody = read(bbSource);
    require(emittedBody == body || emittedBody + "\n" == body,
            "inline source changed during CIRCT export");
    require(bbRTL.find("FILE \"") == std::string::npos &&
            bbRTL.find("\nmodule inline_fixture") == std::string::npos &&
            bbRTL.find(".DEFAULT(0)") != std::string::npos &&
            bbRTL.find(".DEFAULT(1)") != std::string::npos,
            "single RTL stream contains collateral or loses blackbox parameters");
    auto resources = read(bbList);
    auto position = resources.find("inline_fixture.v");
    require(position != std::string::npos &&
            resources.find("inline_fixture.v", position + 1) == std::string::npos,
            "duplicate inline filename was not deduplicated");
    for (auto bad : {annotation("FormerTop.Inline_1", "different body"),
                     annotation("FormerTop.Removed", body),
                     annotation("FormerTop.Inline", body, "../escape.v")}) {
      bc->setAttr("rawAnnotations", ArrayAttr::get(&context, {first, bad}));
      before = dump(*blackboxes);
      require(failed(goldengate::emitSimulatorRTL(*blackboxes, argv[1], bbPath, error)) &&
              before == dump(*blackboxes) && read(bbPath) == bbRTL &&
              read(bbSource) == emittedBody && read(bbList) == resources,
              "invalid blackbox annotation changed source or published collateral");
    }
    llvm::outs() << "Inline blackboxes: legacy and wrapped module targets resolve; duplicate filename emitted once; conflicts, unresolved targets and escaping filenames fail without changing outputs\n";
    auto vector = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "VectorRead" attributes {rawAnnotations = []} {
        firrtl.module @VectorRead(in %clock: !firrtl.clock, in %index: !firrtl.uint<1>, in %data: !firrtl.uint<39>, out %O: !firrtl.uint<39>) {
          %stack = firrtl.reg %clock : !firrtl.clock, !firrtl.vector<uint, 2>
          %a = firrtl.subindex %stack[0] : !firrtl.vector<uint, 2>
          %b = firrtl.subindex %stack[1] : !firrtl.vector<uint, 2>
          firrtl.connect %a, %data : !firrtl.uint, !firrtl.uint<39>
          firrtl.connect %b, %data : !firrtl.uint, !firrtl.uint<39>
          %selected = firrtl.subaccess %stack[%index] : !firrtl.vector<uint, 2>, !firrtl.uint<1>
          firrtl.connect %O, %selected : !firrtl.uint<39>, !firrtl.uint
        }
      }
    })mlir", &context);
    require(bool(vector), "dynamic vector fixture parse");
    auto vc = *vector->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*vector, vc, error)), error);
    unsigned muxes = 0;
    vc.walk([&](MultibitMuxOp mux) {
      ++muxes;
      require(mux.getResult().getType() == UIntType::get(&context, 39),
              "dynamic mux retained an unresolved width");
    });
    require(muxes == 1, "expected one lowered dynamic mux");
    require(succeeded(goldengate::emitSimulatorRTL(*vector, argv[1], unitPath + ".vector.sv", error)), error);
    llvm::outs() << "Unknown vector widths inferred before LowerTypes; 39-bit dynamic mux emitted through backend\n";
    auto real = parseSourceFile<ModuleOp>(argv[1], &context);
    require(bool(real), "boundary parse"); before = dump(*real);
    auto result = goldengate::emitSimulatorRTL(*real, argv[1], argv[2], error);
    require(before == dump(*real), "real boundary changed by backend");
    if (failed(result)) {
      llvm::errs() << error << '\n'; return 1;
    }
    llvm::outs() << "Real boundary RTL emitted through linked CIRCT pipeline; source unchanged\n";
    return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
