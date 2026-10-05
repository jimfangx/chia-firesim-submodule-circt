// See LICENSE for license details.
#include "goldengate/PrintBridgePayload.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringSet.h"
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializePrintBridgeControls(
    CircuitOp circuit, llvm::ArrayRef<FModuleOp> payloads,
    llvm::ArrayRef<FModuleOp> stages, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (payloads.size() != stages.size())
    return reject("PrintBridge control requires one token stage per payload");
  llvm::StringSet<> names;
  std::set<std::pair<std::string, std::string>> identities;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    if (auto m = dyn_cast<FModuleLike>(&op)) names.insert(m.getModuleName());
    if (auto prior = op.getAttrOfType<DictionaryAttr>("goldengate.printControl")) {
      auto target = prior.getAs<StringAttr>("bridgeTarget");
      auto reset = prior.getAs<StringAttr>("resetPortName");
      if (target && reset) identities.emplace(target.getValue().str(), reset.getValue().str());
    }
  }
  OpBuilder b(circuit.getContext());
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w); };
  const char *stageNames[] = {"hostClock", "hostReset", "doneInit", "hValid", "enable",
      "flushNarrowPacket", "bufferReady", "payloadValid", "payloadData",
      "hReady", "tokenValid", "tokenData"};
  // Check interface and identity, including the copied layout. Never pair
  // same-width modules from different clock domains or constructor orders.
  for (unsigned i = 0; i < payloads.size(); ++i) {
    auto payload = payloads[i], stage = stages[i];
    if (!payload || !stage || payload->getParentOp() != circuit || stage->getParentOp() != circuit)
      return reject("PrintBridge control requires materialized modules in this circuit");
    auto layout = payload->getAttrOfType<DictionaryAttr>("goldengate.printPayload");
    auto token = stage->getAttrOfType<DictionaryAttr>("goldengate.printTokenStage");
    auto target = layout ? layout.getAs<StringAttr>("bridgeTarget") : StringAttr();
    auto reset = layout ? layout.getAs<StringAttr>("resetPortName") : StringAttr();
    auto bits = layout ? layout.getAs<IntegerAttr>("tokenBits") : IntegerAttr();
    auto owner = token ? token.getAs<StringAttr>("payloadModule") : StringAttr();
    if (!target || !reset || target.getValue().empty() || reset.getValue().empty() ||
        !bits || bits.getInt() < 8 || bits.getInt() > (1LL << 30) ||
        (bits.getInt() & (bits.getInt() - 1)) || !owner || owner.getValue() != payload.getName() ||
        payload.getNumPorts() != 3 || stage.getNumPorts() != 12 ||
        payload.getPortName(0) != "hBits" || payload.getPortDirection(0) != Direction::In ||
        !isa<BundleType>(payload.getPortType(0)) ||
        !cast<FIRRTLBaseType>(payload.getPortType(0)).isPassive() ||
        payload.getPortName(1) != "valid" || payload.getPortDirection(1) != Direction::Out ||
        payload.getPortType(1) != uint(1) || payload.getPortName(2) != "data" ||
        payload.getPortDirection(2) != Direction::Out || payload.getPortType(2) != uint(bits.getInt()))
      return reject("PrintBridge control payload/stage interface or identity mismatch");
    for (auto field : layout)
      if (token.get(field.getName()) != field.getValue())
        return reject("PrintBridge control stage changed its payload layout");
    for (unsigned p = 0; p < 12; ++p) {
      Type expected = p == 0 ? Type(ClockType::get(circuit.getContext())) :
          Type(uint(p == 8 || p == 11 ? bits.getInt() : 1));
      if (stage.getPortName(p) != stageNames[p] || stage.getPortType(p) != expected ||
          stage.getPortDirection(p) != (p < 9 ? Direction::In : Direction::Out))
        return reject("PrintBridge control token stage interface mismatch");
    }
    if (!identities.emplace(target.getValue().str(), reset.getValue().str()).second)
      return reject("PrintBridge control target/reset identity already materialized or duplicated");
  }
  auto loc = circuit.getLoc();
  for (unsigned i = 0; i < payloads.size(); ++i) {
    auto payload = payloads[i], stage = stages[i];
    std::string name = "GGPrintBridgeControl";
    for (unsigned suffix = 1; names.count(name); ++suffix)
      name = "GGPrintBridgeControl_" + std::to_string(suffix);
    names.insert(name);
    SmallVector<PortInfo> ports;
    auto port = [&](llvm::StringRef n, Type t, Direction d) {
      ports.push_back({b.getStringAttr(n), t, d});
    };
    port("hostClock", ClockType::get(circuit.getContext()), Direction::In);
    for (auto n : {"hostReset", "doneInit", "hValid", "flushNarrowPacket", "bufferReady"})
      port(n, uint(1), Direction::In);
    for (auto n : {"startCycleL", "startCycleH", "endCycleL", "endCycleH"})
      port(n, uint(32), Direction::In);
    port("hBits", payload.getPortType(0), Direction::In);
    for (auto n : {"hReady", "fromHostValid", "tokenValid"}) port(n, uint(1), Direction::Out);
    port("tokenData", payload.getPortType(2), Direction::Out);
    port("currentCycle", uint(64), Direction::Out);
    port("enable", uint(1), Direction::Out);
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto m = b.create<FModuleOp>(loc, b.getStringAttr(name),
        ConventionAttr::get(circuit.getContext(), Convention::Internal), ports);
    NamedAttrList metadata(payload->getAttrOfType<DictionaryAttr>("goldengate.printPayload"));
    metadata.set("payloadModule", b.getStringAttr(payload.getName()));
    metadata.set("tokenStageModule", b.getStringAttr(stage.getName()));
    metadata.set("cycleBits", b.getI64IntegerAttr(64));
    metadata.set("roiInclusive", b.getBoolAttr(true));
    m->setAttr("goldengate.printControl", metadata.getDictionary(circuit.getContext()));
    b.setInsertionPointToStart(m.getBodyBlock());
    auto arg = [&](unsigned p) { return m.getArgument(p); };
    auto connect = [&](Value d, Value v) { b.create<StrictConnectOp>(loc, d, v); };
    auto k = [&](unsigned w, uint64_t v) -> Value {
      return b.create<ConstantOp>(loc, uint(w), APInt(w, v));
    };
    auto pack = b.create<InstanceOp>(loc, payload, "payload");
    auto tokens = b.create<InstanceOp>(loc, stage, "tokens");
    Value current = b.create<RegResetOp>(loc, uint(64), arg(0), arg(1), k(64, 0), "cycleCounter").getResult();
    Value start = b.create<CatPrimOp>(loc, arg(7), arg(6));
    Value end = b.create<CatPrimOp>(loc, arg(9), arg(8));
    Value enable = b.create<AndPrimOp>(loc, b.create<LEQPrimOp>(loc, start, current),
                                    b.create<LEQPrimOp>(loc, current, end));
    connect(pack.getResult(0), arg(10));
    for (unsigned p = 0; p < 4; ++p) connect(tokens.getResult(p), arg(p));
    connect(tokens.getResult(4), enable);
    connect(tokens.getResult(5), arg(4));
    connect(tokens.getResult(6), arg(5));
    connect(tokens.getResult(7), pack.getResult(1));
    connect(tokens.getResult(8), pack.getResult(2));
    // hReady and tFire are identical in Scala's DecoupledHelper: hValid is
    // included deliberately. Count accepted cycles even outside the ROI.
    Value next = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, current, k(64, 1)), 63, 0);
    connect(current, b.create<MuxPrimOp>(loc, tokens.getResult(9), next, current));
    connect(arg(11), tokens.getResult(9));
    connect(arg(12), k(1, 1));
    connect(arg(13), tokens.getResult(10));
    connect(arg(14), tokens.getResult(11));
    connect(arg(15), current);
    connect(arg(16), enable);
    modules.push_back(m);
  }
  return success();
}
