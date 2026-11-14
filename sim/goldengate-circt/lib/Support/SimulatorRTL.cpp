// See LICENSE for license details.
#include "goldengate/SimulatorRTL.h"
#include "circt/Conversion/ExportVerilog.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/Debug/DebugDialect.h"
#include "circt/Dialect/Emit/EmitOps.h"
#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Support/Namespace.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/OM/OMDialect.h"
#include "circt/Dialect/SV/SVDialect.h"
#include "circt/Dialect/SV/SVOps.h"
#include "circt/Dialect/Seq/SeqDialect.h"
#include "circt/Firtool/Firtool.h"
#include "circt/Support/LoweringOptions.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/SHA256.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>

using namespace mlir;
using namespace circt;

// FIRRTL memory simulation creates outlined HW modules but leaves the standard
// initialization-fragment dependencies on their parent. In single-file export,
// a fragment is emitted only at its first reference, so an earlier outlined
// memory silently loses its guarded initializer. Attach the existing defaults
// to the memory itself; never move modules, custom fragments or blackbox text.
LogicalResult goldengate::attachMemoryInitializationFragments(ModuleOp module,
                                                             std::string &error) {
  auto reject = [&](StringRef reason) { error = reason.str(); return failure(); };
  SmallVector<hw::HWModuleOp> memories;
  for (auto candidate : module.getOps<hw::HWModuleOp>()) {
    bool hasGuard = false, valid = true, hasMemory = false;
    for (auto reg : candidate.getOps<sv::RegOp>())
      hasMemory |= reg.getName() == "Memory" &&
                   isa<hw::UnpackedArrayType>(reg.getElementType());
    candidate.walk([&](sv::IfDefOp guard) {
      if (guard.getCond().getName() != "ENABLE_INITIAL_MEM_")
        return;
      hasGuard = true;
      unsigned initializers = 0, registerDeclarations = 0,
               registerInitializers = 0;
      valid &=
          guard->getParentOp() == candidate && guard.getElseRegion().empty();
      if (guard.getThenRegion().empty()) {
        valid = false;
        return;
      }
      for (auto &op : guard.getThenRegion().front()) {
        initializers += isa<sv::InitialOp>(op);
        if (auto registers = dyn_cast<sv::IfDefOp>(op)) {
          // Synchronous reads and longer write latencies introduce pipeline
          // registers. CIRCT declares their random temporaries under a separate
          // REG guard inside the memory initializer's synthesis guard.
          ++registerDeclarations;
          valid &= registers.getCond().getName() == "RANDOMIZE_REG_INIT" &&
                   registers.getElseRegion().empty() &&
                   !registers.getThenRegion().empty();
          if (!registers.getThenRegion().empty()) {
            auto &declarations = registers.getThenRegion().front();
            valid &= !declarations.empty();
            for (auto &declaration : declarations)
              valid &= isa<sv::RegOp>(declaration);
          }
        } else {
          valid &= isa<sv::RegOp, sv::InitialOp>(op);
        }
        if (auto initial = dyn_cast<sv::InitialOp>(op)) {
          if (initial.getBody().empty()) {
            valid = false;
            continue;
          }
          auto &body = initial.getBody().front();
          auto size = body.getOperations().size();
          valid &= size == 2 || size == 3;
          if (size == 2 || size == 3) {
            auto prolog = dyn_cast<sv::VerbatimOp>(body.front());
            auto random =
                dyn_cast<sv::IfDefProceduralOp>(*std::next(body.begin()));
            valid &=
                prolog && prolog.getFormatString() == "`INIT_RANDOM_PROLOG_" &&
                prolog.getNumOperands() == 0 &&
                (!prolog.getSymbolsAttr() || prolog.getSymbolsAttr().empty()) &&
                random && random.getCond().getName() == "RANDOMIZE_MEM_INIT" &&
                random.getElseRegion().empty();
            if (size == 3) {
              auto registers = dyn_cast<sv::IfDefProceduralOp>(body.back());
              ++registerInitializers;
              valid &= registers &&
                       registers.getCond().getName() == "RANDOMIZE_REG_INIT" &&
                       registers.getElseRegion().empty();
            }
          }
        }
      }
      valid &= initializers == 1 && registerDeclarations <= 1 &&
               registerDeclarations == registerInitializers;
    });
    if (hasGuard) {
      if (!hasMemory || !valid)
        return reject("unrecognized outlined memory initialization guard");
      memories.push_back(candidate);
    }
  }
  if (memories.empty())
    return success();

  SymbolTable symbols(module);
  SmallVector<Attribute> required;
  sv::IfDefOp memoryActivation;
  emit::FragmentOp generalFragment;
  bool prepareProlog = false;
  for (StringRef name : {"RANDOM_INIT_MEM_FRAGMENT", "RANDOM_INIT_FRAGMENT"}) {
    auto fragment = dyn_cast_or_null<emit::FragmentOp>(symbols.lookup(name));
    if (!fragment)
      return reject("missing outlined memory initialization fragment");
    bool valid = true;
    llvm::StringMap<unsigned> definitions;
    llvm::StringMap<unsigned> activations;
    SmallVector<Operation *> activationGuards;
    fragment.walk([&](Operation *op) {
      if (op == fragment.getOperation())
        return;
      if (auto text = dyn_cast<sv::VerbatimOp>(op)) {
        valid &= text.getNumOperands() == 0 &&
                 (!text.getSymbolsAttr() || text.getSymbolsAttr().empty());
        SmallVector<StringRef> lines;
        text.getFormatString().split(lines, '\n');
        for (auto line : lines)
          valid &= line.trim().empty() || line.trim().starts_with("//");
      } else if (auto guard = dyn_cast<sv::IfDefOp>(op)) {
        valid &= llvm::is_contained(ArrayRef<StringRef>{"RANDOMIZE",
                                                        "SYNTHESIS",
                                                        "RANDOMIZE_MEM_INIT",
                                                        "RANDOMIZE_REG_INIT",
                                                        "ENABLE_INITIAL_MEM_",
                                                        "RANDOM",
                                                        "INIT_RANDOM",
                                                        "RANDOMIZE_DELAY",
                                                        "INIT_RANDOM_PROLOG_",
                                                        "VERILATOR"},
                                    guard.getCond().getName());
      } else if (auto def = dyn_cast<sv::MacroDefOp>(op)) {
        auto macro = def.getMacroName();
        auto body = def.getFormatString();
        ++definitions[macro];
        SmallVector<std::string> path;
        for (Operation *node = op; node->getParentOp() != fragment;
             node = node->getParentOp()) {
          auto guard = dyn_cast<sv::IfDefOp>(node->getParentOp());
          if (!guard) {
            valid = false;
            break;
          }
          path.push_back(
              (guard.getCond().getName() +
               (node->getParentRegion() == &guard.getThenRegion() ? ":then"
                                                                  : ":else"))
                  .str());
        }
        auto isPath = [&](std::initializer_list<StringRef> expected) {
          return llvm::equal(path, expected);
        };
        bool guarded =
            (macro == "RANDOM" && isPath({"RANDOM:else"})) ||
            (macro == "INIT_RANDOM" && isPath({"INIT_RANDOM:else"})) ||
            (macro == "RANDOMIZE_DELAY" && isPath({"RANDOMIZE_DELAY:else"})) ||
            (macro == "RANDOMIZE" &&
             (isPath({"RANDOMIZE_MEM_INIT:then", "RANDOMIZE:else"}) ||
              (name == "RANDOM_INIT_FRAGMENT" &&
               isPath({"RANDOMIZE_REG_INIT:then", "RANDOMIZE:else"})))) ||
            (macro == "ENABLE_INITIAL_MEM_" &&
             isPath({"ENABLE_INITIAL_MEM_:else", "SYNTHESIS:else"})) ||
            (macro == "INIT_RANDOM_PROLOG_" &&
             ((body == "`INIT_RANDOM" &&
               isPath({"VERILATOR:then",
                       "RANDOMIZE:then",
                       "INIT_RANDOM_PROLOG_:else"})) ||
              (body == "`INIT_RANDOM #`RANDOMIZE_DELAY begin end" &&
               isPath({"VERILATOR:else",
                       "RANDOMIZE:then",
                       "INIT_RANDOM_PROLOG_:else"})) ||
              (body.empty() &&
               isPath({"RANDOMIZE:else", "INIT_RANDOM_PROLOG_:else"}))));
        valid &= guarded &&
                 (!def.getSymbolsAttr() || def.getSymbolsAttr().empty()) &&
                 ((macro == "RANDOM" && body == "$random") ||
                  ((macro == "RANDOMIZE" || macro == "ENABLE_INITIAL_MEM_" ||
                    macro == "INIT_RANDOM") &&
                   body.empty()) ||
                  (macro == "RANDOMIZE_DELAY" && body == "0.002") ||
                  (macro == "INIT_RANDOM_PROLOG_" &&
                   (body.empty() || body == "`INIT_RANDOM" ||
                    body == "`INIT_RANDOM #`RANDOMIZE_DELAY begin end")));
        if (macro == "RANDOMIZE" && guarded) {
          auto inner = cast<sv::IfDefOp>(op->getParentOp());
          ++activations[inner.getCond().getName()];
          auto outer = cast<sv::IfDefOp>(inner->getParentOp());
          valid &= (outer.getThenRegion().empty() ||
                    outer.getThenRegion().front().empty()) &&
                   llvm::hasSingleElement(outer.getElseRegion().front()) &&
                   inner.getElseRegion().empty() &&
                   llvm::hasSingleElement(inner.getThenRegion().front());
          if (name == "RANDOM_INIT_MEM_FRAGMENT")
            memoryActivation = outer;
          else
            activationGuards.push_back(outer);
        }
      } else
        valid = false;
    });
    if (name == "RANDOM_INIT_MEM_FRAGMENT")
      valid &= definitions.size() == 2 && definitions["RANDOMIZE"] == 1 &&
               definitions["ENABLE_INITIAL_MEM_"] == 1;
    else {
      unsigned randomize = definitions.lookup("RANDOMIZE");
      valid &=
          (randomize == 0 || randomize == 2) &&
          definitions.size() == (randomize ? 5 : 4) &&
          (!randomize || (activations.lookup("RANDOMIZE_MEM_INIT") == 1 &&
                          activations.lookup("RANDOMIZE_REG_INIT") == 1)) &&
          definitions["RANDOM"] == 1 && definitions["INIT_RANDOM"] == 1 &&
          definitions["RANDOMIZE_DELAY"] == 1 &&
          definitions["INIT_RANDOM_PROLOG_"] == 3;
      generalFragment = fragment;
      prepareProlog = randomize == 0;
      if (randomize == 2) {
        auto &body = fragment->getRegion(0).front();
        valid &= activationGuards.size() == 2 &&
                 activationGuards[0] == &body.front() &&
                 activationGuards[1] == &*std::next(body.begin());
      }
    }
    if (!valid)
      return reject("unrecognized outlined memory initialization fragment");
    required.push_back(FlatSymbolRefAttr::get(module.getContext(), name));
  }
  if (!memoryActivation ||
      !isa_and_nonnull<sv::MacroDeclOp>(symbols.lookup("RANDOMIZE_REG_INIT")))
    return reject("missing outlined memory randomization activation");
  SmallVector<std::pair<hw::HWModuleOp, ArrayAttr>> pending;
  for (auto memory : memories) {
    SmallVector<Attribute> fragments;
    if (auto attr = memory->getAttr(emit::getFragmentsAttrName())) {
      auto array = dyn_cast<ArrayAttr>(attr);
      if (!array)
        return reject("outlined memory fragments must be an array");
      for (auto ref : array) {
        auto symbol = dyn_cast<FlatSymbolRefAttr>(ref);
        if (!symbol || !isa_and_nonnull<emit::FragmentOp>(
                           symbols.lookup(symbol.getValue())))
          return reject("unresolved outlined memory fragment");
        fragments.push_back(ref);
      }
    }
    // The memory header must establish RANDOMIZE before the general header
    // chooses its random prolog. Existing incompatible order cannot be repaired
    // by moving a caller's fragment references without changing their contract.
    auto mem = llvm::find(fragments, required[0]);
    auto general = llvm::find(fragments, required[1]);
    if (general != fragments.end() && (mem == fragments.end() || general < mem))
      return reject(
          "outlined memory initialization fragments are out of order");
    for (auto ref : required)
      if (!llvm::is_contained(fragments, ref))
        fragments.push_back(ref);
    pending.emplace_back(memory,
                         ArrayAttr::get(module.getContext(), fragments));
  }
  // The first general fragment may belong to an ordinary register module long
  // before the first memory. Its prolog is guarded against later redefinition:
  // a MEM-only build would otherwise freeze an empty prolog before the memory
  // header enables RANDOMIZE. Conversely, an outlined synchronous memory may
  // precede the register header in a REG-only build. Establish both existing
  // activation rules at the general header itself, before prolog selection.
  // Clone only the validated guarded empty definitions; user defaults and the
  // relative order of every existing fragment/body remain intact.
  if (prepareProlog) {
    OpBuilder builder(module.getContext());
    builder.setInsertionPointToStart(&generalFragment->getRegion(0).front());
    builder.clone(*memoryActivation);
    auto registers = cast<sv::IfDefOp>(builder.clone(*memoryActivation));
    auto inner = cast<sv::IfDefOp>(registers.getElseRegion().front().front());
    inner.setCondAttr(
        sv::MacroIdentAttr::get(module.getContext(), "RANDOMIZE_REG_INIT"));
  }
  for (auto [memory, fragments] : pending)
    memory->setAttr(emit::getFragmentsAttrName(), fragments);
  return success();
}

