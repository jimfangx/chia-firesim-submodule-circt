// See LICENSE for license details.
// Oracle: FASEDMemoryTimingModel.scala 348-407; TimingModel.scala;
// AXI4Releaser. The independent model already qualifies all state/reset by
// targetFire. Do not additionally gate combinational valid/ready: the bridge's
// existing five-way token schedule consumes functional egress hValid and
// supplies targetFire.
#include "goldengate/FASEDFRFCFSBridge.h"
#include "goldengate/FASEDFRFCFSTimingModel.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
namespace {
LogicalResult attach(CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef text) {
    error = text.str();
    return failure();
  };
  bool selected = circuit.getName() == "GGSelectedPrintFASEDWriteEgressWrapper";
  if (!selected && circuit.getName() != "GGFASEDWriteEgressWrapper")
    return reject("FRFCFS bridge requires the active write-egress wrapper");
  StringRef wrapperName = selected ? "GGSelectedPrintFASEDFRFCFSBridgeWrapper"
                                   : "GGFASEDFRFCFSBridgeWrapper";
  FModuleOp inner, engine;
  llvm::StringSet<> symbols;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (!symbols.insert(m.getModuleName()).second ||
        m.getModuleName() == wrapperName)
      return reject("FRFCFS bridge requires unique free module symbols");
    if (m.getModuleName() == circuit.getName())
      inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine")
      engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto base =
      circuit->getAttrOfType<IntegerAttr>("goldengate.fasedBaseCapacity");
  if (!inner || !engine || !raw || !base ||
      base.getValue().getBitWidth() > 64 || base.getInt() != 32)
    return reject("FRFCFS bridge requires top, mapped constructor, annotations "
                  "and explicit base32");
  auto key =
      engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto widths =
      key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto integer = [&](DictionaryAttr d, StringRef n) -> int64_t {
    auto a = d ? d.getAs<IntegerAttr>(n) : IntegerAttr();
    return a && a.getValue().getBitWidth() <= 64 ? a.getInt() : -1;
  };
  int64_t flight = integer(edge, "maxFlight"), reuse = integer(edge, "idReuse");
  if (!((flight == 10 && reuse == 1) || (flight == 20 && reuse == 2)) ||
      integer(edge, "maxReadTransfer") != 8 ||
      integer(edge, "maxWriteTransfer") != 8 ||
      integer(widths, "addrBits") != 35 || integer(widths, "dataBits") != 64 ||
      integer(widths, "idBits") != 4)
    return reject("FRFCFS bridge requires selected 35/64/4, transfer8 and 10/1 "
                  "or20/2 constructor");
  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = u(1);
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> es) {
    return BundleType::get(ctx, es);
  };
  auto payload = [&](std::initializer_list<std::pair<StringRef, unsigned>> es) {
    SmallVector<BundleType::BundleElement> fields;
    for (auto [n, w] : es)
      fields.push_back({b.getStringAttr(n), false, u(w)});
    return BundleType::get(ctx, fields);
  };
  auto dec = [&](BundleType bits, bool flipped = false) {
    return bundle({{b.getStringAttr("ready"), !flipped, bit},
                   {b.getStringAttr("valid"), flipped, bit},
                   {b.getStringAttr("bits"), flipped, bits}});
  };
  auto address = payload({{"user", 1},
                          {"id", 4},
                          {"region", 4},
                          {"qos", 4},
                          {"prot", 3},
                          {"cache", 4},
                          {"lock", 1},
                          {"burst", 2},
                          {"size", 3},
                          {"len", 8},
                          {"addr", 35}});
  auto data =
      payload({{"user", 1}, {"strb", 8}, {"id", 4}, {"last", 1}, {"data", 64}});
  auto write = payload({{"user", 1}, {"id", 4}, {"resp", 2}});
  auto read =
      payload({{"user", 1}, {"id", 4}, {"last", 1}, {"data", 64}, {"resp", 2}});
  auto response = [&](BundleType bits) {
    return bundle({{b.getStringAttr("tReady"), true, bit},
                   {b.getStringAttr("hValid"), false, bit},
                   {b.getStringAttr("tBits"), false, bits}});
  };
  auto valid = bundle({{b.getStringAttr("valid"), false, bit},
                       {b.getStringAttr("bits"), false, u(4)}});
  auto timing = bundle({{b.getStringAttr("aw"), false, dec(address)},
                        {b.getStringAttr("w"), false, dec(data)},
                        {b.getStringAttr("b"), false, dec(write, true)},
                        {b.getStringAttr("ar"), false, dec(address)},
                        {b.getStringAttr("r"), false, dec(read, true)}});
  StringRef names[]{"hostClock",
                    "fased_model_reset",
                    "fased_tfire",
                    "fased_timing",
                    "fased_read_egress_req",
                    "fased_read_egress_resp",
                    "fased_write_egress_req",
                    "fased_write_egress_resp"};
  Type types[]{ClockType::get(ctx),
               bit,
               bit,
               timing,
               valid,
               response(read),
               valid,
               response(write)};
  Direction dirs[]{Direction::In,
                   Direction::Out,
                   Direction::Out,
                   Direction::Out,
                   Direction::In,
                   Direction::Out,
                   Direction::In,
                   Direction::Out};
  unsigned indices[8];
  llvm::StringSet<> portNames;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (!portNames.insert(p.name.getValue()).second ||
        p.name.getValue().starts_with("fased_frfcfs_"))
      return reject(
          "FRFCFS bridge requires unique ports and free exported boundaries");
  }
  for (unsigned j = 0; j < 8; ++j) {
    std::optional<unsigned> index;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.type == types[j] && p.direction == dirs[j])
        index = i;
    if (!index)
      return reject("FRFCFS bridge requires exact timing, clock/reset/fire and "
                    "egress boundaries");
    indices[j] = *index;
  }
  bool used = false;
  circuit.walk(
      [&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used)
    return reject("FRFCFS bridge requires an uninstantiated top");
  FModuleOp model;
  if (failed(goldengate::materializeFASEDFRFCFSTimingModel(
          circuit, flight, model, error)))
    return failure();
  llvm::StringSet<> closed;
  for (auto n : {"clock",
                 "reset",
                 "targetFire",
                 "arValid",
                 "arReady",
                 "arId",
                 "arLen",
                 "arAddr",
                 "arBurst",
                 "awValid",
                 "awReady",
                 "awId",
                 "awLen",
                 "awAddr",
                 "awBurst",
                 "wValid",
                 "wReady",
                 "wLast",
                 "rReady",
                 "rValid",
                 "rId",
                 "rLast",
                 "rData",
                 "rResp",
                 "rUser",
                 "bReady",
                 "bValid",
                 "bId",
                 "bResp",
                 "bUser",
                 "egressReadValid",
                 "egressReadId",
                 "egressWriteValid",
                 "egressWriteId",
                 "egressRId",
                 "egressRLast",
                 "egressRData",
                 "egressRResp",
                 "egressRUser",
                 "egressBId",
                 "egressBResp",
                 "egressBUser",
                 "egressRReady",
                 "egressBReady"})
    closed.insert(n);
  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied, exported;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices + 3, 5), i)) {
      copied.push_back(i);
      ports.push_back(p);
    }
  for (auto [i, p] : llvm::enumerate(model.getPorts()))
    if (!closed.contains(p.getName())) {
      exported.push_back(i);
      ports.push_back({b.getStringAttr(("fased_frfcfs_" + p.getName()).str()),
                       p.type,
                       p.direction});
    }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(
      loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"),
       timingModel = b.create<InstanceOp>(loc, model, "timingModel");
  auto con = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto field = [&](Value v, StringRef n) -> Value {
    return b.create<SubfieldOp>(loc, v, n);
  };
  auto pin = [&](StringRef n) -> Value {
    for (unsigned i = 0; i < model.getNumPorts(); ++i)
      if (model.getPortName(i) == n)
        return timingModel.getResult(i);
    llvm_unreachable("unknown model pin");
  };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i];
    auto arg = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc,
                        p.direction == Direction::In ? sim.getResult(i) : arg,
                        p.direction == Direction::In ? arg : sim.getResult(i));
  }

  con(pin("clock"),
      wrapper.getBodyBlock()->getArgument(llvm::find(copied, indices[0]) -
                                          copied.begin()));
  con(pin("reset"), sim.getResult(indices[1]));
  con(pin("targetFire"), sim.getResult(indices[2]));
  auto axi = sim.getResult(indices[3]);
  for (auto n : {"ar", "aw"}) {
    auto channel = field(axi, n);
    std::string stem(n);
    con(pin(stem + "Valid"), field(channel, "valid"));
    con(field(channel, "ready"), pin(stem + "Ready"));
    for (auto pair : {std::pair<StringRef, StringRef>{"Id", "id"},
                      {"Len", "len"},
                      {"Addr", "addr"},
                      {"Burst", "burst"}})
      con(pin((stem + pair.first).str()),
          field(field(channel, "bits"), pair.second));
  }
  con(pin("wValid"), field(field(axi, "w"), "valid"));
  con(field(field(axi, "w"), "ready"), pin("wReady"));
  con(pin("wLast"), field(field(field(axi, "w"), "bits"), "last"));
  for (bool isRead : {true, false}) {
    std::string stem = isRead ? "r" : "b",
                egress = isRead ? "egressR" : "egressB";
    auto channel = field(axi, stem);
    con(pin(stem + "Ready"), field(channel, "ready"));
    con(field(channel, "valid"), pin(stem + "Valid"));
    for (auto pair : {std::pair<StringRef, StringRef>{"Id", "id"},
                      {"Resp", "resp"},
                      {"User", "user"}}) {
      con(field(field(channel, "bits"), pair.second),
          pin((stem + pair.first).str()));
      con(pin((egress + pair.first).str()),
          field(field(sim.getResult(indices[isRead ? 5 : 7]), "tBits"),
                pair.second));
    }
    if (isRead)
      for (auto pair : {std::pair<StringRef, StringRef>{"Last", "last"},
                        {"Data", "data"}}) {
        con(field(field(channel, "bits"), pair.second),
            pin((stem + pair.first).str()));
        con(pin((egress + pair.first).str()),
            field(field(sim.getResult(indices[5]), "tBits"), pair.second));
      }
    auto req = sim.getResult(indices[isRead ? 4 : 6]);
    con(field(req, "valid"),
        pin(isRead ? "egressReadValid" : "egressWriteValid"));
    con(field(req, "bits"), pin(isRead ? "egressReadId" : "egressWriteId"));
    con(field(sim.getResult(indices[isRead ? 5 : 7]), "tReady"),
        pin(egress + "Ready"));
  }
  for (auto [j, i] : llvm::enumerate(exported)) {
    auto p = model.getPorts()[i];
    auto arg = wrapper.getBodyBlock()->getArgument(copied.size() + j);
    b.create<ConnectOp>(
        loc,
        p.direction == Direction::In ? timingModel.getResult(i) : arg,
        p.direction == Direction::In ? arg : timingModel.getResult(i));
  }
  std::string oldPrefix = "~" + circuit.getName().str(),
              newPrefix = "~" + wrapperName.str(),
              modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget =
      [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto v = s.getValue();
      if (v == oldPrefix)
        return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|"))
        return attr;
      std::string suffix = "|" + v.str();
      StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto n = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied)
          if (n == inner.getPortName(i)) {
            suffix = "|" + wrapperName.str() + ">" + ref.str();
            break;
          }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values;
      for (auto v : a)
        values.push_back(retarget(v));
      return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values;
      for (auto v : d)
        values.set(v.getName(), retarget(v.getValue()));
      return values.getDictionary(ctx);
    }
    return attr;
  };
  SmallVector<Attribute> annotations;
  for (auto a : raw)
    annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  circuit.setName(wrapperName);
  return success();
}
} // namespace
LogicalResult goldengate::addFASEDFRFCFSBridge(CircuitOp circuit,
                                               std::string &error) {
  OwningOpRef<CircuitOp> detached = cast<CircuitOp>(circuit->clone());
  if (failed(attach(*detached, error)))
    return failure();
  if (failed(verify(*detached))) {
    error = "FRFCFS bridge produced invalid circuit";
    return failure();
  }
  // Only appended modules and circuit metadata change. Preserve all original
  // operations/handles (in particular banks materialized before this pass).
  llvm::StringSet<> originalSymbols;
  for (auto m : circuit.getOps<FModuleLike>())
    originalSymbols.insert(m.getModuleName());
  for (auto &op : llvm::make_early_inc_range(*detached->getBodyBlock())) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (m && !originalSymbols.contains(m.getModuleName()))
      op.moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  }
  circuit->setAttrs((*detached)->getAttrs());
  return success();
}
