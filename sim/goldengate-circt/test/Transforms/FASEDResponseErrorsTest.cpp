// See LICENSE for license details.
#include "goldengate/FASEDResponseErrors.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
bool selected = false;
int64_t maxFlight = 10;
std::string profile(llvm::StringRef input) {
  std::string result = input.str();
  if (selected) for (auto suffix : {"ResponseErrorsWrapper", "FunctionalModelRegisterWrapper"}) {
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
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<NEQPrimOp>(op)) n = eval(op->getOperand(0)) != eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
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
  auto root = parseSourceString<ModuleOp>(profile(R"(module {
    firrtl.circuit "GGFASEDFunctionalModelRegisterWrapper" {
      firrtl.module @GGFASEDTokenEngine() {}
      firrtl.module @GGFASEDFunctionalModelRegisterWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %fased_host_read_response: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>>>,
        in %fased_host_write_response: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, resp: uint<2>>>,
        in %targetFire: !firrtl.uint<1>, in %modelReset: !firrtl.uint<1>,
        out %other: !firrtl.uint<8>) {}
    } })"), &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  named(c, "GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
    b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI64IntegerAttr(maxFlight))}))}));
  SmallVector<Attribute> annos;
  for (auto name : {"fased_host_read_response.bits.resp", "fased_host_write_response.ready", "other"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr(profile("~GGFASEDFunctionalModelRegisterWrapper|GGFASEDFunctionalModelRegisterWrapper>") + std::string(name)))}));
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
  auto top = named(c, "GGControlErrorWrapper"); auto topBefore = dump(top);
  auto annotations = c->getAttr("rawAnnotations"); FModuleOp bank; std::string error;
  require(succeeded(goldengate::materializeFASEDResponseErrors(c, bank, error)), error);
  require(c.getName() == "GGControlErrorWrapper" && dump(top) == topBefore &&
      c->getAttr("rawAnnotations") == annotations && succeeded(verify(*early)),
      "early materialization changed top identity, ports or annotations");
  unsigned uses = 0; c.walk([&](InstanceOp i) { uses += i.getModuleName() == bank.getName(); });
  require(uses == 0, "materialization prematurely attached the bank");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 2, "early bank lost response error registers");
  for (unsigned i = 0; i < 2; ++i) {
    auto reg = cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name") == (i ? "brespError" : "rrespError") &&
        reg.getAs<IntegerAttr>("offset").getInt() == 76 + 4*i &&
        reg.getAs<BoolAttr>("readable").getValue() && !reg.getAs<BoolAttr>("writeable").getValue(),
        "early bank changed global words 19/20 or read-only permissions");
  }
  auto before = dump(*early); FModuleOp unchanged = top;
  require(failed(goldengate::materializeFASEDResponseErrors(c, unchanged, error)) &&
      unchanged == top && dump(*early) == before, "duplicate materialization changed result or IR");
  require(failed(goldengate::attachFASEDResponseErrors(c, bank, error)) && dump(*early) == before,
      "premature attachment changed IR");
  // The split API must preserve all hardware equations and target transfers.
  auto split = fixture(context), combined = fixture(context);
  auto splitCircuit = *split->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::materializeFASEDResponseErrors(splitCircuit, bank, error)) &&
      succeeded(goldengate::attachFASEDResponseErrors(splitCircuit, bank, error)), error);
  require(succeeded(goldengate::addFASEDResponseErrors(*combined->getOps<CircuitOp>().begin(), error)), error);
  require(dump(*split) == dump(*combined) && succeeded(verify(*split)),
      "split bank construction changed hardware or annotations");

  // Null/foreign/wrong banks, each port's name/type/direction, arity and uses.
  for (unsigned bad = 0; bad < 32; ++bad) {
    auto root = fixture(context), foreign = fixture(context);
    auto circuit = *root->getOps<CircuitOp>().begin(); FModuleOp candidate;
    require(succeeded(goldengate::materializeFASEDResponseErrors(circuit, candidate, error)), error);
    if (bad == 0) candidate = {};
    if (bad == 1) require(succeeded(goldengate::materializeFASEDResponseErrors(
        *foreign->getOps<CircuitOp>().begin(), candidate, error)), error);
    if (bad == 2) candidate = named(circuit, profile("GGFASEDFunctionalModelRegisterWrapper"));
    if (bad >= 3 && bad <= 30) {
      SmallVector<PortInfo> malformed(candidate.getPorts());
      if (bad == 30) malformed.pop_back();
      else {
        unsigned port = (bad - 3) / 3, mutation = (bad - 3) % 3;
        if (mutation == 0) malformed[port].name = b.getStringAttr("wrong");
        if (mutation == 1) malformed[port].type = UIntType::get(&context, 3);
        if (mutation == 2) malformed[port].direction = malformed[port].direction == Direction::In ? Direction::Out : Direction::In;
      }
      candidate.erase(); b.setInsertionPointToEnd(circuit.getBodyBlock());
      candidate = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("GGFASEDResponseErrors"),
          ConventionAttr::get(&context, Convention::Internal), malformed);
    }
    if (bad == 31) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      auto user = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("User"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
      b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), candidate, "used");
    }
    auto before = dump(*root);
    require(failed(goldengate::attachFASEDResponseErrors(circuit, candidate, error)) &&
        !error.empty() && dump(*root) == before, "invalid standalone bank accepted or mutated IR");
  }
  llvm::outs() << "FASED response-error MMIO phases: early global words 19/20, unchanged top/annotations, split/combined equivalence and 34 atomic rejections passed\n";
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDResponseErrors(c, error)), error);
  require(succeeded(verify(*root)), "response errors IR verification failed");
  Interpreter sim(named(c, "GGFASEDResponseErrors")); std::array<unsigned,2> expected{1,1};
  std::mt19937 random(231); unsigned stalledErrors = 0, zeroBCaptures = 0, assertionFailures = 0;
  // First 512 cycles enumerate every reset/valid/ready/response combination.
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    unsigned bits = cycle < 512 ? cycle : random() & 511;
    bool reset = bits & 1;
    unsigned rv = (bits >> 1)&1, rr = (bits >> 2)&1, bv = (bits >> 3)&1, br = (bits >> 4)&1;
    unsigned r = (bits >> 5)&3, b = (bits >> 7)&3;
    sim.memo.clear(); const unsigned inputs[]{unsigned(reset),rv,rr,bv,br,r,b};
    for (unsigned i=0; i<7; ++i) sim.memo[sim.key(sim.arg(i+1))] = inputs[i];
    for (unsigned i=0; i<2; ++i) {
      bool write = random()&1;
      sim.memo[sim.key(sim.arg(8)) + ".write[" + std::to_string(i) + "].valid"] = write;
      sim.memo[sim.key(sim.arg(8)) + ".write[" + std::to_string(i) + "].bits"] = random();
      sim.memo[sim.key(sim.arg(8)) + ".read[" + std::to_string(i) + "].ready"] = random()&1;
    }
    auto check = [&]() {
      for (unsigned i=0; i<2; ++i) {
        std::string lane = "[" + std::to_string(i) + "]";
        require(sim.output(8, "read" + lane + ".bits") == expected[i], "error readback mismatch");
        require(sim.output(8, "read" + lane + ".valid") == 1 && sim.output(8, "write" + lane + ".ready") == 1,
            "MCR handshake mismatch");
      }
    };
    check();
    unsigned diagnostics = 0;
    for (auto print : sim.module.getOps<PrintFOp>()) {
      bool fail = sim.eval(print.getCond());
      bool write = sim.memo.at(sim.key(sim.arg(8)) + ".write[" + std::to_string(diagnostics) + "].valid");
      require(fail == (!reset && write), "read-only diagnostic/reset mismatch");
      require(print.getClock() == sim.arg(0) && print.getSubstitutions().empty() &&
          print.getFormatString() == std::string("Assertion failed: Register ") +
              (diagnostics ? "brespError" : "rrespError") + " is read only\n",
          "read-only diagnostic clock/message mismatch");
      assertionFailures += fail; ++diagnostics;
    }
    require(diagnostics == 2 && sim.module.getOps<AssertOp>().empty() &&
        sim.module.getOps<StopOp>().empty(), "read-only diagnostics must continue simulation");
    auto next = expected;
    if (reset) next = {0,0};
    else { if (rv && rr && r) next[0] = r; if (bv && br && b) next[1] = r; }
    stalledErrors += !reset && ((rv && !rr && r) || (bv && !br && b));
    zeroBCaptures += !reset && bv && br && b && !r;
    sim.edge(); sim.memo.clear(); expected = next; check();
  }
  require(stalledErrors && zeroBCaptures && assertionFailures, "coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto constructor = named(c,"GGFASEDTokenEngine")->getAttr("goldengate.bridgeConstructor");
  require(succeeded(goldengate::addFASEDResponseErrors(c,error)),error);
  require(named(c,"GGFASEDTokenEngine")->getAttr("goldengate.bridgeConstructor") == constructor,
      "retained constructor changed");
  auto top = named(c,profile("GGFASEDResponseErrorsWrapper")), bank = named(c,"GGFASEDResponseErrors");
  require(top.getNumPorts()==8 && top.getPortName(7)=="fased_response_errors_mcr","copied port shape mismatch");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size()==2,"missing register map");
  for (unsigned i=0;i<2;++i) {
    auto d = cast<DictionaryAttr>(regs[i]);
    require(d.getAs<StringAttr>("name").getValue()==(i?"brespError":"rrespError") &&
        d.getAs<IntegerAttr>("offset").getInt()==76+4*i && d.getAs<BoolAttr>("readable").getValue() &&
        !d.getAs<BoolAttr>("writeable").getValue(),"register map mismatch");
  }
  InstanceOp sim, mmio; for(auto i:top.getOps<InstanceOp>()) { if(i.getName()=="sim") sim=i;else mmio=i; }
  unsigned bindings=0;
  for(auto conn:top.getOps<StrictConnectOp>()) {
    if(conn.getDest()==mmio.getResult(0)) { require(conn.getSrc()==top.getArgument(0),"clock mismatch");++bindings; }
    if(conn.getDest()==mmio.getResult(1)) { require(conn.getSrc()==top.getArgument(1),"reset target-gated");++bindings; }
    for(unsigned i=0;i<2;++i) {
      if(conn.getDest()==mmio.getResult(2+2*i)) {
        auto f=conn.getSrc().getDefiningOp<SubfieldOp>();
        require(f && f.getFieldName()=="valid" && f.getInput()==top.getArgument(2+i),"valid binding mismatch");++bindings;
      }
      if(conn.getDest()==mmio.getResult(3+2*i)) {
        auto f=conn.getSrc().getDefiningOp<SubfieldOp>();
        require(f && f.getFieldName()=="ready" && f.getInput()==sim.getResult(2+i),"ready must observe existing producer");++bindings;
      }
      if(conn.getDest()==mmio.getResult(6+i)) {
        auto f=conn.getSrc().getDefiningOp<SubfieldOp>();
        auto parent=f?f.getInput().getDefiningOp<SubfieldOp>():SubfieldOp();
        require(f && f.getFieldName()=="resp" && parent && parent.getFieldName()=="bits" &&
            parent.getInput()==top.getArgument(2+i),"response payload binding mismatch");++bindings;
      }
    }
  }
  require(bindings==8,"missing handshake bindings");
  require(std::distance(top.getOps<ConnectOp>().begin(),top.getOps<ConnectOp>().end())==8,"copied ports lost");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(annos.size()==3,"annotations lost");
  for(auto a:annos) require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().starts_with(
      profile("~GGFASEDResponseErrorsWrapper|GGFASEDResponseErrorsWrapper>")),"target identity mismatch");
}
void rejections(MLIRContext &context) {
  for(unsigned mode=0;mode<23;++mode) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
    auto top=named(c,profile("GGFASEDFunctionalModelRegisterWrapper")),engine=named(c,"GGFASEDTokenEngine");
    if(mode==0)c.setName("WrongTop");
    if(mode==1)c->removeAttr("rawAnnotations");
    if(mode==2)engine->removeAttr("goldengate.bridgeConstructor");
    if(mode==3)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(0))}))}));
    if(mode>=4 && mode<8) {auto names=llvm::to_vector(top.getPortNames());names[mode-4]=b.getStringAttr("missing");top.setPortNames(names);}
    if(mode>=8 && mode<12) {SmallVector<bool> dirs;for(auto p:top.getPorts())dirs.push_back(p.direction==Direction::Out);dirs[mode-8]=true;top.setPortDirections(dirs);}
    if(mode==12) {b.setInsertionPointToStart(engine.getBodyBlock());b.create<InstanceOp>(top.getLoc(),top,"usedTop");}
    if(mode==13) {auto names=llvm::to_vector(top.getPortNames());names[6]=b.getStringAttr("fased_response_errors_mcr");top.setPortNames(names);}
    if(mode==14 || mode==15) {
      unsigned index=mode-12;top.getArgument(index).setType(UIntType::get(&context,32,false));
      auto types=llvm::to_vector(top.getPortTypes());types[index]=TypeAttr::get(UIntType::get(&context,32,false));top.setPortTypes(types);
    }
    if(mode==16 || mode==17) {b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(top.getLoc(),
        b.getStringAttr(mode==16?"GGFASEDResponseErrors":profile("GGFASEDResponseErrorsWrapper")),
        ConventionAttr::get(&context,Convention::Internal),ArrayRef<PortInfo>{});}
    if (mode == 18) engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
      b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight",
        b.getIntegerAttr(IntegerType::get(&context, 128), APInt(128, 10)))}))}));
    if (mode >= 19) {
      SmallVector<NamedAttribute> fields;
      if (mode == 19) fields.push_back(b.getNamedAttr("maxFlight", b.getI64IntegerAttr(-1)));
      if (mode == 20) fields.push_back(b.getNamedAttr("maxFlight", b.getStringAttr("10")));
      engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr(mode == 22
          ? ArrayRef<NamedAttribute>{}
          : ArrayRef<NamedAttribute>{b.getNamedAttr("axi4Edge", b.getDictionaryAttr(fields))}));
    }
    std::string before;llvm::raw_string_ostream out(before);root->print(out);out.flush();std::string error;
    require(failed(goldengate::addFASEDResponseErrors(c,error)) && !error.empty(),"invalid boundary accepted");
    std::string after;llvm::raw_string_ostream next(after);root->print(next);next.flush();require(before==after,"rejection mutated circuit");
  }
}
}
// Clone the production bank and its nine observed wrapper connections. The
// isolated probe exposes the response handshakes; queues remain a separate test.
unsigned port(FModuleOp m, llvm::StringRef name) {
  for (unsigned i=0;i<m.getNumPorts();++i) if(m.getPortName(i)==name)return i;
  throw std::runtime_error("missing production port: "+name.str());
}
Value driver(FModuleOp m, Value dest) {
  Value result;
  for(auto &op:*m.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&op.getOperand(0)==dest) {
    require(!result,"multiple production drivers");result=op.getOperand(1);
  }
  require(bool(result),"missing production driver");return result;
}
void exportProduction(MLIRContext &ctx, const char *input, const char *output) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production response-errors boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  require(c.getName()=="GGFASEDResponseErrorsWrapper","wrong production response-errors root");
  auto top=named(c,c.getName()),inner=named(c,"GGFASEDFunctionalModelRegisterWrapper"),bank=named(c,"GGFASEDResponseErrors");
  InstanceOp sim,original;
  for(auto i:top.getOps<InstanceOp>()) {
    if(i.getInstanceName()=="sim")sim=i;
    if(i.getInstanceName()=="responseErrors")original=i;
  }
  require(sim&&sim.getModuleName()==inner.getName()&&original&&original.getModuleName()==bank.getName(),"wrong production response-errors instances");
  require(bank.getNumPorts()==9,"wrong production bank port count");
  SmallVector<Value> sources;
  auto arg=[&](llvm::StringRef n){return top.getBodyBlock()->getArgument(port(top,n));};
  require(driver(top,original.getResult(0))==arg("hostClock")&&
      driver(top,original.getResult(1))==arg("hostReset"),"response errors must use host clock/reset");
  sources.push_back(arg("hostClock"));sources.push_back(arg("hostReset"));
  for(unsigned i=2;i<8;++i) {
    bool write=i==4||i==5||i==7;
    llvm::StringRef response=write?"fased_host_write_response":"fased_host_read_response";
    auto source=driver(top,original.getResult(i));
    auto field=source.getDefiningOp<SubfieldOp>();
    if(i<6) {
      bool ready=i==3||i==5;
      require(field&&field.getFieldName()==(ready?"ready":"valid")&&
          field.getInput()==(ready?sim.getResult(port(inner,response)):arg(response)),
          "wrong production response handshake source");
    } else {
      auto bits=field?field.getInput().getDefiningOp<SubfieldOp>():SubfieldOp();
      require(field&&field.getFieldName()=="resp"&&bits&&bits.getFieldName()=="bits"&&
          bits.getInput()==arg(response),"wrong production response code source");
    }
    sources.push_back(source);
  }
  auto mcr=arg("fased_response_errors_mcr");
  require(driver(top,mcr)==original.getResult(8)&&mcr.getType()==bank.getPorts()[8].type,"MCR fragment binding differs");
  auto isolated=ModuleOp::create(c.getLoc());OpBuilder b(&ctx);
  b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDResponseErrorsProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());b.insert(bank->clone());
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDResponseErrorsProbe"),bank.getConventionAttr(),bank.getPorts());probe.setPublic();
  b.setInsertionPointToEnd(probe.getBodyBlock());
  auto mmio=b.create<InstanceOp>(c.getLoc(),bank,"responseErrors");IRMapping map;
  for(unsigned i=0;i<9;++i)map.map(original.getResult(i),mmio.getResult(i));
  for(unsigned i=0;i<8;++i)map.map(sources[i],probe.getBodyBlock()->getArgument(i));
  map.map(mcr,probe.getBodyBlock()->getArgument(8));
  unsigned cloned=0;
  for(auto &op:*top.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&
      map.contains(op.getOperand(0))&&map.contains(op.getOperand(1))) {
    b.clone(op,map);++cloned;
  }
  require(cloned==9,"expected clock/reset, six response observations and MCR connections");
  require(succeeded(verify(isolated)),"invalid production response-errors probe");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export response-errors probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned production response-errors bank and nine clock/reset/response/MCR bindings\n";
}
int main(int argc,char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    require(argc==1||argc==3,"usage: goldengate-fased-response-errors-test [boundary.mlir probe.mlir]");
    if(argc==3){exportProduction(context,argv[1],argv[2]);return 0;}
    for (bool profileSelected : {false, true}) {
      selected = profileSelected;
      for (int64_t flight : {int64_t(1), int64_t(9), int64_t(10), int64_t(16),
                            int64_t(20), INT64_MAX}) {
        maxFlight = flight; phases(context);behavior(context);mapping(context);rejections(context);
      }
    }
    llvm::outs()<<"FASED response errors: 393216 cycles across legacy/selected profiles and six positive flight capacities, handshake/host bindings, retained constructors, targets and 276 atomic boundary rejections passed\n";
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
}