unsigned goldengate::normalizeMemoryInitialization(ModuleOp module) {
  unsigned changed = 0;
  auto isConstant = [](Value value, uint64_t expected) {
    auto constant = value.getDefiningOp<hw::ConstantOp>();
    return constant && constant.getValue().getLimitedValue() == expected;
  };
  module.walk([&](sv::ForOp rows) {
    auto guard = dyn_cast<sv::IfDefProceduralOp>(rows->getParentOp());
    if (!guard || guard.getCond().getName() != "RANDOMIZE_MEM_INIT" ||
        rows->getBlock()->getParent() != &guard.getThenRegion() ||
        !rows->getParentOfType<sv::InitialOp>())
      return;
    SmallVector<Operation *> body;
    for (auto &op : rows->getRegion(0).front())
      body.push_back(&op);
    // For words <= 32 bits, CIRCT writes a 32-bit temporary inside the row
    // loop and optionally extracts its low bits. SFC samples that temporary
    // once before the loop. Match the complete body before hoisting the draw
    // and assignment together; bounds, index, extraction and guards survive.
    if (body.size() >= 5 && body.size() <= 7 &&
        isa<sv::MacroRefExprSEOp>(body[0])) {
      auto random = cast<sv::MacroRefExprSEOp>(body[0]);
      auto fill = dyn_cast<sv::BPAssignOp>(body[1]);
      auto temporary = fill ? fill.getDest().getDefiningOp<sv::RegOp>() : nullptr;
      if (!fill || !temporary || temporary.getName() != "_RANDOM_MEM" ||
          temporary.getElementType() != IntegerType::get(module.getContext(), 32) ||
          random.getMacroName() != "RANDOM" || random.getNumOperands() ||
          random.getResult().getType() != temporary.getElementType() ||
          fill.getSrc() != random.getResult() || !random.getResult().hasOneUse() ||
          !llvm::hasNItems(temporary.getResult().getUses(), 2))
        return;
      unsigned next = 2;
      Value index = rows.getInductionVar();
      auto indexExtract = dyn_cast<comb::ExtractOp>(body[next]);
      if (indexExtract) {
        if (indexExtract.getInput() != index || indexExtract.getLowBit() != 0 ||
            !indexExtract.getResult().hasOneUse())
          return;
        index = indexExtract.getResult();
        ++next;
      }
      if (next + 3 > body.size())
        return;
      auto element = dyn_cast<sv::ArrayIndexInOutOp>(body[next++]);
      auto word = dyn_cast<sv::ReadInOutOp>(body[next++]);
      auto memory = element ? element.getInput().getDefiningOp<sv::RegOp>() : nullptr;
      auto array = memory ? dyn_cast<hw::UnpackedArrayType>(memory.getElementType()) : nullptr;
      auto bits = array ? dyn_cast<IntegerType>(array.getElementType()) : nullptr;
      if (!element || !word || !memory || memory.getName() != "Memory" ||
          !bits || bits.getWidth() > 32 || !array.getNumElements() ||
          element.getIndex() != index || word.getInput() != temporary.getResult() ||
          !element.getResult().hasOneUse() || !word.getResult().hasOneUse() ||
          !isConstant(rows.getLowerBound(), 0) ||
          !isConstant(rows.getUpperBound(), array.getNumElements()) ||
          !isConstant(rows.getStep(), 1) ||
          (indexExtract && index.getType().getIntOrFloatBitWidth() !=
              std::max(1u, llvm::Log2_64_Ceil(array.getNumElements()))))
        return;
      Value value = word.getResult();
      if (bits.getWidth() < 32) {
        if (next + 2 > body.size())
          return;
        auto extract = dyn_cast<comb::ExtractOp>(body[next++]);
        if (!extract || extract.getInput() != value || extract.getLowBit() != 0 ||
            extract.getResult().getType() != bits || !extract.getResult().hasOneUse())
          return;
        value = extract.getResult();
      }
      if (next + 1 != body.size())
        return;
      auto store = dyn_cast<sv::BPAssignOp>(body[next]);
      if (!store || store.getDest() != element.getResult() || store.getSrc() != value)
        return;
      random->moveBefore(rows);
      fill->moveBefore(rows);
      ++changed;
      return;
    }
    if (body.size() < 4 || body.size() > 6)
      return;
    auto chunks = dyn_cast<sv::ForOp>(body[0]);
    unsigned next = 1;
    Value index = rows.getInductionVar();
    auto indexExtract = dyn_cast<comb::ExtractOp>(body[next]);
    if (indexExtract) {
      if (indexExtract.getInput() != index || indexExtract.getLowBit() != 0 ||
          !indexExtract.getResult().hasOneUse())
        return;
      index = indexExtract.getResult();
      ++next;
    }
    if (next + 3 > body.size())
      return;
    auto element = dyn_cast<sv::ArrayIndexInOutOp>(body[next++]);
    auto word = dyn_cast<sv::ReadInOutOp>(body[next++]);
    if (!chunks || !element || !word)
      return;
    auto memory = element.getInput().getDefiningOp<sv::RegOp>();
    auto temporary = word.getInput().getDefiningOp<sv::RegOp>();
    if (!memory || !temporary || memory.getName() != "Memory" ||
        temporary.getName() != "_RANDOM_MEM")
      return;
    auto array = dyn_cast<hw::UnpackedArrayType>(memory.getElementType());
    auto bits = dyn_cast<IntegerType>(temporary.getElementType());
    auto memoryBits = array ? dyn_cast<IntegerType>(array.getElementType()) : nullptr;
    if (!array || !bits || bits.getWidth() <= 32 ||
        !memoryBits || memoryBits.getWidth() <= 32 || !array.getNumElements() ||
        (memoryBits != bits && bits.getWidth() !=
            llvm::alignTo(memoryBits.getWidth(), 32u)) ||
        !isConstant(rows.getLowerBound(), 0) ||
        !isConstant(rows.getUpperBound(), array.getNumElements()) ||
        !isConstant(rows.getStep(), 1) ||
        !isConstant(chunks.getLowerBound(), 0) ||
        !isConstant(chunks.getUpperBound(), bits.getWidth()) ||
        !isConstant(chunks.getStep(), 32))
      return;
    if (element.getIndex() != index ||
        (indexExtract && index.getType().getIntOrFloatBitWidth() !=
            std::max(1u, llvm::Log2_64_Ceil(array.getNumElements()))))
      return;
    // CIRCT pads a partial word's random temporary to a complete 32-bit
    // chunk, then reads only its low memory-width bits. Keep that extraction
    // in the row loop while sharing the complete initialized word across rows.
    Value value = word.getResult();
    if (memoryBits != bits) {
      if (next + 2 > body.size())
        return;
      auto extract = dyn_cast<comb::ExtractOp>(body[next++]);
      if (!extract || extract.getInput() != value || extract.getLowBit() != 0 ||
          extract.getResult().getType() != memoryBits ||
          !extract.getResult().hasOneUse())
        return;
      value = extract.getResult();
    }
    if (next + 1 != body.size())
      return;
    auto store = dyn_cast<sv::BPAssignOp>(body[next]);
    if (!store || store.getDest() != element.getResult() || store.getSrc() != value)
      return;
    SmallVector<Operation *> chunkBody;
    for (auto &op : chunks->getRegion(0).front())
      chunkBody.push_back(&op);
    if (chunkBody.size() != 3 && chunkBody.size() != 4)
      return;
    auto random = dyn_cast<sv::MacroRefExprSEOp>(chunkBody[0]);
    unsigned chunkNext = 1;
    Value base = chunks.getInductionVar();
    if (chunkBody.size() == 4) {
      auto extract = dyn_cast<comb::ExtractOp>(chunkBody[chunkNext++]);
      if (!extract || extract.getInput() != base || extract.getLowBit() != 0 ||
          extract.getResult().getType().getIntOrFloatBitWidth() !=
              llvm::Log2_64_Ceil(bits.getWidth()) ||
          !extract.getResult().hasOneUse())
        return;
      base = extract.getResult();
    }
    auto part = dyn_cast<sv::IndexedPartSelectInOutOp>(chunkBody[chunkNext++]);
    auto fill = dyn_cast<sv::BPAssignOp>(chunkBody[chunkNext]);
    if (!random || random.getMacroName() != "RANDOM" || random.getNumOperands() ||
        random.getResult().getType() != IntegerType::get(module.getContext(), 32) ||
        !part || !fill || part.getDecrement() ||
        part.getInput() != temporary.getResult() ||
        part.getBase() != base ||
        part.getResult().getType().getElementType() != random.getResult().getType() ||
        fill.getDest() != part.getResult() || fill.getSrc() != random.getResult() ||
        !random.getResult().hasOneUse() || !part.getResult().hasOneUse() ||
        !word.getResult().hasOneUse() || !element.getResult().hasOneUse() ||
        !llvm::hasNItems(temporary.getResult().getUses(), 2))
      return;

    // SFC's {N{`RANDOM}} evaluates RANDOM once, then reuses that word for
    // every row. Materialize the draw in a register so ExportVerilog cannot
    // inline the side-effecting macro back into the chunk loop.
    OpBuilder declarations(temporary);
    auto seed = declarations.create<sv::RegOp>(temporary.getLoc(),
        random.getResult().getType(), declarations.getStringAttr("_RANDOM_MEM_SEED"));
    random->moveBefore(rows);
    OpBuilder initialization(rows);
    initialization.create<sv::BPAssignOp>(rows.getLoc(), seed, random.getResult());
    OpBuilder readSeed(fill);
    auto sampled = readSeed.create<sv::ReadInOutOp>(fill.getLoc(), seed);
    fill.getSrcMutable().assign(sampled.getResult());
    chunks->moveBefore(rows);
    ++changed;
  });
  return changed;
}

