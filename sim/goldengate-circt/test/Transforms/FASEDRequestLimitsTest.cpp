// See LICENSE for license details.
#include "goldengate/FASEDRequestLimits.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <array>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
bool selected = false;
int64_t maxFlight = 10;
unsigned limit() { return std::min<int64_t>(16, maxFlight); }
unsigned limitWidth() { return llvm::Log2_64_Ceil(limit() + 1); }
std::string profile(llvm::StringRef input) {
  std::string result = input.str();
  if (selected) for (auto suffix : {"ReadAdmissionWrapper", "RequestLimitsWrapper"}) {
    std::string old = std::string("GGFASED") + suffix;
    std::string replacement = std::string("GGSelectedPrintFASED") + suffix;
    size_t pos = 0;
    while ((pos = result.find(old, pos)) != std::string::npos) {
      result.replace(pos, old.size(), replacement); pos += replacement.size();
    }
  }
  return result;
}
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = 0xA5 & ((uint64_t(1) << cast<UIntType>(r.getResult().getType()).getWidthOrSentinel()) - 1); }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 0 && reset == 2, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto text = profile(R"(module {
    firrtl.circuit "GGFASEDReadAdmissionWrapper" {
      firrtl.module @GGFASEDTokenEngine() {}
      firrtl.module @GGFASEDReadAdmissionWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %fased_write_max_reqs: !firrtl.uint<4>, in %fased_read_max_reqs: !firrtl.uint<4>,
        in %targetFire: !firrtl.uint<1>, in %modelReset: !firrtl.uint<1>,
        out %other: !firrtl.uint<8>) {}
    } })");
  size_t pos = 0;
  while ((pos = text.find("!firrtl.uint<4>", pos)) != std::string::npos) {
    text.replace(pos, 15, "!firrtl.uint<" + std::to_string(limitWidth()) + ">");
    pos += 15;
  }
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  named(c, "GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
    b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI64IntegerAttr(maxFlight))}))}));
  SmallVector<Attribute> annos;
  for (auto name : {"fased_write_max_reqs", "fased_read_max_reqs", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr(profile("~GGFASEDReadAdmissionWrapper|GGFASEDReadAdmissionWrapper>") + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
void phases(MLIRContext &context) {
  auto early = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGControlErrorWrapper" {
    firrtl.module @GGControlErrorWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })", &context);
  require(bool(early), "early fixture parse failed");
  auto c = *early->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Target")),
      b.getNamedAttr("target", b.getStringAttr("~GGControlErrorWrapper|GGControlErrorWrapper>other"))})}));
  auto key = b.getDictionaryAttr({b.getNamedAttr("axi4Edge", b.getDictionaryAttr({
      b.getNamedAttr("maxFlight", b.getI64IntegerAttr(maxFlight))}))});
  auto bridge = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
      b.getNamedAttr("widgetClass", b.getStringAttr("midas.models.FASEDMemoryTimingModel")),
      b.getNamedAttr("widgetConstructorKey", key)});
  auto raw = llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations")); raw.push_back(bridge);
  c->setAttr("rawAnnotations", b.getArrayAttr(raw));
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto malformed = cast<CircuitOp>(c->clone());
    if (bad == 0) malformed->removeAttr("rawAnnotations");
    if (bad == 1) malformed->setAttr("rawAnnotations", b.getArrayAttr({bridge, bridge}));
    if (bad >= 2) {
      NamedAttrList fields(bridge.getValue());
      if (bad == 2) fields.erase("widgetConstructorKey");
      else fields.set("class", b.getStringAttr("test.NotBridgeIO"));
      malformed->setAttr("rawAnnotations", b.getArrayAttr({fields.getDictionary(&context)}));
    }
    auto before = dump(malformed); FModuleOp result; std::string error;
    require(failed(goldengate::materializeFASEDRequestLimits(malformed, result, error)) &&
        !result && !error.empty() && dump(malformed) == before,
        "malformed early BridgeIO materialization changed IR");
    malformed.erase();
  }
  auto top = named(c, "GGControlErrorWrapper"); auto topBefore = dump(top);
  auto annotations = c->getAttr("rawAnnotations"); FModuleOp bank; std::string error;
  require(succeeded(goldengate::materializeFASEDRequestLimits(c, bank, error)), error);
  require(c.getName() == "GGControlErrorWrapper" && dump(top) == topBefore &&
      c->getAttr("rawAnnotations") == annotations && succeeded(verify(*early)),
      "early materialization changed top identity, ports or annotations");
  unsigned uses = 0; c.walk([&](InstanceOp i) { uses += i.getModuleName() == bank.getName(); });
  require(uses == 0, "materialization prematurely attached the bank");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 2, "early bank registry missing");
  for (unsigned i = 0; i < 2; ++i)
    require(cast<DictionaryAttr>(regs[i]).getAs<IntegerAttr>("offset").getInt() == 8 + 4 * i,
        "early bank changed global words 2/3 to standalone offsets");
  auto before = dump(*early); FModuleOp unchanged = top;
  require(failed(goldengate::materializeFASEDRequestLimits(c, unchanged, error)) &&
      unchanged == top && dump(*early) == before, "duplicate materialization changed result or IR");
  require(failed(goldengate::attachFASEDRequestLimits(c, bank, error)) && dump(*early) == before,
      "premature attachment changed IR");
  // The split API must preserve all hardware equations and target transfers.
  auto split = fixture(context), combined = fixture(context);
  auto splitCircuit = *split->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::materializeFASEDRequestLimits(splitCircuit, bank, error)) &&
      succeeded(goldengate::attachFASEDRequestLimits(splitCircuit, bank, error)), error);
  require(succeeded(goldengate::addFASEDRequestLimits(*combined->getOps<CircuitOp>().begin(), error)), error);
  require(dump(*split) == dump(*combined) && succeeded(verify(*split)),
      "split bank construction changed hardware or annotations");

  // Null/foreign/wrong banks, each port's name/type/direction, arity and uses.
  for (unsigned bad = 0; bad < 17; ++bad) {
    auto root = fixture(context), foreign = fixture(context);
    auto circuit = *root->getOps<CircuitOp>().begin(); FModuleOp candidate;
    require(succeeded(goldengate::materializeFASEDRequestLimits(circuit, candidate, error)), error);
    if (bad == 0) candidate = {};
    if (bad == 1) require(succeeded(goldengate::materializeFASEDRequestLimits(
        *foreign->getOps<CircuitOp>().begin(), candidate, error)), error);
    if (bad == 2) candidate = named(circuit, profile("GGFASEDReadAdmissionWrapper"));
    if (bad >= 3 && bad <= 15) {
      SmallVector<PortInfo> malformed(candidate.getPorts());
      if (bad == 15) malformed.pop_back();
      else {
        unsigned port = (bad - 3) / 3, mutation = (bad - 3) % 3;
        if (mutation == 0) malformed[port].name = b.getStringAttr("wrong");
        if (mutation == 1) malformed[port].type = UIntType::get(&context, 2);
        if (mutation == 2) malformed[port].direction = malformed[port].direction == Direction::In ? Direction::Out : Direction::In;
      }
      candidate.erase(); b.setInsertionPointToEnd(circuit.getBodyBlock());
      candidate = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("GGFASEDRequestLimits"),
          ConventionAttr::get(&context, Convention::Internal), malformed);
    }
    if (bad == 16) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      auto user = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("User"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
      b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), candidate, "used");
    }
    auto before = dump(*root);
    require(failed(goldengate::attachFASEDRequestLimits(circuit, candidate, error)) &&
        !error.empty() && dump(*root) == before, "invalid standalone bank accepted or mutated IR");
  }
  llvm::outs() << "FASED request-limit MMIO phases: early global words 2/3, unchanged top/annotations, split/combined equivalence and 19 atomic rejections passed\n";
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDRequestLimits(c, error)), error);
  require(succeeded(verify(*root)), "request limit IR verification failed");
  Interpreter sim(named(c, "GGFASEDRequestLimits")); std::array<uint32_t, 2> expected{1, 1};
  uint64_t observed[2]{0, 0};
  std::mt19937 random(228); unsigned resetWrites = 0, zeroStrobeWrites = 0, highBits = 0;
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    bool reset = cycle < 128 ? cycle & 1 : (random() & 31) == 0;
    unsigned mask = cycle < 128 ? (cycle >> 1) & 3 : random() & 3;
    unsigned strobe = cycle < 128 ? (cycle >> 3) & 15 : random() & 15;
    std::array<uint32_t, 2> data{uint32_t(random()), uint32_t(random())};
    const uint32_t edges[]{0, 1, 10, 15, 16, 0x80000000U, UINT32_MAX, 65536};
    if (cycle < 128) data = {edges[cycle % 8], edges[(cycle + 3) % 8]};
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(3)) + ".wstrb"] = strobe;
    for (unsigned i = 0; i < 2; ++i) {
      auto lane = sim.key(sim.arg(3)) + ".write[" + std::to_string(i) + "]";
      sim.memo[lane + ".valid"] = bool(mask & (1 << i)); sim.memo[lane + ".bits"] = data[i];
      sim.memo[sim.key(sim.arg(3)) + ".read[" + std::to_string(i) + "].ready"] = random() & 1;
    }
    auto check = [&]() {
      for (unsigned i = 0; i < 2; ++i) {
        require(sim.output(3, "read[" + std::to_string(i) + "].bits") == expected[i], "full word readback mismatch");
        require(sim.output(3, "read[" + std::to_string(i) + "].valid") == 1 &&
            sim.output(3, "write[" + std::to_string(i) + "].ready") == 1, "MCR handshake mismatch");
        observed[i] |= uint64_t(1) << (expected[i] & ((1U << limitWidth()) - 1));
        require(sim.output(2, i == 0 ? "write" : "read") == (expected[i] & ((1U << limitWidth()) - 1)), "admission limit truncation mismatch");
      }
    };
    check(); auto next = expected;
    for (unsigned i = 0; i < 2; ++i) if (mask & (1 << i)) next[i] = data[i];
    if (reset) next = {limit(), limit()};
    resetWrites += reset && mask; zeroStrobeWrites += !strobe && mask; highBits += bool(data[0] & 0x80000000U);
    sim.edge(); sim.memo.clear(); expected = next; check();
  }
  require(resetWrites && zeroStrobeWrites && highBits &&
      observed[0] == ((uint64_t(1) << (1U << limitWidth())) - 1) && observed[0] == observed[1],
      "reset/write/upper-bit/all-admission-values coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDRequestLimits(c, error)), error);
  auto top = named(c, profile("GGFASEDRequestLimitsWrapper")), bank = named(c, "GGFASEDRequestLimits");
  require(top.getNumPorts() == 6 && top.getPortName(5) == "fased_request_limits_mcr", "limit ports not consumed");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 2, "missing register map");
  for (unsigned i = 0; i < 2; ++i) {
    auto d = cast<DictionaryAttr>(regs[i]);
    require(d.getAs<StringAttr>("name").getValue() == (i == 0 ? "writeMaxReqs" : "readMaxReqs") &&
        d.getAs<IntegerAttr>("offset").getInt() == 8 + 4 * i &&
        d.getAs<BoolAttr>("readable").getValue() && d.getAs<BoolAttr>("writeable").getValue(), "wrong global offsets or permissions");
  }
  InstanceOp sim, mmio; for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getName() == "sim") sim = inst; else mmio = inst;
  }
  unsigned bindings = 0;
  for (auto conn : top.getOps<StrictConnectOp>()) {
    if (conn.getDest() == mmio.getResult(0)) { require(conn.getSrc() == top.getArgument(0), "wrong host clock"); ++bindings; }
    if (conn.getDest() == mmio.getResult(1)) { require(conn.getSrc() == top.getArgument(1), "reset was target-gated"); ++bindings; }
    if (conn.getDest() == sim.getResult(2) || conn.getDest() == sim.getResult(3)) {
      auto field = conn.getSrc().getDefiningOp<SubfieldOp>();
      require(field && field.getInput() == mmio.getResult(2) && field.getFieldName() ==
          (conn.getDest() == sim.getResult(2) ? "write" : "read"), "limit binding mismatch"); ++bindings;
    }
  }
  require(bindings == 4, "missing host or admission binding");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size() == 4, "annotations lost");
  for (unsigned i = 0; i < 4; ++i) {
    auto target = cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue();
    require(target.starts_with(profile("~GGFASEDRequestLimitsWrapper|")) &&
        target.contains(i < 2 ? profile("|GGFASEDReadAdmissionWrapper>") : profile("|GGFASEDRequestLimitsWrapper>")), "target identity lost");
  }
}
void rejections(MLIRContext &context) {
  for (unsigned mode = 0; mode < 15; ++mode) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
    auto top = named(c, profile("GGFASEDReadAdmissionWrapper")), engine = named(c, "GGFASEDTokenEngine");
    if (mode == 0) c.setName("WrongTop");
    if (mode == 1) c->removeAttr("rawAnnotations");
    if (mode == 2) engine->removeAttr("goldengate.bridgeConstructor");
    if (mode == 3) engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI32IntegerAttr(0))}))}));
    if (mode >= 4 && mode < 8) { auto names = llvm::to_vector(top.getPortNames()); names[mode - 4] = b.getStringAttr("missing"); top.setPortNames(names); }
    if (mode >= 8 && mode < 12) {
      SmallVector<bool> dirs; for (auto port : top.getPorts()) dirs.push_back(port.direction == Direction::Out); dirs[mode - 8] = true; top.setPortDirections(dirs);
    }
    if (mode == 12) { b.setInsertionPointToStart(engine.getBodyBlock()); b.create<InstanceOp>(top.getLoc(), top, "usedTop"); }
    if (mode == 13) { auto names = llvm::to_vector(top.getPortNames()); names[6] = b.getStringAttr("fased_request_limits_mcr"); top.setPortNames(names); }
    if (mode == 14) engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getIntegerAttr(b.getIntegerType(128), APInt(128, 10)))}))}));
    std::string before; llvm::raw_string_ostream out(before); root->print(out); out.flush(); std::string error;
    require(failed(goldengate::addFASEDRequestLimits(c, error)) && !error.empty(), "invalid boundary accepted");
    std::string after; llvm::raw_string_ostream next(after); root->print(next); next.flush();
    require(before == after, "rejection mutated circuit");
  }
  for (unsigned mode = 0; mode < 6; ++mode) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
    OpBuilder b(&context); auto engine = named(c, "GGFASEDTokenEngine");
    if (mode == 0) engine.erase();
    if (mode == 1) engine->removeAttr("goldengate.bridgeConstructor");
    if (mode >= 2) {
      Attribute flight = mode == 2 ? Attribute(b.getI64IntegerAttr(0)) :
          mode == 3 ? Attribute(b.getI64IntegerAttr(-1)) :
          mode == 4 ? Attribute(b.getStringAttr("20")) :
          Attribute(b.getIntegerAttr(b.getIntegerType(128), APInt(128, 20)));
      engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
          b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", flight)}))}));
    }
    auto before = dump(*root); FModuleOp unchanged = named(c, profile("GGFASEDReadAdmissionWrapper"));
    auto expected = unchanged; std::string error;
    require(failed(goldengate::materializeFASEDRequestLimits(c, unchanged, error)) &&
        !error.empty() && unchanged == expected && dump(*root) == before,
        "malformed materialization changed result or IR");
  }
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context); auto top = named(c, profile("GGFASEDReadAdmissionWrapper"));
  auto ports = top.getPorts(); ports[2].type = UIntType::get(&context, limitWidth() == 4 ? 5 : 4);
  auto types = llvm::to_vector(top.getPortTypes()); types[2] = TypeAttr::get(ports[2].type);
  top.setPortTypes(types);
  auto before = dump(*root); std::string error;
  require(failed(goldengate::addFASEDRequestLimits(c, error)) && dump(*root) == before,
      "constructor/admission width mismatch accepted or mutated IR");
}
}
// Export the real bank and real wrapper bindings, not a rebuilt test model.
unsigned port(FModuleOp m, llvm::StringRef name) {
  for (unsigned i=0;i<m.getNumPorts();++i) if(m.getPortName(i)==name)return i;
  throw std::runtime_error("missing production port: "+name.str());
}
Value driver(FModuleOp m,Value dest) {
  Value result;
  for(auto &op:*m.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&op.getOperand(0)==dest) {
    require(!result,"multiple production drivers");result=op.getOperand(1);
  }
  require(bool(result),"missing production driver");return result;
}
void exportProduction(MLIRContext &ctx,const char *input,const char *output,bool bankOnly=false) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production request-limit boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  if (bankOnly) {
    auto bank = named(c, "GGFASEDRequestLimits");
    bool used = false; c.walk([&](InstanceOp i) { used |= i.getModuleName() == bank.getName(); });
    require(!used, "materialized bank was already attached");
    auto isolated = ModuleOp::create(c.getLoc()); OpBuilder b(&ctx);
    b.setInsertionPointToEnd(isolated.getBody());
    auto exported = b.create<CircuitOp>(c.getLoc(), b.getStringAttr("GGFASEDRequestLimitsProbe"));
    b.setInsertionPointToEnd(exported.getBodyBlock()); b.insert(bank->clone());
    auto probe = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGFASEDRequestLimitsProbe"), bank.getConventionAttr(), bank.getPorts());
    probe.setPublic(); b.setInsertionPointToStart(probe.getBodyBlock());
    auto mmio = b.create<InstanceOp>(c.getLoc(), bank, "requestLimits");
    for (auto [i, p] : llvm::enumerate(bank.getPorts())) {
      if (i == 2) {
        for (auto name : {"write", "read"})
          b.create<StrictConnectOp>(c.getLoc(),
              b.create<SubfieldOp>(c.getLoc(), probe.getBodyBlock()->getArgument(2), name),
              b.create<SubfieldOp>(c.getLoc(), mmio.getResult(2), name));
        continue;
      }
      auto external = probe.getBodyBlock()->getArgument(i);
      b.create<ConnectOp>(c.getLoc(), p.direction == Direction::In ? mmio.getResult(i) : external,
          p.direction == Direction::In ? external : mmio.getResult(i));
    }
    require(succeeded(verify(isolated)), "invalid materialized bank probe");
    std::error_code ec; llvm::raw_fd_ostream out(output, ec); require(!ec, "cannot export bank probe");
    isolated.print(out); out << '\n'; out.close(); isolated.erase();
    llvm::outs() << "Cloned materialized production bank; admission attachment remains unsupported\n";
    return;
  }
  require(c.getName()=="GGFASEDRequestLimitsWrapper","wrong production request-limit root");
  auto top=named(c,c.getName()),inner=named(c,"GGFASEDReadAdmissionWrapper"),bank=named(c,"GGFASEDRequestLimits");
  InstanceOp sim,original;
  for(auto i:top.getOps<InstanceOp>()) {
    if(i.getInstanceName()=="sim")sim=i;
    if(i.getInstanceName()=="requestLimits")original=i;
  }
  require(sim&&sim.getModuleName()==inner.getName()&&original&&original.getModuleName()==bank.getName(),"wrong production request-limit instances");
  require(bank.getNumPorts()==4,"wrong production bank port count");
  auto hostClock=top.getBodyBlock()->getArgument(port(top,"hostClock"));
  auto hostReset=top.getBodyBlock()->getArgument(port(top,"hostReset"));
  require(driver(top,original.getResult(0))==hostClock&&driver(top,original.getResult(1))==hostReset,"request limits must use host clock/reset");
  auto mcr=top.getBodyBlock()->getArgument(port(top,"fased_request_limits_mcr"));
  require(driver(top,mcr)==original.getResult(3)&&mcr.getType()==bank.getPorts()[3].type,"MCR fragment binding differs");
  auto isolated=ModuleOp::create(c.getLoc());OpBuilder b(&ctx);
  b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDRequestLimitsProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());b.insert(bank->clone());
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDRequestLimitsProbe"),bank.getConventionAttr(),bank.getPorts());probe.setPublic();
  b.setInsertionPointToEnd(probe.getBodyBlock());
  auto mmio=b.create<InstanceOp>(c.getLoc(),bank,"requestLimits");IRMapping map;
  for(unsigned i=0;i<4;++i)map.map(original.getResult(i),mmio.getResult(i));
  map.map(hostClock,probe.getBodyBlock()->getArgument(0));
  map.map(hostReset,probe.getBodyBlock()->getArgument(1));
  map.map(mcr,probe.getBodyBlock()->getArgument(3));
  for(auto direction:{"write","read"}) {
    Value dest=sim.getResult(port(inner,std::string("fased_")+direction+"_max_reqs"));
    auto source=driver(top,dest).getDefiningOp<SubfieldOp>();
    require(source&&source.getInput()==original.getResult(2)&&source.getFieldName()==direction,"wrong admission maximum source");
    b.clone(*source,map);
    map.map(dest,b.create<SubfieldOp>(c.getLoc(),probe.getBodyBlock()->getArgument(2),direction).getResult());
  }
  unsigned cloned=0;
  for(auto &op:*top.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&
      map.contains(op.getOperand(0))&&map.contains(op.getOperand(1))) {
    b.clone(op,map);++cloned;
  }
  require(cloned==5,"expected clock/reset, two admission maxima and MCR connections");
  require(succeeded(verify(isolated)),"invalid production request-limit probe");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export request-limit probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned production request-limit bank and five clock/reset/admission/MCR bindings\n";
}
int main(int argc,char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    require(argc==1||argc==3||(argc==4&&llvm::StringRef(argv[1])=="--materialized-bank"),"usage: goldengate-fased-request-limits-test [boundary.mlir probe.mlir]");
    if(argc==3){exportProduction(context,argv[1],argv[2]);return 0;}
    if(argc==4){exportProduction(context,argv[2],argv[3],true);return 0;}
    unsigned profiles = 0;
    for (bool selectedProfile : {false, true}) {
      selected = selectedProfile;
      for (int64_t flight : {int64_t(1), int64_t(2), int64_t(3), int64_t(7), int64_t(8),
          int64_t(10), int64_t(15), int64_t(16), int64_t(20), INT64_MAX}) {
        maxFlight = flight; ++profiles;
        phases(context); behavior(context); mapping(context); rejections(context);
      }
    }
    llvm::outs() << "FASED request limits: " << profiles * 32768 << " cycles across " << profiles
        << " constructor/root profiles, 1-5 bit admission, reset/write priority, bindings, targets and 900 atomic rejections passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
}
