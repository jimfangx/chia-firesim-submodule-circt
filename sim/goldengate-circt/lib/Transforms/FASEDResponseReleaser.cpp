// See LICENSE for license details.
// Oracle: Util.scala AXI4Releaser and FASEDMemoryTimingModel.scala 360-398.
// Two one-entry pipe queues track response occupancy, not response metadata.
// Egress requests are combinational nextRead/nextWrite acceptance. Target
// responses forward egress payloads even while invalid. Reads retire only on
// an accepted last beat; replacement in that cycle keeps the queue occupied.
// Model state and reset take effect only on targetFire (the SFC model clock).
// Consumes timing and four egress boundaries; preserves constructor/annotations.
// Timing completion metadata and AW/W/AR model ready remain explicit boundaries.
#include "goldengate/FASEDResponseReleaser.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDResponseReleaser(CircuitOp circuit,
                                                  std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDResponseReleaser";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDResponseReleaserWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDWriteEgressWrapper")
    return reject("FASED response releaser requires the active write egress wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("FASED response releaser helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED response releaser needs top, engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto has = [&](llvm::StringRef n, int w) {
    auto a = widths ? widths.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == w;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("FASED response releaser requires the recorded 35/64/4-bit profile");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> es;
    for (auto [n, w] : fields) es.push_back({b.getStringAttr(n), false, uint(w)});
    return BundleType::get(ctx, es);
  };
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> es) { return BundleType::get(ctx, es); };
  auto decoupled = [&](BundleType bits, bool flipped = false) {
    return bundle({{b.getStringAttr("ready"), !flipped, bit}, {b.getStringAttr("valid"), flipped, bit},
                   {b.getStringAttr("bits"), flipped, bits}});
  };
  auto valid = bundle({{b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(4)}});
  auto address = payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
                          {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto write = payload({{"user",1},{"id",4},{"resp",2}});
  auto read = payload({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}});
  auto response = [&](BundleType bits) {
    return bundle({{b.getStringAttr("tReady"), true, bit}, {b.getStringAttr("hValid"), false, bit},
                   {b.getStringAttr("tBits"), false, bits}});
  };
  auto requests = bundle({{b.getStringAttr("aw"), false, decoupled(address)},
                         {b.getStringAttr("w"), false, decoupled(data)},
                         {b.getStringAttr("ar"), false, decoupled(address)}});
  auto timing = bundle({{b.getStringAttr("aw"), false, decoupled(address)},
                       {b.getStringAttr("w"), false, decoupled(data)},
                       {b.getStringAttr("b"), false, decoupled(write, true)},
                       {b.getStringAttr("ar"), false, decoupled(address)},
                       {b.getStringAttr("r"), false, decoupled(read, true)}});
  const llvm::StringRef names[]{"hostClock", "fased_model_reset", "fased_tfire", "fased_timing",
      "fased_read_egress_req", "fased_read_egress_resp", "fased_write_egress_req", "fased_write_egress_resp"};
  const Type types[]{ClockType::get(ctx), bit, bit, timing, valid, response(read), valid, response(write)};
  const Direction dirs[]{Direction::In, Direction::Out, Direction::Out, Direction::Out,
                        Direction::In, Direction::Out, Direction::In, Direction::Out};
  unsigned indices[8];
  for (unsigned j = 0; j < 8; ++j) {
    std::optional<unsigned> i;
    for (auto [n, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.type == types[j] && p.direction == dirs[j]) i = n;
    if (!i) return reject("FASED response releaser needs exact model clock/reset/fire/timing/egress boundaries");
    indices[j] = *i;
  }
  for (auto p : inner.getPorts())
    if (p.name == "fased_timing_requests" || p.name == "fased_next_read" || p.name == "fased_next_write")
      return reject("FASED response releaser boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED response releaser needs an uninstantiated top");
  auto egressReq = bundle({{b.getStringAttr("b"), false, valid}, {b.getStringAttr("r"), false, valid}});
  auto egressResp = bundle({{b.getStringAttr("bBits"), false, write}, {b.getStringAttr("bReady"), true, bit},
                           {b.getStringAttr("rBits"), false, read}, {b.getStringAttr("rReady"), true, bit}});
  auto nextRead = decoupled(payload({{"id",4},{"len",8}}));
  auto nextWrite = decoupled(payload({{"id",4}}));
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"), types[0], Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("targetFire"), bit, Direction::In},
      {b.getStringAttr("b"), decoupled(write), Direction::Out}, {b.getStringAttr("r"), decoupled(read), Direction::Out},
      {b.getStringAttr("egressReq"), egressReq, Direction::Out}, {b.getStringAttr("egressResp"), egressResp, Direction::In},
      {b.getStringAttr("nextRead"), nextRead, Direction::In}, {b.getStringAttr("nextWrite"), nextWrite, Direction::In}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(ctx, Convention::Internal), hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value a, Value z)->Value { return b.create<AndPrimOp>(loc, a, z); };
  auto either = [&](Value a, Value z)->Value { return b.create<OrPrimOp>(loc, a, z); };
  auto neg = [&](Value a)->Value { return b.create<NotPrimOp>(loc, a); };
  auto mux = [&](Value c, Value a, Value z)->Value { return b.create<MuxPrimOp>(loc, c, a, z); };
  auto zero = b.create<ConstantOp>(loc, bit, APInt(1, 0)).getResult();
  for (bool isRead : {false, true}) {
    Value full = b.create<RegResetOp>(loc, bit, arg(0), both(arg(1), arg(2)), zero,
                                     isRead ? "currentRead_full" : "currentWrite_full").getResult();
    Value out = arg(isRead ? 4 : 3), next = arg(isRead ? 7 : 8);
    Value pop = both(full, field(out, "ready"));
    if (isRead) pop = both(pop, field(field(arg(6), "rBits"), "last"));
    Value ready = either(neg(full), pop), push = both(ready, field(next, "valid"));
    Value change = b.create<XorPrimOp>(loc, push, pop);
    connect(full, mux(both(arg(2), change), push, full));
    connect(field(next, "ready"), ready); connect(field(out, "valid"), full);
    connect(field(out, "bits"), field(arg(6), isRead ? "rBits" : "bBits"));
    Value req = field(arg(5), isRead ? "r" : "b");
    connect(field(req, "valid"), push); connect(field(req, "bits"), field(field(next, "bits"), "id"));
    connect(field(arg(6), isRead ? "rReady" : "bReady"), field(out, "ready"));
  }
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices + 3, 5), i)) { copied.push_back(i); ports.push_back(p); }
  unsigned added = ports.size();
  ports.push_back({b.getStringAttr("fased_timing_requests"), requests, Direction::Out});
  ports.push_back({b.getStringAttr("fased_next_read"), nextRead, Direction::In});
  ports.push_back({b.getStringAttr("fased_next_write"), nextWrite, Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), releaser = b.create<InstanceOp>(loc, helper, "releaser");
  for (auto [j, i] : llvm::enumerate(copied)) {
    Value v = wrapper.getBodyBlock()->getArgument(j); auto p = inner.getPorts()[i];
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : v,
                       p.direction == Direction::In ? v : sim.getResult(i));
  }
  auto extra = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(added + i); };
  connect(releaser.getResult(0), wrapper.getBodyBlock()->getArgument(llvm::find(copied, indices[0]) - copied.begin()));
  connect(releaser.getResult(1), sim.getResult(indices[1])); connect(releaser.getResult(2), sim.getResult(indices[2]));
  for (auto n : {"aw", "w", "ar"}) b.create<ConnectOp>(loc, field(extra(0), n), field(sim.getResult(indices[3]), n));
  for (bool isRead : {false, true}) {
    Value axi = field(sim.getResult(indices[3]), isRead ? "r" : "b"), out = releaser.getResult(isRead ? 4 : 3);
    connect(field(out, "ready"), field(axi, "ready"));
    connect(field(axi, "valid"), field(out, "valid")); connect(field(axi, "bits"), field(out, "bits"));
    connect(sim.getResult(indices[isRead ? 4 : 6]), field(releaser.getResult(5), isRead ? "r" : "b"));
    Value resp = sim.getResult(indices[isRead ? 5 : 7]);
    connect(field(releaser.getResult(6), isRead ? "rBits" : "bBits"), field(resp, "tBits"));
    connect(field(resp, "tReady"), field(releaser.getResult(6), isRead ? "rReady" : "bReady"));
  }
  b.create<ConnectOp>(loc, releaser.getResult(7), extra(1)); b.create<ConnectOp>(loc, releaser.getResult(8), extra(2));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str(),
              modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr)->Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto v = s.getValue(); if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto n = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (n == inner.getPortName(i)) { suffix = "|" + wrapperName.str() + ">" + ref.str(); break; }
        for (auto channel : {"aw", "w", "ar"}) {
          llvm::StringRef leaf(ref); if (leaf.consume_front(std::string("fased_timing.") + channel + "."))
            suffix = "|" + wrapperName.str() + ">fased_timing_requests." + channel + "." + leaf.str();
        }
        // Response and request identities on the now-closed egress/model
        // boundaries remain attached to the inner module where they exist.
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
