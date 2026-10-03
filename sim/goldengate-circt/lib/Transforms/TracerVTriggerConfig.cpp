// See LICENSE for license details.
// TracerVBridge.scala: MMIO configuration and host-cycle trigger state.
#include "goldengate/TracerVTokenEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addTracerVTriggerConfig(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGTracerVTriggerConfig";
  constexpr llvm::StringLiteral wrapperName = "GGTracerVTriggerWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGTracerVTokenWrapper")
    return reject("TracerV trigger configuration requires the active token wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("TracerV trigger module or wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getName() == "GGTracerVTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("TracerV trigger configuration needs the native engine and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = uint(1);
  auto control = BundleType::get(ctx, {{b.getStringAttr("initDone"), true, bit},
      {b.getStringAttr("traceEnable"), true, bit}, {b.getStringAttr("trigger"), true, bit},
      {b.getStringAttr("tCycle"), false, uint(64)}});
  auto trace = BundleType::get(ctx, {{b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("iaddr"), false, uint(40)}, {b.getStringAttr("insn"), false, uint(32)}});
  std::optional<unsigned> clock, reset, controlPort;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "tracerv_mcr" || p.name == "tracerv_trace") return reject("TracerV trigger boundary already exists");
    if (p.name == "hostClock" && p.direction == Direction::In && isa<ClockType>(p.type)) clock = i;
    if (p.name == "hostReset" && p.direction == Direction::In && p.type == bit) reset = i;
    if (p.name == "tracerv_control" && p.direction == Direction::Out && p.type == control) controlPort = i;
  }
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto checkWidth = [&](llvm::StringRef name, int64_t w) {
    auto attr = key ? key.getAs<IntegerAttr>(name) : IntegerAttr(); return attr && attr.getInt() == w;
  };
  if (!clock || !reset || !controlPort || !key ||
      key.getAs<StringAttr>("class") != "firechip.bridgeinterfaces.TraceBundleWidths" ||
      !checkWidth("retireWidth", 1) || !checkWidth("iaddrWidth", 40) || !checkWidth("insnWidth", 32) ||
      !checkWidth("causeWidth", 64) || !checkWidth("tvalWidth", 40))
    return reject("TracerV trigger configuration requires the Rocket constructor and exact engine control boundary");
  auto hPort = engine.getNumPorts() == 5 ? dyn_cast<BundleType>(engine.getPortType(2)) : BundleType();
  auto hb = hPort && hPort.getElement("hBits") ? dyn_cast<BundleType>(hPort.getElement("hBits")->type) : BundleType();
  const llvm::StringRef observed[]{"tiletrace_reset", "tiletrace_trace_retiredinsns_0_valid",
      "tiletrace_trace_retiredinsns_0_iaddr", "tiletrace_trace_retiredinsns_0_insn"};
  const unsigned observedWidths[]{1, 1, 40, 32};
  if (!hb) return reject("TracerV token engine has no trace payload");
  for (unsigned i = 0; i < 4; ++i) {
    auto f = hb.getElement(observed[i]);
    if (!f || f->isFlip || f->type != uint(observedWidths[i])) return reject("TracerV observation needs the exact Rocket trace fields");
  }
  InstanceOp tracer; unsigned instances = 0; bool used = false;
  for (auto i : inner.getOps<InstanceOp>()) if (i.getModuleName() == engine.getName()) { tracer = i; ++instances; }
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (instances != 1 || used) return reject("TracerV trigger mapping needs a unique engine in an uninstantiated top");

  // All unsupported boundaries reject before mutation. Observe the masked trace
  // independently of HostPort fire: the Scala PC/insn state runs on host clocks.
  unsigned observationPort = inner.getNumPorts();
  inner.insertPorts({{observationPort, PortInfo(b.getStringAttr("tracerv_trace"), trace, Direction::Out)}});
  b.setInsertionPointToEnd(inner.getBodyBlock());
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value payload = field(tracer.getResult(2), "hBits");
  Value observation = inner.getBodyBlock()->getArgument(observationPort);
  connect(field(observation, "valid"), b.create<AndPrimOp>(loc, field(payload, observed[1]),
      b.create<NotPrimOp>(loc, field(payload, observed[0]))));
  connect(field(observation, "iaddr"), field(payload, observed[2]));
  connect(field(observation, "insn"), field(payload, observed[3]));
  auto word = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(32)}});
  auto words = FVectorType::get(word, 15);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("trace"), trace, Direction::In},
      {b.getStringAttr("control"), control, Direction::In}, {b.getStringAttr("mcr"), mcr, Direction::Out}};
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName), ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  bank->setAttr("goldengate.bridgeConstructor", key);
  const llvm::StringRef names[]{"initDone", "traceEnable", "hostTriggerPCStartHigh", "hostTriggerPCStartLow",
      "hostTriggerPCEndHigh", "hostTriggerPCEndLow", "hostTriggerCycleCountStartHigh", "hostTriggerCycleCountStartLow",
      "hostTriggerCycleCountEndHigh", "hostTriggerCycleCountEndLow", "hostTriggerStartInst", "hostTriggerStartInstMask",
      "hostTriggerEndInst", "hostTriggerEndInstMask", "triggerSelector"};
  const unsigned widths[]{1, 1, 8, 32, 8, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < 15; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)), b.getNamedAttr("readable", b.getBoolAttr(i < 2)),
        b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t value) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, value)); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value { return b.create<SubindexOp>(loc, field(arg(4), group), i); };
  auto mux = [&](Value pred, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, pred, yes, no); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto eq = [&](Value a, Value c) -> Value { return b.create<EQPrimOp>(loc, a, c); };
  auto reg = [&](llvm::StringRef name, unsigned w, uint64_t initial = 0) -> Value {
    return b.create<RegResetOp>(loc, uint(w), arg(0), arg(1), constant(w, initial), name).getResult();
  };
  SmallVector<Value> config;
  Value enabled = b.create<NotPrimOp>(loc, arg(1)), one = constant(1, 1), zeroWord = constant(32, 0);
  for (unsigned i = 0; i < 15; ++i) {
    Value r = reg(names[i], widths[i], i == 1 ? 1 : 0), write = slot("write", i), read = slot("read", i);
    Value data = b.create<BitsPrimOp>(loc, field(write, "bits"), widths[i] - 1, 0);
    connect(r, mux(field(write, "valid"), data, r)); config.push_back(r);
    connect(field(read, "bits"), i < 2 ? b.create<PadPrimOp>(loc, r, 32).getResult() : zeroWord);
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
    if (i >= 2) b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, field(read, "ready")), enabled,
        "TracerV trigger register is write only", ValueRange{}, "");
  }
  Value pcStart = reg("triggerPCStart", 40), pcEnd = reg("triggerPCEnd", 40);
  Value cycleStart = reg("triggerCycleCountStart", 64), cycleEnd = reg("triggerCycleCountEnd", 64);
  connect(pcStart, b.create<CatPrimOp>(loc, config[2], config[3]));
  connect(pcEnd, b.create<CatPrimOp>(loc, config[4], config[5]));
  connect(cycleStart, b.create<CatPrimOp>(loc, config[6], config[7]));
  connect(cycleEnd, b.create<CatPrimOp>(loc, config[8], config[9]));
  Value cycleVal = reg("triggerCycleCountVal", 1), pcVal = reg("triggerPCValVec_0", 1), insnVal = reg("triggerInstValVec_0", 1);
  connect(cycleVal, both(b.create<GEQPrimOp>(loc, field(arg(3), "tCycle"), cycleStart),
      b.create<LEQPrimOp>(loc, field(arg(3), "tCycle"), cycleEnd)));
  Value pc = field(arg(2), "iaddr"), valid = field(arg(2), "valid");
  // Start takes priority if start and end coincide. No HostPort handshake gate.
  connect(pcVal, mux(valid, mux(eq(pcStart, pc), one, mux(both(eq(pcEnd, pc), pcVal), constant(1, 0), pcVal)), pcVal));
  auto insnMatch = [&](unsigned pattern, unsigned mask) -> Value {
    Value diff = b.create<XorPrimOp>(loc, config[pattern], field(arg(2), "insn"));
    return eq(both(diff, config[mask]), zeroWord);
  };
  connect(insnVal, mux(valid, mux(insnMatch(10, 11), one, mux(insnMatch(12, 13), constant(1, 0), insnVal)), insnVal));
  Value trigger = constant(1, 0);
  Value selections[]{one, cycleVal, pcVal, insnVal};
  for (int i = 3; i >= 0; --i) trigger = mux(eq(config[14], constant(32, i)), selections[i], trigger);
  connect(field(arg(3), "initDone"), config[0]); connect(field(arg(3), "traceEnable"), config[1]);
  connect(field(arg(3), "trigger"), trigger);

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != *controlPort && i != observationPort) {
    copied.push_back(i); ports.push_back(p);
  }
  ports.push_back({b.getStringAttr("tracerv_mcr"), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), triggerConfig = b.create<InstanceOp>(loc, bank, "tracervRegisters");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(triggerConfig.getResult(0), outer(*clock)); connect(triggerConfig.getResult(1), outer(*reset));
  b.create<ConnectOp>(loc, triggerConfig.getResult(2), sim.getResult(observationPort));
  b.create<ConnectOp>(loc, triggerConfig.getResult(3), sim.getResult(*controlPort));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), triggerConfig.getResult(4));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto local = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (local == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> vs; for (auto v : arr) vs.push_back(retarget(v)); return b.getArrayAttr(vs); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList vs; for (auto v : d) vs.set(v.getName(), retarget(v.getValue())); return vs.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(wrapperName);
  return success();
}
