// See LICENSE for license details.
// PrintBridge.scala's stream boundary, using junctions.MultiWidthFifo semantics.
#include "goldengate/PrintBridgePayload.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializePrintBridgeStreams(CircuitOp circuit,
    llvm::ArrayRef<FModuleOp> controls, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w); };
  llvm::StringSet<> names;
  std::set<std::pair<std::string, std::string>> identities;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    if (auto m = dyn_cast<FModuleLike>(&op)) names.insert(m.getModuleName());
    if (auto prior = op.getAttrOfType<DictionaryAttr>("goldengate.printStream")) {
      auto target = prior.getAs<StringAttr>("bridgeTarget");
      auto reset = prior.getAs<StringAttr>("resetPortName");
      if (target && reset) identities.emplace(target.getValue().str(), reset.getValue().str());
    }
  }
  const char *expectedNames[] = {"hostClock", "hostReset", "doneInit", "hValid",
      "flushNarrowPacket", "bufferReady", "startCycleL", "startCycleH", "endCycleL",
      "endCycleH", "hBits", "hReady", "fromHostValid", "tokenValid", "tokenData",
      "currentCycle", "enable"};
  // Preflight the complete batch before adding modules or instances. Never
  // combine same-width ports belonging to distinct bridge/reset identities.
  for (auto control : controls) {
    if (!control || control->getParentOp() != circuit || control.getNumPorts() != 17)
      return reject("PrintBridge stream requires materialized controls in this circuit");
    auto layout = control->getAttrOfType<DictionaryAttr>("goldengate.printControl");
    auto target = layout ? layout.getAs<StringAttr>("bridgeTarget") : StringAttr();
    auto reset = layout ? layout.getAs<StringAttr>("resetPortName") : StringAttr();
    auto bits = layout ? layout.getAs<IntegerAttr>("tokenBits") : IntegerAttr();
    auto cycles = layout ? layout.getAs<IntegerAttr>("cycleBits") : IntegerAttr();
    auto inclusive = layout ? layout.getAs<BoolAttr>("roiInclusive") : BoolAttr();
    if (!target || !reset || target.getValue().empty() || reset.getValue().empty() ||
        !bits || bits.getInt() < 8 || bits.getInt() > (1LL << 30) ||
        (bits.getInt() & (bits.getInt()-1)) || !cycles || cycles.getInt() != 64 ||
        !inclusive || !inclusive.getValue())
      return reject("PrintBridge stream has incompatible control metadata");
    for (unsigned p = 0; p < 17; ++p) {
      Type expected = p == 0 ? Type(ClockType::get(ctx)) : p == 10 ? control.getPortType(p) :
          Type(uint(p >= 6 && p <= 9 ? 32 : p == 14 ? bits.getInt() : p == 15 ? 64 : 1));
      if (control.getPortName(p) != expectedNames[p] || control.getPortType(p) != expected ||
          control.getPortDirection(p) != (p < 11 ? Direction::In : Direction::Out) ||
          (p == 10 && (!isa<BundleType>(expected) || !cast<FIRRTLBaseType>(expected).isPassive())))
        return reject("PrintBridge stream control interface mismatch");
    }
    if (!identities.emplace(target.getValue().str(), reset.getValue().str()).second)
      return reject("PrintBridge stream target/reset identity already materialized or duplicated");
  }
  auto unique = [&](llvm::StringRef base) {
    std::string name = base.str();
    for (unsigned suffix = 1; names.count(name); ++suffix)
      name = base.str() + "_" + std::to_string(suffix);
    names.insert(name); return name;
  };
  auto loc = circuit.getLoc();
  for (auto control : controls) {
    auto layout = control->getAttrOfType<DictionaryAttr>("goldengate.printControl");
    unsigned bits = layout.getAs<IntegerAttr>("tokenBits").getInt();
    unsigned ratio = bits < 512 ? 512 / bits : bits / 512;
    unsigned depth = std::max(1u, bits / 512);
    unsigned countBits = llvm::Log2_64_Ceil(depth + 1);
    SmallVector<PortInfo> adapterPorts;
    auto port = [&](llvm::StringRef n, Type t, Direction d) {
      adapterPorts.push_back({b.getStringAttr(n), t, d});
    };
    port("hostClock", ClockType::get(ctx), Direction::In);
    for (auto n : {"hostReset", "tokenValid"}) port(n, uint(1), Direction::In);
    port("tokenData", uint(bits), Direction::In);
    for (auto n : {"flushNarrowPacket", "streamReady"}) port(n, uint(1), Direction::In);
    for (auto n : {"bufferReady", "streamValid"}) port(n, uint(1), Direction::Out);
    port("streamData", uint(512), Direction::Out);
    port("count", uint(countBits), Direction::Out);
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto adapter = b.create<FModuleOp>(loc, b.getStringAttr(unique("GGPrintBridgeStreamAdapter")),
        ConventionAttr::get(ctx, Convention::Internal), adapterPorts);
    NamedAttrList metadata(layout);
    metadata.set("controlModule", b.getStringAttr(control.getName()));
    metadata.set("adapterModule", b.getStringAttr(adapter.getName()));
    metadata.set("streamBits", b.getI64IntegerAttr(512));
    metadata.set("adapterDepth", b.getI64IntegerAttr(depth));
    metadata.set("packingRatio", b.getI64IntegerAttr(bits < 512 ? ratio : 1));
    metadata.set("lowSliceFirst", b.getBoolAttr(true));
    metadata.set("narrowFlushInjection", b.getBoolAttr(bits < 512));
    adapter->setAttr("goldengate.printStreamAdapter", metadata.getDictionary(ctx));
    b.setInsertionPointToStart(adapter.getBodyBlock());
    auto arg = [&](unsigned p) { return adapter.getArgument(p); };
    auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
    auto k = [&](unsigned w, uint64_t v) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, v)); };
    auto mux = [&](Value s, Value t, Value f) -> Value { return b.create<MuxPrimOp>(loc, s, t, f); };
    auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
    if (bits == 512) {
      // The equal-width PrintBridge path bypasses MultiWidthFifo entirely.
      connect(arg(6), arg(5)); connect(arg(7), arg(2)); connect(arg(8), arg(3));
      connect(arg(9), k(1, 0));
    } else {
      unsigned ptrBits = llvm::Log2_64(ratio), sizeBits = ptrBits + 1;
      Value size = b.create<RegResetOp>(loc, uint(sizeBits), arg(0), arg(1),
          k(sizeBits, 0), "size").getResult();
      Value pointer = b.create<RegResetOp>(loc, uint(ptrBits), arg(0), arg(1),
          k(ptrBits, 0), bits < 512 ? "head" : "tail").getResult();
      Value ready = b.create<LTPrimOp>(loc, size, k(sizeBits, bits < 512 ? ratio : 1));
      Value valid = b.create<GEQPrimOp>(loc, size, k(sizeBits, bits < 512 ? ratio : 1));
      Value enqValid = bits < 512 ? Value(b.create<OrPrimOp>(loc, arg(2), arg(4))) : arg(2);
      Value push = both(enqValid, ready), pop = both(valid, arg(5));
      Value increment = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, pointer, k(ptrBits, 1)), ptrBits-1, 0);
      connect(pointer, mux(bits < 512 ? push : pop, increment, pointer));
      // Neither implementation pipes across a full-buffer dequeue. For this
      // one-group specialization push and pop cannot coincide. Keep both
      // contributions explicit to reflect FIFO occupancy in input/output units.
      Value added = b.create<AddPrimOp>(loc, size, mux(push, k(sizeBits, bits < 512 ? 1 : ratio), k(sizeBits, 0)));
      Value next = b.create<SubPrimOp>(loc, added, mux(pop, k(sizeBits, bits < 512 ? ratio : 1), k(sizeBits, 0)));
      connect(size, b.create<BitsPrimOp>(loc, next, sizeBits-1, 0));
      Value data;
      if (bits < 512) {
        for (unsigned i = 0; i < ratio; ++i) {
          Value word = b.create<RegOp>(loc, uint(bits), arg(0), "wdata_" + std::to_string(i)).getResult();
          Value selected = b.create<EQPrimOp>(loc, pointer, k(ptrBits, i));
          connect(word, mux(both(push, selected), arg(3), word));
          // First accepted token occupies the least significant stream slice.
          data = data ? Value(b.create<CatPrimOp>(loc, word, data)) : word;
        }
        connect(arg(9), b.create<BitsPrimOp>(loc, size, sizeBits-1, ptrBits));
      } else {
        Value word = b.create<RegOp>(loc, uint(bits), arg(0), "wdata_0").getResult();
        connect(word, mux(push, arg(3), word));
        Value shift = b.create<CatPrimOp>(loc, pointer, k(9, 0));
        data = b.create<BitsPrimOp>(loc, b.create<DShrPrimOp>(loc, word, shift), 511, 0);
        connect(arg(9), size);
      }
      connect(arg(6), ready); connect(arg(7), valid); connect(arg(8), data);
    }

    SmallVector<PortInfo> ports;
    SmallVector<unsigned> copied;
    for (auto [p, info] : llvm::enumerate(control.getPorts())) {
      if (p == 5 || p == 13 || p == 14) continue;
      ports.push_back(info); copied.push_back(p);
    }
    ports.push_back({b.getStringAttr("streamReady"), uint(1), Direction::In});
    ports.push_back({b.getStringAttr("streamValid"), uint(1), Direction::Out});
    ports.push_back({b.getStringAttr("streamData"), uint(512), Direction::Out});
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(unique("GGPrintBridgeStream")),
        ConventionAttr::get(ctx, Convention::Internal), ports);
    wrapper->setAttr("goldengate.printStream", metadata.getDictionary(ctx));
    b.setInsertionPointToStart(wrapper.getBodyBlock());
    auto inner = b.create<InstanceOp>(loc, control, "control");
    auto fifo = b.create<InstanceOp>(loc, adapter, "widthAdapter");
    for (auto [outer, p] : llvm::enumerate(copied)) {
      Value a = wrapper.getArgument(outer), v = inner.getResult(p);
      connect(control.getPortDirection(p) == Direction::In ? v : a,
          control.getPortDirection(p) == Direction::In ? a : v);
    }
    connect(fifo.getResult(0), wrapper.getArgument(0));
    connect(fifo.getResult(1), wrapper.getArgument(1));
    connect(fifo.getResult(2), inner.getResult(13));
    connect(fifo.getResult(3), inner.getResult(14));
    connect(fifo.getResult(4), wrapper.getArgument(4));
    connect(fifo.getResult(5), wrapper.getArgument(14));
    connect(inner.getResult(5), fifo.getResult(6));
    connect(wrapper.getArgument(15), fifo.getResult(7));
    connect(wrapper.getArgument(16), fifo.getResult(8));
    modules.push_back(wrapper);
  }
  return success();
}