LogicalResult goldengate::normalizeClockSnapshotInitialization(
    ModuleOp module, ArrayRef<ClockSnapshotIdentity> snapshots,
    unsigned &changed, std::string &error) {
  changed = 0;
  auto reject = [&](StringRef reason) { error = reason.str(); return failure(); };
  struct Pending { sv::RegOp reg; sv::BPAssignOp initialization; };
  SmallVector<Pending> pending;
  llvm::StringSet<> identities;
  auto randomGuard = [](Operation *op) -> sv::IfDefProceduralOp {
    auto guard = op->getParentOfType<sv::IfDefProceduralOp>();
    if (!guard || guard.getCond().getName() != "RANDOMIZE_REG_INIT" ||
        !op->getParentOfType<sv::InitialOp>() ||
        op->getBlock()->getParent() != &guard.getThenRegion())
      return {};
    return guard;
  };
  auto isConstant = [](Value value, uint64_t expected) {
    auto constant = value.getDefiningOp<hw::ConstantOp>();
    return constant && constant.getValue().getLimitedValue() == expected;
  };
  for (const auto &identity : snapshots) {
    if (!identities.insert(identity.module + "/" + identity.reg).second)
      return reject("duplicate ClockBridge snapshot identity");
    hw::HWModuleOp owner;
    for (auto candidate : module.getOps<hw::HWModuleOp>())
      if (candidate.getName() == identity.module) {
        if (owner) return reject("ambiguous ClockBridge snapshot module");
        owner = candidate;
      }
    if (!owner) return reject("ClockBridge snapshot module disappeared during lowering");
    sv::RegOp reg;
    unsigned names = 0;
    owner.walk([&](sv::RegOp candidate) {
      if (candidate.getName() == identity.reg) { reg = candidate; ++names; }
    });
    if (names != 1 || reg.getElementType() != IntegerType::get(module.getContext(), 64))
      return reject("ClockBridge snapshot needs its unique scalar 64-bit SV register");
    for (Operation *user : reg.getResult().getUsers()) {
      if (isa<sv::ReadInOutOp>(user)) continue;
      if (auto assignment = dyn_cast<sv::BPAssignOp>(user)) {
        if (assignment.getDest() == reg.getResult()) continue;
      }
      if (auto assignment = dyn_cast<sv::PAssignOp>(user)) {
        if (assignment.getDest() == reg.getResult()) continue;
      }
      return reject("ClockBridge snapshot has an unrecognized or partial SV register use");
    }
    sv::BPAssignOp initialization;
    unsigned assignments = 0;
    owner.walk([&](sv::BPAssignOp assignment) {
      if (assignment.getDest() == reg.getResult()) {
        initialization = assignment;
        ++assignments;
      }
    });
    if (assignments != 1 || !randomGuard(initialization))
      return reject("ClockBridge snapshot needs one guarded random initializer");
    auto guard = randomGuard(initialization);
    auto concat = initialization.getSrc().getDefiningOp<comb::ConcatOp>();
    if (!concat || concat.getResult().getType() != reg.getElementType())
      return reject("unrecognized ClockBridge snapshot random word");
    if (initialization->hasAttr("goldengate.clockSnapshotInitNormalized")) {
      if (concat.getNumOperands() != 2 || concat.getOperand(0) != concat.getOperand(1))
        return reject("malformed normalized ClockBridge snapshot initializer");
      auto read = concat.getOperand(0).getDefiningOp<sv::ReadInOutOp>();
      auto seed = read ? read.getInput().getDefiningOp<sv::RegOp>() : nullptr;
      sv::BPAssignOp capture;
      unsigned captures = 0;
      if (seed) owner.walk([&](sv::BPAssignOp assignment) {
        if (assignment.getDest() == seed.getResult()) { capture = assignment; ++captures; }
      });
      auto draw = capture ? capture.getSrc().getDefiningOp<sv::MacroRefExprSEOp>() : nullptr;
      if (!seed || seed.getElementType() != IntegerType::get(module.getContext(), 32) ||
          captures != 1 || randomGuard(capture) != guard || !draw ||
          draw.getMacroName() != "RANDOM" || draw.getNumOperands() ||
          !draw.getResult().hasOneUse() || !capture->isBeforeInBlock(initialization))
        return reject("malformed captured ClockBridge snapshot random seed");
      continue;
    }
    // Match CIRCT's packed random-array slices, not arbitrary initializer
    // expressions. Every selected bit must come from the same 32-bit RANDOM
    // array, filled by one recognized complete constant-bound loop.
    Value array;
    for (Value part : concat.getOperands()) {
      auto extract = part.getDefiningOp<comb::ExtractOp>();
      if (extract) {
        if (extract.getInput().getType() != IntegerType::get(module.getContext(), 32) ||
            extract.getLowBit() + part.getType().getIntOrFloatBitWidth() > 32)
          return reject("unrecognized ClockBridge snapshot random slice");
        part = extract.getInput();
      }
      auto read = part.getDefiningOp<sv::ReadInOutOp>();
      auto element = read ? read.getInput().getDefiningOp<sv::ArrayIndexInOutOp>() : nullptr;
      auto index = element ? element.getIndex().getDefiningOp<hw::ConstantOp>() : nullptr;
      auto inout = element ? dyn_cast<hw::InOutType>(element.getInput().getType()) : hw::InOutType();
      auto type = inout ? dyn_cast<hw::UnpackedArrayType>(inout.getElementType()) : hw::UnpackedArrayType();
      if (!element || !index || !type || type.getElementType() != IntegerType::get(module.getContext(), 32) ||
          index.getValue().getLimitedValue() >= type.getNumElements() ||
          (array && array != element.getInput()))
        return reject("unrecognized ClockBridge snapshot packed random source");
      array = element.getInput();
    }
    auto arrayOp = array ? array.getDefiningOp() : nullptr;
    if (!arrayOp || (!isa<sv::LogicOp, sv::RegOp>(arrayOp)) ||
        arrayOp->getBlock() != initialization->getBlock())
      return reject("ClockBridge snapshot random array is outside its initialization guard");
    auto arrayType = cast<hw::UnpackedArrayType>(cast<hw::InOutType>(array.getType()).getElementType());
    unsigned fills = 0;
    bool recognized = false;
    guard.walk([&](sv::BPAssignOp fill) {
      auto element = fill.getDest().getDefiningOp<sv::ArrayIndexInOutOp>();
      if (!element || element.getInput() != array) return;
      ++fills;
      auto loop = dyn_cast<sv::ForOp>(fill->getParentOp());
      auto draw = fill.getSrc().getDefiningOp<sv::MacroRefExprSEOp>();
      if (!loop || loop->getBlock() != initialization->getBlock() ||
          !loop->isBeforeInBlock(initialization) || !draw ||
          draw.getMacroName() != "RANDOM" || draw.getNumOperands() ||
          !draw.getResult().hasOneUse() || draw->getBlock() != fill->getBlock() ||
          !isConstant(loop.getLowerBound(), 0) ||
          !isConstant(loop.getUpperBound(), arrayType.getNumElements()) ||
          !isConstant(loop.getStep(), 1)) return;
      Value index = element.getIndex();
      Operation *indexExtract = nullptr;
      if (auto extract = index.getDefiningOp<comb::ExtractOp>()) {
        if (extract.getLowBit() != 0 || extract.getInput() != loop.getInductionVar()) return;
        indexExtract = extract;
        index = extract.getInput();
      }
      SmallVector<Operation *> body;
      for (auto &op : loop->getRegion(0).front()) body.push_back(&op);
      unsigned next = 0;
      if (body.size() != (indexExtract ? 4u : 3u) || body[next++] != draw) return;
      if (indexExtract && body[next++] != indexExtract) return;
      recognized = index == loop.getInductionVar() &&
                   body[next++] == element && body[next] == fill;
    });
    if (fills != 1 || !recognized)
      return reject("unrecognized ClockBridge snapshot random-array fill");
    pending.push_back({reg, initialization});
  }
  // Every identity and shape is validated before modifying any initializer.
  if (pending.empty()) return success();
  for (auto &item : pending) {
    circt::Namespace names;
    item.reg->getParentOfType<hw::HWModuleOp>().walk([&](Operation *op) {
      if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
    });
    OpBuilder declaration(item.reg);
    auto seed = declaration.create<sv::RegOp>(item.reg.getLoc(),
        declaration.getI32Type(), declaration.getStringAttr(
            names.newName((item.reg.getName() + "_RANDOM_INIT").str())));
    OpBuilder b(item.initialization);
    auto draw = b.create<sv::MacroRefExprSEOp>(item.initialization.getLoc(),
        b.getI32Type(), FlatSymbolRefAttr::get(module.getContext(), "RANDOM"), ValueRange{});
    b.create<sv::BPAssignOp>(item.initialization.getLoc(), seed, draw);
    auto sampled = b.create<sv::ReadInOutOp>(item.initialization.getLoc(), seed);
    auto word = b.create<comb::ConcatOp>(item.initialization.getLoc(),
        ValueRange{sampled.getResult(), sampled.getResult()});
    item.initialization.getSrcMutable().assign(word.getResult());
    item.initialization->setAttr("goldengate.clockSnapshotInitNormalized", b.getUnitAttr());
    ++changed;
  }
  return success();
}

