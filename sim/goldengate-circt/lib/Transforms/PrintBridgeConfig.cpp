// See LICENSE for license details.
// Widget.genWOReg and MCRIO.bindReg use ReadWrite registers, ignore strobes,
// and bind writes after Pulsify. Keep that priority and the unreset ROI words.
#include "goldengate/PrintBridgePayload.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include <set>
using namespace mlir;
using namespace circt::firrtl;

namespace {
LogicalResult materializeConfigs(CircuitOp circuit,
    llvm::ArrayRef<FModuleOp> controls, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error, bool streamForm) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w); };
  llvm::StringSet<> names;
  std::set<std::pair<std::string, std::string>> identities;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    if (auto m = dyn_cast<FModuleLike>(&op)) names.insert(m.getModuleName());
    if (auto prior = op.getAttrOfType<DictionaryAttr>(streamForm ? "goldengate.printStreamConfig" : "goldengate.printConfig")) {
      auto target = prior.getAs<StringAttr>("bridgeTarget");
      auto reset = prior.getAs<StringAttr>("resetPortName");
      if (target && reset) identities.emplace(target.getValue().str(), reset.getValue().str());
    }
  }
  const char *portNames[] = {"hostClock", "hostReset", "doneInit", "hValid",
      "flushNarrowPacket", "bufferReady", "startCycleL", "startCycleH", "endCycleL",
      "endCycleH", "hBits", "hReady", "fromHostValid", "tokenValid", "tokenData",
      "currentCycle", "enable"};
  const char *streamNames[] = {"hostClock", "hostReset", "doneInit", "hValid",
      "flushNarrowPacket", "startCycleL", "startCycleH", "endCycleL", "endCycleH",
      "hBits", "hReady", "fromHostValid", "currentCycle", "enable", "streamReady",
      "streamValid", "streamData"};
  auto sourceMetadata = streamForm ? "goldengate.printStream" : "goldengate.printControl";
  // Validate the entire collection before creating any state or instances.
  for (auto control : controls) {
    if (!control || control->getParentOp() != circuit || control.getNumPorts() != 17)
      return reject("PrintBridge config requires materialized controls in this circuit");
    auto layout = control->getAttrOfType<DictionaryAttr>(sourceMetadata);
    auto target = layout ? layout.getAs<StringAttr>("bridgeTarget") : StringAttr();
    auto reset = layout ? layout.getAs<StringAttr>("resetPortName") : StringAttr();
    auto bits = layout ? layout.getAs<IntegerAttr>("tokenBits") : IntegerAttr();
    auto cycles = layout ? layout.getAs<IntegerAttr>("cycleBits") : IntegerAttr();
    auto inclusive = layout ? layout.getAs<BoolAttr>("roiInclusive") : BoolAttr();
    if (!target || !reset || target.getValue().empty() || reset.getValue().empty() ||
        !bits || bits.getInt() < 8 || bits.getInt() > (1LL << 30) ||
        (bits.getInt() & (bits.getInt()-1)) || !cycles || cycles.getInt() != 64 ||
        !inclusive || !inclusive.getValue())
      return reject("PrintBridge config has incompatible control metadata");
    if (streamForm) {
      auto streamBits = layout.getAs<IntegerAttr>("streamBits");
      auto depth = layout.getAs<IntegerAttr>("adapterDepth");
      auto packing = layout.getAs<IntegerAttr>("packingRatio");
      auto lowFirst = layout.getAs<BoolAttr>("lowSliceFirst");
      auto flush = layout.getAs<BoolAttr>("narrowFlushInjection");
      auto source = layout.getAs<StringAttr>("controlModule");
      auto adapter = layout.getAs<StringAttr>("adapterModule");
      if (!streamBits || streamBits.getInt() != 512 || !depth ||
          depth.getInt() != (bits.getInt() < 512 ? 1 : bits.getInt() / 512) ||
          !packing || packing.getInt() != (bits.getInt() < 512 ? 512 / bits.getInt() : 1) ||
          !lowFirst || !lowFirst.getValue() || !flush || flush.getValue() != (bits.getInt() < 512) ||
          !source || source.getValue().empty() || !adapter || adapter.getValue().empty())
        return reject("PrintBridge config has incompatible stream metadata");
    }
    for (unsigned p = 0; p < 17; ++p) {
      unsigned hBitsPort = streamForm ? 9 : 10;
      Type expected = p == 0 ? Type(ClockType::get(ctx)) : p == hBitsPort ? control.getPortType(p) :
          Type(uint(streamForm ? (p >= 5 && p <= 8 ? 32 : p == 12 ? 64 : p == 16 ? 512 : 1) :
              (p >= 6 && p <= 9 ? 32 : p == 14 ? bits.getInt() : p == 15 ? 64 : 1)));
      Direction direction = (streamForm ? p < 10 || p == 14 : p < 11) ? Direction::In : Direction::Out;
      if (control.getPortName(p) != (streamForm ? streamNames[p] : portNames[p]) || control.getPortType(p) != expected ||
          control.getPortDirection(p) != direction ||
          (p == hBitsPort && (!isa<BundleType>(expected) || !cast<FIRRTLBaseType>(expected).isPassive())))
        return reject("PrintBridge config control interface mismatch");
    }
    if (!identities.emplace(target.getValue().str(), reset.getValue().str()).second)
      return reject("PrintBridge config target/reset identity already materialized or duplicated");
  }
  const char *registerNames[] = {"startCycleL", "startCycleH", "endCycleL", "endCycleH",
      "doneInit", "flushNarrowPacket"};
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(32)}});
  auto words = FVectorType::get(token, 6);
  auto mcrType = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  auto loc = circuit.getLoc();
  for (auto control : controls) {
    llvm::StringRef base = streamForm ? "GGPrintBridgeStreamConfig" : "GGPrintBridgeConfig";
    std::string name = base.str();
    for (unsigned suffix = 1; names.count(name); ++suffix)
      name = base.str() + "_" + std::to_string(suffix);
    names.insert(name);
    SmallVector<PortInfo> ports;
    SmallVector<unsigned> copied;
    for (auto [p, info] : llvm::enumerate(control.getPorts())) {
      unsigned roiStart = streamForm ? 5 : 6;
      if (p == 2 || p == 4 || (p >= roiStart && p < roiStart + 4)) continue;
      ports.push_back(info); copied.push_back(p);
    }
    ports.push_back({b.getStringAttr("mcr"), mcrType, Direction::Out});
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto m = b.create<FModuleOp>(loc, b.getStringAttr(name),
        ConventionAttr::get(ctx, Convention::Internal), ports);
    auto layout = control->getAttrOfType<DictionaryAttr>(sourceMetadata);
    unsigned tokenBits = layout.getAs<IntegerAttr>("tokenBits").getInt();
    // BridgeStreamConstants.streamWidthBits is 512. Equal/wide tokens still
    // have the unused, one-cycle flush register, exactly as PrintBridge.scala.
    unsigned pulseLength = tokenBits < 512 ? 512 / tokenBits : 1;
    NamedAttrList metadata(layout);
    metadata.set(streamForm ? "streamModule" : "controlModule", b.getStringAttr(control.getName()));
    metadata.set("flushPulseLength", b.getI64IntegerAttr(pulseLength));
    SmallVector<Attribute> registerMap;
    for (unsigned word = 0; word < 6; ++word) {
      NamedAttrList entry;
      entry.set("name", b.getStringAttr(registerNames[word]));
      entry.set("word", b.getI64IntegerAttr(word));
      entry.set("byteOffset", b.getI64IntegerAttr(word * 4));
      entry.set("bits", b.getI64IntegerAttr(word < 4 ? 32 : 1));
      entry.set("permissions", b.getStringAttr("ReadWrite"));
      entry.set("reset", b.getStringAttr(word < 4 ? "none" : "zero"));
      registerMap.push_back(entry.getDictionary(ctx));
    }
    metadata.set("registers", b.getArrayAttr(registerMap));
    m->setAttr(streamForm ? "goldengate.printStreamConfig" : "goldengate.printConfig", metadata.getDictionary(ctx));
    b.setInsertionPointToStart(m.getBodyBlock());
    auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
    auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
    auto k = [&](unsigned w, uint64_t v) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, v)); };
    auto mux = [&](Value s, Value t, Value f) -> Value { return b.create<MuxPrimOp>(loc, s, t, f); };
    Value zero = k(1, 0), one = k(1, 1);
    auto inner = b.create<InstanceOp>(loc, control, streamForm ? "stream" : "control");
    for (auto [outer, p] : llvm::enumerate(copied)) {
      Value arg = m.getArgument(outer), instance = inner.getResult(p);
      connect(control.getPortDirection(p) == Direction::In ? instance : arg,
          control.getPortDirection(p) == Direction::In ? arg : instance);
    }
    SmallVector<Value> regs;
    for (unsigned word = 0; word < 6; ++word) {
      Value reg = word < 4 ? b.create<RegOp>(loc, uint(32), m.getArgument(0), registerNames[word]).getResult() :
          b.create<RegResetOp>(loc, uint(1), m.getArgument(0), m.getArgument(1), zero, registerNames[word]).getResult();
      regs.push_back(reg);
    }
    Value flushNext = zero;
    if (pulseLength > 1) {
      unsigned width = llvm::Log2_64_Ceil(pulseLength);
      Value count = b.create<RegResetOp>(loc, uint(width), m.getArgument(0), m.getArgument(1), k(width, 0), "flushCount").getResult();
      Value last = b.create<EQPrimOp>(loc, count, k(width, pulseLength - 1));
      Value next = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, count, k(width, 1)), width-1, 0);
      connect(count, mux(last, k(width, 0), mux(regs[5], next, count)));
      flushNext = mux(last, zero, regs[5]);
    }
    Value mcr = m.getArgument(ports.size()-1);
    for (unsigned word = 0; word < 6; ++word) {
      Value wr = b.create<SubindexOp>(loc, field(mcr, "write"), word);
      Value rd = b.create<SubindexOp>(loc, field(mcr, "read"), word);
      Value data = field(wr, "bits");
      if (word >= 4) data = b.create<BitsPrimOp>(loc, data, 0, 0);
      // bindRegs runs after Pulsify: an MMIO write wins over self-clear.
      connect(regs[word], mux(field(wr, "valid"), data, word == 5 ? flushNext : regs[word]));
      connect(field(wr, "ready"), one); connect(field(rd, "valid"), one);
      Value read = regs[word];
      if (word >= 4) read = b.create<PadPrimOp>(loc, read, 32);
      connect(field(rd, "bits"), read);
    }
    for (unsigned word = 0; word < 4; ++word) connect(inner.getResult((streamForm ? 5 : 6)+word), regs[word]);
    connect(inner.getResult(2), regs[4]); connect(inner.getResult(4), regs[5]);
    modules.push_back(m);
  }
  return success();
}
} // namespace

LogicalResult goldengate::materializePrintBridgeConfigs(CircuitOp circuit,
    llvm::ArrayRef<FModuleOp> controls, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error) {
  return materializeConfigs(circuit, controls, modules, error, false);
}

LogicalResult goldengate::materializePrintBridgeStreamConfigs(CircuitOp circuit,
    llvm::ArrayRef<FModuleOp> streams, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error) {
  return materializeConfigs(circuit, streams, modules, error, true);
}
