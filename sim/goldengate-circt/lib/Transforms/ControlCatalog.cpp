// See LICENSE for license details.
// SimulationMapping/PreLinkRenaming oracle: link host definitions without
// capturing target modules or duplicating the already allocated CPU transport.
#include "goldengate/ControlCatalog.h"
#include "goldengate/ControlAddressDecode.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;

namespace {
LogicalResult importCatalog(CircuitOp destination, CircuitOp source,
    ArrayRef<StringRef> roots, ArrayRef<StringRef> preservedRoots, std::string &error,
    const llvm::StringMap<ArrayAttr> &registers = {}) {
  auto reject = [&](Twine why) { error = why.str(); return failure(); };
  if (source == destination || source.getContext() != destination.getContext() || roots.empty())
    return reject("control catalog requires distinct circuits and explicit roots");
  if (failed(verify(source)) || failed(verify(destination)))
    return reject("control catalog requires valid FIRRTL circuits");
  llvm::StringMap<FModuleLike> definitions, existing;
  for (auto m : source.getOps<FModuleLike>()) definitions[m.getModuleName()] = m;
  for (auto m : destination.getOps<FModuleLike>()) existing[m.getModuleName()] = m;
  llvm::StringSet<> rootNames;
  for (auto root : roots)
    if (root.empty() || root == source.getName() || !rootNames.insert(root).second)
      return reject("control catalog roots must be unique host modules");
  llvm::StringMap<std::string> externalNames;
  auto importedName = [&](StringRef name) {
    auto alias = externalNames.find(name);
    if (alias != externalNames.end()) return alias->second;
    return llvm::is_contained(preservedRoots, name) ? name.str() : "CatalogRocket_" + name.str();
  };
  auto equal = [&](Operation *a, Operation *b) {
    OwningOpRef<Operation *> normalized(a->clone());
    // FIRRTL stores declaration locations as port attributes in addition to
    // operation locations. File/line differences do not change the bank ABI.
    normalized->setAttr(FModuleLike::getPortLocationsAttrName(),
        cast<FModuleLike>(b).getPortLocationsAttr());
    return OperationEquivalence::isEquivalentTo(normalized.get(), b,
        OperationEquivalence::IgnoreLocations);
  };
  // A shared bank is safe only when its children are the same definitions too.
  // Comparing just the bank's instance symbols would accept different storage.
  llvm::StringSet<> sharedDone, sharedActive;
  std::function<LogicalResult(StringRef)> shared = [&](StringRef name) {
    if (sharedActive.count(name)) return reject("recursive shared control dependency: " + name);
    if (sharedDone.count(name)) return success();
    auto src = definitions.lookup(name), dst = existing.lookup(name);
    if (!src || !dst || !equal(src, dst))
      return reject("existing control definition differs: " + name);
    sharedActive.insert(name);
    auto result = success();
    src->walk([&](InstanceOp i) {
      if (succeeded(result)) result = shared(i.getModuleName());
    });
    sharedActive.erase(name);
    if (succeeded(result)) sharedDone.insert(name);
    return result;
  };
  // Build and verify privately, then publish only new definitions. Existing
  // handles (including Print hosts held by the caller) survive the transaction.
  OwningOpRef<CircuitOp> candidate(cast<CircuitOp>(destination->clone()));
  llvm::StringMap<SmallVector<FModuleLike>> physicalExternals;
  auto physicalName = [](FModuleLike module) {
    auto def = module->getAttrOfType<StringAttr>("defname");
    return def ? def.getValue() : module.getModuleName();
  };
  // Unrelated parameterized declarations (e.g. plusarg_reader) legitimately
  // share a defname. Only declarations used by this import are compared.
  for (auto m : candidate->getOps<FModuleLike>()) if (isa<FExtModuleOp>(m.getOperation()))
    physicalExternals[physicalName(m)].push_back(m);
  llvm::StringSet<> done, active;
  SmallVector<std::string> added;
  std::function<LogicalResult(StringRef)> import = [&](StringRef name) {
    if (active.count(name)) return reject("recursive control dependency: " + name);
    if (done.count(name)) return success();
    auto src = definitions.lookup(name);
    if (!src || name == source.getName())
      return reject("missing or target-top control dependency: " + name);
    // Symbol renaming does not rename an external module's Verilog defname.
    // Reuse the matching declaration instead of emitting the same blackbox
    // twice; incompatible ports/parameters must fail before publishing.
    if (isa<FExtModuleOp>(src.getOperation())) {
      auto found = physicalExternals.find(physicalName(src));
      if (found != physicalExternals.end()) {
        OwningOpRef<Operation *> copy(src->clone());
        for (auto prior : found->second) {
          SymbolTable::setSymbolName(copy.get(), prior.getModuleName());
          if (!equal(copy.get(), prior)) continue;
          externalNames[name] = prior.getModuleName().str();
          done.insert(name); return success();
        }
        return reject("physical control blackbox differs: " + physicalName(src));
      }
    }
    if (llvm::is_contained(preservedRoots, name) && existing.count(name)) {
      if (failed(shared(name))) return failure();
      done.insert(name); return success();
    }
    active.insert(name);
    OwningOpRef<Operation *> copy(src->clone());
    if (auto rows = registers.lookup(name))
      copy->setAttr("goldengate.mmioRegisters", rows);
    auto symbol = importedName(name);
    SymbolTable::setSymbolName(copy.get(), symbol);
    auto result = success();
    copy->walk([&](InstanceOp i) {
      if (failed(result)) return;
      auto child = i.getModuleName();
      result = import(child);
      if (succeeded(result))
        i.setModuleNameAttr(FlatSymbolRefAttr::get(destination.getContext(), importedName(child)));
    });
    active.erase(name);
    if (failed(result)) return failure();
    if (auto prior = existing.lookup(symbol)) {
      if (!equal(copy.get(), prior)) return reject("control catalog symbol collision: " + symbol);
    } else {
      auto *newModule = copy.release();
      candidate->getBodyBlock()->push_back(newModule);
      if (isa<FExtModuleOp>(newModule))
        physicalExternals[physicalName(cast<FModuleLike>(newModule))].push_back(cast<FModuleLike>(newModule));
      added.push_back(symbol);
    }
    done.insert(name); return success();
  };
  for (auto root : roots) if (failed(import(root))) return failure();
  if (failed(verify(*candidate))) return reject("imported control catalog failed FIRRTL verification");
  SymbolTable symbols(*candidate);
  for (auto &name : added) {
    auto *op = symbols.lookup(name);
    op->remove(); destination.getBodyBlock()->push_back(op);
  }
  return success();
}

} // namespace

