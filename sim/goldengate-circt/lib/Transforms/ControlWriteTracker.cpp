// See LICENSE for license details.
// Bind the AW outstanding ReorderQueue to write-route acceptance and B fire.
// Required input: uninstantiated B-arbiter wrapper, retained annotations and
// the decoder's one-to-63-region catalog. Region count determines route width
// and normal/error retirement count; all boundaries validate before mutation.
// Copied targets transfer to the new top; consumed handshake/retirement targets
// stay on the inner module. Every annotation class and payload is retained.
#include "goldengate/ControlWriteTracker.h"
#include "goldengate/ControlTransactionTracker.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addControlWriteTracker(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlWriteTrackerWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlWriteTracker";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGControlWriteArbiterWrapper")
    return reject("control write tracker requires the B arbiter wrapper");
  FModuleOp inner, decoder;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("control write tracker module already exists");
    if (m.getModuleName() == "GGControlAddressDecode") decoder = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto catalog = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr();
  if (!inner || !raw || !catalog || catalog.empty() || catalog.size() > 63)
    return reject("control write tracker requires a top, retained annotations and one to 63 decoded regions");
  const unsigned slaveCount = catalog.size();
  const unsigned routeWidth = llvm::Log2_64_Ceil(slaveCount + 1);
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("control write tracker requires an uninstantiated top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  std::map<std::string, unsigned> old, copied, hi, exposed;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) old[p.name.getValue().str()] = i;
  auto exact = [&](std::string n, Type t, Direction d) {
    auto it = old.find(n);
    return it != old.end() && inner.getPorts()[it->second].type == t && inner.getPorts()[it->second].direction == d;
  };
  if (!exact("hostClock", ClockType::get(ctx), Direction::In) || !exact("hostReset", uint(1), Direction::In) ||
      !exact("ctrl_decode_aw_target", uint(routeWidth), Direction::Out) ||
      !exact("ctrl_write_dispatch_master_aw_bits_id", uint(12), Direction::In) ||
      !exact("ctrl_write_route_aw_tracker_ready", uint(1), Direction::In) ||
      !exact("ctrl_write_route_aw_track_valid", uint(1), Direction::Out))
    return reject("control write tracker requires exact clock, AW target, ID and acceptance boundaries");
  std::set<std::string> consumed{"ctrl_write_route_aw_tracker_ready", "ctrl_write_route_aw_track_valid"};
  for (unsigned i = 0; i < slaveCount + 1; ++i) for (auto suffix : {"valid", "tag"}) {
    std::string n = "ctrl_write_tracker_deq_" + std::to_string(i) + "_" + suffix;
    if (!exact(n, uint(StringRef(suffix) == "tag" ? 12 : 1), Direction::Out))
      return reject("control write tracker requires all decoded-slave and error B retirement boundaries");
    consumed.insert(n);
  }
  auto hp = controlTransactionTrackerPorts(ctx, slaveCount);
  for (auto [i, p] : llvm::enumerate(hp)) hi[p.name.getValue().str()] = i;
  SmallVector<PortInfo> ports;
  for (auto p : inner.getPorts()) if (!consumed.count(p.name.getValue().str())) {
    copied[p.name.getValue().str()] = ports.size(); ports.push_back(p);
  }
  for (auto p : hp) if (p.name.getValue().starts_with("deq_") && p.direction == Direction::Out) {
    std::string n = "ctrl_write_tracker_" + p.name.getValue().str();
    if (old.count(n)) return reject("control write tracker output boundary already exists");
    exposed[p.name.getValue().str()] = ports.size(); p.name = b.getStringAttr(n); ports.push_back(p);
  }
  auto helper = createControlTransactionTracker(circuit, helperName, "aw_queue", slaveCount);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), tracker = b.create<InstanceOp>(loc, helper, "controlWriteTracker");
  auto result = [&](std::string n) { return sim.getResult(old.at(n)); };
  auto track = [&](std::string n) { return tracker.getResult(hi.at(n)); };
  auto external = [&](std::string n) { return wrapper.getBodyBlock()->getArgument(copied.at(n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  for (auto [n, i] : copied) {
    auto d = inner.getPorts()[old.at(n)].direction; Value outside = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, d == Direction::In ? result(n) : outside, d == Direction::In ? outside : result(n));
  }
  connect(track("clock"), external("hostClock")); connect(track("reset"), external("hostReset"));
  connect(track("enq_valid"), result("ctrl_write_route_aw_track_valid"));
  connect(track("enq_bits_tag"), external("ctrl_write_dispatch_master_aw_bits_id"));
  connect(track("enq_bits_data"), result("ctrl_decode_aw_target"));
  connect(result("ctrl_write_route_aw_tracker_ready"), track("enq_ready"));
  for (unsigned i = 0; i < slaveCount + 1; ++i) for (auto suffix : {"valid", "tag"}) {
    std::string n = "deq_" + std::to_string(i) + "_" + suffix;
    connect(track(n), result("ctrl_write_tracker_" + n));
  }
  for (auto [n, i] : exposed) connect(wrapper.getBodyBlock()->getArgument(i), track(n));
  // Consumed acceptance/retirement targets stay on the inner module; copied
  // master/clock/response ports transfer to the new top. Keep every class/key.
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
