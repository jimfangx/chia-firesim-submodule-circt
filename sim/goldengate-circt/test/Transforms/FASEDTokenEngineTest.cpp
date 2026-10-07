// See LICENSE for license details.
#include "goldengate/FASEDTokenEngine.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <functional>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, bool normalized = false) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGBlockDevResponseSchedulerWrapper\" { firrtl.module @GGBlockDevResponseSchedulerWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  auto bit = uint(1);
  const llvm::StringRef locals[]{"axi4_aw_fwd", "axi4_w_fwd", "axi4_b_rev", "axi4_ar_fwd", "axi4_r_rev", "reset",
      "axi4_aw_rev", "axi4_w_rev", "axi4_b_fwd", "axi4_ar_rev", "axi4_r_fwd"};
  const std::vector<std::string> fields[]{
      {"bits_user","bits_id","bits_region","bits_qos","bits_prot","bits_cache","bits_lock","bits_burst","bits_size","bits_len","bits_addr","valid"},
      {"bits_user","bits_strb","bits_id","bits_last","bits_data","valid"}, {""},
      {"bits_user","bits_id","bits_region","bits_qos","bits_prot","bits_cache","bits_lock","bits_burst","bits_size","bits_len","bits_addr","valid"},
      {""}, {""}, {""}, {""}, {"bits_user","bits_id","bits_resp","valid"}, {""},
      {"bits_user","bits_id","bits_last","bits_data","bits_resp","valid"}};
  const unsigned pairs[]{6,7,8,9,10,5,0,1,2,3,4};
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 11; ++j) {
    FIRRTLBaseType payload = FIRRTLBaseType(bit);
    if (j == 0 || j == 1 || j == 3 || j == 8 || j == 10) {
      SmallVector<BundleType::BundleElement> elements;
      for (auto &name : fields[j]) {
        unsigned w = name == "bits_data" ? 64 : name == "bits_addr" ? 35 :
            name == "bits_len" || name == "bits_strb" ? 8 :
            name == "bits_id" || name == "bits_region" || name == "bits_qos" || name == "bits_cache" ? 4 :
            name == "bits_size" || name == "bits_prot" ? 3 : name == "bits_resp" || name == "bits_burst" ? 2 : 1;
        if (!normalized || name != "valid")
          elements.push_back({b.getStringAttr(normalized ? name.substr(5) : name), false, uint(w)});
      }
      payload = BundleType::get(&ctx, elements);
      if (normalized) payload = BundleType::get(&ctx, {
          {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
    }
    auto token = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)), token, j < 6 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("ep_0_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) { return b.getNamedAttr(n, b.getStringAttr(v)); };
  auto dict = [&](std::initializer_list<NamedAttribute> a) { return b.getDictionaryAttr(a); };
  auto path = [&](unsigned j, llvm::StringRef suffix) {
    std::string normalizedSuffix = suffix.str();
    if (normalized && suffix.starts_with(".bits.bits_")) normalizedSuffix.replace(10, 1, ".");
    return b.getStringAttr("~GGBlockDevResponseSchedulerWrapper|GGBlockDevResponseSchedulerWrapper>token" + std::to_string(j) + normalizedSuffix);
  };
  SmallVector<Attribute> raw{dict({str("class", goldengate::AnnotationClasses::BridgeIO),
      str("widgetClass", "midas.models.FASEDMemoryTimingModel"), str("target", "~FireSim|FireSim>ep_0"),
      b.getNamedAttr("widgetConstructorKey", dict({str("class", "firesim.lib.bridges.CompleteConfig"),
          b.getNamedAttr("axi4Widths", dict({b.getNamedAttr("addrBits", b.getI64IntegerAttr(35)),
              b.getNamedAttr("dataBits", b.getI64IntegerAttr(64)), b.getNamedAttr("idBits", b.getI64IntegerAttr(4))}))})),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 11; ++j) {
    bool forward = j == 0 || j == 1 || j == 3 || j == 8 || j == 10, pipe = j == 5;
    NamedAttrList info;
    info.set("class", b.getStringAttr(pipe ? goldengate::AnnotationClasses::PipeChannel : forward ? goldengate::AnnotationClasses::DecoupledForwardChannel : goldengate::AnnotationClasses::DecoupledReverseChannel));
    if (pipe) info.set("latency", b.getI64IntegerAttr(1));
    if (forward) {
      info.set(j < 6 ? "validSource" : "validSink", path(j, ".bits.valid"));
      info.set(j < 6 ? "readySink" : "readySource", path(pairs[j], ".bits"));
    }
    SmallVector<Attribute> ends;
    for (auto &f : fields[j]) ends.push_back(path(j, f.empty() ? ".bits" : ".bits." + f));
    raw.push_back(dict({str("class", goldengate::AnnotationClasses::ChannelConnection), str("globalName", "ep_0_" + locals[j].str()),
        b.getNamedAttr("channelInfo", info.getDictionary(&ctx)), str("clock", "sameClock"),
        b.getNamedAttr(j < 6 ? "sources" : "sinks", b.getArrayAttr(ends))}));
  }
  raw.push_back(dict({str("class", "test.Annotation"), str("target", "~GGBlockDevResponseSchedulerWrapper|GGBlockDevResponseSchedulerWrapper>other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(raw)); return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    auto link = [&](Value d, Value z) {
      if (!isa<BundleType>(d.getType())) { require(drivers.emplace(key(d), z).second, "multiple drivers"); return; }
      std::function<void(std::string,std::string,Type,bool)> expand = [&](std::string dest, std::string src, Type t, bool flip) {
        if (auto bundle = dyn_cast<BundleType>(t)) {
          for (auto e : bundle.getElements()) expand(dest + "." + e.name.str(), src + "." + e.name.str(), e.type, flip != e.isFlip);
        } else require(links.emplace(flip ? src : dest, flip ? dest : src).second, "multiple aggregate drivers");
      };
      expand(key(d), key(z), d.getType(), false);
    };
    for (auto c : m.getOps<StrictConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto c : m.getOps<ConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) { memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n); }
  APInt read(std::string k, unsigned w) {
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    if (drivers.count(k)) return eval(drivers.at(k)).zextOrTrunc(w);
    if (links.count(k)) return read(links.at(k), w);
    auto prefix = k;
    while (prefix.find('.') != std::string::npos) {
      prefix.resize(prefix.rfind('.'));
      if (drivers.count(prefix)) return read(key(drivers.at(prefix)) + k.substr(prefix.size()), w);
    }
    throw std::runtime_error("missing aggregate driver: " + k);
  }
  APInt output(Value v, llvm::StringRef path, unsigned w = 64) {
    return read(key(v) + (path.empty() ? "" : "." + path.str()), w);
  }
  APInt eval(Value v) {
    auto k = key(v); unsigned w = width(v);
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ult(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else if (isa_and_nonnull<SubfieldOp>(op)) n = read(k, w);
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDTokenEngine(c, error)), error);
  require(succeeded(verify(*root)), "invalid FASED IR");
  Interpreter sim(named(c, "GGFASEDTokenEngine")); std::mt19937_64 rng(207);
  unsigned samples = 0, excluded = 0, pendingReset = 0, hostResetFire = 0;
  for (unsigned flags = 0; flags < 4096; ++flags) {
    sim.memo.clear(); auto flag = [&](unsigned i) { return flags >> i & 1; };
    sim.put(sim.arg(1), "", flag(0)); sim.put(sim.arg(2), "toHost.hValid", flag(1));
    sim.put(sim.arg(2), "fromHost.hReady", flag(2)); sim.put(sim.arg(2), "hBits.reset", flag(3));
    sim.put(sim.arg(4), "hReady", flag(4)); sim.put(sim.arg(5), "writeValid", flag(5));
    sim.put(sim.arg(5), "readValid", flag(6)); sim.put(sim.arg(5), "hostMemIdle", flag(7));
    bool offer = flag(1) && flag(2) && flag(5) && flag(6) && (!flag(3) || flag(7));
    bool fire = offer && flag(4);
    require(sim.output(sim.arg(6), "").getBoolValue() == fire, "targetFire differs");
    require(sim.output(sim.arg(2), "toHost.hReady").getBoolValue() == fire &&
        sim.output(sim.arg(2), "fromHost.hValid").getBoolValue() == fire, "host gates differ");
    require(sim.output(sim.arg(4), "hValid").getBoolValue() == offer, "ingress own-capacity exclusion differs");
    require(sim.output(sim.arg(7), "").getBoolValue() == (flag(0) || (flag(3) && offer)), "ingress reset differs");
    require(sim.output(sim.arg(8), "").getBoolValue() == (flag(0) || (flag(3) && fire)), "egress reset differs");
    require(sim.output(sim.arg(9), "").getBoolValue() == flag(3), "model reset differs");
    for (auto n : {"aw", "w", "ar"}) {
      std::string channel = std::string("axi4.") + n;
      sim.put(sim.arg(3), std::string(n) + ".ready", flag(8));
      sim.put(sim.arg(2), "hBits." + channel + ".valid", flag(9));
      require(sim.output(sim.arg(4), std::string("hBits.") + n + ".valid").getBoolValue() == (flag(8) && flag(9)), "ingress snoop must follow the target AXI handshake");
    }
    // Exercise every payload leaf in both timing directions, including leaves
    // optimized out of the golden RTL and the 35-bit address high bits.
    auto axi = cast<BundleType>(sim.arg(3).getType());
    for (auto ch : axi.getElements()) {
      bool reply = ch.name == "r" || ch.name == "b";
      auto bits = cast<BundleType>(cast<BundleType>(ch.type).getElement("bits")->type);
      for (auto leaf : bits.getElements()) {
        auto width = *cast<UIntType>(leaf.type).getWidth(); uint64_t value = rng() & APInt::getLowBitsSet(64, width).getZExtValue();
        std::string field = ch.name.str() + ".bits." + leaf.name.str();
        sim.put(sim.arg(reply ? 3 : 2), reply ? field : "hBits.axi4." + field, value);
        require(sim.output(sim.arg(reply ? 2 : 3), reply ? "hBits.axi4." + field : field).getZExtValue() == value, "timing payload routing differs");
        if (!reply) require(sim.output(sim.arg(4), "hBits." + field).getZExtValue() == value, "ingress payload routing differs");
      }
    }
    ++samples; excluded += offer && !fire; pendingReset += flag(3) && !flag(7); hostResetFire += flag(0) && fire;
  }
  require(excluded && pendingReset && hostResetFire, "missing FASED reset/capacity coverage");
  llvm::outs() << samples << " FASED token/reset/AXI payload cases passed\n";
}
void wiring(MLIRContext &ctx, bool normalized = false) {
  auto root = fixture(ctx, normalized); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDTokenEngine(c, error)), error);
  auto wrapper = named(c, "GGFASEDTokenWrapper"); Interpreter sim(wrapper);
  require(wrapper.getNumPorts() == 10, "incorrect seven-port FASED boundary");
  InstanceOp inner, widget;
  for (auto i : wrapper.getOps<InstanceOp>()) { if (i.getModuleName() == "GGFASEDTokenEngine") widget = i; else inner = i; }
  require(sim.key(sim.drivers.at(sim.key(widget.getResult(0)))) == sim.key(sim.arg(0)) &&
      sim.key(sim.drivers.at(sim.key(widget.getResult(1)))) == sim.key(sim.arg(1)), "host clock/reset binding differs");
  for (unsigned valid = 0; valid < 64; ++valid) for (unsigned ready = 0; ready < 32; ++ready) for (unsigned gates = 0; gates < 4; ++gates) {
    sim.memo.clear();
    for (unsigned j = 0; j < 11; ++j) sim.put(inner.getResult(j+2), j < 6 ? "valid" : "ready", j < 6 ? valid >> j & 1 : ready >> (j-6) & 1);
    sim.put(widget.getResult(2), "toHost.hReady", gates & 1); sim.put(widget.getResult(2), "fromHost.hValid", gates >> 1);
    require(sim.output(widget.getResult(2), "toHost.hValid").getBoolValue() == (valid == 63) && sim.output(widget.getResult(2), "fromHost.hReady").getBoolValue() == (ready == 31), "aggregate host predicate differs");
    for (unsigned j = 0; j < 11; ++j) {
      unsigned all = j < 6 ? 63 : 31, own = 1 << (j < 6 ? j : j-6), observed = j < 6 ? valid : ready;
      bool gate = j < 6 ? gates & 1 : gates & 2;
      require(sim.output(inner.getResult(j+2), j < 6 ? "ready" : "valid").getBoolValue() == (gate && (observed | own) == all), "eleven-channel own-predicate exclusion differs");
    }
  }
  const llvm::StringRef channels[]{"aw","w","b","ar","r","reset","aw","w","b","ar","r"};
  unsigned leaves = 0; std::mt19937_64 rng(208);
  for (unsigned trial = 0; trial < 10; ++trial) {
    sim.memo.clear();
    for (unsigned j = 0; j < 11; ++j) {
      auto token = cast<BundleType>(inner.getResult(j+2).getType()); auto payload = token.getElement("bits")->type;
      SmallVector<std::pair<std::string,unsigned>> fields;
      if (auto b = dyn_cast<BundleType>(payload)) {
        if (normalized) {
          auto data = cast<BundleType>(b.getElement("bits")->type);
          for (auto e : data.getElements()) fields.emplace_back("bits_" + e.name.str(), *cast<UIntType>(e.type).getWidth());
          fields.emplace_back("valid", 1);
        } else for (auto e : b.getElements()) fields.emplace_back(e.name.str(), *cast<UIntType>(e.type).getWidth());
      } else fields.emplace_back("", 1);
      for (auto [name,width] : fields) {
        uint64_t value = rng() & APInt::getLowBitsSet(64,width).getZExtValue();
        std::string tokenField = "bits" + (name.empty() ? "" : "." + name);
        if (normalized && llvm::StringRef(name).starts_with("bits_")) tokenField = "bits.bits." + name.substr(5);
        std::string hostField = j == 5 ? "hBits.reset" : "hBits.axi4." + channels[j].str() + "." +
            (name.empty() ? "ready" : name == "valid" ? "valid" : "bits." + name.substr(5));
        if (j < 6) { sim.put(inner.getResult(j+2), tokenField, value);
          require(sim.output(widget.getResult(2), hostField).getZExtValue() == value, "toHost AXI token leaf miswired");
        } else { sim.put(widget.getResult(2), hostField, value);
          require(sim.output(inner.getResult(j+2), tokenField).getZExtValue() == value, "fromHost AXI token leaf miswired"); }
        ++leaves;
      }
    }
  }
  require(leaves == 460, "missing AXI token payload coverage");
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == 13, "annotation classes consumed");
  auto target = cast<DictionaryAttr>(raw[0]).getAs<StringAttr>("target");
  require(target.getValue() == "~GGFASEDTokenWrapper|GGFASEDTokenEngine>hPort", "bridge target not transferred");
  for (unsigned j = 0; j < 11; ++j) {
    auto ends = cast<DictionaryAttr>(raw[j+1]).getAs<ArrayAttr>(j < 6 ? "sinks" : "sources");
    require(ends && !ends.empty(), "channel endpoint not transferred");
    for (auto e : ends) require(bool(goldengate::resolveAnnotationTarget(c, cast<StringAttr>(e).getValue(), error)), "transferred payload target does not resolve");
  }
  llvm::outs() << "8192 eleven-channel wrapper gate cases, 460 payload bindings and retained target transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  for (unsigned n = 0; n < 16; ++n) {
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> annotations(raw.begin(), raw.end());
    if (n == 0) c.setName("wrongTop");
    else if (n == 1) c->removeAttr("rawAnnotations");
    else if (n == 2) annotations.push_back(annotations[0]);
    else if (n == 3) { NamedAttrList bridge(cast<DictionaryAttr>(annotations[0])); bridge.erase("widgetConstructorKey"); annotations[0] = bridge.getDictionary(&ctx); }
    else {
      unsigned j = (n-4) % 11; NamedAttrList channel(cast<DictionaryAttr>(annotations[j+1]));
      if (n == 15) channel.set("clock", b.getStringAttr("otherClock"));
      else channel.set(j < 6 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr("~wrong|wrong>bad")}));
      annotations[j+1] = channel.getDictionary(&ctx);
    }
    if (n != 1) c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    std::string before, after, error; llvm::raw_string_ostream bs(before); root->print(bs); bs.flush();
    require(failed(goldengate::addFASEDTokenEngine(c, error)) && !error.empty(), "malformed FASED accepted");
    llvm::raw_string_ostream as(after); root->print(as); as.flush(); require(before == after, "failed FASED preflight mutated circuit");
  }
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDTokenEngine(c, error)), error);
  require(failed(goldengate::addFASEDTokenEngine(c, error)), "repeat pass accepted");
  llvm::outs() << "16 malformed boundaries and repeat pass rejected\n";
}
}
int main() {
  try { MLIRContext ctx; ctx.getOrLoadDialect<FIRRTLDialect>(); ctx.getOrLoadDialect<circt::hw::HWDialect>();
    behavior(ctx); wiring(ctx); wiring(ctx, true); rejection(ctx); return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
