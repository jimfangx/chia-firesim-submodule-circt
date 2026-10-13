// See LICENSE for license details.
// Oracle: BigTokenToNICTokenAdapter / NICTokenToBigTokenAdapter in
// SimpleNICBridge.scala. Normal mode, seven tokens, host reset counters only.
// Preserve BIGToken's exact data/RVL/pad layout and seventh-token bypass.
// Incoming ready bits are discarded by the existing NIC token queue boundary;
// outgoing ready bits are constant one, as in SimpleNICBridgeModule.
// Consumed port targets remain on the retained inner module; copied port and
// circuit identities transfer. Annotation classes and other bodies survive.
#include "goldengate/NICPacketAdapters.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addNICPacketAdapters(CircuitOp circuit, std::string &error) {
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("NIC packet adapters require retained annotations");
  unsigned nics = 0;
  for (auto attr : raw) {
    Annotation a(attr); auto w = a.getMember<StringAttr>("widgetClass");
    if (a.isClass(AnnotationClasses::BridgeIO) && w &&
        w.getValue() == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (cast<DictionaryAttr>(attr).get("widgetConstructorKey"))
        return reject("NIC packet adapters require a constructor-free widget");
      ++nics;
    }
  }
  if (!nics) return success();
  if (nics != 1 || circuit.getName() != "GGNICTokenQueuesWrapper")
    return reject("NIC packet adapters require one active NIC queue boundary");
  constexpr llvm::StringLiteral decoderName = "GGNICBigTokenDecoder";
  constexpr llvm::StringLiteral encoderName = "GGNICBigTokenEncoder";
  constexpr llvm::StringLiteral wrapperName = "GGNICPacketAdaptersWrapper";
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == decoderName || m.getModuleName() == encoderName || m.getModuleName() == wrapperName)
      return reject("NIC packet adapter module already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  if (!inner) return reject("NIC packet adapters require an internal top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto token = [&](unsigned w) { return BundleType::get(ctx, {
      {b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, uint(w)}}); };
  auto port = [&](llvm::StringRef name, Type type, Direction dir) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == dir) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(ctx), Direction::In);
  auto reset = port("hostReset", bit, Direction::In);
  auto incoming = port("nic_from_host_enq", token(66), Direction::In);
  auto outgoing = port("nic_to_host_deq", token(66), Direction::Out);
  if (!clock || !reset || !incoming || !outgoing)
    return reject("NIC packet adapters need typed host clock/reset and queue ports");
  for (auto p : inner.getPorts())
    if (p.name == "nic_from_host_stream" || p.name == "nic_to_host_stream")
      return reject("NIC packet stream boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("NIC packet adapters require an uninstantiated top");
  // All rejection checks precede mutation.
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto bits = [&](Value v, unsigned hi, unsigned lo) -> Value { return b.create<BitsPrimOp>(loc, v, hi, lo); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto mux = [&](Value c, Value a, Value d) -> Value { return b.create<MuxPrimOp>(loc, c, a, d); };
  auto cat = [&](Value a, Value d) -> Value { return b.create<CatPrimOp>(loc, a, d); };
  auto createAdapter = [&](llvm::StringRef name, unsigned inWidth, unsigned outWidth) {
    SmallVector<PortInfo> ps{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
        {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("in"), token(inWidth), Direction::In},
        {b.getStringAttr("out"), token(outWidth), Direction::Out}};
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto m = b.create<FModuleOp>(loc, b.getStringAttr(name), ConventionAttr::get(ctx, Convention::Internal), ps);
    b.setInsertionPointToStart(m.getBodyBlock()); return m;
  };
  auto reg = [&](FModuleOp m, unsigned w, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, uint(w), m.getBodyBlock()->getArgument(0),
        m.getBodyBlock()->getArgument(1), constant(w, 0), name).getResult();
  };
  auto increment = [&](Value v, unsigned w) { return bits(b.create<AddPrimOp>(loc, v, constant(w, 1)), w - 1, 0); };
  auto decoder = createAdapter(decoderName, 512, 66);
  auto di = decoder.getBodyBlock()->getArgument(2), dout = decoder.getBodyBlock()->getArgument(3);
  auto index = reg(decoder, 32, "loopIter");
  Value last = b.create<EQPrimOp>(loc, index, constant(32, 6));
  Value fire = both(field(di, "valid"), field(dout, "ready"));
  connect(index, mux(fire, mux(last, constant(32, 0), increment(index, 32)), index));
  connect(field(dout, "valid"), field(di, "valid"));
  connect(field(di, "ready"), both(field(dout, "ready"), last));
  Value packet = field(di, "bits");
  auto decoded = [&](unsigned i) { return cat(bits(packet, 43 + 3*i, 43 + 3*i),
      cat(bits(packet, 45 + 3*i, 45 + 3*i), bits(packet, 127 + 64*i, 64 + 64*i))); };
  Value payload = decoded(0), lowIndex = bits(index, 2, 0);
  for (unsigned i = 1; i < 7; ++i)
    payload = mux(b.create<EQPrimOp>(loc, lowIndex, constant(3, i)), decoded(i), payload);
  connect(field(dout, "bits"), payload);

  auto encoder = createAdapter(encoderName, 66, 512);
  auto ei = encoder.getBodyBlock()->getArgument(2), eout = encoder.getBodyBlock()->getArgument(3);
  auto counter = reg(encoder, 32, "specialCounter"), trace = reg(encoder, 43, "token_trace_counter");
  Value seventh = b.create<EQPrimOp>(loc, counter, constant(32, 6));
  Value ready = b.create<OrPrimOp>(loc, b.create<NotPrimOp>(loc, seventh), field(eout, "ready"));
  Value valid = both(seventh, field(ei, "valid"));
  connect(field(ei, "ready"), ready); connect(field(eout, "valid"), valid);
  connect(counter, mux(both(field(ei, "valid"), ready), mux(seventh, constant(32, 0), increment(counter, 32)), counter));
  connect(trace, mux(both(valid, field(eout, "ready")), increment(trace, 43), trace));
  SmallVector<Value> tokens;
  for (unsigned i = 0; i < 6; ++i) {
    Value r = b.create<RegOp>(loc, uint(66), encoder.getBodyBlock()->getArgument(0), "buffer_" + std::to_string(i)).getResult();
    // Oracle writes on valid, not fire. Counts 0..5 always accept; buffer 6
    // is dead because the seventh token is bypassed directly below.
    connect(r, mux(both(field(ei, "valid"), b.create<EQPrimOp>(loc, counter, constant(32, i))), field(ei, "bits"), r));
    tokens.push_back(r);
  }
  tokens.push_back(field(ei, "bits"));
  Value packed = trace;
  for (auto t : tokens) packed = cat(cat(bits(t, 64, 64), cat(constant(1, 1), bits(t, 65, 65))), packed);
  for (auto t : tokens) packed = cat(bits(t, 63, 0), packed);
  connect(field(eout, "bits"), packed);

  SmallVector<PortInfo> ps; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) if (i != *incoming && i != *outgoing) { copied.push_back(i); ps.push_back(p); }
  unsigned streamIn = ps.size(); ps.push_back({b.getStringAttr("nic_from_host_stream"), token(512), Direction::In});
  unsigned streamOut = ps.size(); ps.push_back({b.getStringAttr("nic_to_host_stream"), token(512), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ps);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), dec = b.create<InstanceOp>(loc, decoder, "from_host"), enc = b.create<InstanceOp>(loc, encoder, "to_host");
  auto arg = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  for (auto [j,i] : llvm::enumerate(copied)) { auto p = inner.getPorts()[i];
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : arg(j), p.direction == Direction::In ? arg(j) : sim.getResult(i)); }
  for (auto adapter : {dec, enc}) {
    connect(adapter.getResult(0), arg(llvm::find(copied, *clock) - copied.begin()));
    connect(adapter.getResult(1), arg(llvm::find(copied, *reset) - copied.begin()));
  }
  b.create<ConnectOp>(loc, dec.getResult(2), arg(streamIn)); b.create<ConnectOp>(loc, sim.getResult(*incoming), dec.getResult(3));
  b.create<ConnectOp>(loc, enc.getResult(2), sim.getResult(*outgoing)); b.create<ConnectOp>(loc, arg(streamOut), enc.getResult(3));
  std::string op = "~" + circuit.getName().str(), np = "~" + wrapperName.str(), mp = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) { auto v = s.getValue(); if (v == op) return b.getStringAttr(np); if (!v.consume_front(op + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(mp)) { auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) { suffix.replace(0, mp.size(), "|" + wrapperName.str() + ">"); break; } }
      return b.getStringAttr(np + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto v : arr) out.push_back(retarget(v)); return b.getArrayAttr(out); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto v : d) out.set(v.getName(), retarget(v.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName); return success();
}
