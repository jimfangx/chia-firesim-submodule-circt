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
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>

using namespace mlir;
using namespace circt;

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
    if (body.size() != 4 && body.size() != 5)
      return;
    auto chunks = dyn_cast<sv::ForOp>(body[0]);
    auto element = dyn_cast<sv::ArrayIndexInOutOp>(body[body.size() - 3]);
    auto word = dyn_cast<sv::ReadInOutOp>(body[body.size() - 2]);
    auto store = dyn_cast<sv::BPAssignOp>(body.back());
    if (!chunks || !element || !word || !store ||
        store.getDest() != element.getResult() || store.getSrc() != word.getResult())
      return;
    auto memory = element.getInput().getDefiningOp<sv::RegOp>();
    auto temporary = word.getInput().getDefiningOp<sv::RegOp>();
    if (!memory || !temporary || memory.getName() != "Memory" ||
        temporary.getName() != "_RANDOM_MEM")
      return;
    auto array = dyn_cast<hw::UnpackedArrayType>(memory.getElementType());
    auto bits = dyn_cast<IntegerType>(temporary.getElementType());
    if (!array || !bits || bits.getWidth() <= 32 ||
        array.getElementType() != bits || !array.getNumElements() ||
        !isConstant(rows.getLowerBound(), 0) ||
        !isConstant(rows.getUpperBound(), array.getNumElements()) ||
        !isConstant(rows.getStep(), 1) ||
        !isConstant(chunks.getLowerBound(), 0) ||
        !isConstant(chunks.getUpperBound(), bits.getWidth()) ||
        !isConstant(chunks.getStep(), 32))
      return;
    Value index = rows.getInductionVar();
    if (body.size() == 5) {
      auto extract = dyn_cast<comb::ExtractOp>(body[1]);
      if (!extract || extract.getInput() != index || extract.getLowBit() != 0 ||
          extract.getResult().getType().getIntOrFloatBitWidth() !=
              std::max(1u, llvm::Log2_64_Ceil(array.getNumElements())))
        return;
      index = extract.getResult();
    }
    if (element.getIndex() != index)
      return;
    SmallVector<Operation *> chunkBody;
    for (auto &op : chunks->getRegion(0).front())
      chunkBody.push_back(&op);
    if (chunkBody.size() != 3)
      return;
    auto random = dyn_cast<sv::MacroRefExprSEOp>(chunkBody[0]);
    auto part = dyn_cast<sv::IndexedPartSelectInOutOp>(chunkBody[1]);
    auto fill = dyn_cast<sv::BPAssignOp>(chunkBody[2]);
    if (!random || random.getMacroName() != "RANDOM" || random.getNumOperands() ||
        random.getResult().getType() != IntegerType::get(module.getContext(), 32) ||
        !part || !fill || part.getDecrement() ||
        part.getInput() != temporary.getResult() ||
        part.getBase() != chunks.getInductionVar() ||
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
    if (!child || !child.getName().starts_with("GG") ||
        !child.getName().ends_with("Wrapper")) break;
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

LogicalResult goldengate::emitSimulatorRTL(ModuleOp source,
                                          StringRef inputFilename,
                                          StringRef outputFilename,
                                          std::string &error) {
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
  circuit->removeAttr("rawAnnotations");

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
      failed(firtool::populateCHIRRTLToLowFIRRTL(passes, options, inputFilename)) ||
      failed(firtool::populateLowFIRRTLToHW(passes, options)) ||
      failed(firtool::populateHWToSV(passes, options)))
    return reject("cannot construct CIRCT simulator RTL pipeline");
  if (failed(passes.run(*lowered)))
    return reject("CIRCT simulator RTL lowering failed; see pass diagnostics");
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
  if (!collateral->getBody()->empty() &&
      failed(exportSplitVerilog(*collateral,
                               llvm::sys::path::parent_path(outputFilename))))
    return reject("CIRCT blackbox collateral export failed; see diagnostics");

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
  return success();
}
