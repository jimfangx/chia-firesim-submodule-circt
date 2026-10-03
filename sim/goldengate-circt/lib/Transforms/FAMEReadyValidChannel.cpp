// See LICENSE for license details.
#include "goldengate/FAMEReadyValidChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;
namespace {
std::string moduleName(unsigned width) {
  return "GGFAMEReadyValid" + std::to_string(width);
}

// ShiftQueue(2, flow) from rocket-chip/util/ShiftQueue.scala. Queue readiness
// is independent of dequeue readiness: a full queue cannot enqueue on a pop.
struct Queue { Value ready, valid, bits; };
Queue queue(OpBuilder &b, Location loc, Value clock, Value reset,
            Value enqValid, Value enqBits, Value deqReady, bool flow,
            llvm::StringRef name) {
  auto bit = UIntType::get(b.getContext(), 1, false);
  Value zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value v0 = b.create<RegResetOp>(loc, bit, clock, reset, zero, (name + "_valid_0").str()).getResult();
  Value v1 = b.create<RegResetOp>(loc, bit, clock, reset, zero, (name + "_valid_1").str()).getResult();
  Value d0 = b.create<RegOp>(loc, enqBits.getType(), clock, (name + "_bits_0").str()).getResult();
  Value d1 = b.create<RegOp>(loc, enqBits.getType(), clock, (name + "_bits_1").str()).getResult();
  Value ready = b.create<NotPrimOp>(loc, v1);
  Value fire = b.create<AndPrimOp>(loc, ready, enqValid);
  Value advance0 = flow ? b.create<AndPrimOp>(loc, fire, v0).getResult() : fire;
  Value shift0 = b.create<OrPrimOp>(loc, v1, advance0);
  Value append0 = b.create<AndPrimOp>(loc, fire, b.create<NotPrimOp>(loc, v0));
  Value write0 = b.create<MuxPrimOp>(loc, deqReady, shift0, append0);
  Value next0 = b.create<MuxPrimOp>(loc, deqReady, shift0,
                                   b.create<OrPrimOp>(loc, fire, v0));
  b.create<StrictConnectOp>(loc, v0, next0);
  Value data0 = b.create<MuxPrimOp>(loc, v1, d1, enqBits);
  b.create<StrictConnectOp>(loc, d0, b.create<MuxPrimOp>(loc, write0, data0, d0));
  Value push1 = b.create<AndPrimOp>(loc, fire, v0);
  Value shift1 = b.create<AndPrimOp>(loc, fire, v1);
  Value append1 = b.create<AndPrimOp>(loc, push1, b.create<NotPrimOp>(loc, v1));
  Value write1 = b.create<MuxPrimOp>(loc, deqReady, shift1, append1);
  Value next1 = b.create<MuxPrimOp>(loc, deqReady, shift1,
                                   b.create<OrPrimOp>(loc, push1, v1));
  b.create<StrictConnectOp>(loc, v1, next1);
  b.create<StrictConnectOp>(loc, d1, b.create<MuxPrimOp>(loc, write1, enqBits, d1));
  return {ready, flow ? b.create<OrPrimOp>(loc, v0, enqValid).getResult() : v0,
          flow ? b.create<MuxPrimOp>(loc, v0, d0, enqBits).getResult() : d0};
}

struct Pair {
  std::string name;
  unsigned forward, reverse, width;
  BundleType payload;
  bool targetSource;
};

LogicalResult findPairs(CircuitOp circuit, FModuleOp &top,
                        SmallVectorImpl<Pair> &pairs, std::string &error) {
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!top || !raw) {
    error = "ReadyValidChannel needs a top module and retained annotations";
    return failure();
  }
  std::map<std::string, DictionaryAttr> reverse;
  for (auto attr : raw) {
    Annotation a(attr);
    if (!a.isClass(goldengate::AnnotationClasses::ChannelConnection)) continue;
    auto info = a.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") !=
                     goldengate::AnnotationClasses::DecoupledReverseChannel) continue;
    auto name = a.getMember<StringAttr>("globalName");
    if (!name || !reverse.emplace(name.getValue().str(), cast<DictionaryAttr>(attr)).second) {
      error = "ReadyValidChannel has a missing or duplicate reverse name";
      return failure();
    }
  }
  auto bit = UIntType::get(circuit.getContext(), 1, false);
  std::set<unsigned> used;
  std::set<std::string> names;
  for (auto attr : raw) {
    Annotation a(attr);
    if (!a.isClass(goldengate::AnnotationClasses::ChannelConnection)) continue;
    auto info = a.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") !=
                     goldengate::AnnotationClasses::DecoupledForwardChannel) continue;
    auto name = a.getMember<StringAttr>("globalName");
    if (!name || !name.getValue().ends_with("_fwd")) {
      error = "ReadyValidChannel forward name must end in _fwd";
      return failure();
    }
    std::string base = name.getValue().drop_back(4).str();
    auto rev = reverse.find(base + "_rev");
    auto validSource = info.getAs<StringAttr>("validSource");
    auto validSink = info.getAs<StringAttr>("validSink");
    bool source = bool(validSource);
    auto ready = info.getAs<StringAttr>(source ? "readySink" : "readySource");
    auto valid = source ? validSource : validSink;
    if (rev == reverse.end() || bool(validSource) == bool(validSink) || !ready) {
      error = "ReadyValidChannel " + base + " lacks a boundary handshake pair";
      return failure();
    }
    auto f = goldengate::resolveAnnotationTarget(circuit, valid.getValue(), error);
    auto r = goldengate::resolveAnnotationTarget(circuit, ready.getValue(), error);
    if (!f || !r || f->module != top || r->module != top || !f->port || !r->port) {
      error = "ReadyValidChannel " + base + " handshake must resolve to top ports: " + error;
      return failure();
    }
    auto ft = dyn_cast<BundleType>(top.getPortType(*f->port));
    auto rt = dyn_cast<BundleType>(top.getPortType(*r->port));
    auto decoupled = [&](BundleType type) {
      return type && type.getElements().size() == 3 &&
        type.getElementIndex("ready") && type.getElementIndex("valid") &&
        type.getElementIndex("bits") &&
        type.getElement("ready")->type == bit && type.getElement("ready")->isFlip &&
        type.getElement("valid")->type == bit && !type.getElement("valid")->isFlip &&
        !type.getElement("bits")->isFlip;
    };
    auto payload = ft && ft.getElementIndex("bits")
                       ? dyn_cast<BundleType>(ft.getElement("bits")->type) : BundleType();
    if (!decoupled(ft) || !decoupled(rt) || rt.getElement("bits")->type != bit ||
        !payload || !payload.getElementIndex("valid") ||
        payload.getElement("valid")->type != bit || payload.getElement("valid")->isFlip ||
        f->fieldID != ft.getFieldID(*ft.getElementIndex("bits")) +
                      payload.getFieldID(*payload.getElementIndex("valid")) ||
        r->fieldID != rt.getFieldID(*rt.getElementIndex("bits")) ||
        top.getPortDirection(*f->port) != (source ? Direction::Out : Direction::In) ||
        top.getPortDirection(*r->port) != (source ? Direction::In : Direction::Out)) {
      error = "ReadyValidChannel " + base + " has incompatible Decoupled ports";
      return failure();
    }
    // The SFC handoff is lowered to a flat bundle of passive UInt fields.
    // Reject unhandled aggregates instead of silently losing payload bits.
    unsigned width = 0;
    std::set<unsigned> fields;
    for (auto [i, field] : llvm::enumerate(payload.getElements())) {
      auto uint = dyn_cast<UIntType>(field.type);
      if (!uint || uint.getWidthOrSentinel() < 0 || field.isFlip) {
        error = "ReadyValidChannel " + base + " needs known-width passive UInt fields";
        return failure();
      }
      if (field.name.getValue() != "valid") width += uint.getWidthOrSentinel();
      fields.insert(ft.getFieldID(*ft.getElementIndex("bits")) + payload.getFieldID(i));
    }
    auto endpoints = a.getMember<ArrayAttr>(source ? "sources" : "sinks");
    auto opposite = a.getMember<ArrayAttr>(source ? "sinks" : "sources");
    Annotation ra(rev->second);
    auto revEndpoints = ra.getMember<ArrayAttr>(source ? "sinks" : "sources");
    auto revOpposite = ra.getMember<ArrayAttr>(source ? "sources" : "sinks");
    bool validEndpoints = a.getMember<Attribute>("clock") == ra.getMember<Attribute>("clock") && width > 0 && endpoints && endpoints.size() == fields.size() &&
                          (!opposite || opposite.empty()) && revEndpoints &&
                          revEndpoints.size() == 1 && (!revOpposite || revOpposite.empty());
    if (validEndpoints) {
      for (auto endpoint : endpoints) {
        auto spelling = dyn_cast<StringAttr>(endpoint);
        auto t = spelling ? goldengate::resolveAnnotationTarget(circuit, spelling.getValue(), error)
                          : std::nullopt;
        if (!t || t->module != top || t->port != f->port || (!t->fieldID || !fields.erase(*t->fieldID))) {
          validEndpoints = false; break;
        }
      }
      auto spelling = dyn_cast<StringAttr>(revEndpoints[0]);
      auto t = spelling ? goldengate::resolveAnnotationTarget(circuit, spelling.getValue(), error)
                        : std::nullopt;
      validEndpoints &= t && t->module == top && t->port == r->port && t->fieldID == r->fieldID;
    }
    if (!validEndpoints || !names.insert(base).second ||
        !used.insert(*f->port).second || !used.insert(*r->port).second) {
      error = "ReadyValidChannel " + base + " has missing, shared or inconsistent endpoints";
      return failure();
    }
    pairs.push_back({base, *f->port, *r->port, width, payload, source});
    reverse.erase(rev);
  }
  if (!reverse.empty()) {
    error = "ReadyValidChannel has an unpaired reverse channel";
    return failure();
  }
  return success();
}
} // namespace

