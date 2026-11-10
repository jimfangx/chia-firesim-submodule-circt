// See LICENSE for license details.
// Oracle: midas/widgets/AutoCounterBridge.scala and Widget.MCRIO.bindReg.
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include <set>

using namespace mlir;
using namespace circt::firrtl;
namespace {
constexpr llvm::StringLiteral widgetClass = "midas.widgets.AutoCounterBridgeModule";
constexpr llvm::StringLiteral parametersClass = "midas.widgets.AutoCounterParameters";
struct Event { std::string name, label; unsigned width; bool accumulate; };
struct Constructor { DictionaryAttr bridge, key; SmallVector<Event> events; std::string trigger, reset; };

LogicalResult parseConstructors(CircuitOp circuit, SmallVectorImpl<Constructor> &constructors,
                               std::string &error) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("AutoCounter hosts require retained annotations");
  auto hasText = [](DictionaryAttr dict, llvm::StringRef field, llvm::StringRef expected) {
    auto text = dict ? dict.getAs<StringAttr>(field) : StringAttr();
    return text && text.getValue() == expected;
  };
  std::set<std::string> identities;
  for (auto item : raw) {
    auto bridge = dyn_cast<DictionaryAttr>(item);
    if (!hasText(bridge, "class", goldengate::AnnotationClasses::BridgeIO) ||
        !hasText(bridge, "widgetClass", widgetClass)) continue;
    auto key = bridge.getAs<DictionaryAttr>("widgetConstructorKey");
    auto target = bridge.getAs<StringAttr>("target");
    auto trigger = key ? key.getAs<StringAttr>("triggerName") : StringAttr();
    auto reset = key ? key.getAs<StringAttr>("resetPortName") : StringAttr();
    auto events = key ? key.getAs<ArrayAttr>("eventMetadata") : ArrayAttr();
    if (!hasText(key, "class", parametersClass) || !target ||
        target.getValue().empty() || !trigger || trigger.getValue().empty() ||
        !reset || reset.getValue().empty() || !events || events.empty() ||
        !identities.insert((target.getValue() + ":" + reset.getValue()).str()).second)
      return reject("AutoCounter hosts require distinct complete bridge constructors");
    Constructor c{bridge, key, {}, trigger.getValue().str(), reset.getValue().str()};
    llvm::StringSet<> fields, labels;
    fields.insert(c.trigger); fields.insert(c.reset); labels.insert("local_cycle");
    if (c.trigger == c.reset) return reject("AutoCounter trigger and reset fields overlap");
    for (auto attr : events) {
      auto e = dyn_cast<DictionaryAttr>(attr);
      auto name = e ? e.getAs<StringAttr>("portName") : StringAttr();
      auto label = e ? e.getAs<StringAttr>("label") : StringAttr();
      auto description = e ? e.getAs<StringAttr>("description") : StringAttr();
      auto width = e ? e.getAs<IntegerAttr>("width") : IntegerAttr();
      auto op = e ? e.getAs<DictionaryAttr>("opType") : DictionaryAttr();
      auto cls = op ? op.getAs<StringAttr>("class") : StringAttr();
      if (!name || name.getValue().empty() || !fields.insert(name.getValue()).second ||
          !label || label.getValue().empty() || !labels.insert(label.getValue()).second ||
          !description || !width || width.getValue().getBitWidth() > 64 ||
          width.getInt() <= 0 || width.getInt() > (1 << 20) || !cls ||
          (cls != goldengate::AnnotationClasses::AutoCounterAccumulate &&
           cls != goldengate::AnnotationClasses::AutoCounterIdentity))
        return reject("AutoCounter events need unique typed fields/labels and supported operations");
      c.events.push_back({name.getValue().str(), label.getValue().str(),
          unsigned(width.getInt()), cls == goldengate::AnnotationClasses::AutoCounterAccumulate});
    }
    constructors.push_back(std::move(c));
  }
  return success();
}

