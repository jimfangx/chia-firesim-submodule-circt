// See LICENSE for license details.
#include "goldengate/PrintBridgePayload.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include <set>
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
using namespace mlir;
using namespace circt::firrtl;

// Composition is transactional: intermediate builders may reject annotations
// targeting a consumed bank. Never leave a partial bridge in the live circuit.
LogicalResult goldengate::materializePrintBridgeHosts(CircuitOp circuit,
    ArrayRef<FModuleOp> controls, unsigned addressBits, unsigned idBits,
    llvm::SmallVectorImpl<FModuleOp> &modules, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (addressBits < 5 || !idBits)
    return reject("PrintBridge host requires five address bits and nonzero IDs");
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("PrintBridge host requires retained annotations");
  for (auto control : controls)
    if (!control || control->getParentOp() != circuit)
      return reject("PrintBridge host controls must belong to this circuit");
  std::set<std::pair<std::string, std::string>> identities;
  for (auto m : circuit.getOps<FModuleOp>())
    if (auto prior = m->getAttrOfType<DictionaryAttr>("goldengate.printHost")) {
      auto target = prior.getAs<StringAttr>("bridgeTarget");
      auto reset = prior.getAs<StringAttr>("resetPortName");
      if (target && reset) identities.emplace(target.getValue().str(), reset.getValue().str());
    }
  for (auto control : controls)
    if (auto layout = control->getAttrOfType<DictionaryAttr>("goldengate.printControl")) {
      auto target = layout.getAs<StringAttr>("bridgeTarget");
      auto reset = layout.getAs<StringAttr>("resetPortName");
      if (target && reset && !identities.emplace(target.getValue().str(), reset.getValue().str()).second)
        return reject("PrintBridge host target/reset identity already materialized or duplicated");
    }
  if (controls.empty()) return success();
  llvm::StringSet<> originalNames;
  for (auto m : circuit.getOps<FModuleLike>()) originalNames.insert(m.getModuleName());
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  SmallVector<FModuleOp> stagedControls, streams, configs, hosts;
  for (auto control : controls)
    for (auto m : staged->getOps<FModuleOp>())
      if (m.getName() == control.getName()) stagedControls.push_back(m);
  if (failed(materializePrintBridgeStreams(*staged, stagedControls, streams, error)) ||
      failed(materializePrintBridgeStreamConfigs(*staged, streams, configs, error)))
    return failure();
  llvm::StringSet<> names;
  for (auto m : staged->getOps<FModuleLike>()) names.insert(m.getModuleName());
  auto unique = [&](StringRef base) {
    std::string name = base.str();
    for (unsigned suffix = 1; names.count(name); ++suffix)
      name = base.str() + "_" + std::to_string(suffix);
    names.insert(name); return name;
  };
  OpBuilder b(circuit.getContext());
  for (auto config : configs) {
    auto wrapperName = unique("GGPrintBridgeHost");
    auto adapterName = unique("GGPrintBridgeHostMCRFile");
    FModuleOp host;
    if (failed(materializePrintBridgeStreamAXI(*staged, config, addressBits, idBits,
          wrapperName, adapterName, host, error)))
      return failure();
    NamedAttrList metadata(config->getAttrOfType<DictionaryAttr>("goldengate.printStreamConfig"));
    metadata.set("configModule", b.getStringAttr(config.getName()));
    metadata.set("mcrModule", b.getStringAttr(adapterName));
    metadata.set("addressBits", b.getI64IntegerAttr(addressBits));
    metadata.set("idBits", b.getI64IntegerAttr(idBits));
    host->setAttr("goldengate.printHost", metadata.getDictionary(circuit.getContext()));
    hosts.push_back(host);
  }
  if (failed(verify(*staged))) return reject("PrintBridge host composition produced invalid IR");
  // Existing operations and annotation targets keep their exact identities.
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleLike>(&op))
      if (!originalNames.count(m.getModuleName())) op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  modules.append(hosts.begin(), hosts.end());
  return success();
}