unsigned goldengate::normalizeInitializationIndices(ModuleOp module) {
  unsigned changed = 0;
  module.walk([&](sv::IndexedPartSelectInOutOp select) {
    if (!select->getParentOfType<sv::InitialOp>())
      return;
    auto index = dyn_cast<BlockArgument>(select.getBase());
    if (!index)
      return;
    auto loop = dyn_cast<sv::ForOp>(index.getOwner()->getParentOp());
    if (!loop || loop.getInductionVar() != index)
      return;
    auto upper = loop.getUpperBound().getDefiningOp<hw::ConstantOp>();
    auto wordType = dyn_cast<IntegerType>(select.getInput().getType().getElementType());
    auto indexType = dyn_cast<IntegerType>(index.getType());
    if (!upper || !wordType || !indexType)
      return;
    unsigned bits = wordType.getWidth();
    unsigned needed = std::max(1u, llvm::Log2_64_Ceil(bits));
    // SV for-loop bodies execute only for unsigned induction values strictly
    // below upperBound. Upper <= word width proves the removed high bits are
    // zero even if a part select extends beyond the end of a partial word.
    // Unknown bounds and unbounded indices must retain out-of-range behavior.
    if (indexType.getWidth() <= needed ||
        upper.getValue().getLimitedValue() > bits)
      return;
    OpBuilder builder(select);
    auto base = builder.create<comb::ExtractOp>(select.getLoc(), index, 0, needed);
    select.getBaseMutable().assign(base.getResult());
    ++changed;
  });
  return changed;
}

namespace {
// SFC connects even unused instance outputs to declared wires. Preserve that
// boundary in the backend clone: open output connections otherwise trigger
// Verilator PINCONNECTEMPTY with FireSim's fatal warning policy. Only SSA
// results without consumers are terminated; inputs and live outputs are intact.
void terminateUnusedInstanceOutputs(ModuleOp module) {
  module.walk([&](hw::InstanceOp instance) {
    OpBuilder builder(instance);
    builder.setInsertionPointAfter(instance);
    for (auto result : instance.getResults()) {
      if (!result.use_empty())
        continue;
      auto name = (instance.getInstanceName() + "_unused_" +
                   instance.getResultName(result.getResultNumber()).getValue()).str();
      auto sink = builder.create<sv::WireOp>(instance.getLoc(), result.getType(),
                                            name);
      builder.create<sv::AssignOp>(instance.getLoc(), sink, result);
    }
  });
}

// Golden Gate retains source annotations across circuit wrapping. Transfer only
// these module-local sources to the backend clone; replaying the entire archive
// would resolve obsolete targets and repeat Golden Gate transforms.
LogicalResult attachInlineBlackBoxes(firrtl::CircuitOp circuit,
                                    StringRef outputFilename,
                                    std::string &error) {
  if (circuit->hasAttr("goldengate.inlineBlackBoxesPrepared"))
    return success();
  auto archive = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!archive)
    return success();
  SymbolTable symbols(circuit);
  llvm::StringMap<StringRef> sources;
  SmallVector<std::pair<firrtl::FExtModuleOp, DictionaryAttr>> pending;
  auto reject = [&](const llvm::Twine &reason) {
    error = reason.str();
    return failure();
  };
  for (Attribute attribute : archive) {
    auto annotation = dyn_cast<DictionaryAttr>(attribute);
    if (!annotation)
      continue;
    auto cls = annotation.getAs<StringAttr>("class");
    if (!cls || cls.getValue() != firrtl::blackBoxInlineAnnoClass)
      continue;
    auto target = annotation.getAs<StringAttr>("target");
    auto name = annotation.getAs<StringAttr>("name");
    auto text = annotation.getAs<StringAttr>("text");
    if (!target || !name || !text || name.getValue().empty() ||
        name.getValue() == "." || name.getValue() == ".." ||
        name.getValue().contains('/') || name.getValue().contains('\\'))
      return reject("inline blackbox requires a module target, local filename "
                    "and source text");
    // SFC ModuleName serializes as Circuit.Module; newer annotations use
    // ~Circuit|Module. Circuit wrapping changes the prefix, not this symbol.
    StringRef moduleName;
    auto value = target.getValue();
    if (value.starts_with("~")) {
      auto split = value.drop_front().split('|');
      if (split.first.empty() || split.second.contains('|') ||
          split.second.contains('>') || split.second.contains('/'))
        return reject("inline blackbox target must identify a module");
      moduleName = split.second;
    } else {
      auto split = value.split('.');
      if (split.first.empty() || split.second.contains('.'))
        return reject("inline blackbox target must identify a module");
      moduleName = split.second;
    }
    auto module =
        dyn_cast_or_null<firrtl::FExtModuleOp>(symbols.lookup(moduleName));
    if (!module)
      return reject("inline blackbox target does not resolve to an external "
                    "module: " + value);
    auto inserted = sources.try_emplace(name.getValue(), text.getValue());
    if (!inserted.second && inserted.first->second != text.getValue())
      return reject("conflicting inline blackbox source filename: " +
                    name.getValue());
    NamedAttrList attached(annotation);
    attached.erase("target");
    pending.emplace_back(module, attached.getDictionary(circuit.getContext()));
  }
  for (auto [module, annotation] : pending) {
    firrtl::AnnotationSet annotations(module);
    if (!llvm::is_contained(annotations.getArray(), annotation))
      annotations.addAnnotations(ArrayRef<Attribute>{annotation});
    annotations.applyToOperation(module);
  }
  if (!pending.empty()) {
    // CIRCT's BlackBoxReader emits unique filenames and the standard resource
    // list. Keep those files beside the candidate RTL for FireSim's append step.
    auto *context = circuit.getContext();
    firrtl::AnnotationSet annotations(circuit);
    auto directory = DictionaryAttr::get(context, {
        {StringAttr::get(context, "class"),
         StringAttr::get(context, firrtl::blackBoxTargetDirAnnoClass)},
        {StringAttr::get(context, "targetDir"),
         StringAttr::get(context, llvm::sys::path::parent_path(outputFilename))}});
    annotations.addAnnotations(ArrayRef<Attribute>{directory});
    annotations.applyToOperation(circuit);
  }
  return success();
}
} // namespace

LogicalResult goldengate::normalizeHostHierarchy(ModuleOp source,
                                                StringRef outputFilename,
                                                unsigned &inlinedWrappers,
                                                std::string &error) {
  using namespace firrtl;
  inlinedWrappers = 0;
  auto reject = [&](StringRef reason) {
    error = reason.str();
    return failure();
  };
  auto circuits = source.getOps<CircuitOp>();
  if (std::distance(circuits.begin(), circuits.end()) != 1 ||
      failed(verify(source)))
    return reject("host hierarchy requires one valid FIRRTL circuit");
  auto original = *circuits.begin();
  if (original->hasAttr("goldengate.hostHierarchyNormalized"))
    return success();
  auto *context = source.getContext();
  context->loadDialect<debug::DebugDialect>();
  OwningOpRef<ModuleOp> candidate = cast<ModuleOp>(source->clone());
  auto circuit = *candidate->getOps<CircuitOp>().begin();
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw)
    return reject("host hierarchy requires retained annotations");
  SymbolTable symbols(circuit);
  auto top = dyn_cast_or_null<FModuleOp>(symbols.lookup("FPGATop"));
  if (!top)
    return reject("host hierarchy requires the assembled FPGATop");

  // Explicit user XDC references must remain resolvable. Preserve every
  // module mentioned in their local or nonlocal paths rather than rewriting
  // serialized reference strings. Generated clock constraints are attached
  // to actual target instances and follow the final instance graph.
  llvm::StringSet<> protectedModules;
  std::function<void(Attribute)> protect = [&](Attribute attr) {
    if (!attr) return;
    if (auto text = dyn_cast<StringAttr>(attr)) {
      StringRef value = text.getValue();
      if (!value.starts_with("~")) return;
      auto hierarchy = value.split('|').second.split('>').first;
      SmallVector<StringRef> path;
      hierarchy.split(path, '/');
      if (!path.empty()) protectedModules.insert(path.front());
      for (auto step : ArrayRef<StringRef>(path).drop_front())
        protectedModules.insert(step.split(':').second);
    } else if (auto array = dyn_cast<ArrayAttr>(attr)) {
      for (auto item : array) protect(item);
    } else if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
      for (auto item : dict) protect(item.getValue());
    }
  };
  for (auto attr : raw) {
    auto annotation = dyn_cast<DictionaryAttr>(attr);
    auto cls = annotation ? annotation.getAs<StringAttr>("class") : StringAttr();
    if (cls && cls.getValue() == AnnotationClasses::InternalXDC)
      protect(annotation.get("argumentList"));
  }
  // An attached inner symbol is scoped to its owning module. Keep that
  // boundary even when no retained JSON annotation mentions it; consumers
  // may already hold native InnerRefAttr identities. Preserve symbol-bearing
  // instances as well, since inlining their children removes the instance.
  for (auto module : circuit.getOps<FModuleOp>()) {
    bool hasSymbols = false;
    for (unsigned port = 0; port < module.getNumPorts(); ++port) {
      auto symbol = module.getPortSymbolAttr(port);
      hasSymbols |= symbol && !symbol.empty();
    }
    module.walk([&](Operation *op) {
      auto symbol = op->getAttrOfType<hw::InnerSymAttr>("inner_sym");
      hasSymbols |= symbol && !symbol.empty();
      if (auto instance = dyn_cast<InstanceOp>(op))
        if (symbol && !symbol.empty())
          protectedModules.insert(instance.getModuleName());
    });
    if (hasSymbols) protectedModules.insert(module.getName());
  }

  llvm::DenseMap<Attribute, unsigned> uses;
  circuit.walk([&](InstanceOp instance) { ++uses[instance.getModuleNameAttr()]; });
  SmallVector<FModuleOp> wrappers;
  llvm::DenseSet<Operation *> visited;
  FModuleOp parent = top;
  while (visited.insert(parent.getOperation()).second) {
    InstanceOp sim;
    for (auto instance : parent.getOps<InstanceOp>())
      if (instance.getName() == "sim") {
        if (sim) return reject("ambiguous host assembly wrapper chain");
        sim = instance;
      }
    if (!sim) break;
    auto child = dyn_cast_or_null<FModuleOp>(symbols.lookup(sim.getModuleName()));
    if (!child || !child.getName().starts_with("GG")) break;
    auto wrapperName = child.getName();
    if (!wrapperName.ends_with("Wrapper")) {
      // Repeated bridge attachments use Namespace's decimal suffix, e.g.
      // GGTracerVBridgeControlWrapper_0. These are the same assembly boundary
      // contract; unrelated suffixes must not turn target modules into wrappers.
      auto numbered = wrapperName.rsplit('_');
      if (!numbered.first.ends_with("Wrapper") || numbered.second.empty() ||
          !llvm::all_of(numbered.second,
                        [](char c) { return c >= '0' && c <= '9'; }))
        break;
    }
    // Retain the innermost simulation boundary (channel/target wrapper).
    // Only assembly modules that themselves forward through `sim` qualify.
    bool forwards = false;
    for (auto instance : child.getOps<InstanceOp>())
      forwards |= instance.getName() == "sim";
    if (!forwards) break;
    if (uses[sim.getModuleNameAttr()] != 1)
      return reject("host assembly wrapper must have one instance");
    if (!protectedModules.count(child.getName())) wrappers.push_back(child);
    parent = child;
  }
  if (wrappers.empty()) return success();

  // Attach retained blackbox sources before the native inliner removes dead
  // modules. RTL emission then uses those attached annotations, not obsolete
  // module targets from the historical archive.
  if (failed(attachInlineBlackBoxes(circuit, outputFilename, error)))
    return failure();
  circuit->setAttr("goldengate.inlineBlackBoxesPrepared", UnitAttr::get(context));
  llvm::DenseSet<Operation *> existing;
  top.walk([&](Operation *op) { existing.insert(op); });
  auto inlineAnno = DictionaryAttr::get(context, {
      {StringAttr::get(context, "class"),
       StringAttr::get(context, "firrtl.passes.InlineAnnotation")}});
  for (auto wrapper : wrappers) {
    // Builders leave assembly modules public. Once their unique instance is
    // inlined they are internal implementation details, not exported roots;
    // otherwise CIRCT retains every progressively flattened duplicate body.
    SymbolTable::setSymbolVisibility(wrapper, SymbolTable::Visibility::Private);
    AnnotationSet annotations(wrapper);
    annotations.addAnnotations(ArrayRef<Attribute>{inlineAnno});
    annotations.applyToOperation(wrapper);
  }
  PassManager passes(context);
  passes.nest<CircuitOp>().addPass(createInlinerPass());
  if (failed(passes.run(*candidate)))
    return reject("CIRCT host assembly wrapper inlining failed");

  // Native inlining prefixes names with every removed instance. Bound those
  // names too: sim_sim_... is a construction artifact, not a useful identity.
  // Existing declarations and explicit XDC roots keep their original names.
  // CIRCT inner symbols remain untouched and retain their native rename map.
  if (!protectedModules.count(top.getName())) {
    circt::Namespace names;
    for (auto name : top.getPortNames())
      names.newName(cast<StringAttr>(name).getValue());
    SmallVector<Operation *> shorten;
    top.walk([&](Operation *op) {
      auto name = op->getAttrOfType<StringAttr>("name");
      if (!name) return;
      if (!existing.count(op) && name.getValue().starts_with("sim_sim_"))
        shorten.push_back(op);
      else names.newName(name.getValue());
    });
    for (auto *op : shorten) {
      StringRef base = op->getAttrOfType<StringAttr>("name").getValue();
      while (base.consume_front("sim_")) {}
      // The inliner also prefixes empty names on anonymous side effects.
      // Preserve anonymity: Namespace::newName("") invents `_0`, which the
      // backend turns into `assert___0`. Verilator can decode that label's
      // incomplete `__0` escape as a NUL in its VPI scope name and emit
      // uncompilable C++.
      // Nonempty declaration/verification names still need collision handling.
      op->setAttr("name", StringAttr::get(
          context, base.empty() ? base : names.newName(base)));
    }
  }
  circuit->setAttr("goldengate.hostHierarchyNormalized", UnitAttr::get(context));
  if (failed(verify(*candidate)))
    return reject("host hierarchy normalization produced invalid FIRRTL IR");
  inlinedWrappers = wrappers.size();
  original->setAttrs(circuit->getAttrs());
  original.getBody().takeBody(circuit.getBody());
  return success();
}

