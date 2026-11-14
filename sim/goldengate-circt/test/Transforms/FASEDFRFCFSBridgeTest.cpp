// See LICENSE for license details.
#include "goldengate/FASEDFRFCFSBridge.h"
#include "FASEDFRFCFSBridgeFixture.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, StringRef why) {
  if (!ok)
    throw std::runtime_error(why.str());
}
FModuleOp named(CircuitOp c, StringRef n) {
  for (auto m : c.getOps<FModuleOp>())
    if (m.getName() == n)
      return m;
  throw std::runtime_error("missing module");
}
std::string print(Operation *op) {
  std::string s;
  llvm::raw_string_ostream out(s);
  op->print(out);
  return s;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    auto link = [&](Value d, Value z) {
      if (!isa<BundleType>(d.getType())) {
        require(drivers.emplace(key(d), z).second, "multiple drivers");
        return;
      }
      std::function<void(std::string, std::string, Type, bool)> expand =
          [&](std::string dest, std::string src, Type t, bool flip) {
            if (auto bundle = dyn_cast<BundleType>(t)) {
              for (auto e : bundle.getElements())
                expand(dest + "." + e.name.str(),
                       src + "." + e.name.str(),
                       e.type,
                       flip != e.isFlip);
            } else
              require(
                  links.emplace(flip ? src : dest, flip ? dest : src).second,
                  "multiple aggregate drivers");
          };
      expand(key(d), key(z), d.getType(), false);
    };
    for (auto c : m.getOps<StrictConnectOp>())
      link(c.getDest(), c.getSrc());
    for (auto c : m.getOps<ConnectOp>())
      link(c.getDest(), c.getSrc());
    for (auto r : m.getOps<RegResetOp>())
      state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto r : m.getOps<RegOp>())
      state[r.getResult()] = APInt(width(r.getResult()), 0);
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>())
      return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>())
      return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) {
    memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n);
  }
  APInt read(std::string k, unsigned w) {
    if (memo.count(k))
      return memo.at(k).zextOrTrunc(w);
    if (drivers.count(k))
      return eval(drivers.at(k)).zextOrTrunc(w);
    if (links.count(k))
      return read(links.at(k), w);
    auto prefix = k;
    while (prefix.find('.') != std::string::npos) {
      prefix.resize(prefix.rfind('.'));
      if (drivers.count(prefix))
        return read(key(drivers.at(prefix)) + k.substr(prefix.size()), w);
    }
    throw std::runtime_error("missing aggregate driver: " + k);
  }
  APInt output(Value v, llvm::StringRef path, unsigned w = 64) {
    return read(key(v) + (path.empty() ? "" : "." + path.str()), w);
  }
  APInt eval(Value v) {
    auto k = key(v);
    unsigned w = width(v);
    if (memo.count(k))
      return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp();
    APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp, RegOp>(op))
      n = state.lookup(v);
    else if (drivers.count(k))
      n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op))
      n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op))
      n = eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op))
      n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<AndPrimOp>(op))
      n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op))
      n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op))
      n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op))
      n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op))
      n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<LTPrimOp>(op))
      n = APInt(1, eval(op->getOperand(0)).ult(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GEQPrimOp>(op))
      n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op))
      n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op))
      n = eval(op->getOperand(0)).zextOrTrunc(w) +
          eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op))
      n = eval(op->getOperand(0)).zextOrTrunc(w) -
          eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op))
      n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else if (isa_and_nonnull<SubfieldOp>(op))
      n = read(k, w);
    else
      throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w);
    memo[k] = n;
    return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero()
                                ? eval(drivers.at(key(r.getResult())))
                                : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>())
      next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};

