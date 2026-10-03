// See LICENSE for license details.
// Port junctions/ReorderQueue.scala's small-tag-space storage semantics.
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlTransactionTracker.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addControlReadTracker(CircuitOp circuit,
                                               std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlReadTrackerWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlReadTracker";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGControlReadDispatchWrapper")
    return reject("control read tracker requires the AR dispatch wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("control read tracker module already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto bindings = inner ? inner->getAttrOfType<ArrayAttr>("goldengate.controlReadBindings") : ArrayAttr();
  if (!inner || !raw || !bindings || bindings.size() != 7)
    return reject("control read tracker requires retained annotations and seven read bindings");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("control read tracker requires an uninstantiated top");
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
    if (p.name.getValue().starts_with("ctrl_read_tracker_"))
      return reject("control read tracker boundary already exists");
    old.emplace(p.name.getValue().str(), i);
  }
  struct Required { const char *name; Type type; Direction direction; };
  const Required required[]{{"hostClock", ClockType::get(ctx), Direction::In},
      {"hostReset", uint(1), Direction::In},
      {"ctrl_read_dispatch_tracker_ready", uint(1), Direction::In},
      {"ctrl_read_dispatch_track_valid", uint(1), Direction::Out},
      {"ctrl_read_dispatch_track_tag", uint(12), Direction::Out},
      {"ctrl_read_dispatch_track_target", uint(4), Direction::Out},
      {"ctrl_error_r_ready", uint(1), Direction::In},
      {"ctrl_error_r_valid", uint(1), Direction::Out},
      {"ctrl_error_r_bits_last", uint(1), Direction::Out},
      {"ctrl_error_r_bits_id", uint(12), Direction::Out}};
  for (auto r : required) {
    auto it = old.find(r.name);
    if (it == old.end() || inner.getPorts()[it->second].type != r.type ||
        inner.getPorts()[it->second].direction != r.direction)
      return reject("control read tracker requires exact U250 clock, dispatch and error boundaries");
  }
  std::set<std::string> widgetPorts;
  const std::map<unsigned, llvm::StringRef> expected{{2, "tracerv_ctrl"}, {4, "loadmem_ctrl"},
      {5, "peekPokeBridge_ctrl"}, {6, "uartBridge_ctrl"}, {7, "clockBridge_ctrl"},
      {9, "resetBridge_ctrl"}, {10, "cpuStream_ctrl"}};
  for (auto a : bindings) {
    auto d = dyn_cast<DictionaryAttr>(a);
    auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
    auto slave = d ? d.getAs<IntegerAttr>("slave") : IntegerAttr();
    if (!port || !slave || !expected.count(slave.getInt()) ||
        expected.at(slave.getInt()) != port.getValue() || widgets.count(slave.getInt()) ||
        !widgetPorts.insert(port.getValue().str()).second)
      return reject("control read tracker requires the seven mapped U250 widget indices");
    auto it = old.find(port.getValue().str());
    if (it == old.end() || inner.getPorts()[it->second].type != controlType ||
        inner.getPorts()[it->second].direction != Direction::In)
      return reject("control read tracker widget response bundle differs");
    widgets[slave.getInt()] = port.getValue().str();
  }
  const std::set<std::string> consumed{"ctrl_read_dispatch_tracker_ready",
      "ctrl_read_dispatch_track_valid", "ctrl_read_dispatch_track_tag", "ctrl_read_dispatch_track_target"};
  auto hp = controlTransactionTrackerPorts(ctx);
  for (auto [i, p] : llvm::enumerate(hp)) hpIndex[p.name.getValue().str()] = i;
  SmallVector<PortInfo> ports;
  for (auto p : inner.getPorts()) if (!consumed.count(p.name.getValue().str())) {
    copied[p.name.getValue().str()] = ports.size(); ports.push_back(p);
  }
  for (unsigned i = 6; i < hp.size(); ++i) {
    unsigned slave = (i - 6) / 4;
    if (hp[i].direction == Direction::Out || (slave != 11 && !widgets.count(slave))) {
      auto p = hp[i]; exposed[p.name.getValue().str()] = ports.size();
      p.name = b.getStringAttr("ctrl_read_tracker_" + p.name.getValue().str()); ports.push_back(p);
    }
  }
  auto helper = createControlTransactionTracker(circuit, helperName, "ar_queue");
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  wrapper->setAttr("goldengate.controlReadBindings", bindings);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), tracker = b.create<InstanceOp>(loc, helper, "controlReadTracker");
  auto topArg = [&](std::string n) { return wrapper.getBodyBlock()->getArgument(copied.at(n)); };
  auto result = [&](std::string n) { return sim.getResult(old.at(n)); };
  auto track = [&](std::string n) { return tracker.getResult(hpIndex.at(n)); };
  for (auto [n, i] : copied) {
    auto d = inner.getPorts()[old.at(n)].direction; Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, d == Direction::In ? result(n) : external, d == Direction::In ? external : result(n));
  }
  connect(track("clock"), topArg("hostClock")); connect(track("reset"), topArg("hostReset"));
  connect(track("enq_valid"), result("ctrl_read_dispatch_track_valid"));
  connect(track("enq_bits_tag"), result("ctrl_read_dispatch_track_tag"));
  connect(track("enq_bits_data"), result("ctrl_read_dispatch_track_target"));
  connect(result("ctrl_read_dispatch_tracker_ready"), track("enq_ready"));
  for (auto [n, i] : exposed) {
    Value external = wrapper.getBodyBlock()->getArgument(i), v = track(n);
    if (hp[hpIndex.at(n)].direction == Direction::In) connect(v, external);
    else connect(external, v);
  }
  for (auto [i, n] : widgets) {
    Value r = field(result(n), "r"), bits = field(r, "bits");
    Value fireLast = both(both(field(field(topArg(n), "r"), "ready"), field(r, "valid")), field(bits, "last"));
    connect(track("deq_" + std::to_string(i) + "_valid"), fireLast);
    connect(track("deq_" + std::to_string(i) + "_tag"), field(bits, "id"));
  }
  connect(track("deq_11_valid"), both(both(topArg("ctrl_error_r_ready"), result("ctrl_error_r_valid")), result("ctrl_error_r_bits_last")));
  connect(track("deq_11_tag"), result("ctrl_error_r_bits_id"));
  std::string op = "~" + circuit.getName().str(), np = "~" + wrapperName.str(), mp = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue(); if (v == op) return b.getStringAttr(np); if (!v.consume_front(op + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(mp) && copied.count(ref.take_front(ref.find_first_of(".[")).str()))
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