namespace {
// These records describe native identities, never a cross-compiler relation.
struct NativeMemoryOrigin {
  std::string module, memory, reader, writer, capture;
  uint64_t depth;
  unsigned width;
};
constexpr StringLiteral memoryOriginMark = "goldengate.fasedReadBufferMemory";

static LogicalResult collectNativeMemoryOrigins(
    firrtl::CircuitOp circuit, SmallVectorImpl<NativeMemoryOrigin> &origins,
    std::string &error) {
  bool invalid = false;
  llvm::StringSet<> identities;
  circuit.walk([&](Operation *op) {
    auto mark = op->getAttr(memoryOriginMark);
    if (!mark) return;
    auto memory = dyn_cast<firrtl::MemOp>(op);
    auto parent = op->getParentOfType<firrtl::FModuleOp>();
    auto type = memory ? dyn_cast<firrtl::UIntType>(memory.getDataType())
                       : firrtl::UIntType();
    bool valid =
        isa<UnitAttr>(mark) && memory && parent && type && type.getWidth() &&
        *type.getWidth() >= 9 && *type.getWidth() <= 1025 &&
        llvm::isPowerOf2_64(*type.getWidth() - 1) &&
        memory.getNumResults() == 2 && memory.getReadLatency() == 1 &&
        memory.getWriteLatency() == 1 &&
        memory.getRuw() == firrtl::RUWAttr::Undefined &&
        memory.getDepth() >= 16 && llvm::isPowerOf2_64(memory.getDepth()) &&
        memory.getPortKind(0) == firrtl::MemOp::PortKind::Read &&
        memory.getPortKind(1) == firrtl::MemOp::PortKind::Write &&
        !memory.getInnerSymAttr() && memory.getAnnotations().empty();
    if (!valid) { invalid = true; return; }
    auto subfield = [&](unsigned port, StringRef name) -> firrtl::SubfieldOp {
      firrtl::SubfieldOp found;
      for (auto *user : memory.getResult(port).getUsers()) {
        auto field = dyn_cast<firrtl::SubfieldOp>(user);
        if (!field) { valid = false; continue; }
        if (field.getFieldName() == name) {
          if (found) valid = false;
          found = field;
        }
      }
      if (!found) valid = false;
      return found;
    };
    auto source = [&](unsigned port, StringRef name) -> Value {
      auto field = subfield(port, name);
      return field ? firrtl::getDriverFromConnect(field.getResult()) : Value();
    };
    auto enableValue = source(0, "en");
    auto enable = enableValue ? enableValue.getDefiningOp<firrtl::ConstantOp>()
                              : firrtl::ConstantOp();
    auto readClock = source(0, "clk"), writeClock = source(1, "clk");
    auto clock = dyn_cast_or_null<BlockArgument>(readClock);
    valid &= enable && enable.getValue().isOne() && readClock == writeClock &&
             clock && clock.getOwner() == parent.getBodyBlock() &&
             parent.getPortName(clock.getArgNumber()) == "clock";
    // AutoILA runs LowerTypes over the complete circuit, even with no probes.
    // Its native read-buffer payload leaves are then module arguments instead
    // of subfields. Check the live typed port and direction, not an arbitrary
    // signal with a matching suffix. Keep the original aggregate check too.
    auto flatPort = [&](Value value, StringRef name, unsigned width,
                        firrtl::Direction direction) {
      auto port = dyn_cast<BlockArgument>(value);
      return port && port.getOwner() == parent.getBodyBlock() &&
             parent.getPortName(port.getArgNumber()) == name &&
             parent.getPortDirection(port.getArgNumber()) == direction &&
             value.getType() == firrtl::UIntType::get(circuit.getContext(), width);
    };
    // Check the live writer concatenation, rather than trusting a field label.
    auto packedValue = source(1, "data");
    auto packed = packedValue ? packedValue.getDefiningOp<firrtl::CatPrimOp>()
                              : firrtl::CatPrimOp();
    if (!packed) valid = false;
    else {
      auto data = packed.getLhs().getDefiningOp<firrtl::SubfieldOp>();
      auto last = packed.getRhs().getDefiningOp<firrtl::SubfieldOp>();
      bool aggregate =
          data && last && data.getFieldName() == "data" &&
          last.getFieldName() == "last" && data.getInput() == last.getInput() &&
          data.getType() == firrtl::UIntType::get(circuit.getContext(),
                                                  *type.getWidth() - 1) &&
          last.getType() == firrtl::UIntType::get(circuit.getContext(), 1);
      bool flattened =
          flatPort(packed.getLhs(),
                   "enq_bits_data",
                   *type.getWidth() - 1,
                   firrtl::Direction::In) &&
          flatPort(packed.getRhs(), "enq_bits_last", 1, firrtl::Direction::In);
      valid &= aggregate || flattened;
    }
    auto readData = subfield(0, "data");
    unsigned dataSlices = 0, lastSlices = 0;
    if (readData) for (auto *user : readData.getResult().getUsers()) {
      auto bits = dyn_cast<firrtl::BitsPrimOp>(user);
      if (!bits) { valid = false; continue; }
      StringRef name;
      if (bits.getHi() == *type.getWidth() - 1 && bits.getLo() == 1) {
        name = "data"; ++dataSlices;
      } else if (bits.getHi() == 0 && bits.getLo() == 0) {
        name = "last"; ++lastSlices;
      } else
        valid = false;
      if (!bits.getResult().hasOneUse()) { valid = false; continue; }
      auto connect = dyn_cast<firrtl::StrictConnectOp>(*bits.getResult().getUsers().begin());
      auto dest = connect ? connect.getDest().getDefiningOp<firrtl::SubfieldOp>()
                          : firrtl::SubfieldOp();
      bool destination = dest && dest.getFieldName() == name;
      if (connect)
        destination |=
            flatPort(connect.getDest(),
                     name == "data" ? "deq_bits_data" : "deq_bits_last",
                     name == "data" ? *type.getWidth() - 1 : 1,
                     firrtl::Direction::Out);
      valid &= connect && connect.getSrc() == bits.getResult() && destination;
    }
    valid &= dataSlices == 1 && lastSlices == 1 &&
             identities.insert((parent.getName() + "/" + memory.getName()).str()).second;
    if (!valid) { invalid = true; return; }
    origins.push_back({parent.getName().str(),
                       memory.getName().str(),
                       memory.getPortNameStr(0).str(),
                       memory.getPortNameStr(1).str(),
                       "",
                       memory.getDepth(),
                       unsigned(*type.getWidth())});
    op->removeAttr(memoryOriginMark);
  });
  if (invalid) {
    error = "native read-buffer origin mark requires checked data/last "
            "packing, constant-enabled synchronous reader and common clock";
    return failure();
  }
  return success();
}

static void recordNativeReadCapture(firrtl::MemOp memory, StringRef capture,
                                   SmallVectorImpl<NativeMemoryOrigin> &origins) {
  auto parent = memory->getParentOfType<firrtl::FModuleOp>();
  for (auto &origin : origins)
    if (origin.module == parent.getName() && origin.memory == memory.getName())
      origin.capture = capture.str();
}

static bool plainVerilogIdentifier(StringRef name) {
  if (name.empty() || !(llvm::isAlpha(name.front()) || name.front() == '_'))
    return false;
  return llvm::all_of(name, [](char ch) {
    return llvm::isAlnum(ch) || ch == '_' || ch == '$';
  });
}

static LogicalResult describeNativeMemoryOrigins(
    ModuleOp lowered, StringRef root, ArrayRef<NativeMemoryOrigin> origins,
    StringRef rtl, llvm::json::Object &description, std::string &error) {
  auto reject = [&](StringRef reason) { error = reason.str(); return failure(); };
  SymbolTable symbols(lowered);
  auto top = dyn_cast_or_null<hw::HWModuleOp>(symbols.lookup(root));
  if (!top) return reject("native memory origins require the lowered top module");
  llvm::json::Array memories;
  for (const auto &origin : origins) {
    auto parent = dyn_cast_or_null<hw::HWModuleOp>(symbols.lookup(origin.module));
    if (!parent || origin.capture.empty())
      return reject("native memory origin lost its module or read capture");
    hw::InstanceOp storage;
    sv::RegOp capture;
    for (auto instance : parent.getOps<hw::InstanceOp>())
      if (instance.getInstanceName() == origin.memory + "_ext") {
        if (storage) return reject("ambiguous native memory origin instance");
        storage = instance;
      }
    for (auto reg : parent.getOps<sv::RegOp>())
      if (reg.getName() == origin.capture) capture = reg;
    auto storageModule = storage ? dyn_cast_or_null<hw::HWModuleOp>(
        symbols.lookup(storage.getModuleName())) : hw::HWModuleOp();
    sv::RegOp array;
    if (storageModule) for (auto reg : storageModule.getOps<sv::RegOp>())
      if (reg.getName() == "Memory") array = reg;
    auto arrayType = array ? dyn_cast<hw::UnpackedArrayType>(array.getElementType())
                           : hw::UnpackedArrayType();
    if (!storage || !capture || !arrayType ||
        arrayType.getNumElements() != origin.depth ||
        arrayType.getElementType() !=
            IntegerType::get(lowered.getContext(), origin.width) ||
        capture.getElementType() !=
            IntegerType::get(lowered.getContext(), origin.width))
      return reject("native memory origin does not resolve to the declared "
                    "storage and capture width");
    auto parentName = hw::getVerilogModuleName(parent);
    auto storageName = hw::getVerilogModuleName(storageModule);
    if (!plainVerilogIdentifier(parentName) || !plainVerilogIdentifier(storageName) ||
        !plainVerilogIdentifier(storage.getInstanceName()) ||
        !plainVerilogIdentifier(capture.getName()))
      return reject("native memory origin requires directly representable Verilog identifiers");
    llvm::json::Array paths;
    llvm::StringSet<> active;
    bool invalidHierarchy = false;
    std::function<void(hw::HWModuleOp, std::string)> visit =
        [&](hw::HWModuleOp mod, std::string path) {
      if (!active.insert(mod.getName()).second) { invalidHierarchy = true; return; }
      if (mod == parent) paths.push_back(path);
      for (auto instance : mod.getOps<hw::InstanceOp>()) {
        auto child = dyn_cast_or_null<hw::HWModuleOp>(symbols.lookup(instance.getModuleName()));
        if (!child) continue;
        if (!plainVerilogIdentifier(instance.getInstanceName())) {
          invalidHierarchy = true; continue;
        }
        visit(child, path + "." + instance.getInstanceName().str());
      }
      active.erase(mod.getName());
    };
    visit(top, hw::getVerilogModuleName(top).str());
    if (invalidHierarchy || paths.empty())
      return reject("native memory origin hierarchy is cyclic, unsupported or unreachable");
    llvm::json::Array fields;
    fields.push_back(llvm::json::Object{
        {"name", "data"}, {"hi", origin.width - 1}, {"lo", 1}});
    fields.push_back(llvm::json::Object{{"name", "last"}, {"hi", 0}, {"lo", 0}});
    memories.push_back(llvm::json::Object{
        {"origin", "FASEDReadBuffer"},
        {"source",
         llvm::json::Object{{"module", origin.module},
                            {"memory", origin.memory},
                            {"read_port", origin.reader},
                            {"write_port", origin.writer},
                            {"width", origin.width},
                            {"depth", origin.depth}}},
        {"fields", std::move(fields)},
        {"lowered",
         llvm::json::Object{{"module", parentName},
                            {"memory_instance", storage.getInstanceName()},
                            {"memory_module", storageName},
                            {"storage", array.getName()},
                            {"read_capture", capture.getName()}}},
        {"instance_paths", std::move(paths)},
        {"read_capture",
         llvm::json::Object{{"latency", 1},
                            {"enable", "constant_one"},
                            {"clock", "clock"},
                            {"reset", "none"},
                            {"disabled", "hold"},
                            {"collision", "pre_edge_data"}}}});
  }
  auto hash = llvm::SHA256::hash(llvm::arrayRefFromStringRef(rtl));
  description = llvm::json::Object{
      {"schema_version", 1}, {"kind", "goldengate.native-memory-origins"},
      {"binding", llvm::json::Object{{"scope", "compiler_emitted_prefix"},
          {"bytes", rtl.size()}, {"sha256", llvm::toHex(hash, true)}}},
      {"memories", std::move(memories)}};
  return success();
}
} // namespace

