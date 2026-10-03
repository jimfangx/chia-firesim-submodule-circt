// See LICENSE for license details.
// Oracle: TimingModel.scala 100-102, LatencyBandwidthPipe.scala 67/81.
// The recorded Rocket timing model is LatencyPipe. Its 64-bit model cycle
// advances on targetFire; reset is sampled on that same enabled clock edge.
// Release cycles use zero-extended 32-bit runtime latencies, modulo 2^64,
// less the one-cycle AXI4Releaser delay. Latency MMIO and completion queues
// remain explicit boundaries for subsequent native transforms.
#include "goldengate/FASEDTimingCycle.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDTimingCycle(CircuitOp circuit,
                                             std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDTimingCycle";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDTimingCycleWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDResponseReleaserWrapper")
    return reject("FASED timing cycle requires the active response releaser wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("FASED timing cycle helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED timing cycle needs top, engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto has = [&](llvm::StringRef n, int w) {
    auto a = widths ? widths.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == w;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("FASED timing cycle requires the recorded 35/64/4-bit profile");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  const llvm::StringRef names[]{"hostClock", "fased_model_reset", "fased_tfire"};
  const Type types[]{ClockType::get(ctx), bit, bit};
  const Direction dirs[]{Direction::In, Direction::Out, Direction::Out};
  unsigned indices[3];
  for (unsigned j = 0; j < 3; ++j) {
    std::optional<unsigned> i;
    for (auto [n, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.type == types[j] && p.direction == dirs[j]) i = n;
    if (!i) return reject("FASED timing cycle needs exact model clock/reset/fire boundaries");
    indices[j] = *i;
  }
  const llvm::StringRef addedNames[]{"fased_timing_cycle", "fased_read_latency", "fased_write_latency",
      "fased_read_release_cycle", "fased_write_release_cycle"};
  for (auto p : inner.getPorts())
    if (llvm::is_contained(ArrayRef<llvm::StringRef>(addedNames), p.name.getValue()))
      return reject("FASED timing cycle boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED timing cycle needs an uninstantiated top");
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"), types[0], Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("targetFire"), bit, Direction::In},
      {b.getStringAttr("tCycle"), uint(64), Direction::Out},
      {b.getStringAttr("readLatency"), uint(32), Direction::In},
      {b.getStringAttr("writeLatency"), uint(32), Direction::In},
      {b.getStringAttr("readReleaseCycle"), uint(64), Direction::Out},
      {b.getStringAttr("writeReleaseCycle"), uint(64), Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(ctx, Convention::Internal), hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto zero = b.create<ConstantOp>(loc, uint(64), APInt(64, 0)).getResult();
  auto one = b.create<ConstantOp>(loc, uint(64), APInt(64, 1)).getResult();
  Value reset = b.create<AndPrimOp>(loc, arg(1), arg(2));
  Value cycle = b.create<RegResetOp>(loc, uint(64), arg(0), reset, zero, "tCycle").getResult();
  Value increment = b.create<AddPrimOp>(loc, cycle, one);
  increment = b.create<BitsPrimOp>(loc, increment, 63, 0);
  connect(cycle, b.create<MuxPrimOp>(loc, arg(2), increment, cycle));
  connect(arg(3), cycle);
  for (unsigned j = 0; j < 2; ++j) {
    Value latency = b.create<PadPrimOp>(loc, arg(4 + j), 64);
    Value deadline = b.create<AddPrimOp>(loc, cycle, latency);
    deadline = b.create<SubPrimOp>(loc, deadline, one);
    connect(arg(6 + j), b.create<BitsPrimOp>(loc, deadline, 63, 0));
  }
  SmallVector<PortInfo> ports(inner.getPorts()); unsigned added = ports.size();
  for (unsigned j = 0; j < 5; ++j)
    ports.push_back({b.getStringAttr(addedNames[j]), uint(j == 1 || j == 2 ? 32 : 64),
                    j == 1 || j == 2 ? Direction::In : Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), timer = b.create<InstanceOp>(loc, helper, "timer");
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    Value v = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : v,
                       p.direction == Direction::In ? v : sim.getResult(i));
  }
  connect(timer.getResult(0), wrapper.getBodyBlock()->getArgument(indices[0]));
  connect(timer.getResult(1), sim.getResult(indices[1]));
  connect(timer.getResult(2), sim.getResult(indices[2]));
  for (unsigned j = 0; j < 5; ++j) {
    Value v = wrapper.getBodyBlock()->getArgument(added + j), t = timer.getResult(3 + j);
    connect(j == 1 || j == 2 ? t : v, j == 1 || j == 2 ? v : t);
  }
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str(),
              modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr)->Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto v = s.getValue(); if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto n = ref.take_front(ref.find_first_of(".["));
        for (auto p : inner.getPorts()) if (n == p.name) {
          suffix = "|" + wrapperName.str() + ">" + ref.str(); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) { SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values); }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) { NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(ctx); }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName); return success();
}