FModuleOp buildBank(CircuitOp circuit, const Constructor &c, llvm::StringRef name) {
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w); };
  const unsigned counters = c.events.size() + 1, words = 2 * counters + 7;
  SmallVector<BundleType::BundleElement> payload{{b.getStringAttr(c.trigger), false, uint(1)},
      {b.getStringAttr(c.reset), false, uint(1)}};
  for (auto &e : c.events) payload.push_back({b.getStringAttr(e.name), false, uint(e.width)});
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, uint(1)},
      {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(32)}});
  auto lanes = FVectorType::get(token, words);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, lanes},
      {b.getStringAttr("write"), true, lanes}, {b.getStringAttr("wstrb"), true, uint(4)}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("hostReset"), uint(1), Direction::In},
      {b.getStringAttr("hValid"), uint(1), Direction::In},
      {b.getStringAttr("fromHostReady"), uint(1), Direction::In},
      {b.getStringAttr("hBits"), BundleType::get(ctx, payload), Direction::In},
      {b.getStringAttr("hReady"), uint(1), Direction::Out},
      {b.getStringAttr("fromHostValid"), uint(1), Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(name), ConventionAttr::get(ctx, Convention::Internal), ports);
  bank->setAttr("goldengate.autocounterParameters", c.key);
  bank->setAttr("goldengate.bridgeTarget", c.bridge.getAs<StringAttr>("target"));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getArgument(i); };
  auto k = [&](unsigned w, uint64_t value) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, value)); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value a, Value d) -> Value { return b.create<AndPrimOp>(loc, a, d); };
  auto no = [&](Value a) -> Value { return b.create<NotPrimOp>(loc, a); };
  auto mux = [&](Value p, Value a, Value d) -> Value { return b.create<MuxPrimOp>(loc, p, a, d); };
  auto reg = [&](unsigned w, llvm::StringRef n) -> Value {
    return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), k(w, 0), n).getResult();
  };
  auto next64 = [&](Value a, Value d) -> Value {
    return b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, a, d), 63, 0);
  };
  auto to64 = [&](Value a) -> Value {
    unsigned w = *cast<UIntType>(a.getType()).getWidth();
    if (w < 64) return b.create<PadPrimOp>(loc, a, 64);
    if (w > 64) return b.create<BitsPrimOp>(loc, a, 63, 0);
    return a;
  };
  Value zero = k(1, 0), one = k(1, 1), zero64 = k(64, 0);
  Value cycles = reg(64, "cycles"), rateLow = reg(32, "readrate_low"), rateHigh = reg(32, "readrate_high");
  Value init = reg(1, "init_done"), readDone = reg(1, "readdone");
  Value fire = both(both(arg(2), arg(3)), init);
  connect(arg(5), fire); connect(arg(6), one);
  connect(cycles, mux(fire, next64(cycles, k(64, 1)), cycles));
  SmallVector<Value> values{cycles};
  Value underReset = field(arg(4), c.reset), trigger = field(arg(4), c.trigger);
  for (auto [i, e] : llvm::enumerate(c.events)) {
    Value event = to64(field(arg(4), e.name));
    if (e.accumulate) {
      Value count = reg(64, "counter_" + std::to_string(i));
      connect(count, mux(both(fire, no(underReset)), next64(count, event), count));
      values.push_back(count);
    } else values.push_back(mux(underReset, zero64, event));
  }
  Value period = reg(64, "periodcycles"), sample = reg(1, "doSample");
  Value due = b.create<EQPrimOp>(loc, period, b.create<CatPrimOp>(loc, rateHigh, rateLow));
  connect(period, mux(fire, mux(due, zero64, next64(period, k(64, 1))), period));
  connect(sample, mux(fire, due, sample));
  // Queue(Vec(...),2), flow=false, pipe=false. Full samples are dropped by the
  // SFC widget: queue readiness deliberately does not gate targetFire.
  Value writePtr = reg(1, "enq_ptr"), readPtr = reg(1, "deq_ptr"), fullFlag = reg(1, "maybe_full");
  Value same = b.create<EQPrimOp>(loc, writePtr, readPtr);
  Value full = both(same, fullFlag), empty = both(same, no(fullFlag));
  Value valid = no(empty), push = both(both(both(sample, fire), trigger), no(full));
  Value pop = both(valid, readDone);
  connect(writePtr, mux(push, no(writePtr), writePtr));
  connect(readPtr, mux(pop, no(readPtr), readPtr));
  connect(fullFlag, mux(b.create<XorPrimOp>(loc, push, pop), push, fullFlag));
  SmallVector<Value> sampled;
  for (auto [i, value] : llvm::enumerate(values)) {
    Value slots[2];
    for (unsigned slot = 0; slot < 2; ++slot) {
      slots[slot] = b.create<RegOp>(loc, uint(64), arg(0), "sample_" + std::to_string(i) + "_" + std::to_string(slot)).getResult();
      connect(slots[slot], mux(both(push, slot ? writePtr : no(writePtr)), value, slots[slot]));
    }
    sampled.push_back(mux(readPtr, slots[1], slots[0]));
  }
  SmallVector<std::string> labels{"local_cycle"};
  for (auto &e : c.events) labels.push_back(e.label);
  SmallVector<Attribute> rows;
  auto lane = [&](llvm::StringRef channel, unsigned word) -> Value {
    return b.create<SubindexOp>(loc, field(arg(7), channel), word);
  };
  auto bind = [&](unsigned word, llvm::StringRef label, Value value, bool readable, bool writeable, Value defaultValue = {}) {
    auto rd = lane("read", word), wr = lane("write", word);
    connect(field(rd, "valid"), one); connect(field(wr, "ready"), one);
    connect(field(rd, "bits"), readable ? Value(b.create<PadPrimOp>(loc, value, 32)) : Value(b.create<InvalidValueOp>(loc, uint(32))));
    if (writeable) {
      Value data = field(wr, "bits"); unsigned width = *cast<UIntType>(value.getType()).getWidth();
      if (width < 32) data = b.create<BitsPrimOp>(loc, data, width - 1, 0);
      connect(value, mux(field(wr, "valid"), data, defaultValue ? defaultValue : value));
    } else b.create<AssertOp>(loc, arg(0), no(field(wr, "valid")), no(arg(1)), "AutoCounter register is read only", ValueRange{}, "");
    if (!readable) b.create<AssertOp>(loc, arg(0), no(field(rd, "ready")), no(arg(1)), "AutoCounter register is write only", ValueRange{}, "");
    rows.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(label)),
        b.getNamedAttr("offset", b.getI64IntegerAttr(word * 4)),
        b.getNamedAttr("readable", b.getBoolAttr(readable)), b.getNamedAttr("writeable", b.getBoolAttr(writeable))}));
  };
  for (unsigned i = 0; i < counters; ++i) {
    bind(2*i, "autocounter_low_" + labels[i], b.create<BitsPrimOp>(loc, sampled[i], 31, 0), true, false);
    bind(2*i+1, "autocounter_high_" + labels[i], b.create<BitsPrimOp>(loc, sampled[i], 63, 32), true, false);
  }
  unsigned fixed = 2*counters;
  bind(fixed, "cycles_low", b.create<BitsPrimOp>(loc, sampled[0], 31, 0), true, false);
  bind(fixed+1, "cycles_high", b.create<BitsPrimOp>(loc, sampled[0], 63, 32), true, false);
  bind(fixed+2, "readrate_low", rateLow, false, true);
  bind(fixed+3, "readrate_high", rateHigh, false, true);
  bind(fixed+4, "init_done", init, false, true);
  bind(fixed+5, "countersready", valid, true, false);
  bind(fixed+6, "readdone", readDone, true, true, zero);
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
  return bank;
}
} // namespace