// SFC's VerilogEmitter emits an asynchronous reader as an unconditional array
// access, including when its read enable is low. CIRCT otherwise inserts an X
// mux on that output. Refine only this unspecified disabled-read value to the
// SFC choice. Keep synchronous/read-write ports on their existing lowering:
// Firtool's global ignore-read-enable option also changes their pipelines.
static void normalizeAsynchronousReadEnables(ModuleOp module) {
  module.walk([](firrtl::MemOp memory) {
    if (memory.getReadLatency() != 0)
      return;
    OpBuilder builder(memory);
    for (auto [index, port] : llvm::enumerate(memory.getResults())) {
      if (memory.getPortKind(index) != firrtl::MemOp::PortKind::Read)
        continue;
      SmallVector<firrtl::SubfieldOp> enables;
      bool leafUsers = true;
      for (auto *user : port.getUsers()) {
        if (auto field = dyn_cast<firrtl::SubfieldOp>(user)) {
          if (field.getFieldName() == "en")
            enables.push_back(field);
        } else {
          leafUsers = false;
        }
      }
      // Whole-port aggregate connections can also drive en. Preserve their
      // original lowering rather than introduce a competing constant driver.
      if (!leafUsers || enables.empty())
        continue;
      // Retain the original drivers and conditional connects on a dead sink
      // instead of rewriting control flow. Only the memory input is constant.
      builder.setInsertionPoint(memory);
      auto enable = builder.create<firrtl::WireOp>(
          memory.getLoc(), enables.front().getType());
      for (auto field : enables) {
        field.replaceAllUsesWith(enable.getResult());
        field.erase();
      }
      builder.setInsertionPointAfter(memory);
      auto input = builder.create<firrtl::SubfieldOp>(memory.getLoc(), port, "en");
      auto one = builder.create<firrtl::ConstantOp>(
          memory.getLoc(), firrtl::UIntType::get(memory.getContext(), 1),
          APInt(1, 1));
      builder.create<firrtl::StrictConnectOp>(memory.getLoc(), input, one);
    }
  });
}

