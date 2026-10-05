// See LICENSE for license details.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/CPUStreamQueue.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include <set>
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
using namespace mlir;
using namespace circt::firrtl;

// CPU stream allocation wraps local hosts without renaming existing targets.
LogicalResult goldengate::materializePrintBridgeHostQueues(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (!circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("PrintBridge host queue requires retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context);
  auto bit = UIntType::get(context, 1), word = UIntType::get(context, 512);
  std::set<std::pair<std::string, std::string>> identities;
  llvm::StringSet<> names;
  for (auto m : circuit.getOps<FModuleLike>()) {
    names.insert(m.getModuleName());
    if (auto prior = m->getAttrOfType<DictionaryAttr>("goldengate.printHost"))
      if (prior.get("queueModule")) {
        auto target = prior.getAs<StringAttr>("bridgeTarget"), reset = prior.getAs<StringAttr>("resetPortName");
        if (target && reset) identities.emplace(target.getValue().str(), reset.getValue().str());
      }
  }
  const char *portNames[] = {"hostClock", "hostReset", "hValid", "hBits", "hReady", "fromHostValid",
      "currentCycle", "enable", "streamReady", "streamValid", "streamData", "ctrl"};
  for (auto host : hosts) {
    if (!host || host->getParentOp() != circuit)
      return reject("PrintBridge host queue inputs must belong to this circuit");
    auto info = host->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    auto target = info ? info.getAs<StringAttr>("bridgeTarget") : StringAttr{};
    auto reset = info ? info.getAs<StringAttr>("resetPortName") : StringAttr{};
    auto width = info ? info.getAs<IntegerAttr>("streamBits") : IntegerAttr{};
    auto tokenBits = info ? info.getAs<IntegerAttr>("tokenBits") : IntegerAttr{};
    auto addressBits = info ? info.getAs<IntegerAttr>("addressBits") : IntegerAttr{};
    auto idBits = info ? info.getAs<IntegerAttr>("idBits") : IntegerAttr{};
    if (!tokenBits || tokenBits.getInt() < 8 || tokenBits.getInt() > (1LL << 30) || !llvm::isPowerOf2_64(tokenBits.getInt()) ||
        !addressBits || addressBits.getInt() < 5 || !idBits || idBits.getInt() <= 0)
      return reject("PrintBridge host queue requires valid token and AXI width metadata");
    if (!target || target.getValue().empty() || !reset || reset.getValue().empty() ||
        !width || width.getInt() != 512 || info.get("queueModule"))
      return reject("PrintBridge host queue requires an unbuffered 512-bit host identity");
    if (!identities.emplace(target.getValue().str(), reset.getValue().str()).second)
      return reject("PrintBridge host queue identity already materialized or duplicated");
    if (host.getNumPorts() != 12)
      return reject("PrintBridge host queue requires the twelve-port local host boundary");
    for (unsigned i = 0; i < 12; ++i) {
      auto p = host.getPorts()[i];
      auto direction = i < 4 || i == 8 || i == 11 ? Direction::In : Direction::Out;
      Type expected = i == 0 ? Type(ClockType::get(context)) :
          i == 6 ? Type(UIntType::get(context, 64)) : i == 10 ? Type(word) : Type(bit);
      if (p.name != portNames[i] || p.direction != direction ||
          (i != 3 && i != 11 && p.type != expected) ||
          ((i == 3 || i == 11) && !isa<BundleType>(p.type)) ||
          (i == 3 && !cast<FIRRTLBaseType>(p.type).isPassive()))
        return reject("PrintBridge host queue port contract mismatch");
    }
  }
  if (hosts.empty()) return success();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  auto unique = [&](StringRef base) {
    std::string name = base.str();
    for (unsigned suffix = 1; names.count(name); ++suffix)
      name = base.str() + "_" + std::to_string(suffix);
    names.insert(name); return name;
  };
  llvm::StringSet<> originalNames;
  for (auto m : circuit.getOps<FModuleLike>()) originalNames.insert(m.getModuleName());
  SmallVector<FModuleOp> buffered;
  for (auto host : hosts) {
    FModuleOp inner;
    for (auto m : staged->getOps<FModuleOp>()) if (m.getName() == host.getName()) inner = m;
    auto queueName = unique("GGPrintBridgeCPUQueue6144");
    auto queue = createCPUStreamQueue6144(*staged, queueName);
    SmallVector<PortInfo> ports(inner.getPorts());
    ports.push_back({b.getStringAttr("streamCount"), UIntType::get(context, 13), Direction::Out});
    b.setInsertionPointToEnd(staged->getBodyBlock());
    auto wrapper = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr(unique("GGPrintBridgeHostQueued")),
        inner.getConventionAttr(), ports);
    b.setInsertionPointToStart(wrapper.getBodyBlock());
    auto producer = b.create<InstanceOp>(circuit.getLoc(), inner, "host");
    auto fifo = b.create<InstanceOp>(circuit.getLoc(), queue, "outgoingQueue");
    auto connect = [&](Value dest, Value src) { b.create<ConnectOp>(circuit.getLoc(), dest, src); };
    auto field = [&](Value value, StringRef name) -> Value { return b.create<SubfieldOp>(circuit.getLoc(), value, name); };
    for (unsigned i = 0; i < 12; ++i) {
      if (i >= 8 && i <= 10) continue;
      Value outer = wrapper.getArgument(i), local = producer.getResult(i);
      connect(inner.getPortDirection(i) == Direction::In ? local : outer,
          inner.getPortDirection(i) == Direction::In ? outer : local);
    }
    connect(fifo.getResult(0), wrapper.getArgument(0));
    connect(fifo.getResult(1), wrapper.getArgument(1));
    connect(producer.getResult(8), field(fifo.getResult(2), "ready"));
    connect(field(fifo.getResult(2), "valid"), producer.getResult(9));
    connect(field(fifo.getResult(2), "bits"), producer.getResult(10));
    connect(field(fifo.getResult(3), "ready"), wrapper.getArgument(8));
    connect(wrapper.getArgument(9), field(fifo.getResult(3), "valid"));
    connect(wrapper.getArgument(10), field(fifo.getResult(3), "bits"));
    connect(wrapper.getArgument(12), fifo.getResult(4));
    NamedAttrList info(inner->getAttrOfType<DictionaryAttr>("goldengate.printHost"));
    info.set("hostModule", b.getStringAttr(inner.getName()));
    info.set("queueModule", b.getStringAttr(queueName));
    info.set("queueDepth", b.getI64IntegerAttr(6144));
    info.set("widthBytes", b.getI64IntegerAttr(64));
    info.set("countBits", b.getI64IntegerAttr(13));
    info.set("synchronousRead", b.getBoolAttr(true));
    info.set("flow", b.getBoolAttr(false)); info.set("pipe", b.getBoolAttr(false));
    wrapper->setAttr("goldengate.printHost", info.getDictionary(context));
    queue->setAttr("goldengate.printCPUQueue", info.getDictionary(context));
    buffered.push_back(wrapper);
  }
  if (failed(verify(*staged))) return reject("PrintBridge host queue composition produced invalid IR");
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleLike>(&op))
      if (!originalNames.count(m.getModuleName())) op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  modules.append(buffered.begin(), buffered.end());
  return success();
}

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
  SmallVector<FModuleOp> queued;
  if (failed(materializePrintBridgeHostQueues(*staged, hosts, queued, error))) return failure();
  if (failed(verify(*staged))) return reject("PrintBridge host composition produced invalid IR");
  // Existing operations and annotation targets keep their exact identities.
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleLike>(&op))
      if (!originalNames.count(m.getModuleName())) op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  modules.append(queued.begin(), queued.end());
  return success();
}
