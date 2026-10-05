// See LICENSE for license details.
// PrintBridge.scala token control and dataPipe, expressed as FIRRTL SSA/state.
// This boundary sits before DMA packing/serialization and after payload packing.
#include "goldengate/PrintBridgePayload.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializePrintBridgeTokenStages(
    CircuitOp circuit, llvm::ArrayRef<FModuleOp> payloads,
    llvm::SmallVectorImpl<FModuleOp> &modules, std::string &error) {
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  struct Stage { FModuleOp payload; DictionaryAttr layout; unsigned bits, idleBits; };
  SmallVector<Stage> stages;
  llvm::StringSet<> names;
  std::set<std::pair<std::string, std::string>> identities;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    if (auto m = dyn_cast<FModuleLike>(&op)) names.insert(m.getModuleName());
    if (auto prior = op.getAttrOfType<DictionaryAttr>("goldengate.printTokenStage")) {
      auto target = prior.getAs<StringAttr>("bridgeTarget");
      auto reset = prior.getAs<StringAttr>("resetPortName");
      if (target && reset) identities.emplace(target.getValue().str(), reset.getValue().str());
    }
  }
  for (auto payload : payloads) {
    if (!payload) return reject("PrintBridge token stage received a null payload module");
    auto layout = payload->getAttrOfType<DictionaryAttr>("goldengate.printPayload");
    auto target = layout ? layout.getAs<StringAttr>("bridgeTarget") : StringAttr();
    auto reset = layout ? layout.getAs<StringAttr>("resetPortName") : StringAttr();
    auto bits = layout ? layout.getAs<IntegerAttr>("tokenBits") : IntegerAttr();
    auto idle = layout ? layout.getAs<IntegerAttr>("idleCycleBits") : IntegerAttr();
    if (payload->getParentOp() != circuit || !target || !reset || !bits || !idle ||
        target.getValue().empty() || reset.getValue().empty() ||
        bits.getInt() < 8 || bits.getInt() > (1LL << 30) ||
        (bits.getInt() & (bits.getInt() - 1)) ||
        idle.getInt() != std::min<int64_t>(16, bits.getInt()) - 1 ||
        payload.getNumPorts() != 3 || payload.getPortName(1) != "valid" ||
        payload.getPortName(2) != "data" ||
        payload.getPortDirection(1) != Direction::Out ||
        payload.getPortDirection(2) != Direction::Out ||
        payload.getPortType(1) != UIntType::get(circuit.getContext(), 1) ||
        payload.getPortType(2) != UIntType::get(circuit.getContext(), bits.getInt()))
      return reject("PrintBridge token stage requires a materialized sized payload in this circuit");
    if (!identities.emplace(target.getValue().str(), reset.getValue().str()).second)
      return reject("PrintBridge token target/reset identity already materialized or duplicated");
    stages.push_back({payload, layout, unsigned(bits.getInt()), unsigned(idle.getInt())});
  }
  OpBuilder b(circuit.getContext());
  auto loc = circuit.getLoc();
  for (auto &s : stages) {
    std::string name = "GGPrintBridgeTokenStage";
    for (unsigned suffix = 1; names.count(name); ++suffix)
      name = "GGPrintBridgeTokenStage_" + std::to_string(suffix);
    names.insert(name);
    auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w); };
    SmallVector<PortInfo> ports;
    auto port = [&](llvm::StringRef n, Type t, Direction d) {
      ports.push_back({b.getStringAttr(n), t, d});
    };
    port("hostClock", ClockType::get(circuit.getContext()), Direction::In);
    for (auto n : {"hostReset", "doneInit", "hValid", "enable", "flushNarrowPacket",
                   "bufferReady", "payloadValid"}) port(n, uint(1), Direction::In);
    port("payloadData", uint(s.bits), Direction::In);
    port("hReady", uint(1), Direction::Out);
    port("tokenValid", uint(1), Direction::Out);
    port("tokenData", uint(s.bits), Direction::Out);
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto m = b.create<FModuleOp>(loc, b.getStringAttr(name),
        ConventionAttr::get(circuit.getContext(), Convention::Internal), ports);
    NamedAttrList metadata(s.layout);
    metadata.set("payloadModule", b.getStringAttr(s.payload.getName()));
    metadata.set("queueDepth", b.getI64IntegerAttr(1));
    metadata.set("pipe", b.getBoolAttr(true));
    metadata.set("flow", b.getBoolAttr(false));
    m->setAttr("goldengate.printTokenStage", metadata.getDictionary(circuit.getContext()));
    b.setInsertionPointToStart(m.getBodyBlock());
    auto arg = [&](unsigned i) { return m.getArgument(i); };
    auto k = [&](unsigned w, uint64_t v) -> Value {
      return b.create<ConstantOp>(loc, uint(w), APInt(w, v));
    };
    auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
    auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
    auto neg = [&](Value a) -> Value { return b.create<NotPrimOp>(loc, a); };
    auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
    auto connect = [&](Value d, Value v) { b.create<StrictConnectOp>(loc, d, v); };
    Value zero = k(1, 0);
    Value full = b.create<RegResetOp>(loc, uint(1), arg(0), arg(1), zero, "dataPipe_full").getResult();
    // Chisel Queue's payload storage is unreset; only occupancy is reset.
    Value data = b.create<RegOp>(loc, uint(s.bits), arg(0), "dataPipe_data").getResult();
    Value idle = b.create<RegResetOp>(loc, uint(s.idleBits), arg(0), arg(1),
                                   k(s.idleBits, 0), "idleCycles").getResult();
    Value request = both(both(arg(2), arg(3)), neg(arg(5)));
    Value readyToEnq = either(neg(arg(4)), arg(6));
    Value tFire = both(request, readyToEnq);
    connect(arg(9), tFire); // Deliberately includes hValid, as the Scala helper does.
    Value enqueue = both(both(tFire, arg(7)), arg(4));
    Value push = both(enqueue, either(neg(full), arg(6)));
    Value pop = both(full, arg(6));
    connect(full, mux(b.create<XorPrimOp>(loc, push, pop), push, full));
    connect(data, mux(push, arg(8), data));
    Value rollover = b.create<AndRPrimOp>(loc, idle);
    Value nonzero = b.create<NEQPrimOp>(loc, idle, k(s.idleBits, 0));
    Value increment = b.create<BitsPrimOp>(loc,
        b.create<AddPrimOp>(loc, idle, k(s.idleBits, 1)), s.idleBits - 1, 0);
    connect(idle, mux(either(enqueue, arg(5)), k(s.idleBits, 0),
        mux(both(tFire, arg(4)), mux(rollover, k(s.idleBits, 1), increment), idle)));
    Value idleToken = b.create<PadPrimOp>(loc, b.create<CatPrimOp>(loc, idle, zero), s.bits);
    connect(arg(11), mux(full, data, idleToken));
    // Excluding readyToEnqToken from fire keeps valid asserted when downstream
    // backpressure stalls tFire. Pending data drains even outside ROI/during flush.
    Value idleValid = both(both(request, arg(4)), either(both(arg(7), nonzero), rollover));
    connect(arg(10), either(idleValid, full));
    modules.push_back(m);
  }
  return success();
}