// SFC's one-cycle read/write port captures DATA whenever enabled (even during
// a write) and otherwise holds it. CIRCT's default address pipeline instead
// returns X for disabled/write responses. Match SFC's choice for undefined RUW
// and its explicit old-data behavior using an asynchronous reader plus an
// enabled data register. Nonblocking writes still see the same edge and mask.
// Handle ground integer RW ports, including mixed R/W/RW memories; retain symbolic
// or annotated memories and aggregate users whose port correspondence must remain
// intact. This operates only on the private backend clone.
static void normalizeSynchronousReadWritePorts(ModuleOp module) {
  SmallVector<firrtl::MemOp> memories;
  module.walk([&](firrtl::MemOp memory) { memories.push_back(memory); });
  for (auto memory : memories) {
    // Mixed R/W/RW memories must not bypass both synchronous read refinements.
    // Split their RW accessors but retain global readLatency=1 here: the next
    // reader normalization captures pre-edge DATA for every original reader
    // and every split RW reader together before setting that latency to zero.
    if (memory.getReadLatency() == 1 && memory.getWriteLatency() == 1 &&
        memory.getNumResults() > 1 &&
        memory.getRuw() != firrtl::RUWAttr::New &&
        !memory.getInnerSymAttr() && memory.getAnnotations().empty()) {
      auto dataType = dyn_cast<firrtl::IntType>(memory.getDataType());
      bool supported = dataType && dataType.getWidth() > 0 &&
                       memory.getMaskBits() == 1;
      bool hasReadWriter = false;
      for (auto [i, port] : llvm::enumerate(memory.getResults())) {
        auto kind = memory.getPortKind(i);
        hasReadWriter |= kind == firrtl::MemOp::PortKind::ReadWrite;
        if (kind == firrtl::MemOp::PortKind::Debug)
          supported = false;
        if (kind != firrtl::MemOp::PortKind::Write &&
            llvm::any_of(port.getUsers(), [](Operation *user) {
              return !isa<firrtl::SubfieldOp>(user);
            }))
          supported = false;
      }
      for (auto annotations : memory.getPortAnnotations())
        if (!cast<ArrayAttr>(annotations).empty()) supported = false;
      if (!supported || !hasReadWriter) continue;

      OpBuilder builder(memory);
      auto loc = memory.getLoc();
      circt::Namespace portNames;
      for (unsigned i = 0; i < memory.getNumResults(); ++i)
        portNames.newName(memory.getPortNameStr(i));
      SmallVector<Type> types;
      SmallVector<Attribute> names;
      SmallVector<unsigned> indices;
      for (auto [i, port] : llvm::enumerate(memory.getResults())) {
        indices.push_back(types.size());
        auto name = memory.getPortNameStr(i);
        if (memory.getPortKind(i) != firrtl::MemOp::PortKind::ReadWrite) {
          types.push_back(port.getType());
          names.push_back(builder.getStringAttr(name));
          continue;
        }
        for (auto [kind, suffix] : {
                 std::pair{firrtl::MemOp::PortKind::Read, "_read"},
                 std::pair{firrtl::MemOp::PortKind::Write, "_write"}}) {
          types.push_back(firrtl::MemOp::getTypeForPort(memory.getDepth(), dataType, kind));
          names.push_back(builder.getStringAttr(portNames.newName(name + suffix)));
        }
      }
      auto replacement = builder.create<firrtl::MemOp>(
          loc, types, 1, 1, memory.getDepth(), memory.getRuw(), names,
          memory.getName(), memory.getNameKind());
      for (auto attribute : memory->getAttrs()) {
        auto name = attribute.getName().strref();
        if (name != "portNames" && name != "portAnnotations")
          replacement->setAttr(attribute.getName(), attribute.getValue());
      }
      for (auto [i, port] : llvm::enumerate(memory.getResults())) {
        auto reader = replacement.getResult(indices[i]);
        if (memory.getPortKind(i) != firrtl::MemOp::PortKind::ReadWrite) {
          port.replaceAllUsesWith(reader);
          continue;
        }
        llvm::StringMap<Value> proxies;
        for (auto element : cast<firrtl::BundleType>(port.getType()).getElements())
          proxies[element.name.getValue()] =
              builder.create<firrtl::WireOp>(loc, element.type).getResult();
        for (auto *user : llvm::make_early_inc_range(port.getUsers())) {
          auto field = cast<firrtl::SubfieldOp>(user);
          field.replaceAllUsesWith(proxies.lookup(field.getFieldName()));
          field.erase();
        }
        auto field = [&](Value value, StringRef name) -> Value {
          return builder.create<firrtl::SubfieldOp>(loc, value, name);
        };
        auto connect = [&](Value dest, Value source) {
          builder.create<firrtl::StrictConnectOp>(loc, dest, source);
        };
        auto writer = replacement.getResult(indices[i] + 1);
        for (auto accessor : {reader, writer}) {
          connect(field(accessor, "clk"), proxies.lookup("clk"));
          connect(field(accessor, "addr"), proxies.lookup("addr"));
        }
        connect(field(reader, "en"), proxies.lookup("en"));
        connect(field(writer, "en"), builder.create<firrtl::AndPrimOp>(
            loc, proxies.lookup("en"), proxies.lookup("wmode")));
        connect(field(writer, "data"), proxies.lookup("wdata"));
        connect(field(writer, "mask"), proxies.lookup("wmask"));
        connect(proxies.lookup("rdata"), field(reader, "data"));
      }
      memory.erase();
      continue;
    }
    if (memory.getReadLatency() != 1 || memory.getWriteLatency() != 1 ||
        memory.getNumResults() != 1 ||
        memory.getPortKind(0) != firrtl::MemOp::PortKind::ReadWrite ||
        memory.getRuw() == firrtl::RUWAttr::New ||
        memory.getInnerSymAttr() || !memory.getAnnotations().empty())
      continue;
    auto dataType = dyn_cast<firrtl::IntType>(memory.getDataType());
    if (!dataType || dataType.getWidth() <= 0 || memory.getMaskBits() != 1 ||
        llvm::any_of(memory.getPortAnnotations(), [](Attribute annotations) {
          return !cast<ArrayAttr>(annotations).empty();
        }) ||
        llvm::any_of(memory.getResult(0).getUsers(), [](Operation *user) {
          return !isa<firrtl::SubfieldOp>(user);
        }))
      continue;

    OpBuilder builder(memory);
    auto loc = memory.getLoc();
    // Keep original drivers/observations on leaf wires, including conditional
    // connections. Leaf proxies also work after LowerTypes has already run;
    // introducing a duplex bundle at that boundary would violate Low FIRRTL.
    llvm::StringMap<Value> proxies;
    auto portType = cast<firrtl::BundleType>(memory.getResult(0).getType());
    for (auto element : portType.getElements())
      proxies[element.name.getValue()] =
          builder.create<firrtl::WireOp>(loc, element.type).getResult();
    for (auto *user : llvm::make_early_inc_range(memory.getResult(0).getUsers())) {
      auto oldField = cast<firrtl::SubfieldOp>(user);
      oldField.replaceAllUsesWith(proxies.lookup(oldField.getFieldName()));
      oldField.erase();
    }
    SmallVector<Type> types{
        firrtl::MemOp::getTypeForPort(memory.getDepth(), dataType,
                                    firrtl::MemOp::PortKind::Read),
        firrtl::MemOp::getTypeForPort(memory.getDepth(), dataType,
                                    firrtl::MemOp::PortKind::Write)};
    auto portName = memory.getPortNameStr(0);
    SmallVector<Attribute> names{
        builder.getStringAttr((portName + "_read").str()),
        builder.getStringAttr((portName + "_write").str())};
    auto replacement = builder.create<firrtl::MemOp>(
        loc, types, 0, 1, memory.getDepth(), firrtl::RUWAttr::Undefined,
        names, memory.getName(), memory.getNameKind());
    // Preserve initialization, prefix and any non-structural backend metadata.
    for (auto attribute : memory->getAttrs()) {
      auto name = attribute.getName().strref();
      if (name != "portNames" && name != "portAnnotations" &&
          name != "readLatency" && name != "ruw")
        replacement->setAttr(attribute.getName(), attribute.getValue());
    }
    auto field = [&](Value port, StringRef name) -> Value {
      return builder.create<firrtl::SubfieldOp>(loc, port, name);
    };
    auto input = [&](StringRef name) { return proxies.lookup(name); };
    auto connect = [&](Value dest, Value source) {
      builder.create<firrtl::StrictConnectOp>(loc, dest, source);
    };
    auto reader = replacement.getResult(0);
    auto writer = replacement.getResult(1);
    auto clock = input("clk"), enable = input("en"), address = input("addr");
    for (auto port : {reader, writer}) {
      connect(field(port, "clk"), clock);
      connect(field(port, "addr"), address);
    }
    connect(field(reader, "en"), builder.create<firrtl::ConstantOp>(
        loc, firrtl::UIntType::get(memory.getContext(), 1), APInt(1, 1)));
    connect(field(writer, "en"), builder.create<firrtl::AndPrimOp>(
        loc, enable, input("wmode")));
    connect(field(writer, "data"), input("wdata"));
    connect(field(writer, "mask"), input("wmask"));
    auto data = builder.create<firrtl::RegOp>(
        loc, dataType, clock,
        (memory.getName() + "_" + portName + "_rdata").str()).getResult();
    connect(data, builder.create<firrtl::MuxPrimOp>(
        loc, enable, field(reader, "data"), data));
    connect(input("rdata"), data);
    memory.erase();
  }
}

// SFC also captures data for independent one-cycle readers. An address register
// followed by a combinational array read observes the post-edge write on a
// collision, and loses the held data when disabled. Refine Undefined/Old RUW
// to SFC's pre-edge data capture without changing any writer, mask or clock.
// Run after InferReadWrite so combined ports retain the separate policy above.
static void normalizeSynchronousReadPorts(
    ModuleOp module, SmallVectorImpl<NativeMemoryOrigin> &origins) {
  SmallVector<firrtl::MemOp> memories;
  module.walk([&](firrtl::MemOp memory) { memories.push_back(memory); });
  for (auto memory : memories) {
    if (memory.getReadLatency() != 1 || memory.getWriteLatency() != 1 ||
        memory.getRuw() == firrtl::RUWAttr::New ||
        memory.getInnerSymAttr() || !memory.getAnnotations().empty())
      continue;
    auto dataType = dyn_cast<firrtl::IntType>(memory.getDataType());
    if (!dataType || dataType.getWidth() <= 0 ||
        llvm::any_of(memory.getPortAnnotations(), [](Attribute annotations) {
          return !cast<ArrayAttr>(annotations).empty();
        }))
      continue;
    SmallVector<unsigned> readers;
    bool supported = true;
    for (unsigned i = 0; i < memory.getNumResults(); ++i) {
      if (memory.getPortKind(i) == firrtl::MemOp::PortKind::ReadWrite) {
        supported = false;
        break;
      }
      if (memory.getPortKind(i) == firrtl::MemOp::PortKind::Read) {
        if (llvm::any_of(memory.getResult(i).getUsers(), [](Operation *user) {
              return !isa<firrtl::SubfieldOp>(user);
            }))
          supported = false;
        readers.push_back(i);
      }
    }
    if (!supported || readers.empty())
      continue;
    OpBuilder builder(memory);
    auto loc = memory.getLoc();
    for (auto i : readers) {
      builder.setInsertionPoint(memory);
      auto port = memory.getResult(i);
      // Redirect old leaf uses first, retaining conditional enable drivers and
      // output observations. Then install the new asynchronous reader inputs.
      auto type = cast<firrtl::BundleType>(port.getType());
      llvm::StringMap<Value> proxies;
      for (auto element : type.getElements())
        proxies[element.name.getValue()] =
            builder.create<firrtl::WireOp>(loc, element.type).getResult();
      for (auto *user : llvm::make_early_inc_range(port.getUsers())) {
        auto field = cast<firrtl::SubfieldOp>(user);
        field.replaceAllUsesWith(proxies.lookup(field.getFieldName()));
        field.erase();
      }
      builder.setInsertionPointAfter(memory);
      auto field = [&](StringRef name) -> Value {
        return builder.create<firrtl::SubfieldOp>(loc, port, name);
      };
      auto connect = [&](Value dest, Value src) {
        builder.create<firrtl::StrictConnectOp>(loc, dest, src);
      };
      connect(field("clk"), proxies.lookup("clk"));
      connect(field("addr"), proxies.lookup("addr"));
      connect(field("en"), builder.create<firrtl::ConstantOp>(
          loc, firrtl::UIntType::get(memory.getContext(), 1), APInt(1, 1)));
      // No random_init_start: SeqToSV appends this new state after existing
      // initialization slots, as for the combined-port refinement above.
      auto data = builder.create<firrtl::RegOp>(
          loc, dataType, proxies.lookup("clk"),
          (memory.getName() + "_" + memory.getPortNameStr(i) + "_rdata").str())
                      .getResult();
      recordNativeReadCapture(memory,
          data.getDefiningOp<firrtl::RegOp>().getName(), origins);
      connect(data, builder.create<firrtl::MuxPrimOp>(
          loc, proxies.lookup("en"), field("data"), data));
      connect(proxies.lookup("data"), data);
    }
    memory.setReadLatency(0);
  }
}