Value pin(InstanceOp i, StringRef n) {
  for (unsigned p = 0; p < i.getNumResults(); ++p)
    if (i.getPortName(p) == n)
      return i.getResult(p);
  throw std::runtime_error("missing pin " + n.str());
}
void test(MLIRContext &ctx, unsigned flight, bool selected) {
  auto root = goldengate::test::makeFASEDFRFCFSWriteEgressFixture(
      ctx, flight, selected);
  auto c = *root->getOps<CircuitOp>().begin();
  std::string oldName = c.getName().str();
  auto originalModule = named(c, oldName);
  auto original = print(originalModule);
  auto engine = named(c, "GGFASEDTokenEngine");
  auto constructor = engine->getAttr("goldengate.bridgeConstructor");
  std::string error;
  require(succeeded(goldengate::addFASEDFRFCFSBridge(c, error)), error);
  require(succeeded(verify(*root)), "invalid attached model IR");
  require(named(c, oldName) == originalModule &&
              print(originalModule) == original,
          "original wrapper or handle changed");
  require(named(c, "GGFASEDTokenEngine") == engine,
          "preexisting engine handle changed");
  require(
      named(c, "GGFASEDTokenEngine")->getAttr("goldengate.bridgeConstructor") ==
          constructor,
      "constructor changed");
  auto wrapper = named(c, c.getName());
  Interpreter w(wrapper);
  InstanceOp sim, model;
  for (auto i : wrapper.getOps<InstanceOp>())
    if (i.getName() == "sim")
      sim = i;
    else if (i.getName() == "timingModel")
      model = i;
  require(sim && model, "missing attached sim or model");
  for (auto p : wrapper.getPorts())
    require(p.name != "fased_timing" && p.name != "fased_read_egress_req" &&
                p.name != "fased_read_egress_resp" &&
                p.name != "fased_write_egress_req" &&
                p.name != "fased_write_egress_resp",
            "egress/model boundary still external");
  auto helper = named(c, "GGFASEDFRFCFSTimingModel" + std::to_string(flight));
  require(w.drivers.at(w.key(pin(model, "clock"))) == w.arg(0),
          "host clock binding differs");
  unsigned exportCount = 0;
  for (auto [j, q] : llvm::enumerate(wrapper.getPorts())) {
    if (!q.getName().starts_with("fased_frfcfs_"))
      continue;
    ++exportCount;
    StringRef modelName =
        q.getName().drop_front(StringRef("fased_frfcfs_").size());
    bool found = false;
    for (auto [i, p] : llvm::enumerate(helper.getPorts()))
      if (p.getName() == modelName) {
        found = true;
        require(p.type == q.type && p.direction == q.direction,
                "export type/direction changed");
        // Aggregate connects expand to leaf links in this interpreter. Check
        // every leaf against its exact peer, rather than expecting a driver
        // on the bundle root (notably the DRAM timing programming bundle).
        std::function<void(std::string, std::string, Type, bool)> binding =
            [&](std::string dest, std::string src, Type type, bool flip) {
              if (auto bundle = dyn_cast<BundleType>(type)) {
                for (auto e : bundle.getElements())
                  binding(dest + "." + e.name.str(),
                          src + "." + e.name.str(),
                          e.type,
                          flip != e.isFlip);
                return;
              }
              if (flip)
                std::swap(dest, src);
              if (w.links.count(dest))
                require(w.links.at(dest) == src,
                        "export aggregate leaf routed incorrectly: " +
                            q.getName().str());
              else
                require(w.drivers.count(dest) &&
                            w.key(w.drivers.at(dest)) == src,
                        "export port unbound or routed incorrectly: " +
                            q.getName().str());
            };
        auto modelKey = w.key(model.getResult(i)), wrapperKey = w.key(w.arg(j));
        binding(q.direction == Direction::In ? modelKey : wrapperKey,
                q.direction == Direction::In ? wrapperKey : modelKey,
                p.type,
                false);
      }
    require(found, "export lacks native model port");
  }
  require(exportCount + 44 == helper.getNumPorts(),
          "missing or extra configuration/statistic exports");
  // The wrapper connects ready/valid combinationally, including target stalls;
  // the selected helper owns targetFire-qualified state and reset.
  std::mt19937_64 rng(0x465246434653 + flight);
  unsigned stalls = 0, resetStalls = 0;
  Interpreter release(named(c, "GGFASEDAXI4Releaser"));
  auto ra = [&](StringRef n) -> Value {
    for (unsigned i = 0; i < release.module.getNumPorts(); ++i)
      if (release.module.getPortName(i) == n)
        return release.arg(i);
    throw std::runtime_error("release port");
  };
  bool rf = false, bf = false;
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    w.memo.clear();
    bool reset = rng() & 1, fire = rng() & 1;
    w.put(pin(sim, "fased_model_reset"), "", reset);
    w.put(pin(sim, "fased_tfire"), "", fire);
    require(w.output(pin(model, "reset"), "", 1).getBoolValue() == reset &&
                w.output(pin(model, "targetFire"), "", 1).getBoolValue() ==
                    fire,
            "reset/fire must remain independent before model");
    stalls += !fire;
    resetStalls += reset && !fire;
    for (auto ch : {"aw", "ar", "w"}) {
      std::string stem(ch);
      bool valid = rng() & 1, ready = rng() & 1;
      w.put(pin(sim, "fased_timing"), stem + ".valid", valid);
      w.put(pin(model, stem + "Ready"), "", ready);
      require(w.output(pin(model, stem + "Valid"), "", 1).getBoolValue() ==
                  valid,
              "request valid gated or miswired");
      require(w.output(pin(sim, "fased_timing"), stem + ".ready", 1)
                      .getBoolValue() == ready,
              "request ready miswired");
      for (auto f :
           stem == "w"
               ? std::vector<std::pair<std::string, std::string>>{{"Last",
                                                                   "last"}}
               : std::vector<std::pair<std::string, std::string>>{
                     {"Id", "id"},
                     {"Len", "len"},
                     {"Addr", "addr"},
                     {"Burst", "burst"}}) {
        uint64_t value = rng();
        unsigned width =
            *cast<UIntType>(pin(model, stem + f.first).getType()).getWidth();
        w.put(pin(sim, "fased_timing"), stem + ".bits." + f.second, value);
        require(
            w.output(pin(model, stem + f.first), "", width).getZExtValue() ==
                APInt(64, value).trunc(width).getZExtValue(),
            "request payload changed");
      }
    }
    for (bool read : {false, true}) {
      std::string stem = read ? "r" : "b",
                  egress = read ? "egressR" : "egressB",
                  resp = read ? "fased_read_egress_resp"
                              : "fased_write_egress_resp",
                  req =
                      read ? "fased_read_egress_req" : "fased_write_egress_req";
      bool ready = rng() & 1, valid = rng() & 1, request = rng() & 1,
           egReady = rng() & 1;
      unsigned id = rng() & 15;
      w.put(pin(sim, "fased_timing"), stem + ".ready", ready);
      w.put(pin(model, stem + "Valid"), "", valid);
      w.put(pin(model, read ? "egressReadValid" : "egressWriteValid"),
            "",
            request);
      w.put(pin(model, read ? "egressReadId" : "egressWriteId"), "", id);
      w.put(pin(model, egress + "Ready"), "", egReady);
      require(w.output(pin(model, stem + "Ready"), "", 1).getBoolValue() ==
                      ready &&
                  w.output(pin(sim, "fased_timing"), stem + ".valid", 1)
                          .getBoolValue() == valid,
              "target response handshake changed");
      require(w.output(pin(sim, req), "valid", 1).getBoolValue() == request &&
                  w.output(pin(sim, req), "bits", 4).getZExtValue() == id &&
                  w.output(pin(sim, resp), "tReady", 1).getBoolValue() ==
                      egReady,
              "functional egress request/ready changed");
      std::vector<std::pair<std::string, std::string>> fields{
          {"Id", "id"}, {"Resp", "resp"}, {"User", "user"}};
      if (read) {
        fields.push_back({"Last", "last"});
        fields.push_back({"Data", "data"});
      }
      for (auto f : fields) {
        uint64_t value = rng();
        unsigned width =
            *cast<UIntType>(pin(model, stem + f.first).getType()).getWidth();
        w.put(pin(model, stem + f.first), "", value);
        w.put(pin(sim, resp), "tBits." + f.second, value);
        auto expect = APInt(64, value).zextOrTrunc(width).getZExtValue();
        require(w.output(pin(sim, "fased_timing"),
                         stem + ".bits." + f.second,
                         width)
                            .getZExtValue() == expect &&
                    w.output(pin(model, egress + f.first), "", width)
                            .getZExtValue() == expect,
                "functional egress payload changed");
      }
    }
    release.memo.clear();
    auto put = [&](StringRef n, uint64_t v) { release.put(ra(n), "", v); };
    bool nr = rng() & 1, nw = rng() & 1, rr = rng() & 1, br = rng() & 1,
         last = rng() & 1;
    put("reset", w.output(pin(model, "reset"), "", 1).getZExtValue());
    put("targetFire", w.output(pin(model, "targetFire"), "", 1).getZExtValue());
    put("nextReadValid", nr);
    put("nextWriteValid", nw);
    put("rReady", rr);
    put("bReady", br);
    put("egressRLast", last);
    bool rp = rf && rr && last, bp = bf && br, rpush = nr && (!rf || rp),
         bpush = nw && (!bf || bp);
    require(release.output(ra("rValid"), "", 1).getBoolValue() == rf &&
                release.output(ra("bValid"), "", 1).getBoolValue() == bf,
            "response occupancy changed");
    release.edge();
    rf = fire ? (reset ? false : rpush ? true : rp ? false : rf) : rf;
    bf = fire ? (reset ? false : bpush ? true : bp ? false : bf) : bf;
    for (auto r : release.module.getOps<RegResetOp>())
      require(release.state[r.getResult()].getBoolValue() ==
                  (r.getName() == "currentRead_full" ? rf : bf),
              "bridge-driven target reset/hold/retire changed");
  }
  require(stalls && resetStalls, "missing stalled reset cases");
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == 7, "lost annotations");
  for (auto [i, a] : llvm::enumerate(raw)) {
    auto target =
        cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();
    require(target.starts_with("~" + c.getName().str() + "|"),
            "circuit annotation not retargeted");
    require(target.contains(i == 0 ? "|" + c.getName().str() + ">other"
                                   : "|" + oldName + ">"),
            "copied or consumed annotation moved incorrectly");
  }
  auto before = print(c);
  require(failed(goldengate::addFASEDFRFCFSBridge(c, error)) &&
              before == print(c),
          "repeat rejection changed circuit");
  llvm::outs() << flight << "/" << selected
               << ": 4096 wrapper propagation and target-stall/reset/retire "
                  "cycles passed\n";
}
void rejects(MLIRContext &ctx) {
  for (unsigned mode = 1; mode <= 21; ++mode)
    for (bool selected : {false, true}) {
      auto root = goldengate::test::makeFASEDFRFCFSWriteEgressFixture(
          ctx, 10, selected, mode);
      auto c = *root->getOps<CircuitOp>().begin();
      auto before = print(c);
      std::string error;
      require(failed(goldengate::addFASEDFRFCFSBridge(c, error)) &&
                  !error.empty(),
              "bad boundary accepted");
      require(before == print(c), "bad boundary mutated circuit");
    }
  for (unsigned mode = 0; mode < 8; ++mode) {
    auto root =
        goldengate::test::makeFASEDFRFCFSWriteEgressFixture(ctx, 10, false);
    auto c = *root->getOps<CircuitOp>().begin();
    OpBuilder b(&ctx);
    auto e = named(c, "GGFASEDTokenEngine");
    NamedAttrList key(
        cast<DictionaryAttr>(e->getAttr("goldengate.bridgeConstructor")));
    if (mode < 3) {
      if (mode == 0)
        c->removeAttr("goldengate.fasedBaseCapacity");
      else
        c->setAttr("goldengate.fasedBaseCapacity",
                   mode == 1 ? Attribute(b.getI64IntegerAttr(16))
                             : Attribute(b.getStringAttr("32")));
    } else {
      auto group = mode == 7 ? "axi4Widths" : "axi4Edge";
      NamedAttrList attrs(cast<DictionaryAttr>(key.get(group)));
      attrs.set(mode == 3   ? "maxFlight"
                : mode == 4 ? "idReuse"
                : mode == 5 ? "maxReadTransfer"
                : mode == 6 ? "maxWriteTransfer"
                            : "addrBits",
                b.getI64IntegerAttr(mode == 7 ? 34 : 0));
      key.set(group, attrs.getDictionary(&ctx));
      e->setAttr("goldengate.bridgeConstructor", key.getDictionary(&ctx));
    }
    auto before = print(c);
    std::string error;
    require(failed(goldengate::addFASEDFRFCFSBridge(c, error)) &&
                !error.empty() && before == print(c),
            "profile rejection not atomic");
  }
  llvm::outs() << "50 invalid boundary/profile atomic rejection cases passed\n";
}
} // namespace
int main() {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned flight : {10, 20})
      for (bool selected : {false, true})
        test(ctx, flight, selected);
    rejects(ctx);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << "\n";
    return 1;
  }
}
