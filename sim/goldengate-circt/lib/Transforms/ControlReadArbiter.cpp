// See LICENSE for license details.
// Port NastiRouter's HellaPeekingArbiter response selection and burst locking.
#include "goldengate/ControlReadArbiter.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addControlReadArbiter(CircuitOp circuit,
                                               std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlReadArbiterWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlReadArbiter";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGControlReadTrackerWrapper")
    return reject("control read arbiter requires the AR tracker wrapper");
  FModuleOp inner, decoder;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("control read arbiter module already exists");
    if (m.getModuleName() == "GGControlAddressDecode") decoder = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto bindings = inner ? inner->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings") : ArrayAttr();
  auto catalog = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr();
  if (!inner || !raw || !bindings || !catalog || catalog.empty() || catalog.size() > 63)
    return reject("control read arbiter requires retained annotations, read bindings and one to 63 decoded regions");
  const unsigned slaveCount = catalog.size(), sourceCount = slaveCount + 1;
  const unsigned indexWidth = llvm::Log2_64_Ceil(sourceCount);
  SmallVector<StringAttr> regionNames;
  std::set<std::string> uniqueNames;
  for (auto [i, a] : llvm::enumerate(catalog)) {
    auto d = dyn_cast<DictionaryAttr>(a);
    auto name = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto slave = d ? d.getAs<IntegerAttr>("slave") : IntegerAttr();
    if (!name || name.getValue().empty() || !slave || slave.getInt() != int64_t(i) ||
        !uniqueNames.insert(name.getValue().str()).second)
      return reject("control read arbiter requires unique region names and ordered decoded-slave indices");
    regionNames.push_back(name);
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("control read arbiter requires an uninstantiated top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> xs) { return BundleType::get(ctx, xs); };
  auto token = [&](FIRRTLBaseType bits) {
    return bundle({{b.getStringAttr("ready"), true, uint(1)},
        {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, bits}});
  };
  auto bType = token(bundle({{b.getStringAttr("resp"), false, uint(2)},
      {b.getStringAttr("id"), false, uint(12)}, {b.getStringAttr("user"), false, uint(1)}}));
  auto rType = token(bundle({{b.getStringAttr("resp"), false, uint(2)},
      {b.getStringAttr("data"), false, uint(32)}, {b.getStringAttr("last"), false, uint(1)},
      {b.getStringAttr("id"), false, uint(12)}, {b.getStringAttr("user"), false, uint(1)}}));
  auto controlType = bundle({{b.getStringAttr("b"), true, bType}, {b.getStringAttr("r"), true, rType}});
  std::map<std::string, unsigned> old, copied, hpIndex, exposed;
  std::map<unsigned, std::string> widgets;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name.getValue().starts_with("ctrl_read_arb_"))
      return reject("control read arbiter boundary already exists");
    old.emplace(p.name.getValue().str(), i);
  }
  struct Required { const char *name; Type type; Direction direction; };
  const Required required[]{{"hostClock", ClockType::get(ctx), Direction::In},
      {"hostReset", uint(1), Direction::In},
      {"ctrl_error_r_ready", uint(1), Direction::In},
      {"ctrl_error_r_valid", uint(1), Direction::Out},
      {"ctrl_error_r_bits_last", uint(1), Direction::Out},
      {"ctrl_error_r_bits_id", uint(12), Direction::Out},
      {"ctrl_error_r_bits_data", uint(32), Direction::Out},
      {"ctrl_error_r_bits_resp", uint(2), Direction::Out},
      {"ctrl_error_r_bits_user", uint(1), Direction::Out}};
  for (auto r : required) {
    auto it = old.find(r.name);
    if (it == old.end() || inner.getPorts()[it->second].type != r.type ||
        inner.getPorts()[it->second].direction != r.direction)
      return reject("control read arbiter requires exact clock and error response boundaries");
  }
  std::set<std::string> widgetPorts;
  for (auto a : bindings) {
    auto d = dyn_cast<DictionaryAttr>(a);
    auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
    auto slave = d ? d.getAs<IntegerAttr>("slave") : IntegerAttr();
    auto name = d ? d.getAs<StringAttr>("name") : StringAttr();
    if (!port || !slave || !name || slave.getInt() < 0 || slave.getInt() >= slaveCount ||
        regionNames[slave.getInt()] != name || widgets.count(slave.getInt()) ||
        !widgetPorts.insert(port.getValue().str()).second)
      return reject("control read arbiter read bindings must identify unique decoded slaves and matching region names");
    auto it = old.find(port.getValue().str());
    if (it == old.end() || inner.getPorts()[it->second].type != controlType ||
        inner.getPorts()[it->second].direction != Direction::In)
      return reject("control read arbiter widget response bundle differs");
    widgets[slave.getInt()] = port.getValue().str();
  }
  for (unsigned i = 0; i < slaveCount; ++i) if (!widgets.count(i)) {
    for (auto suffix : {"valid", "tag"}) {
      std::string n = "ctrl_read_tracker_deq_" + std::to_string(i) + "_" + suffix;
      auto it = old.find(n);
      if (it == old.end() || inner.getPorts()[it->second].direction != Direction::In ||
          inner.getPorts()[it->second].type != uint(StringRef(suffix) == "tag" ? 12 : 1))
        return reject("control read arbiter requires explicit tracker retirement inputs for every unbound decoded slave");
    }
  }
  std::set<std::string> consumed;
  for (auto p : inner.getPorts()) if (p.name.getValue().starts_with("ctrl_error_r_")) consumed.insert(p.name.getValue().str());
  for (unsigned i = 0; i < slaveCount; ++i) if (!widgets.count(i)) for (auto s : {"valid", "tag"})
    consumed.insert("ctrl_read_tracker_deq_" + std::to_string(i) + "_" + s);
  struct Leaf { const char *name; unsigned width; };
  const Leaf leaves[]{{"resp", 2}, {"data", 32}, {"last", 1}, {"id", 12}, {"user", 1}};
  SmallVector<PortInfo> hp;
  auto port = [&](std::string n, Type t, Direction d) {
    hpIndex[n] = hp.size(); hp.push_back({b.getStringAttr(n), t, d});
  };
  port("clock", ClockType::get(ctx), Direction::In); port("reset", uint(1), Direction::In);
  for (unsigned i = 0; i < sourceCount; ++i) {
    std::string p = "in_" + std::to_string(i) + "_";
    port(p + "ready", uint(1), Direction::Out); port(p + "valid", uint(1), Direction::In);
    for (auto l : leaves) port(p + "bits_" + l.name, uint(l.width), Direction::In);
  }
  port("out_ready", uint(1), Direction::In); port("out_valid", uint(1), Direction::Out);
  for (auto l : leaves) port(std::string("out_bits_") + l.name, uint(l.width), Direction::Out);
  SmallVector<PortInfo> ports;
  auto bOnly = bundle({{b.getStringAttr("b"), true, bType}});
  for (auto p : inner.getPorts()) if (!consumed.count(p.name.getValue().str())) {
    if (widgetPorts.count(p.name.getValue().str())) p.type = bOnly;
    copied[p.name.getValue().str()] = ports.size(); ports.push_back(p);
  }
  for (unsigned i = 2; i < hp.size(); ++i) {
    auto n = hp[i].name.getValue(); bool outside = n.starts_with("out_");
    for (unsigned slave = 0; slave < slaveCount; ++slave)
      if (!widgets.count(slave)) outside |= n.starts_with("in_" + std::to_string(slave) + "_");
    if (outside) {
      auto p = hp[i]; exposed[n.str()] = ports.size();
      p.name = b.getStringAttr("ctrl_read_arb_" + n.str()); ports.push_back(p);
    }
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(ctx, Convention::Internal), hp);
  helper->setAttr("goldengate.readArbiterSources", b.getI32IntegerAttr(sourceCount));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](llvm::StringRef n) { return helper.getBodyBlock()->getArgument(hpIndex.at(n.str())); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto invert = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value s, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, s, y, n); };
  auto eq = [&](Value a, unsigned i) -> Value { return b.create<EQPrimOp>(loc, a, constant(indexWidth, i)); };
  Value index = b.create<RegResetOp>(loc, uint(indexWidth), arg("clock"), arg("reset"), constant(indexWidth, 0), "lockIdx").getResult();
  Value locked = b.create<RegResetOp>(loc, uint(1), arg("clock"), arg("reset"), constant(1, 0), "locked").getResult();
  // Choose the first valid source following the previous lock index. The
  // PriorityMux default is the final rotated source, even if all are invalid.
  // Reset and every subsequent index update keep lockIdx within the response source catalog.
  Value choice = constant(indexWidth, 0);
  for (unsigned start = 0; start < sourceCount; ++start) {
    Value candidate = constant(indexWidth, start);
    for (unsigned offset = sourceCount - 1; offset > 0; --offset) {
      unsigned i = (start + offset) % sourceCount;
      candidate = mux(arg("in_" + std::to_string(i) + "_valid"), constant(indexWidth, i), candidate);
    }
    choice = mux(eq(index, start), candidate, choice);
  }
  Value chosen = mux(locked, index, choice);
  auto select = [&](std::string suffix) -> Value {
    Value v = arg("in_0_" + suffix);
    for (unsigned i = 1; i < sourceCount; ++i) v = mux(eq(chosen, i), arg("in_" + std::to_string(i) + "_" + suffix), v);
    return v;
  };
  Value valid = select("valid"), last = select("bits_last"), fire = both(arg("out_ready"), valid);
  connect(arg("out_valid"), valid);
  for (auto l : leaves) connect(arg(std::string("out_bits_") + l.name), select(std::string("bits_") + l.name));
  for (unsigned i = 0; i < sourceCount; ++i) connect(arg("in_" + std::to_string(i) + "_ready"), both(arg("out_ready"), eq(chosen, i)));
  connect(index, mux(both(fire, invert(locked)), choice, index));
  // The unlock overrides lock acquisition, including a one-beat transaction.
  connect(locked, mux(fire, invert(last), locked));
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  wrapper->setAttr("goldengate.controlReadBindings", bindings);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), arb = b.create<InstanceOp>(loc, helper, "readArbiter");
  auto topArg = [&](std::string n) { return wrapper.getBodyBlock()->getArgument(copied.at(n)); };
  auto result = [&](std::string n) { return sim.getResult(old.at(n)); };
  auto ap = [&](std::string n) { return arb.getResult(hpIndex.at(n)); };
  for (auto [n, i] : copied) {
    Value external = wrapper.getBodyBlock()->getArgument(i);
    if (widgetPorts.count(n)) {
      b.create<ConnectOp>(loc, field(external, "b"), field(result(n), "b"));
    } else {
      auto d = inner.getPorts()[old.at(n)].direction;
      b.create<ConnectOp>(loc, d == Direction::In ? result(n) : external, d == Direction::In ? external : result(n));
    }
  }
  connect(ap("clock"), topArg("hostClock")); connect(ap("reset"), topArg("hostReset"));
  for (auto [n, i] : exposed) {
    Value external = wrapper.getBodyBlock()->getArgument(i), v = ap(n);
    if (hp[hpIndex.at(n)].direction == Direction::In) connect(v, external); else connect(external, v);
  }
  for (auto [i, n] : widgets) {
    Value r = field(result(n), "r"), bits = field(r, "bits"); std::string p = "in_" + std::to_string(i) + "_";
    connect(field(r, "ready"), ap(p + "ready")); connect(ap(p + "valid"), field(r, "valid"));
    for (auto l : leaves) connect(ap(p + "bits_" + l.name), field(bits, l.name));
  }
  std::string errorSource = "in_" + std::to_string(slaveCount) + "_";
  connect(result("ctrl_error_r_ready"), ap(errorSource + "ready")); connect(ap(errorSource + "valid"), result("ctrl_error_r_valid"));
  for (auto l : leaves) connect(ap(errorSource + "bits_" + l.name), result(std::string("ctrl_error_r_bits_") + l.name));
  for (unsigned i = 0; i < slaveCount; ++i) if (!widgets.count(i)) {
    std::string p = "in_" + std::to_string(i) + "_", q = "ctrl_read_tracker_deq_" + std::to_string(i) + "_";
    connect(result(q + "valid"), both(both(ap(p + "ready"), ap(p + "valid")), ap(p + "bits_last")));
    connect(result(q + "tag"), ap(p + "bits_id"));
  }
  std::string op = "~" + circuit.getName().str(), np = "~" + wrapperName.str(), mp = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue(); if (v == op) return b.getStringAttr(np); if (!v.consume_front(op + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(mp) && ([&] {
        auto root = ref.take_front(ref.find_first_of(".[")).str();
        if (!copied.count(root)) return false;
        if (!widgetPorts.count(root)) return true;
        // A whole response bundle or its removed R field remains in the inner
        // module. Only the retained B field moves to the replacement port.
        return ref.starts_with(root + ".b.") || ref == root + ".b";
      })())
        suffix.replace(0, mp.size(), "|" + wrapperName.str() + ">");
      return b.getStringAttr(np + suffix);
    }
    if (auto xs = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto x : xs) out.push_back(retarget(x)); return b.getArrayAttr(out); }
    if (auto xs = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto x : xs) out.set(x.getName(), retarget(x.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}
