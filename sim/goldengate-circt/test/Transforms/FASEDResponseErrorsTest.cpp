// See LICENSE for license details.
#include "goldengate/FASEDResponseErrors.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
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
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGFASEDFunctionalModelRegisterWrapper" {
      firrtl.module @GGFASEDTokenEngine() {}
      firrtl.module @GGFASEDFunctionalModelRegisterWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %fased_host_read_response: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>>>,
        in %fased_host_write_response: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, resp: uint<2>>>,
        in %targetFire: !firrtl.uint<1>, in %modelReset: !firrtl.uint<1>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  named(c, "GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
    b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI32IntegerAttr(10))}))}));
  SmallVector<Attribute> annos;
  for (auto name : {"fased_host_read_response.bits.resp", "fased_host_write_response.ready", "other"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGFASEDFunctionalModelRegisterWrapper|GGFASEDFunctionalModelRegisterWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
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
    unsigned asserts = 0;
    for (auto assertion : sim.module.getOps<AssertOp>()) {
      // FIRRTL assert operands are clock, predicate, enable.
      bool fail = sim.eval(assertion->getOperand(2)) && !sim.eval(assertion->getOperand(1));
      bool write = sim.memo.at(sim.key(sim.arg(8)) + ".write[" + std::to_string(asserts) + "].valid");
      require(fail == (!reset && write), "read-only assertion/reset mismatch");
      assertionFailures += fail; ++asserts;
    }
    require(asserts == 2, "missing read-only assertions");
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
  require(succeeded(goldengate::addFASEDResponseErrors(c,error)),error);
  auto top = named(c,"GGFASEDResponseErrorsWrapper"), bank = named(c,"GGFASEDResponseErrors");
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
      "~GGFASEDResponseErrorsWrapper|GGFASEDResponseErrorsWrapper>"),"target identity mismatch");
}
void rejections(MLIRContext &context) {
  for(unsigned mode=0;mode<18;++mode) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
    auto top=named(c,"GGFASEDFunctionalModelRegisterWrapper"),engine=named(c,"GGFASEDTokenEngine");
    if(mode==0)c.setName("WrongTop");
    if(mode==1)c->removeAttr("rawAnnotations");
    if(mode==2)engine->removeAttr("goldengate.bridgeConstructor");
    if(mode==3)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(9))}))}));
    if(mode>=4 && mode<8) {auto names=llvm::to_vector(top.getPortNames());names[mode-4]=b.getStringAttr("missing");top.setPortNames(names);}
    if(mode>=8 && mode<12) {SmallVector<bool> dirs;for(auto p:top.getPorts())dirs.push_back(p.direction==Direction::Out);dirs[mode-8]=true;top.setPortDirections(dirs);}
    if(mode==12) {b.setInsertionPointToStart(engine.getBodyBlock());b.create<InstanceOp>(top.getLoc(),top,"usedTop");}
    if(mode==13) {auto names=llvm::to_vector(top.getPortNames());names[6]=b.getStringAttr("fased_response_errors_mcr");top.setPortNames(names);}
    if(mode==14 || mode==15) {
      unsigned index=mode-12;top.getArgument(index).setType(UIntType::get(&context,32,false));
      auto types=llvm::to_vector(top.getPortTypes());types[index]=TypeAttr::get(UIntType::get(&context,32,false));top.setPortTypes(types);
    }
    if(mode==16 || mode==17) {b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(top.getLoc(),
        b.getStringAttr(mode==16?"GGFASEDResponseErrors":"GGFASEDResponseErrorsWrapper"),
        ConventionAttr::get(&context,Convention::Internal),ArrayRef<PortInfo>{});}
    std::string before;llvm::raw_string_ostream out(before);root->print(out);out.flush();std::string error;
    require(failed(goldengate::addFASEDResponseErrors(c,error)) && !error.empty(),"invalid boundary accepted");
    std::string after;llvm::raw_string_ostream next(after);root->print(next);next.flush();require(before==after,"rejection mutated circuit");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    behavior(context);mapping(context);rejections(context);
    llvm::outs()<<"FASED response errors: 32768 cycles, handshake/host bindings, targets and 18 atomic rejections passed\n";
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
}