LogicalResult goldengate::importControlCatalog(CircuitOp destination,
    CircuitOp source, ArrayRef<StringRef> roots, std::string &error) {
  return importCatalog(destination, source, roots, roots, error);
}

LogicalResult goldengate::importRocketControlCatalog(CircuitOp destination,
    CircuitOp source, std::string &error) {
  const StringRef roots[]{"GGSimulationMasterBank", "GGPeekPokeMCRFile", "GGPeekPokeMMIOBank",
      "GGResetPulseBridgeMCRFile", "GGResetPulseBridge", "GGBlockDevMMIOBank",
      "GGUARTMCRFile", "GGUARTMMIOBank", "GGFASEDLatencyRegisters", "GGFASEDRequestLimits",
      "GGFASEDHistograms", "GGFASEDStatistics", "GGFASEDFunctionalModelRegister", "GGFASEDResponseErrors",
      "GGTracerVMCRFile", "GGTracerVTriggerConfig", "GGTSIMMIOBank", "GGClockBridgeMCRFile",
      "GGSingleClockBridge", "GGLoadMemMCRFile", "GGLoadMemWriteMMIOBank"};
  // Transport wrappers include the target hierarchy. Import the six actual
  // engine dependencies directly, retaining the established private names.
  SmallVector<StringRef> all(roots, roots + std::size(roots));
  all.append({"GGLoadMemWriter", "GGLoadMemRequestQueue2", "GGLoadMemRequestArbiter",
      "GGLoadMemWriteDataFIFO", "GGLoadMemReadRequests", "GGLoadMemReadDataFIFO"});
  // The native construction wrappers own these register descriptors, while
  // the direct engines implement the data path. Retain the checked descriptors
  // on the imported engines without importing the wrappers' target hierarchy.
  ControlMMIOWidget loadmem;
  if (failed(deriveControlMMIOWidget(source, "LoadMemWidget_0", "GGLoadMemMCRFile",
      {"GGLoadMemWriteMMIOBank", "GGLoadMemWriteDataWrapper",
       "GGLoadMemReadRequestWrapper", "GGLoadMemReadDataWrapper"}, loadmem, error)))
    return failure();
  if (loadmem.registerCount != 9) {
    error = "Rocket control catalog requires nine LoadMem register words"; return failure();
  }
  const StringRef wrappers[]{"GGLoadMemWriteDataWrapper", "GGLoadMemReadRequestWrapper", "GGLoadMemReadDataWrapper"};
  const StringRef engines[]{"GGLoadMemWriteDataFIFO", "GGLoadMemReadRequests", "GGLoadMemReadDataFIFO"};
  llvm::StringMap<ArrayAttr> registers;
  for (auto [wrapper, engine] : llvm::zip(wrappers, engines))
    for (auto module : source.getOps<FModuleOp>()) if (module.getName() == wrapper)
      registers[engine] = module->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  return importCatalog(destination, source, all, roots, error, registers);
}