LogicalResult goldengate::emitSimulatorRTL(ModuleOp source,
                                          StringRef inputFilename,
                                          StringRef outputFilename,
                                          std::string &error,
                                          StringRef memoryOriginsFilename) {
  auto reject = [&](StringRef reason) {
    error = reason.str();
    return failure();
  };
  if (outputFilename.empty())
    return reject("simulator RTL requires an output filename");
  auto circuits = source.getOps<firrtl::CircuitOp>();
  if (std::distance(circuits.begin(), circuits.end()) != 1 ||
      failed(verify(source)))
    return reject("simulator RTL requires one valid FIRRTL circuit");

  auto *context = source.getContext();
  bool printOperations = context->shouldPrintOpOnDiagnostic();
  context->printOpOnDiagnostic(false);
  auto restoreDiagnostics = llvm::make_scope_exit(
      [&] { context->printOpOnDiagnostic(printOperations); });
  context->loadDialect<firrtl::FIRRTLDialect, chirrtl::CHIRRTLDialect,
                       hw::HWDialect, comb::CombDialect, seq::SeqDialect,
                       sv::SVDialect, om::OMDialect, debug::DebugDialect>();
  OwningOpRef<ModuleOp> lowered = cast<ModuleOp>(source->clone());
  // rawAnnotations is Golden Gate's serialized archive, including classes
  // already interpreted by its analyses and collateral writers. Replaying it
  // through LowerAnnotations would resolve pre-rewrite targets a second time.
  // Never remove attached operation/port annotations or the source archive.
  auto circuit = *lowered->getOps<firrtl::CircuitOp>().begin();
  if (failed(attachInlineBlackBoxes(circuit, outputFilename, error)))
    return failure();
  SmallVector<ClockSnapshotIdentity> snapshots;
  llvm::StringMap<llvm::StringSet<>> marked;
  SmallVector<NativeMemoryOrigin> memoryOrigins;
  if (failed(collectNativeMemoryOrigins(circuit, memoryOrigins, error)))
    return failure();
  const std::string originRoot = circuit.getName().str();
  bool invalidSnapshot = false;
  circuit.walk([&](Operation *op) {
    auto mark = op->getAttr("goldengate.clockSnapshot");
    if (!mark) return;
    auto reg = dyn_cast<firrtl::RegOp>(op);
    auto parent = op->getParentOfType<firrtl::FModuleOp>();
    auto type = reg ? dyn_cast<firrtl::UIntType>(reg.getResult().getType()) : firrtl::UIntType();
    if (!isa<UnitAttr>(mark) || !reg || !parent || !type || type.getWidth() != 64 ||
        (reg.getName() != "hCycle_mmreg" && reg.getName() != "tCycle_mmreg") ||
        !marked[parent.getName()].insert(reg.getName()).second) {
      invalidSnapshot = true;
      return;
    }
    snapshots.push_back({parent.getName().str(), reg.getName().str()});
    // Names and types are captured explicitly before standard lowering; the
    // backend does not depend on custom attributes surviving FIRRTL conversion.
    op->removeAttr("goldengate.clockSnapshot");
  });
  for (const auto &entry : marked)
    invalidSnapshot |= entry.second.size() != 2;
  if (invalidSnapshot)
    return reject("ClockBridge snapshot marks require the exact two unreset UInt64 registers per module");
  circuit->removeAttr("rawAnnotations");
  normalizeSynchronousReadWritePorts(*lowered);
  normalizeAsynchronousReadEnables(*lowered);
  // Keep the exclusions above for RW ports already present in the source
  // (notably whole-port connections). The late rewrite below is for ports
  // newly formed by firtool's InferReadWrite, not a second attempt to rewrite
  // an explicitly excluded source port. InferReadWrite replaces separate R/W
  // memories. Use a private attached annotation because standard memory passes
  // preserve annotations even when recreating/unmasking a source RW memory.
  lowered->walk([](firrtl::MemOp memory) {
    for (unsigned i = 0; i < memory.getNumResults(); ++i)
      if (memory.getPortKind(i) == firrtl::MemOp::PortKind::ReadWrite) {
        OpBuilder builder(memory);
        SmallVector<Attribute> annotations(memory.getAnnotations().begin(),
                                            memory.getAnnotations().end());
        annotations.push_back(builder.getDictionaryAttr({builder.getNamedAttr(
            "class", builder.getStringAttr("goldengate.PreserveRWLowering"))}));
        memory.setAnnotationsAttr(builder.getArrayAttr(annotations));
        break;
      }
  });

  // SFC emits width-declared mux temporaries. Keep that boundary in CIRCT's
  // exporter: inlining a narrow mux in an array index gives it a wider
  // SystemVerilog expression context and produces Verilator width diagnostics.
  // Explicit casts likewise retain the bit widths established by FIRRTL/HW.
  LoweringOptions lowering(*lowered);
  lowering.disallowMuxInlining = true;
  lowering.explicitBitcast = true;
  // Keep the complete index expression width-declared as well. A spilled mux
  // alone does not protect a narrow OR tree from a wider SV bit-select context.
  // Use CIRCT's standard index spilling, including its Vivado keep attribute,
  // so the U250 synthesis flow retains this boundary too.
  lowering.mitigateVivadoArrayIndexConstPropBug = true;
  lowering.setAsAttribute(*lowered);

  firtool::FirtoolOptions options;
  options.setOutputFilename(outputFilename)
      .setNoDedup(true)
      .setDisableHoistingHWPassthrough(true)
      .setBlackBoxRootPath(llvm::sys::path::parent_path(outputFilename));
  std::string rtl;
  llvm::raw_string_ostream out(rtl);
  PassManager passes(context);
  if (failed(firtool::populatePreprocessTransforms(passes, options)) ||
      failed(firtool::populateCHIRRTLToLowFIRRTL(passes, options, inputFilename)))
    return reject("cannot construct CIRCT simulator RTL pipeline");
  if (failed(passes.run(*lowered)))
    return reject("CIRCT simulator RTL lowering failed; see pass diagnostics");
  // Real Directory inputs have separate R/W ports. Only firtool's standard
  // InferReadWrite stage creates the combined port whose data-capture choice
  // we refine. Keep every standard compiler stage, and apply this refinement
  // at the FIRRTL/HW boundary where inferred ports are finally available.
  // Existing registers retain their random_init_start slots. New data regs
  // deliberately have no slot: standard SeqToSV appends unassigned registers
  // above the existing maximum instead of renumbering surviving source regs.
  normalizeSynchronousReadWritePorts(*lowered);
  normalizeSynchronousReadPorts(*lowered, memoryOrigins);
  lowered->walk([](firrtl::MemOp memory) {
    SmallVector<Attribute> annotations;
    for (auto annotation : memory.getAnnotations()) {
      auto cls = cast<DictionaryAttr>(annotation).getAs<StringAttr>("class");
      if (!cls || cls.getValue() != "goldengate.PreserveRWLowering")
        annotations.push_back(annotation);
    }
    memory.setAnnotationsAttr(ArrayAttr::get(memory.getContext(), annotations));
  });
  PassManager backend(context);
  if (failed(firtool::populateLowFIRRTLToHW(backend, options)) ||
      failed(firtool::populateHWToSV(backend, options)))
    return reject("cannot construct CIRCT simulator RTL pipeline");
  if (failed(backend.run(*lowered)))
    return reject("CIRCT simulator RTL lowering failed; see pass diagnostics");
  unsigned snapshotsChanged;
  if (failed(normalizeClockSnapshotInitialization(*lowered, snapshots,
                                                 snapshotsChanged, error)))
    return failure();
  if (failed(attachMemoryInitializationFragments(*lowered, error)))
    return failure();
  normalizeMemoryInitialization(*lowered);
  normalizeInitializationIndices(*lowered);
  terminateUnusedInstanceOutputs(*lowered);
  if (failed(verify(*lowered)))
    return reject("CIRCT simulator RTL normalization produced invalid IR");

  // The single-file exporter includes emit.file payloads in its stream, even
  // resource lists that are not Verilog. Export those operations separately
  // through CIRCT's file exporter, leaving only simulator RTL in this stream.
  OwningOpRef<ModuleOp> collateral = ModuleOp::create(source.getLoc());
  for (auto &op : llvm::make_early_inc_range(*lowered->getBody()))
    if (isa<emit::FileOp, emit::FileListOp>(op))
      op.moveBefore(collateral->getBody(), collateral->getBody()->end());
  PassManager exportPasses(context);
  if (failed(firtool::populateExportVerilog(exportPasses, options, out)) ||
      failed(exportPasses.run(*lowered)))
    return reject("CIRCT simulator RTL export failed; see pass diagnostics");
  out.flush();
  llvm::json::Object memoryOriginDescription;
  if (!memoryOriginsFilename.empty() &&
      failed(describeNativeMemoryOrigins(*lowered, originRoot, memoryOrigins, rtl,
                                         memoryOriginDescription, error)))
    return failure();
  if (!collateral->getBody()->empty() &&
      failed(exportSplitVerilog(*collateral,
                               llvm::sys::path::parent_path(outputFilename))))
    return reject("CIRCT blackbox collateral export failed; see diagnostics");

  llvm::SmallString<256> originsTemporary;
  if (!memoryOriginsFilename.empty()) {
    if (memoryOriginsFilename == outputFilename)
      return reject("memory origin collateral must have a separate output path");
    int originsFD;
    auto ec = llvm::sys::fs::createUniqueFile(memoryOriginsFilename + ".tmp-%%%%%%",
                                            originsFD, originsTemporary);
    if (ec) return reject("cannot open native memory origin output: " + ec.message());
    llvm::raw_fd_ostream file(originsFD, true);
    file << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(memoryOriginDescription)));
    file.close();
    if (file.has_error()) {
      error = "cannot finish native memory origin output: " + file.error().message();
      file.clear_error();
      llvm::sys::fs::remove(originsTemporary);
      return failure();
    }
  }
  auto cleanupOrigins = llvm::make_scope_exit([&] {
    if (!originsTemporary.empty()) llvm::sys::fs::remove(originsTemporary);
  });
  llvm::SmallString<256> temporary;
  int fd;
  auto ec = llvm::sys::fs::createUniqueFile(outputFilename + ".tmp-%%%%%%",
                                           fd, temporary);
  if (ec)
    return reject("cannot open simulator RTL output: " + ec.message());
  llvm::raw_fd_ostream file(fd, true);
  file << rtl;
  file.close();
  if (file.has_error()) {
    error = "cannot finish simulator RTL output: " + file.error().message();
    file.clear_error();
    llvm::sys::fs::remove(temporary);
    return failure();
  }
  ec = llvm::sys::fs::rename(temporary, outputFilename);
  if (ec) {
    llvm::sys::fs::remove(temporary);
    return reject("cannot publish simulator RTL output: " + ec.message());
  }
  if (!memoryOriginsFilename.empty()) {
    ec = llvm::sys::fs::rename(originsTemporary, memoryOriginsFilename);
    if (ec) return reject("cannot publish native memory origin output: " + ec.message());
  }
  return success();
}