LogicalResult goldengate::addFAMEReadyValidChannel(CircuitOp circuit,
                                                  unsigned width,
                                                  std::string &error) {
  if (!width) { error = "ReadyValidChannel payload must have positive width"; return failure(); }
  for (auto m : circuit.getOps<FModuleLike>())
    if (m.getModuleName() == moduleName(width)) {
      error = "ReadyValidChannel module already exists"; return failure();
    }
  auto *ctx = circuit.getContext();
  auto bit = UIntType::get(ctx, 1, false), data = UIntType::get(ctx, width, false);
  SmallVector<PortInfo> ports;
  auto port = [&](const char *name, Type type, Direction direction) {
    ports.push_back({StringAttr::get(ctx, name), type, direction});
  };
  port("clock", ClockType::get(ctx), Direction::In);
  port("reset", bit, Direction::In);
  port("io_enq_target_ready", bit, Direction::Out);
  port("io_enq_target_valid", bit, Direction::In);
  port("io_enq_target_bits", data, Direction::In);
  port("io_enq_fwd_hReady", bit, Direction::Out);
  port("io_enq_fwd_hValid", bit, Direction::In);
  port("io_enq_rev_hReady", bit, Direction::In);
  port("io_enq_rev_hValid", bit, Direction::Out);
  port("io_deq_target_ready", bit, Direction::In);
  port("io_deq_target_valid", bit, Direction::Out);
  port("io_deq_target_bits", data, Direction::Out);
  port("io_deq_fwd_hReady", bit, Direction::In);
  port("io_deq_fwd_hValid", bit, Direction::Out);
  port("io_deq_rev_hReady", bit, Direction::Out);
  port("io_deq_rev_hValid", bit, Direction::In);
  port("io_targetReset_bits", bit, Direction::In);
  port("io_targetReset_valid", bit, Direction::In);
  port("io_targetReset_ready", bit, Direction::Out);
  OpBuilder b(circuit.getBodyBlock(), circuit.getBodyBlock()->begin());
  auto module = b.create<FModuleOp>(circuit.getLoc(), StringAttr::get(ctx, moduleName(width)),
      ConventionAttr::get(ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned i) { return module.getBodyBlock()->getArgument(i); };
  Location loc = module.getLoc();
  auto wire = [&](Type type, llvm::StringRef name) -> Value { return b.create<WireOp>(loc, type, name).getResult(); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto land = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto lor = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  Value zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value fwdReady = wire(bit, "enqFwdQ_deq_ready"), revReady = wire(bit, "deqRevQ_deq_ready");
  Value packed = b.create<CatPrimOp>(loc, arg(3), arg(4));
  Queue fwd = queue(b, loc, arg(0), arg(1), arg(6), packed, fwdReady, true, "enqFwdQ");
  Queue rev = queue(b, loc, arg(0), arg(1), arg(15), arg(9), revReady, true, "deqRevQ");
  Value deqFired = b.create<RegResetOp>(loc, bit, arg(0), arg(1), zero, "deqFwdFired").getResult();
  Value enqFired = b.create<RegResetOp>(loc, bit, arg(0), arg(1), zero, "enqRevFired").getResult();
  Value enqDone = lor(enqFired, arg(7)), deqDone = lor(deqFired, arg(12));
  Value hostDone = land(enqDone, deqDone);
  Value resetReady = land(land(fwd.valid, rev.valid), hostDone);
  Value fire = land(arg(17), resetReady);
  connect(arg(18), resetReady);
  connect(fwdReady, land(land(arg(17), rev.valid), hostDone));
  connect(revReady, land(land(arg(17), fwd.valid), hostDone));
  connect(arg(5), fwd.ready);
  connect(arg(14), rev.ready);
  connect(arg(8), b.create<NotPrimOp>(loc, enqFired));
  connect(arg(13), b.create<NotPrimOp>(loc, deqFired));
  connect(enqFired, b.create<MuxPrimOp>(loc, fire, zero, enqDone));
  connect(deqFired, b.create<MuxPrimOp>(loc, fire, zero, deqDone));
  Value fwdValid = b.create<BitsPrimOp>(loc, fwd.bits, width, width);
  Value fwdBits = b.create<BitsPrimOp>(loc, fwd.bits, width - 1, 0);
  Value last = b.create<RegOp>(loc, data, arg(0), "enqBitsLast").getResult();
  connect(last, b.create<MuxPrimOp>(loc, fire, fwdBits, last));
  Value refReset = lor(arg(1), land(fire, arg(16)));
  Value refBits = b.create<MuxPrimOp>(loc, fire, fwdBits, last);
  Queue reference = queue(b, loc, arg(0), refReset, land(fire, fwdValid),
                          refBits, land(fire, rev.bits), false, "reference");
  connect(arg(2), reference.ready);
  connect(arg(10), reference.valid);
  connect(arg(11), reference.bits);
  return success();
}

LogicalResult goldengate::addFAMEBoundaryReadyValidChannels(CircuitOp circuit,
                                                          std::string &error) {
  FModuleOp top, wrapper;
  SmallVector<Pair> pairs;
  if (failed(findPairs(circuit, top, pairs, error))) return failure();
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == "GGFAMEPipeWrapper") wrapper = m;
  InstanceOp child;
  if (wrapper)
    for (auto i : wrapper.getOps<InstanceOp>())
      if (i.getModuleName() == top.getName()) child = i;
  if (!wrapper || !child) {
    error = "ReadyValidChannel needs the pipe wrapper and its target instance"; return failure();
  }
  std::optional<unsigned> clock, reset;
  for (unsigned i = 0; i < wrapper.getPorts().size(); ++i) {
    if (wrapper.getPortName(i) == "hostClock") clock = i;
    if (wrapper.getPortName(i) == "hostReset") reset = i;
  }
  if (!clock || !reset) { error = "ReadyValidChannel wrapper lacks host controls"; return failure(); }
  std::set<unsigned> widths;
  SmallVector<ConnectOp> passthroughs;
  for (const auto &p : pairs) {
    widths.insert(p.width);
    for (unsigned port : {p.forward, p.reverse}) {
      Value internal = child.getResult(port), external = wrapper.getBodyBlock()->getArgument(port);
      ConnectOp found;
      for (auto c : wrapper.getOps<ConnectOp>())
        if ((c.getDest() == internal && c.getSrc() == external) ||
            (c.getDest() == external && c.getSrc() == internal)) {
          if (found) { error = "duplicate channel passthrough"; return failure(); }
          found = c;
        }
      if (!found) { error = "ReadyValidChannel port is already connected"; return failure(); }
      passthroughs.push_back(found);
    }
  }
  for (auto m : circuit.getOps<FModuleLike>())
    if (widths.count(0) || llvm::any_of(widths, [&](unsigned width) {
          return m.getModuleName() == moduleName(width);
        })) { error = "ReadyValidChannel module symbol collision"; return failure(); }
  for (unsigned width : widths)
    if (failed(addFAMEReadyValidChannel(circuit, width, error))) return failure();
  for (auto c : passthroughs) c.erase();
  OpBuilder b(wrapper.getBodyBlock(), wrapper.getBodyBlock()->end());
  Location loc = wrapper.getLoc();
  auto field = [&](Value v, llvm::StringRef name) -> Value { return b.create<SubfieldOp>(loc, v, name); };
  auto connect = [&](Value dest, Value src) { b.create<ConnectOp>(loc, dest, src); };
  auto bit = UIntType::get(circuit.getContext(), 1, false);
  Value zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  for (const auto &p : pairs) {
    FModuleOp definition;
    for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == moduleName(p.width)) definition = m;
    auto instance = b.create<InstanceOp>(loc, definition, "ReadyValidChannel_" + p.name);
    auto arg = [&](unsigned i) { return instance.getResult(i); };
    Value fInternal = child.getResult(p.forward), rInternal = child.getResult(p.reverse);
    Value fExternal = wrapper.getBodyBlock()->getArgument(p.forward);
    Value rExternal = wrapper.getBodyBlock()->getArgument(p.reverse);
    Value enqF = p.targetSource ? fInternal : fExternal, enqR = p.targetSource ? rInternal : rExternal;
    Value deqF = p.targetSource ? fExternal : fInternal, deqR = p.targetSource ? rExternal : rInternal;
    connect(arg(0), wrapper.getBodyBlock()->getArgument(*clock));
    connect(arg(1), wrapper.getBodyBlock()->getArgument(*reset));
    connect(field(enqR, "bits"), arg(2));
    connect(arg(3), field(field(enqF, "bits"), "valid"));
    connect(field(enqF, "ready"), arg(5));
    connect(arg(6), field(enqF, "valid"));
    connect(arg(7), field(enqR, "ready"));
    connect(field(enqR, "valid"), arg(8));
    connect(arg(9), field(deqR, "bits"));
    connect(field(field(deqF, "bits"), "valid"), arg(10));
    connect(arg(12), field(deqF, "ready"));
    connect(field(deqF, "valid"), arg(13));
    connect(field(deqR, "ready"), arg(14));
    connect(arg(15), field(deqR, "valid"));
    // SimWrapper.genReadyValidChannel uses an always-valid false reset token.
    connect(arg(16), zero); connect(arg(17), one);
    Value packed;
    unsigned offset = p.width;
    for (auto f : p.payload.getElements()) {
      if (f.name.getValue() == "valid") continue;
      unsigned width = cast<UIntType>(f.type).getWidthOrSentinel();
      Value input = field(field(enqF, "bits"), f.name.getValue());
      Value output = field(field(deqF, "bits"), f.name.getValue());
      if (!width) {
        // Zero-width fields carry no token information; FIRRTL retains their
        // target identity until normal type lowering removes them.
        connect(output, input); continue;
      }
      packed = packed ? b.create<CatPrimOp>(loc, packed, input).getResult() : input;
      connect(output, b.create<BitsPrimOp>(loc, arg(11), offset - 1, offset - width));
      offset -= width;
    }
    connect(arg(4), packed);
  }
  return success();
}