LogicalResult goldengate::materializeAutoCounterBridgeHosts(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, SmallVectorImpl<FModuleOp> &hosts,
    std::string &error) {
  if ((addressBits != 25 && addressBits != 32) || idBits != 12) {
    error = "AutoCounter host requires the supported 25/32-bit, ID12 control platform"; return failure();
  }
  SmallVector<Constructor> constructors;
  if (failed(parseConstructors(circuit, constructors, error))) return failure();
  SmallVector<std::string> names;
  for (auto [i, c] : llvm::enumerate(constructors)) {
    std::string base = "GGAutoCounterBridge" + (i ? "_" + std::to_string(i) : "");
    for (auto suffix : {"Bank", "MCRFile", "Host"})
      for (auto m : circuit.getOps<FModuleLike>()) if (m.getModuleName() == base + suffix) {
        error = "AutoCounter host module already exists"; return failure();
      }
    names.push_back(base);
  }
  if (constructors.empty()) return success();
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  SmallVector<std::string> hostNames;
  for (auto [i, c] : llvm::enumerate(constructors)) {
    auto bank = buildBank(*staged, c, names[i] + "Bank"); FModuleOp host;
    if (failed(materializeAutoCounterBridgeAXI(*staged, bank, addressBits, idBits,
          names[i] + "Host", names[i] + "MCRFile", host, error))) return failure();
    OpBuilder b(circuit.getContext());
    host->setAttr("goldengate.autocounterHost", b.getDictionaryAttr({
        b.getNamedAttr("bridgeTarget", c.bridge.getAs<StringAttr>("target")),
        b.getNamedAttr("parameters", c.key), b.getNamedAttr("bankModule", bank.getNameAttr()),
        b.getNamedAttr("mcrModule", b.getStringAttr(names[i]+"MCRFile")),
        b.getNamedAttr("addressBits", b.getI64IntegerAttr(addressBits)),
        b.getNamedAttr("idBits", b.getI64IntegerAttr(idBits)),
        b.getNamedAttr("sampleQueueDepth", b.getI64IntegerAttr(2))}));
    hostNames.push_back(host.getName().str());
  }
  if (failed(verify(*staged))) { error = "AutoCounter materialization produced invalid FIRRTL IR"; return failure(); }
  llvm::StringSet<> original;
  for (auto m : circuit.getOps<FModuleLike>()) original.insert(m.getModuleName());
  for (auto &op : llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))
    if (auto m = dyn_cast<FModuleLike>(&op)) if (!original.count(m.getModuleName()))
      op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  for (auto &name : hostNames) for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) hosts.push_back(m);
  return success();
}
