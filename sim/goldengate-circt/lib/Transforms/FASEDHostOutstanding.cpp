// See LICENSE for license details.
// Requires: active uninstantiated GGFASEDTokenWrapper, retained annotations,
// exact host clock/reset/readiness and the Rocket constructor maxFlight=10.
// Consumes: readiness.hostMemIdle; preserves read/write egress readiness.
// Produces: two host-clocked saturating counters, host idle/read inflight and
// explicit host AXI handshake boundary for subsequent ingress/egress wiring.
// Transfers: circuit/copied-port targets and consumed readiness leaf targets.
// Mutates: hierarchy and ports; preserves constructor/channel/clock metadata.
// Oracle: FASEDMemoryTimingModel.scala 329-340 and widgets/Lib.scala counter.
// The recorded BaseParams(16,16) capped by edge.maxFlight=10 is the supported
// profile. Target reset and targetFire never gate or clear host transactions.
#include "goldengate/FASEDHostOutstanding.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDHostOutstanding(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDHostOutstanding";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDHostOutstandingWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDTokenWrapper")
    return reject("FASED host outstanding needs the active FASED token wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("FASED host outstanding helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED host outstanding needs top, token engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto maxFlight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!maxFlight || maxFlight.getInt() != 10)
    return reject("FASED host outstanding currently supports the recorded maxFlight=10 profile");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto bit = UIntType::get(ctx, 1, false), countType = UIntType::get(ctx, 4, false);
  auto readiness = BundleType::get(ctx, {{b.getStringAttr("readValid"), false, bit},
      {b.getStringAttr("writeValid"), false, bit}, {b.getStringAttr("hostMemIdle"), false, bit}});
  const llvm::StringRef required[]{"hostClock", "hostReset", "fased_readiness"};
  const Type types[]{ClockType::get(ctx), bit, readiness};
  unsigned indices[3];
  for (unsigned j = 0; j < 3; ++j) {
    std::optional<unsigned> index;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == required[j] && p.type == types[j] && p.direction == Direction::In) index = i;
    if (!index) return reject("FASED host outstanding needs exact host clock/reset/readiness inputs");
    indices[j] = *index;
  }
  for (auto p : inner.getPorts())
    if (p.name == "fased_egress_readiness" || p.name == "fased_host_transactions" ||
        p.name == "fased_host_mem_idle" || p.name == "fased_host_read_inflight")
      return reject("FASED host outstanding output boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED host outstanding needs an uninstantiated top");
  SmallVector<BundleType::BundleElement> handshakes;
  for (auto name : {"arReady", "arValid", "rReady", "rValid", "rLast", "awReady", "awValid", "bReady", "bValid"})
    handshakes.push_back({b.getStringAttr(name), false, bit});
  auto transactions = BundleType::get(ctx, handshakes);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("transactions"), transactions, Direction::In},
      {b.getStringAttr("hostMemIdle"), bit, Direction::Out}, {b.getStringAttr("hostReadInflight"), bit, Direction::Out},
      {b.getStringAttr("readCount"), countType, Direction::Out}, {b.getStringAttr("writeCount"), countType, Direction::Out}};
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(ctx, Convention::Internal), helperPorts);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto constant = [&](uint64_t n) -> Value { return b.create<ConstantOp>(loc, countType, APInt(4, n)); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  auto t = [&](llvm::StringRef n) { return field(arg(2), n); };
  Value reads = b.create<RegResetOp>(loc, countType, arg(0), arg(1), constant(0), "hostOutstandingReads").getResult();
  Value writes = b.create<RegResetOp>(loc, countType, arg(0), arg(1), constant(0), "hostOutstandingWrites").getResult();
  Value readEmpty = b.create<EQPrimOp>(loc, reads, constant(0));
  Value writeEmpty = b.create<EQPrimOp>(loc, writes, constant(0));
  auto update = [&](Value count, Value inc, Value dec, Value empty) {
    Value full = b.create<GEQPrimOp>(loc, count, constant(10));
    Value up = both(both(inc, neg(dec)), neg(full));
    Value down = both(both(neg(inc), dec), neg(empty));
    Value plus = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, count, constant(1)), 3, 0);
    Value minus = b.create<BitsPrimOp>(loc, b.create<SubPrimOp>(loc, count, constant(1)), 3, 0);
    connect(count, mux(up, plus, mux(down, minus, count)));
  };
  update(reads, both(t("arReady"), t("arValid")), both(both(t("rReady"), t("rValid")), t("rLast")), readEmpty);
  update(writes, both(t("awReady"), t("awValid")), both(t("bReady"), t("bValid")), writeEmpty);
  connect(arg(3), both(readEmpty, writeEmpty)); connect(arg(4), neg(readEmpty));
  connect(arg(5), reads); connect(arg(6), writes);

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (i != indices[2]) { copied.push_back(i); ports.push_back(p); }
  unsigned added = ports.size();
  auto egressReadiness = BundleType::get(ctx, {{b.getStringAttr("readValid"), false, bit}, {b.getStringAttr("writeValid"), false, bit}});
  ports.push_back({b.getStringAttr("fased_egress_readiness"), egressReadiness, Direction::In});
  ports.push_back({b.getStringAttr("fased_host_transactions"), transactions, Direction::In});
  ports.push_back({b.getStringAttr("fased_host_mem_idle"), bit, Direction::Out});
  ports.push_back({b.getStringAttr("fased_host_read_inflight"), bit, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto counters = b.create<InstanceOp>(loc, helper, "hostOutstanding");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  auto extra = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(added + i); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(counters.getResult(0), outer(indices[0])); connect(counters.getResult(1), outer(indices[1]));
  connect(counters.getResult(2), extra(1));
  for (auto n : {"readValid", "writeValid"}) connect(field(sim.getResult(indices[2]), n), field(extra(0), n));
  connect(field(sim.getResult(indices[2]), "hostMemIdle"), counters.getResult(3));
  connect(extra(2), counters.getResult(3)); connect(extra(3), counters.getResult(4));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue();
      if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
        // Leaf identities now refer to the egress boundary or counter output;
        // a whole readiness bundle identity remains on the inner simulator.
        if (ref == "fased_readiness.readValid" || ref == "fased_readiness.writeValid")
          suffix = "|" + wrapperName.str() + ">fased_egress_readiness." + ref.drop_front(16).str();
        else if (ref == "fased_readiness.hostMemIdle")
          suffix = "|" + wrapperName.str() + ">fased_host_mem_idle";
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(ctx);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
