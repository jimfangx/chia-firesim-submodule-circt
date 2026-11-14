// See LICENSE for license details.
#include "goldengate/SimulatorRTL.h"
#include "goldengate/LowerTypes.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Emit/EmitOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/SV/SVDialect.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/ADT/StringExtras.h"
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
    context.loadDialect<circt::emit::EmitDialect>();
    auto checkMemoryFragments = [&](unsigned variant) {
      std::string fixture = R"mlir(module {
        sv.macro.decl @RANDOM
        sv.macro.decl @INIT_RANDOM
        sv.macro.decl @RANDOMIZE_DELAY
        sv.macro.decl @INIT_RANDOM_PROLOG_
        sv.macro.decl @RANDOMIZE
        sv.macro.decl @ENABLE_INITIAL_MEM_
        sv.macro.decl @RANDOMIZE_MEM_INIT
        sv.macro.decl @RANDOMIZE_REG_INIT
        sv.macro.decl @SYNTHESIS
        sv.macro.decl @VERILATOR
        emit.fragment @CUSTOM { sv.verbatim "// Keep this custom fragment in place" }
        emit.fragment @RANDOM_INIT_MEM_FRAGMENT {
          sv.ifdef @RANDOMIZE {} else {
            sv.ifdef @RANDOMIZE_MEM_INIT { sv.macro.def @RANDOMIZE "" }
          }
          sv.ifdef @SYNTHESIS {} else {
            sv.ifdef @ENABLE_INITIAL_MEM_ {} else { sv.macro.def @ENABLE_INITIAL_MEM_ "" }
          }
        }
        emit.fragment @RANDOM_INIT_FRAGMENT {
          sv.ifdef @RANDOM {} else { sv.macro.def @RANDOM "$random" }
          sv.ifdef @INIT_RANDOM {} else { sv.macro.def @INIT_RANDOM "" }
          sv.ifdef @RANDOMIZE_DELAY {} else { sv.macro.def @RANDOMIZE_DELAY "0.002" }
          sv.ifdef @INIT_RANDOM_PROLOG_ {} else {
            sv.ifdef @RANDOMIZE {
              sv.ifdef @VERILATOR { sv.macro.def @INIT_RANDOM_PROLOG_ "`INIT_RANDOM" }
              else { sv.macro.def @INIT_RANDOM_PROLOG_ "`INIT_RANDOM #`RANDOMIZE_DELAY begin end" }
            } else { sv.macro.def @INIT_RANDOM_PROLOG_ "" }
          }
        }
        hw.module @MemoryOne() attributes {emit.fragments = [@CUSTOM]} {
          %Memory = sv.reg : !hw.inout<uarray<2xi8>>
          sv.ifdef @ENABLE_INITIAL_MEM_ {
            sv.initial {
              sv.verbatim "`INIT_RANDOM_PROLOG_"
              sv.ifdef.procedural @RANDOMIZE_MEM_INIT {}
            }
          }
          hw.output
        }
        hw.module @MemoryTwo() {
          %Memory = sv.reg : !hw.inout<uarray<2xi8>>
          sv.ifdef @ENABLE_INITIAL_MEM_ {
            sv.initial {
              sv.verbatim "`INIT_RANDOM_PROLOG_"
              sv.ifdef.procedural @RANDOMIZE_MEM_INIT {}
            }
          }
          hw.output
        }
        hw.module @Other() { hw.output }
      })mlir";
      if (variant == 1) fixture.replace(fixture.find("// Keep"), 7, "$finish; // Keep");
      if (variant == 2) fixture.replace(fixture.find("$random"), 7, "$urandom");
      if (variant == 3) fixture.replace(fixture.find("[@CUSTOM]"), 9, "[@MISSING]");
      if (variant == 4) {
        auto second = fixture.find("hw.module @MemoryTwo");
        auto prolog = fixture.find("`INIT_RANDOM_PROLOG_", second);
        fixture.replace(prolog, std::string("`INIT_RANDOM_PROLOG_").size(), "$finish;");
      }
      if (variant == 5) {
        auto guarded = fixture.find("sv.ifdef @RANDOM {} else { sv.macro.def @RANDOM \"$random\" }");
        fixture.replace(guarded, std::string("sv.ifdef @RANDOM {} else { sv.macro.def @RANDOM \"$random\" }").size(), "sv.macro.def @RANDOM \"$random\"");
      }
      if (variant == 6) fixture.replace(fixture.find("[@CUSTOM]"), 9,
          "[@CUSTOM, @RANDOM_INIT_FRAGMENT, @RANDOM_INIT_MEM_FRAGMENT]");
      if (variant == 7) fixture.insert(fixture.find("emit.fragment @RANDOM_INIT_FRAGMENT {") +
          std::string("emit.fragment @RANDOM_INIT_FRAGMENT {").size(), "sv.verbatim \"$finish;\"");
      if (variant == 8) {
        auto second = fixture.find("hw.module @MemoryTwo");
        auto begin = fixture.find("sv.initial", second);
        auto end = fixture.find("          }", begin);
        fixture.erase(begin, end - begin + std::string("          }").size());
      }
      if (variant == 9) {
        auto second = fixture.find("hw.module @MemoryTwo");
        auto guard = fixture.find("sv.ifdef.procedural @RANDOMIZE_MEM_INIT", second);
        fixture.replace(guard, std::string("sv.ifdef.procedural @RANDOMIZE_MEM_INIT").size(),
                        "sv.ifdef.procedural @SYNTHESIS");
      }
      if (variant >= 10 && variant <= 14) {
        auto second = fixture.find("hw.module @MemoryTwo");
        auto guard = fixture.find("sv.ifdef @ENABLE_INITIAL_MEM_ {", second) +
                     std::string("sv.ifdef @ENABLE_INITIAL_MEM_ {").size();
        fixture.insert(guard, R"mlir(
          sv.ifdef @RANDOMIZE_REG_INIT {
            %_RANDOM = sv.reg : !hw.inout<i32>
          })mlir");
        auto memoryInit =
            fixture.find("sv.ifdef.procedural @RANDOMIZE_MEM_INIT {}", guard) +
            std::string("sv.ifdef.procedural @RANDOMIZE_MEM_INIT {}").size();
        if (variant != 14)
          fixture.insert(memoryInit,
                         "sv.ifdef.procedural @RANDOMIZE_REG_INIT {}");
        if (variant == 11) {
          auto declarations =
              fixture.find("sv.ifdef @RANDOMIZE_REG_INIT", guard);
          fixture.replace(declarations,
                          std::string("sv.ifdef @RANDOMIZE_REG_INIT").size(),
                          "sv.ifdef @SYNTHESIS");
        }
        if (variant == 12) {
          auto registers =
              fixture.find("sv.ifdef.procedural @RANDOMIZE_REG_INIT", guard);
          fixture.replace(
              registers,
              std::string("sv.ifdef.procedural @RANDOMIZE_REG_INIT").size(),
              "sv.ifdef.procedural @SYNTHESIS");
        }
        if (variant == 13) {
          auto declaration = fixture.find("%_RANDOM = sv.reg", guard);
          fixture.insert(declaration, "sv.verbatim \"$finish;\"");
        }
      }
      if (variant == 15 || variant == 16) {
        auto start = fixture.find("emit.fragment @RANDOM_INIT_FRAGMENT {") +
                     std::string("emit.fragment @RANDOM_INIT_FRAGMENT {").size();
        // A partially prepared or unguarded activation must reject atomically.
        fixture.insert(start, variant == 15
            ? "sv.ifdef @RANDOMIZE {} else { sv.ifdef @RANDOMIZE_MEM_INIT { sv.macro.def @RANDOMIZE \"\" } }"
            : "sv.macro.def @RANDOMIZE \"\"");
      }
      if (variant == 17) {
        auto end = fixture.rfind("        }", fixture.find("hw.module @MemoryOne"));
        fixture.insert(end,
            "sv.ifdef @RANDOMIZE {} else { sv.ifdef @RANDOMIZE_MEM_INIT { sv.macro.def @RANDOMIZE \"\" } }\n"
            "sv.ifdef @RANDOMIZE {} else { sv.ifdef @RANDOMIZE_REG_INIT { sv.macro.def @RANDOMIZE \"\" } }\n");
      }
      auto parsed = parseSourceString<ModuleOp>(fixture, &context);
      require(bool(parsed), "memory fragment fixture parse");
      auto before = dump(*parsed);
      std::string error;
      bool accepted = variant == 0 || variant == 1 || variant == 10;
      // A custom fragment is retained verbatim, never inspected or relocated.
      require(succeeded(goldengate::attachMemoryInitializationFragments(
                  *parsed, error)) == accepted,
              "memory fragment validation accepted/rejected wrong shape");
      if (!accepted) {
        require(before == dump(*parsed), "rejected memory fragment changed IR");
        return;
      }
      auto modules = parsed->getOps<circt::hw::HWModuleOp>();
      auto first = *modules.begin();
      auto refs = first->getAttrOfType<ArrayAttr>("emit.fragments");
      require(refs && refs.size() == 3 && cast<FlatSymbolRefAttr>(refs[0]).getValue() == "CUSTOM" &&
              cast<FlatSymbolRefAttr>(refs[1]).getValue() == "RANDOM_INIT_MEM_FRAGMENT" &&
              cast<FlatSymbolRefAttr>(refs[2]).getValue() == "RANDOM_INIT_FRAGMENT",
              "memory fragment dependencies changed custom order");
      auto other = *std::next(modules.begin(), 2);
      require(!other->hasAttr("emit.fragments"), "unrelated module gained memory fragments");
      auto prepared = dump(*parsed);
      auto general = prepared.find("emit.fragment @RANDOM_INIT_FRAGMENT");
      auto randomDefault = prepared.find("sv.ifdef  @RANDOM ", general);
      require(prepared.find("sv.ifdef  @RANDOMIZE_MEM_INIT", general) < randomDefault &&
              prepared.find("sv.ifdef  @RANDOMIZE_REG_INIT", general) < randomDefault &&
              succeeded(verify(*parsed)),
              "general random prolog depends on a later module header");
      before = dump(*parsed);
      require(succeeded(goldengate::attachMemoryInitializationFragments(*parsed, error)) &&
              before == dump(*parsed), "memory fragment attachment is not idempotent");
    };
    for (unsigned variant = 0; variant != 18; ++variant) checkMemoryFragments(variant);
    llvm::outs() << "Memory fragments: outlined dependencies resolved before export; custom order preserved; unknown defaults, unresolved refs and later malformed guards reject atomically; idempotent\n";
    auto checkMemoryInit = [&](unsigned bits, bool dynamicRows,
                               unsigned chunkLimit, bool otherMacro,
                               unsigned expected, bool decrement = false,
                               unsigned lowBit = 0, bool extraUse = false,
                               bool wrongGuard = false,
                               unsigned temporaryBits = 0,
                               bool extractChunkIndex = false,
                               unsigned chunkIndexLowBit = 0) {
      std::string word = "i" + std::to_string(bits);
      bool narrow = bits <= 32;
      unsigned randomBits = narrow ? 32 : (temporaryBits ? temporaryBits : bits);
      std::string randomWord = "i" + std::to_string(randomBits);
      unsigned chunkIndexBits = llvm::Log2_64_Ceil(randomBits);
      bool extractWord = randomBits != bits;
      std::string source = "module { sv.macro.decl @RANDOM\n"
        "sv.macro.decl @OTHER\nsv.macro.decl @RANDOMIZE_MEM_INIT\n"
        "hw.module @MemoryInit(in %limit: i4) {\n"
        "%Memory = sv.reg : !hw.inout<uarray<8x" + word + ">>\n"
        "%_RANDOM_MEM = sv.reg : !hw.inout<" + randomWord + ">\n"
        "%zero = hw.constant 0 : i4\n%rows = hw.constant 8 : i4\n"
        "%one = hw.constant 1 : i4\n%start = hw.constant 0 : i10\n"
        "%bits = hw.constant " + std::to_string(chunkLimit) + " : i10\n"
        "%step = hw.constant 32 : i10\nsv.initial {\n"
        "sv.ifdef.procedural @" + (wrongGuard ? "OTHER" : "RANDOMIZE_MEM_INIT") + " {\n"
        "sv.for %i = %zero to " + (dynamicRows ? "%limit" : "%rows") +
        " step %one : i4 {\n" +
        (narrow ? "" : "sv.for %j = %start to %bits step %step : i10 {\n") +
        "%random = sv.macro.ref.se @" + (otherMacro ? "OTHER" : "RANDOM") +
        "() : () -> i32\n" +
        (narrow ? "sv.bpassign %_RANDOM_MEM, %random : i32\n" :
        (extractChunkIndex ? "%base = comb.extract %j from " +
          std::to_string(chunkIndexLowBit) + " : (i10) -> i" +
          std::to_string(chunkIndexBits) + "\n" : "") +
        std::string("%part = sv.indexed_part_select_inout %_RANDOM_MEM[") +
        (extractChunkIndex ? "%base " : "%j ") +
        (decrement ? "decrement " : "") + ": 32] : !hw.inout<" +
        randomWord + ">, i" + (extractChunkIndex ? std::to_string(chunkIndexBits) : "10") +
        "\nsv.bpassign %part, %random : i32\n}\n") +
        "%index = comb.extract %i from 0 : (i4) -> i3\n"
        "%element = sv.array_index_inout %Memory[%index] : !hw.inout<uarray<8x" +
        word + ">>, i3\n%value = sv.read_inout %_RANDOM_MEM : !hw.inout<" +
        randomWord + ">\n" +
        (extractWord ? "%low = comb.extract %value from " +
          std::to_string(lowBit) + " : (" + randomWord + ") -> " + word + "\n" : "") +
        "sv.bpassign %element, " + (extractWord ? "%low" : "%value") +
        " : " + word + "\n" +
        (extraUse ? "%extra = sv.read_inout %_RANDOM_MEM : !hw.inout<i32>\n" : "") +
        "}\n}\n}\n"
        "hw.output\n} }";
      auto memory = parseSourceString<ModuleOp>(source, &context);
      require(bool(memory), "memory initializer fixture parse");
      auto before = dump(*memory);
      SmallVector<circt::sv::ForOp> loops;
      memory->walk<WalkOrder::PreOrder>([&](circt::sv::ForOp op) { loops.push_back(op); });
      auto rows = loops[0];
      auto chunks = narrow ? rows : loops[1];
      SmallVector<Value> rowBounds(rows->getOperands()), chunkBounds(chunks->getOperands());
      require(goldengate::normalizeMemoryInitialization(*memory) == expected,
              "memory initializer accepted/rejected the wrong pattern");
      require(succeeded(verify(*memory)) &&
              llvm::equal(rowBounds, rows->getOperands()) &&
              llvm::equal(chunkBounds, chunks->getOperands()),
              "memory initialization changed bounds or produced invalid IR");
      if (expected) {
        circt::sv::MacroRefExprSEOp random;
        memory->walk([&](circt::sv::MacroRefExprSEOp op) { random = op; });
        require((narrow || (chunks->getBlock() == rows->getBlock() &&
                chunks->isBeforeInBlock(rows))) &&
                random->getBlock() == rows->getBlock() &&
                random->isBeforeInBlock(chunks) && random.getResult().hasOneUse(),
                "random draw or chunk fill remains inside a row/chunk loop");
        auto assignment = dyn_cast<circt::sv::BPAssignOp>(*random.getResult().getUsers().begin());
        require(assignment && assignment.getDest().getDefiningOp<circt::sv::RegOp>() &&
                assignment.getDest().getType().getElementType().getIntOrFloatBitWidth() == 32,
                "random draw is not captured once in a 32-bit seed");
      } else {
        require(before == dump(*memory), "unrecognized memory initialization changed");
      }
      require(goldengate::normalizeMemoryInitialization(*memory) == 0,
              "memory initialization normalization is not idempotent");
    };
    checkMemoryInit(64, false, 64, false, 1);
    checkMemoryInit(33, false, 33, false, 1);
    checkMemoryInit(512, false, 512, false, 1);
    checkMemoryInit(33, false, 64, false, 1, false, 0, false, false, 64, true);
    checkMemoryInit(35, false, 64, false, 1, false, 0, false, false, 64, true);
    checkMemoryInit(65, false, 96, false, 1, false, 0, false, false, 96);
    checkMemoryInit(531, false, 544, false, 1, false, 0, false, false, 544);
    checkMemoryInit(35, false, 64, false, 0, false, 1, false, false, 64, true);
    checkMemoryInit(35, false, 64, false, 0, false, 0, false, false, 64, true, 1);
    checkMemoryInit(35, false, 96, false, 0, false, 0, false, false, 96);
    checkMemoryInit(65, false, 64, false, 0, false, 0, false, false, 96);
    checkMemoryInit(64, true, 64, false, 0);
    checkMemoryInit(64, false, 32, false, 0);
    checkMemoryInit(64, false, 64, true, 0);
    checkMemoryInit(64, false, 64, false, 0, true);
    checkMemoryInit(1, false, 1, false, 1);
    checkMemoryInit(3, false, 3, false, 1);
    checkMemoryInit(32, false, 32, false, 1);
    checkMemoryInit(3, true, 3, false, 0);
    checkMemoryInit(3, false, 3, true, 0);
    checkMemoryInit(3, false, 3, false, 0, false, 1);
    checkMemoryInit(3, false, 3, false, 0, false, 0, true);
    checkMemoryInit(3, false, 3, false, 0, false, 0, false, true);
    llvm::outs() << "Memory initialization: one guarded seed shared across chunks/rows, including partial words; dynamic bounds, partial fills and other macros unchanged\n";
    auto checkSnapshots = [&](unsigned variant, bool accepted) {
      bool narrow = variant == 1;
      std::string source = "module { sv.macro.decl @RANDOM\nsv.macro.decl @OTHER\n"
        "sv.macro.decl @RANDOMIZE_REG_INIT\n"
        "hw.module @Snapshots() {\n"
        "%hCycle_mmreg = sv.reg : !hw.inout<i64>\n"
        "%tCycle_mmreg = sv.reg : !hw.inout<i" + std::string(narrow ? "32" : "64") + ">\n"
        "%unmarked = sv.reg : !hw.inout<i64>\n"
        "%c0 = hw.constant 0 : i3\n%c1 = hw.constant 1 : i3\n%c4 = hw.constant " +
        std::string(variant == 5 ? "3" : "4") + " : i3\n"
        "%i0 = hw.constant 0 : i2\n%i1 = hw.constant 1 : i2\n"
        "sv.initial { sv.ifdef.procedural @" +
        std::string(variant == 2 ? "OTHER" : "RANDOMIZE_REG_INIT") + " {\n"
        "%_RANDOM = sv.logic : !hw.inout<uarray<4xi32>>\n"
        "sv.for %i = %c0 to %c4 step %c1 : i3 {\n"
        "%r = sv.macro.ref.se @" + std::string(variant == 4 ? "OTHER" : "RANDOM") + "() : () -> i32\n"
        "%ix = comb.extract %i from 0 : (i3) -> i2\n"
        "%dst = sv.array_index_inout %_RANDOM[%ix] : !hw.inout<uarray<4xi32>>, i2\n"
        "sv.bpassign %dst, %r : i32\n}\n"
        "%p0 = sv.array_index_inout %_RANDOM[%i0] : !hw.inout<uarray<4xi32>>, i2\n"
        "%p1 = sv.array_index_inout %_RANDOM[%i1] : !hw.inout<uarray<4xi32>>, i2\n"
        "%v0 = sv.read_inout %p0 : !hw.inout<i32>\n"
        "%v1 = sv.read_inout %p1 : !hw.inout<i32>\n"
        "%packed = comb.concat %v0, %v1 : i32, i32\n"
        "sv.bpassign %hCycle_mmreg, %packed : i64\n"
        "sv.bpassign %tCycle_mmreg, " + std::string(narrow ? "%v0 : i32\n" : "%packed : i64\n") +
        (variant == 3 ? "sv.bpassign %tCycle_mmreg, %packed : i64\n" : "") +
        (variant == 8 ? "%partial = sv.indexed_part_select_inout %hCycle_mmreg[%i0: 32] : !hw.inout<i64>, i2\nsv.bpassign %partial, %v0 : i32\n" : "") +
        "sv.bpassign %unmarked, %packed : i64\n"
        "}\n}\nhw.output\n} }";
      auto fixture = parseSourceString<ModuleOp>(source, &context);
      require(bool(fixture), "ClockBridge initialization fixture parse");
      auto before = dump(*fixture);
      SmallVector<goldengate::ClockSnapshotIdentity> identities{
          {"Snapshots", "hCycle_mmreg"}, {"Snapshots", "tCycle_mmreg"}};
      if (variant == 6) identities[1].reg = "missing";
      if (variant == 7) identities.push_back(identities[0]);
      unsigned changed = 99;
      std::string error;
      auto result = goldengate::normalizeClockSnapshotInitialization(*fixture, identities, changed, error);
      require(succeeded(result) == accepted, "ClockBridge initialization accepted/rejected wrong shape: " + error);
      if (!accepted) {
        require(changed == 0 && before == dump(*fixture), "rejected ClockBridge initializer mutated IR");
        return;
      }
      require(changed == 2 && succeeded(verify(*fixture)), "ClockBridge initialization did not normalize both snapshots");
      unsigned captures = 0, snapshots = 0;
      Value unmarkedSource;
      fixture->walk([&](circt::sv::BPAssignOp assign) {
        auto reg = assign.getDest().getDefiningOp<circt::sv::RegOp>();
        if (!reg) return;
        if (reg.getName() == "unmarked") unmarkedSource = assign.getSrc();
        if (assign.getSrc().getDefiningOp<circt::sv::MacroRefExprSEOp>()) ++captures;
        if (!assign->hasAttr("goldengate.clockSnapshotInitNormalized")) return;
        auto word = assign.getSrc().getDefiningOp<circt::comb::ConcatOp>();
        require(word && word.getNumOperands() == 2 && word.getOperand(0) == word.getOperand(1),
                "ClockBridge snapshot does not duplicate one captured random word");
        ++snapshots;
      });
      require(captures == 2 && snapshots == 2 && unmarkedSource &&
              !unmarkedSource.getDefiningOp<circt::comb::ConcatOp>().getOperand(0)
                   .getDefiningOp<circt::sv::ReadInOutOp>().getInput()
                   .getDefiningOp<circt::sv::RegOp>(),
              "ClockBridge initialization changed unmarked state or merged snapshot draws");
      before = dump(*fixture);
      require(succeeded(goldengate::normalizeClockSnapshotInitialization(*fixture, identities, changed, error)) &&
              changed == 0 && before == dump(*fixture), "ClockBridge initialization is not idempotent");
    };
    checkSnapshots(0, true);
    for (unsigned variant = 1; variant <= 8; ++variant) checkSnapshots(variant, false);
    llvm::outs() << "ClockBridge snapshots: two independent captured 32-bit random seeds; unmarked state unchanged; unknown types/guards/fills/identities reject atomically; idempotent\n";
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
    for (unsigned readLatency : {0u, 1u, 2u}) {
      for (unsigned writeLatency : {1u, 2u}) {
        for (bool aggregateReader : {false, true}) {
          if (aggregateReader && (readLatency != 0 || writeLatency != 1))
            continue;
          for (auto policy : {RUWAttr::Undefined, RUWAttr::Old, RUWAttr::New}) {
            if (policy != RUWAttr::Undefined &&
                (readLatency != 1 || writeLatency != 1 || aggregateReader))
              continue;
            for (bool signedData : {false, true}) {
              if (signedData && (readLatency != 1 || writeLatency != 1 || aggregateReader))
                continue;
              std::string fixtureSource = R"mlir(module {
          firrtl.circuit "MemoryInit65" {
            firrtl.module @MemoryInit65(in %clock: !firrtl.clock, in %wen: !firrtl.uint<1>, in %waddr: !firrtl.uint<5>, in %wdata: !firrtl.uint<65>, in %raddr: !firrtl.uint<5>, in %ren: !firrtl.uint<1>, out %rdata: !firrtl.uint<65>) {}
          }
        })mlir";
              if (signedData)
                for (size_t pos = 0; (pos = fixtureSource.find("uint<65>", pos)) != std::string::npos; pos += 8)
                  fixtureSource.replace(pos, 8, "sint<65>");
              auto memoryFixture = parseSourceString<ModuleOp>(fixtureSource, &context);
              require(bool(memoryFixture), "padded memory FIRRTL fixture parse");
              auto memoryCircuit = *memoryFixture->getOps<CircuitOp>().begin();
              auto memoryModule = *memoryCircuit.getOps<FModuleOp>().begin();
              OpBuilder memoryBuilder(memoryModule.getBodyBlock(),
                                      memoryModule.getBodyBlock()->end());
              auto memoryLoc = memoryModule.getLoc();
              FIRRTLBaseType dataType = signedData ? FIRRTLBaseType(SIntType::get(&context, 65))
                                                    : FIRRTLBaseType(UIntType::get(&context, 65));
              SmallVector<Type> memoryTypes{
                  MemOp::getTypeForPort(32, dataType, MemOp::PortKind::Read),
                  MemOp::getTypeForPort(32, dataType, MemOp::PortKind::Write)};
              SmallVector<Attribute> memoryNames{memoryBuilder.getStringAttr("R0"),
                                                 memoryBuilder.getStringAttr("W0")};
              auto dataMemory = memoryBuilder.create<MemOp>(memoryLoc,
                                                            memoryTypes,
                                                            readLatency,
                                                            writeLatency,
                                                            32,
                                                            policy,
                                                            memoryNames,
                                                            "data");
              auto one = memoryBuilder.create<ConstantOp>(
                  memoryLoc, UIntType::get(&context, 1), APInt(1, 1));
              auto memoryField = [&](Value port, StringRef name) -> Value {
                return memoryBuilder.create<SubfieldOp>(memoryLoc, port, name);
              };
              auto memoryConnect = [&](Value dest, Value src) {
                memoryBuilder.create<StrictConnectOp>(memoryLoc, dest, src);
              };
              auto reader = dataMemory.getResult(0), writer = dataMemory.getResult(1);
              if (aggregateReader) {
                // Retain the normal backend when an aggregate drives the port; a
                // leaf-only rewrite must never add a competing enable driver.
                auto proxy = memoryBuilder.create<WireOp>(memoryLoc, memoryTypes[0]);
                memoryBuilder.create<ConnectOp>(memoryLoc, reader, proxy.getResult());
                reader = proxy.getResult();
              }
              memoryConnect(memoryField(reader, "clk"), memoryModule.getArgument(0));
              memoryConnect(memoryField(reader, "en"), memoryModule.getArgument(5));
              memoryConnect(memoryField(reader, "addr"), memoryModule.getArgument(4));
              memoryConnect(memoryModule.getArgument(6), memoryField(reader, "data"));
              memoryConnect(memoryField(writer, "clk"), memoryModule.getArgument(0));
              memoryConnect(memoryField(writer, "en"), memoryModule.getArgument(1));
              memoryConnect(memoryField(writer, "addr"), memoryModule.getArgument(2));
              memoryConnect(memoryField(writer, "data"), memoryModule.getArgument(3));
              memoryConnect(memoryField(writer, "mask"), one);
              auto memoryBefore = dump(*memoryFixture);
              auto memoryPath = std::string(argv[2]) + ".memory65.sv";
              if (readLatency != 0 || writeLatency != 1)
                memoryPath = std::string(argv[2]) + ".memory65.r" +
                             std::to_string(readLatency) + ".w" +
                             std::to_string(writeLatency) + ".sv";
              if (aggregateReader)
                memoryPath = std::string(argv[2]) + ".memory65.aggregate.sv";
              if (policy != RUWAttr::Undefined)
                memoryPath += policy == RUWAttr::Old ? ".old.sv" : ".new.sv";
              if (signedData) memoryPath += ".signed.sv";
              require(succeeded(goldengate::emitSimulatorRTL(
                          *memoryFixture, "", memoryPath, error)),
                      error);
              auto memoryRTL = read(memoryPath);
              if (readLatency == 0 && !aggregateReader)
                require(memoryRTL.find(".R0_en   (1'h1)") !=
                            std::string::npos,
                        "asynchronous reader is not permanently enabled for SFC compatibility");
              else if (readLatency == 1 && writeLatency == 1 && policy != RUWAttr::New)
                require(memoryRTL.find("data_R0_rdata") != std::string::npos &&
                            memoryRTL.find("_R0_addr_d0") == std::string::npos &&
                            memoryRTL.find(".R0_en   (1'h1)") != std::string::npos,
                        "independent synchronous reader does not capture/hold SFC data");
              else if (readLatency != 0)
                require(memoryRTL.find(" : 65'bx;") != std::string::npos &&
                            memoryRTL.find("_R0_addr_d0 <= R0_addr;") !=
                                std::string::npos,
                        "asynchronous normalization changed synchronous read lowering");
              else
                require(memoryRTL.find(".R0_en   (ren)") != std::string::npos,
                        "leaf normalization changed a whole-port aggregate driver");
              auto firstModule = memoryRTL.find("module data_32x65");
              require(
                  firstModule != std::string::npos &&
                      memoryRTL.find("`define ENABLE_INITIAL_MEM_") < firstModule &&
                      memoryRTL.find("`define RANDOM ") < firstModule &&
                      memoryRTL.find("`define INIT_RANDOM_PROLOG_") < firstModule,
                  "outlined memory was exported before its initialization defaults");
              auto seedPosition = memoryRTL.find("_RANDOM_MEM_SEED = `RANDOM");
              auto chunkPosition = memoryRTL.find("for (logic", seedPosition);
              auto rowPosition = memoryRTL.find("for (logic", chunkPosition + 1);
              require(memoryBefore == dump(*memoryFixture) &&
                          seedPosition != std::string::npos &&
                          chunkPosition != std::string::npos &&
                          rowPosition != std::string::npos &&
                          memoryRTL.find("_RANDOM_MEM[64:0]", rowPosition) !=
                              std::string::npos &&
                          memoryRTL.find("= _RANDOM_MEM_SEED", chunkPosition) <
                              rowPosition,
                      "real FIRRTL padded initializer did not share one seed across "
                      "chunks and rows");
              if (readLatency != 0 || writeLatency != 1)
                require(memoryRTL.find("`ifdef RANDOMIZE_REG_INIT", firstModule) !=
                            std::string::npos,
                        "outlined memory lost pipeline register initialization");
            }
          }
        }
      }
    }
    llvm::outs()
        << "Padded memory backend fixtures: real 32x65 FIRRTL lowering at read "
           "latencies 0/1/2 and write latencies 1/2 retains memory/pipeline "
           "guards, live ports and shared memory seed; asynchronous disabled "
           "reads use the SFC array value; "
           "one-cycle independent readers capture/hold data; longer pipelines "
           "and source unchanged\n";
    for (StringRef variant : {"undefined", "old", "inferred", "new", "read2", "write2",
                              "aggregate", "wide-mask", "annotated", "port-annotated",
                              "symbol", "signed-undefined", "signed-old", "signed-inferred", "signed-new"}) {
      auto variantName = variant.str();
      bool signedData = variant.consume_front("signed-");
      std::string combinedSource = R"mlir(module {
        firrtl.circuit "CombinedMemory" {
          firrtl.module @CombinedMemory(in %clock: !firrtl.clock, in %en: !firrtl.uint<1>, in %write: !firrtl.uint<1>, in %addr: !firrtl.uint<10>, in %wdata: !firrtl.uint<23>, out %rdata: !firrtl.uint<23>, in %mask: !firrtl.uint<1>) {}
        }
      })mlir";
      if (signedData)
        for (size_t pos = 0; (pos = combinedSource.find("uint<23>", pos)) != std::string::npos; pos += 8)
          combinedSource.replace(pos, 8, "sint<23>");
      auto combined = parseSourceString<ModuleOp>(combinedSource, &context);
      require(bool(combined), "combined read/write memory fixture parse");
      auto combinedCircuit = *combined->getOps<CircuitOp>().begin();
      auto combinedModule = *combinedCircuit.getOps<FModuleOp>().begin();
      OpBuilder combinedBuilder(combinedModule.getBodyBlock(),
                                combinedModule.getBodyBlock()->end());
      auto combinedLoc = combinedModule.getLoc();
      FIRRTLBaseType combinedDataType = signedData ? FIRRTLBaseType(SIntType::get(&context, 23))
                                                   : FIRRTLBaseType(UIntType::get(&context, 23));
      if (variant == "inferred") {
        combinedModule.insertPorts({{7, PortInfo(
            combinedBuilder.getStringAttr("prior"), UIntType::get(&context, 24), Direction::Out)}});
        auto prefix = combinedBuilder.create<RegOp>(combinedLoc,
            combinedDataType, combinedModule.getArgument(0), "prefix");
        auto suffix = combinedBuilder.create<RegOp>(combinedLoc,
            UIntType::get(&context, 1), combinedModule.getArgument(0), "suffix");
        combinedBuilder.create<StrictConnectOp>(combinedLoc, prefix.getResult(),
            combinedModule.getArgument(4));
        combinedBuilder.create<StrictConnectOp>(combinedLoc, suffix.getResult(),
            combinedModule.getArgument(1));
        Value prefixBits = prefix.getResult();
        if (signedData)
          prefixBits = combinedBuilder.create<AsUIntPrimOp>(combinedLoc, prefixBits);
        combinedBuilder.create<StrictConnectOp>(combinedLoc, combinedModule.getArgument(7),
            combinedBuilder.create<CatPrimOp>(combinedLoc, prefixBits, suffix.getResult()));
      }
      SmallVector<Type> combinedTypes{MemOp::getTypeForPort(
          1024, combinedDataType, MemOp::PortKind::ReadWrite,
          variant == "wide-mask" ? 23 : 1)};
      SmallVector<Attribute> combinedNames{combinedBuilder.getStringAttr("RW0")};
      if (variant == "inferred") {
        // Directory reaches this backend as separate read/write ports, not
        // the combined port tested above. Firtool infers "rw" only later.
        combinedTypes = {
            MemOp::getTypeForPort(1024, combinedDataType, MemOp::PortKind::Read),
            MemOp::getTypeForPort(1024, combinedDataType, MemOp::PortKind::Write)};
        combinedNames = {combinedBuilder.getStringAttr("regout"),
                         combinedBuilder.getStringAttr("MPORT")};
      }
      auto combinedMemory = combinedBuilder.create<MemOp>(combinedLoc,
                                                          combinedTypes,
                                                          variant == "read2" ? 2 : 1,
                                                          variant == "write2" ? 2 : 1,
                                                          1024,
                                                          variant == "new" ? RUWAttr::New :
                                                          variant == "old" ? RUWAttr::Old : RUWAttr::Undefined,
                                                          combinedNames,
                                                          "cc_dir");
      auto dontTouch = combinedBuilder.getDictionaryAttr({
          combinedBuilder.getNamedAttr("class", combinedBuilder.getStringAttr(
              "firrtl.transforms.DontTouchAnnotation"))});
      if (variant == "annotated")
        combinedMemory.setAnnotationsAttr(combinedBuilder.getArrayAttr({dontTouch}));
      if (variant == "port-annotated")
        combinedMemory.setAllPortAnnotations({combinedBuilder.getArrayAttr({dontTouch})});
      if (variant == "symbol")
        combinedMemory.setInnerSymAttr(circt::hw::InnerSymAttr::get(
            combinedBuilder.getStringAttr("memory_symbol")));
      Value combinedPort = combinedMemory.getResult(0);
      if (variant == "aggregate") {
        auto proxy = combinedBuilder.create<WireOp>(combinedLoc, combinedTypes[0]);
        combinedBuilder.create<ConnectOp>(combinedLoc, combinedPort, proxy.getResult());
        combinedPort = proxy.getResult();
      }
      auto combinedField = [&](StringRef name) -> Value {
        return combinedBuilder.create<SubfieldOp>(
            combinedLoc, combinedPort, name);
      };
      if (variant == "inferred") {
        auto writer = combinedMemory.getResult(1);
        auto write = combinedModule.getArgument(2);
        auto notWrite = combinedBuilder.create<NotPrimOp>(combinedLoc, write);
        auto readEnable = combinedBuilder.create<AndPrimOp>(combinedLoc,
            combinedModule.getArgument(1), notWrite);
        auto writeEnable = combinedBuilder.create<AndPrimOp>(combinedLoc,
            combinedModule.getArgument(1), write);
        auto connectField = [&](Value port, StringRef name, Value value) {
          combinedBuilder.create<StrictConnectOp>(combinedLoc,
              combinedBuilder.create<SubfieldOp>(combinedLoc, port, name), value);
        };
        for (auto port : {combinedPort, writer}) {
          connectField(port, "clk", combinedModule.getArgument(0));
          connectField(port, "addr", combinedModule.getArgument(3));
        }
        connectField(combinedPort, "en", readEnable);
        connectField(writer, "en", writeEnable);
        connectField(writer, "data", combinedModule.getArgument(4));
        connectField(writer, "mask", combinedModule.getArgument(6));
        combinedBuilder.create<StrictConnectOp>(combinedLoc,
            combinedModule.getArgument(5), combinedField("data"));
      } else {
        for (auto [field, argument] : {std::pair<StringRef, unsigned>{"clk", 0},
                                       {"en", 1},
                                       {"wmode", 2},
                                       {"addr", 3},
                                       {"wdata", 4}})
          combinedBuilder.create<StrictConnectOp>(
              combinedLoc,
              combinedField(field),
              combinedModule.getArgument(argument));
        Value mask = combinedModule.getArgument(6);
        if (variant == "wide-mask")
          mask = combinedBuilder.create<ConstantOp>(combinedLoc,
              UIntType::get(&context, 23), APInt::getAllOnes(23));
        combinedBuilder.create<StrictConnectOp>(
            combinedLoc, combinedField("wmask"), mask);
        combinedBuilder.create<StrictConnectOp>(
            combinedLoc, combinedModule.getArgument(5), combinedField("rdata"));
      }
      auto combinedBefore = dump(*combined);
      auto combinedPath = std::string(argv[2]) + ".combined.sv";
      if (variantName != "undefined")
        combinedPath = std::string(argv[2]) + ".combined." + variantName + ".sv";
      require(succeeded(goldengate::emitSimulatorRTL(
                  *combined, "", combinedPath, error)),
              error);
      auto combinedRTL = read(combinedPath);
      auto combinedPosition = combinedRTL.find("module cc_dir_1024x23");
      bool normalized = variant == "undefined" || variant == "old" || variant == "inferred";
      require(
          combinedPosition != std::string::npos &&
              combinedRTL.find("`define ENABLE_INITIAL_MEM_") <
                  combinedPosition &&
              combinedBefore == dump(*combined),
          "actual combined read/write memory geometry lost guarded "
          "initialization or changed source");
      if (normalized) {
        auto regName = variant == "inferred" ? "cc_dir_rw_rdata" : "cc_dir_RW0_rdata";
        require(combinedRTL.find(std::string("[22:0] ") + regName + ";") != std::string::npos &&
                    combinedRTL.find("if (") != std::string::npos &&
                    combinedRTL.find(std::string(regName) + " <= ") != std::string::npos &&
                    (combinedRTL.find(std::string(regName) + " = _RANDOM") != std::string::npos ||
                     combinedRTL.find(std::string(regName) + " = {_RANDOM") != std::string::npos) &&
                    combinedRTL.find("_RW0_raddr_d0") == std::string::npos,
                "synchronous RW port did not capture enabled old data with guarded initialization");
        if (variant == "inferred")
          require(combinedRTL.find("prefix = _RANDOM[1'h0][22:0]") != std::string::npos &&
                      combinedRTL.find("suffix = _RANDOM[1'h0][23]") != std::string::npos &&
                      combinedRTL.find("_RANDOM[1'h1][14:0]") != std::string::npos,
                  "late data register overlaps or renumbers existing random initialization slots");
      } else if (variant == "write2")
        require(combinedRTL.find("_RW0_addr_d0 = _RANDOM") != std::string::npos &&
                    combinedRTL.find("_RW0_en_d0 = _RANDOM") != std::string::npos &&
                    combinedRTL.find("_RW0_mode_d0 = _RANDOM") != std::string::npos,
                "longer-write-latency RW port changed its original shared pipeline");
      else
        require(combinedRTL.find("_RW0_raddr_d0 = _RANDOM") != std::string::npos &&
                    combinedRTL.find("_RW0_ren_d0 = _RANDOM") != std::string::npos &&
                    combinedRTL.find("_RW0_rmode_d0 = _RANDOM") != std::string::npos,
                "unsupported RW port shape changed its original pipeline lowering");
    }
    llvm::outs()
        << "Combined read/write backend fixtures: explicit/inferred 1024x23 undefined/old RUW "
           "capture enabled data; new RUW, longer latencies, aggregate uses, wide masks, "
           "annotations and symbols retain existing pipelines; source IR preserved\n";
    auto snapshots = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "SnapshotFixture" {
        firrtl.module @SnapshotFixture(in %clock: !firrtl.clock, in %latch: !firrtl.uint<1>, in %small: !firrtl.uint<1>, in %hCycle: !firrtl.uint<64>, in %tCycle: !firrtl.uint<64>, out %smallOut: !firrtl.uint<1>, out %h: !firrtl.uint<64>, out %t: !firrtl.uint<64>, out %other: !firrtl.uint<64>) {
          %preceding = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<1>
          firrtl.strictconnect %preceding, %small : !firrtl.uint<1>
          firrtl.strictconnect %smallOut, %preceding : !firrtl.uint<1>
          %hCycle_mmreg = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<64>
          %tCycle_mmreg = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<64>
          %unmarked_mmreg = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<64>
          %nextH = firrtl.mux(%latch, %hCycle, %hCycle_mmreg) : (!firrtl.uint<1>, !firrtl.uint<64>, !firrtl.uint<64>) -> !firrtl.uint<64>
          %nextT = firrtl.mux(%latch, %tCycle, %tCycle_mmreg) : (!firrtl.uint<1>, !firrtl.uint<64>, !firrtl.uint<64>) -> !firrtl.uint<64>
          firrtl.strictconnect %hCycle_mmreg, %nextH : !firrtl.uint<64>
          firrtl.strictconnect %tCycle_mmreg, %nextT : !firrtl.uint<64>
          firrtl.strictconnect %unmarked_mmreg, %hCycle : !firrtl.uint<64>
          firrtl.strictconnect %h, %hCycle_mmreg : !firrtl.uint<64>
          firrtl.strictconnect %t, %tCycle_mmreg : !firrtl.uint<64>
          firrtl.strictconnect %other, %unmarked_mmreg : !firrtl.uint<64>
        }
      }
    })mlir",
                                                 &context);
    require(bool(snapshots), "marked FIRRTL snapshot fixture parse");
    snapshots->walk([&](RegOp reg) {
      if (reg.getName() == "hCycle_mmreg" || reg.getName() == "tCycle_mmreg")
        reg->setAttr("goldengate.clockSnapshot", UnitAttr::get(&context));
    });
    auto snapshotBefore = dump(*snapshots);
    auto snapshotPath = unitPath + ".snapshots.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*snapshots, "", snapshotPath, error)), error);
    auto snapshotRTL = read(snapshotPath);
    require(snapshotBefore == dump(*snapshots) &&
            llvm::Regex("hCycle_mmreg = \\{hCycle_mmreg_RANDOM_INIT, hCycle_mmreg_RANDOM_INIT\\}").match(snapshotRTL) &&
            llvm::Regex("tCycle_mmreg = \\{tCycle_mmreg_RANDOM_INIT, tCycle_mmreg_RANDOM_INIT\\}").match(snapshotRTL) &&
            llvm::Regex("unmarked_mmreg = \\{_RANDOM\\[").match(snapshotRTL),
            "backend did not preserve marked snapshot identities and unmarked packed initialization");
    OwningOpRef<ModuleOp> unmarkedSnapshots = cast<ModuleOp>(snapshots->clone());
    unmarkedSnapshots->walk([&](RegOp reg) { reg->removeAttr("goldengate.clockSnapshot"); });
    auto unmarkedPath = snapshotPath + ".unmarked.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*unmarkedSnapshots, "", unmarkedPath, error)), error);
    auto unmarkedRTL = read(unmarkedPath);
    require(llvm::Regex("hCycle_mmreg = \\{_RANDOM\\[").match(unmarkedRTL) &&
            llvm::Regex("tCycle_mmreg = \\{_RANDOM\\[").match(unmarkedRTL),
            "unmarked negative control did not retain standard packed initialization");
    snapshots->walk([&](RegOp reg) {
      if (reg.getName() == "tCycle_mmreg") reg->removeAttr("goldengate.clockSnapshot");
    });
    auto invalidBefore = dump(*snapshots);
    require(failed(goldengate::emitSimulatorRTL(*snapshots, "", snapshotPath, error)) &&
            invalidBefore == dump(*snapshots) && read(snapshotPath) == snapshotRTL,
            "incomplete snapshot marks replaced source or emitted RTL");
    llvm::outs() << "Snapshot backend fixture: explicit FIRRTL markers survive identity capture, non-word-aligned packed lowering normalized only for marked snapshots; incomplete marks leave existing output intact\n";
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
    auto unusedOutput = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "UnusedOutput" {
        firrtl.extmodule @Peripheral(in request: !firrtl.uint<1>, out live: !firrtl.uint<8>, out unused: !firrtl.uint<8>)
        firrtl.module @UnusedOutput(in %request: !firrtl.uint<1>, out %response: !firrtl.uint<8>) {
          %device:3 = firrtl.instance device @Peripheral(in request: !firrtl.uint<1>, out live: !firrtl.uint<8>, out unused: !firrtl.uint<8>)
          firrtl.strictconnect %device#0, %request : !firrtl.uint<1>
          firrtl.strictconnect %response, %device#1 : !firrtl.uint<8>
        }
      }
    })mlir", &context);
    require(bool(unusedOutput), "unused output fixture parse");
    before = dump(*unusedOutput);
    auto unusedPath = unitPath + ".unused.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*unusedOutput, "", unusedPath, error)), error);
    auto unusedRTL = read(unusedPath);
    require(before == dump(*unusedOutput) &&
            unusedRTL.find("wire [7:0] device_unused_unused;") != std::string::npos &&
            llvm::Regex("\\.unused[[:space:]]*\\(device_unused_unused\\)").match(unusedRTL) &&
            llvm::Regex("\\.request[[:space:]]*\\(request\\)").match(unusedRTL) &&
            llvm::Regex("\\.live[[:space:]]*\\(response\\)").match(unusedRTL),
            "unused output sink changed live connectivity or source IR");
    llvm::outs() << "Unused instance outputs terminate in width-declared wires; live inputs/outputs and source IR unchanged\n";
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
    auto orIndex = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "OrIndex" {
        firrtl.module @OrIndex(in %data: !firrtl.vector<uint<1>, 32>, in %a: !firrtl.uint<3>, in %b: !firrtl.uint<3>, out %bit: !firrtl.uint<1>) {
          %index = firrtl.or %a, %b : (!firrtl.uint<3>, !firrtl.uint<3>) -> !firrtl.uint<3>
          %low = firrtl.subaccess %data[%index] : !firrtl.vector<uint<1>, 32>, !firrtl.uint<3>
          firrtl.strictconnect %bit, %low : !firrtl.uint<1>
        }
      }
    })mlir", &context);
    require(bool(orIndex), "OR index fixture parse");
    before = dump(*orIndex);
    auto orPath = unitPath + ".or.sv";
    require(succeeded(goldengate::emitSimulatorRTL(*orIndex, "", orPath, error)), error);
    auto orRTL = read(orPath);
    auto expression = orRTL.find("{2'h0, a | b}");
    require(expression != std::string::npos &&
            orRTL.rfind("wire [4:0]", expression) != std::string::npos &&
            orRTL.find("_GEN[a | b]") == std::string::npos &&
            before == dump(*orIndex),
            "OR index lost its three-bit arithmetic/explicit zero extension or changed source IR");
    llvm::outs() << "OR index preserves three-bit arithmetic inside explicit zero extension to a five-bit index wire\n";
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
    // The mark belongs to native-generated packing, not to a text-name guess.
    for (unsigned depth : {128u, 256u, 512u}) {
      unsigned dataBits = depth == 512 ? 256 : 64;
      auto originFixture = parseSourceString<ModuleOp>(R"mlir(module {
        firrtl.circuit "OriginTest" {
          firrtl.module @OriginTest(in %clock: !firrtl.clock, in %wen: !firrtl.uint<1>, in %waddr: !firrtl.uint<8>, in %raddr: !firrtl.uint<8>, in %enq_bits: !firrtl.bundle<data: uint<64>, last: uint<1>>, out %deq_bits: !firrtl.bundle<data: uint<64>, last: uint<1>>) {}
        }
      })mlir", &context);
      require(bool(originFixture), "native memory origin fixture parse");
      auto originCircuit = *originFixture->getOps<CircuitOp>().begin();
      auto originModule = *originCircuit.getOps<FModuleOp>().begin();
      OpBuilder b(originModule.getBodyBlock(), originModule.getBodyBlock()->end());
      auto loc = originModule.getLoc();
      auto uint = [&](unsigned width) { return UIntType::get(&context, width); };
      auto sf = [&](Value value, StringRef name) -> Value {
        return b.create<SubfieldOp>(loc, value, name);
      };
      auto connect = [&](Value dest, Value src) {
        b.create<StrictConnectOp>(loc, dest, src);
      };
      if (dataBits != 64) {
        auto payload =
            BundleType::get(&context,
                            {{b.getStringAttr("data"), false, uint(dataBits)},
                             {b.getStringAttr("last"), false, uint(1)}});
        SmallVector<Attribute> portTypes(
            originModule.getPortTypesAttr().begin(),
            originModule.getPortTypesAttr().end());
        for (unsigned i : {2u, 3u, 4u, 5u}) {
          Type type = i < 4 ? Type(uint(9)) : Type(payload);
          portTypes[i] = TypeAttr::get(type);
          originModule.getArgument(i).setType(type);
        }
        originModule->setAttr("portTypes", b.getArrayAttr(portTypes));
      }
      SmallVector<Type> types{
          MemOp::getTypeForPort(
              depth, uint(dataBits + 1), MemOp::PortKind::Read),
          MemOp::getTypeForPort(
              depth, uint(dataBits + 1), MemOp::PortKind::Write)};
      SmallVector<Attribute> names{b.getStringAttr("read"), b.getStringAttr("write")};
      auto memory = b.create<MemOp>(loc, types, 1, 1, depth, RUWAttr::Undefined,
                                    names, "ram");
      memory->setAttr("goldengate.fasedReadBufferMemory", b.getUnitAttr());
      auto reader = memory.getResult(0), writer = memory.getResult(1);
      auto one = b.create<ConstantOp>(loc, uint(1), APInt(1, 1));
      Value raddr = originModule.getArgument(3), waddr = originModule.getArgument(2);
      if (depth == 128) {
        raddr = b.create<BitsPrimOp>(loc, raddr, 6, 0);
        waddr = b.create<BitsPrimOp>(loc, waddr, 6, 0);
      }
      connect(sf(reader, "clk"), originModule.getArgument(0));
      connect(sf(writer, "clk"), originModule.getArgument(0));
      connect(sf(reader, "en"), one);
      connect(sf(writer, "en"), originModule.getArgument(1));
      connect(sf(writer, "mask"), one);
      connect(sf(reader, "addr"), raddr);
      connect(sf(writer, "addr"), waddr);
      auto packed = b.create<CatPrimOp>(loc, sf(originModule.getArgument(4), "data"),
                                        sf(originModule.getArgument(4), "last"));
      connect(sf(writer, "data"), packed);
      auto output = sf(reader, "data");
      connect(sf(originModule.getArgument(5), "data"),
              b.create<BitsPrimOp>(loc, output, dataBits, 1));
      connect(sf(originModule.getArgument(5), "last"),
              b.create<BitsPrimOp>(loc, output, 0, 0));
      auto beforeOrigin = dump(*originFixture);
      auto originPath = std::string(argv[2]) + ".origin" + std::to_string(depth) + ".sv";
      auto originJSON = originPath + ".json";
      require(succeeded(goldengate::emitSimulatorRTL(*originFixture, "", originPath,
                                                     error, originJSON)), error);
      auto description = llvm::json::parse(read(originJSON));
      require(bool(description), "native memory origin JSON parse");
      auto *object = description->getAsObject();
      auto *binding = object ? object->getObject("binding") : nullptr;
      auto *memories = object ? object->getArray("memories") : nullptr;
      auto hash = llvm::SHA256::hash(llvm::arrayRefFromStringRef(read(originPath)));
      require(binding && binding->getString("scope") == "compiler_emitted_prefix" &&
                  binding->getInteger("bytes") == int64_t(read(originPath).size()) &&
                  binding->getString("sha256") == llvm::toHex(hash, true) &&
                  memories && memories->size() == 1, "native origin binding mismatch");
      auto *record = (*memories)[0].getAsObject();
      auto *lowered = record->getObject("lowered");
      auto *source = record->getObject("source");
      require(source && source->getInteger("depth") == depth &&
                  source->getInteger("width") == dataBits + 1 && lowered &&
                  lowered->getString("module") == "OriginTest" &&
                  lowered->getString("memory_instance") == "ram_ext" &&
                  lowered->getString("read_capture") == "ram_read_rdata" &&
                  (*record->getArray("instance_paths"))[0].getAsString() ==
                      "OriginTest" &&
                  read(originJSON).find("assum") == std::string::npos &&
                  beforeOrigin == dump(*originFixture),
              "native origin lost actual identity or changed public source IR");
      auto baselinePath = originPath + ".plain.sv";
      require(succeeded(goldengate::emitSimulatorRTL(*originFixture, "", baselinePath, error)) &&
                  read(baselinePath) == read(originPath),
              "origin sidecar changed emitted simulator RTL");
      auto secondJSON = originPath + ".second.json";
      require(succeeded(goldengate::emitSimulatorRTL(*originFixture, "", baselinePath,
                                                     error, secondJSON)) &&
                  read(secondJSON) == read(originJSON),
              "origin sidecar depends on output directory or file name");
      // The three WithAutoILA profiles run this exact production stage even
      // when no probes are selected. Its ground payload ports must retain the
      // same checked packing and native capture origin as the aggregate form.
      OwningOpRef<ModuleOp> flatOrigin = cast<ModuleOp>(originFixture->clone());
      auto flatCircuit = *flatOrigin->getOps<CircuitOp>().begin();
      flatCircuit->setAttr("rawAnnotations", b.getArrayAttr({}));
      require(succeeded(goldengate::lowerTypesWithRetainedTargets(
                  *flatOrigin, flatCircuit, error,
                  goldengate::RetainedTargetScope::FpgaDebugOnly)), error);
      auto flatBefore = dump(*flatOrigin);
      auto flatPath = originPath + ".lowered.sv", flatJSON = flatPath + ".json";
      require(succeeded(goldengate::emitSimulatorRTL(
                  *flatOrigin, "", flatPath, error, flatJSON)) &&
                  flatBefore == dump(*flatOrigin) && read(flatPath) == read(originPath) &&
                  read(flatJSON) == read(originJSON),
              "AutoILA LowerTypes lost native origin or changed RTL/source");
      OwningOpRef<ModuleOp> unmarkedFlat = cast<ModuleOp>(flatOrigin->clone());
      unmarkedFlat->walk([&](MemOp mem) {
        mem->removeAttr("goldengate.fasedReadBufferMemory");
      });
      require(succeeded(goldengate::emitSimulatorRTL(
                  *unmarkedFlat, "", flatPath + ".unmarked.sv", error)) &&
                  read(flatPath + ".unmarked.sv") == read(flatPath),
              "recognizing a lowered origin changed emitted hardware");
      auto goodFlatRTL = read(flatPath), goodFlatJSON = read(flatJSON);
      for (StringRef malformed : {"field", "destination", "direction", "width", "reader-enable"}) {
        OwningOpRef<ModuleOp> bad = cast<ModuleOp>(flatOrigin->clone());
        auto badCircuit = *bad->getOps<CircuitOp>().begin();
        auto badModule = *badCircuit.getOps<FModuleOp>().begin();
        auto portIndex = [&](StringRef name) {
          for (unsigned i = 0; i < badModule.getNumPorts(); ++i)
            if (badModule.getPortName(i) == name) return i;
          throw std::runtime_error("missing lowered origin fixture port");
        };
        auto dataIndex = portIndex("enq_bits_data"), lastIndex = portIndex("enq_bits_last");
        if (malformed == "field" || malformed == "destination") {
          SmallVector<Attribute> portNames(badModule.getPortNamesAttr().begin(),
                                           badModule.getPortNamesAttr().end());
          portNames[malformed == "field" ? dataIndex : portIndex("deq_bits_data")] =
              b.getStringAttr("unrelated_data");
          badModule->setAttr("portNames", b.getArrayAttr(portNames));
        } else if (malformed == "direction") {
          SmallVector<bool> directions(badModule.getPortDirections().begin(),
                                        badModule.getPortDirections().end());
          directions[dataIndex] = !directions[dataIndex];
          badModule->setAttr("portDirections", b.getDenseBoolArrayAttr(directions));
        } else if (malformed == "width") {
          // 63+2 remains a well-typed 65-bit concatenation, but it does not
          // represent the declared data64/last1 origin.
          SmallVector<Attribute> portTypes(badModule.getPortTypesAttr().begin(),
                                           badModule.getPortTypesAttr().end());
          portTypes[dataIndex] = TypeAttr::get(uint(dataBits - 1));
          portTypes[lastIndex] = TypeAttr::get(uint(2));
          badModule->setAttr("portTypes", b.getArrayAttr(portTypes));
          badModule.getArgument(dataIndex).setType(uint(dataBits - 1));
          badModule.getArgument(lastIndex).setType(uint(2));
        } else {
          badModule.walk([&](MemOp mem) {
            for (auto *user : mem.getResult(0).getUsers()) {
              auto field = dyn_cast<SubfieldOp>(user);
              if (field && field.getFieldName() == "en") {
                auto driver = circt::firrtl::getDriverFromConnect(field);
                auto constant = driver.getDefiningOp<ConstantOp>();
                constant.setValueAttr(IntegerAttr::get(
                    constant.getValueAttr().getType(), APInt(1, 0)));
              }
            }
          });
        }
        if (malformed != "direction")
          require(succeeded(verify(*bad)), "malformed origin fixture must remain well-typed");
        auto badBefore = dump(*bad);
        require(failed(goldengate::emitSimulatorRTL(*bad, "", flatPath, error, flatJSON)) &&
                    error.find("native read-buffer origin mark") != std::string::npos &&
                    badBefore == dump(*bad) && read(flatPath) == goodFlatRTL &&
                    read(flatJSON) == goodFlatJSON,
                "malformed flat origin changed source or published outputs");
      }
      auto goodRTL = read(originPath), goodJSON = read(originJSON);
      memory->setAttr("goldengate.fasedReadBufferMemory", b.getStringAttr("invalid"));
      require(failed(goldengate::emitSimulatorRTL(*originFixture, "", originPath,
                                                  error, originJSON)) &&
                  read(originPath) == goodRTL && read(originJSON) == goodJSON,
              "malformed native origin mark changed published outputs");
      memory->setAttr("goldengate.fasedReadBufferMemory", b.getUnitAttr());
      one.setValueAttr(IntegerAttr::get(one.getValueAttr().getType(), APInt(1, 0)));
      require(failed(goldengate::emitSimulatorRTL(*originFixture, "", originPath,
                                                  error, originJSON)) &&
                  read(originPath) == goodRTL && read(originJSON) == goodJSON,
              "variable/disabled read origin was accepted or changed outputs");
    }
    llvm::outs() << "Native memory origins: aggregate and AutoILA-lowered data/last packing resolve identical storage/capture/hierarchy and bound RTL; malformed flat fields, directions, widths and enables reject atomically; deterministic and descriptive only\n";
    auto real = parseSourceFile<ModuleOp>(argv[1], &context);
    require(bool(real), "boundary parse"); before = dump(*real);
    auto result = goldengate::emitSimulatorRTL(
        *real, argv[1], argv[2], error, std::string(argv[2]) + ".memory-origins.json");
    require(before == dump(*real), "real boundary changed by backend");
    if (failed(result)) {
      llvm::errs() << error << '\n'; return 1;
    }
    llvm::outs() << "Real boundary RTL emitted through linked CIRCT pipeline; source unchanged\n";
    return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
