// See LICENSE for license details.
#include "goldengate/FASEDFRFCFSController.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok,StringRef message) { if(!ok) throw std::runtime_error(message.str()); }
static std::string print(Operation *op) { std::string s;llvm::raw_string_ostream os(s);op->print(os);return s; }
static void checkDecode(FModuleOp m) {
  llvm::DenseMap<Value,uint64_t> values;
  llvm::DenseMap<Value,Value> sources;
  auto arg=[&](StringRef n)->Value { for(unsigned i=0;i<m.getNumPorts();++i) if(m.getPortName(i)==n)return m.getBodyBlock()->getArgument(i);llvm_unreachable("port"); };
  for(auto con:m.getOps<StrictConnectOp>()) sources[con.getDest()]=con.getSrc();
  Value address;
  for(auto inst:m.getOps<InstanceOp>()) if(inst.getName()=="transactions") address=inst.getResult(23);
  require(bool(address),"missing frontend address");
  std::function<uint64_t(Value)> eval=[&](Value v)->uint64_t {
    if(values.count(v))return values.lookup(v);
    if(sources.count(v))return eval(sources.lookup(v));
    auto *op=v.getDefiningOp();
    if(auto c=dyn_cast_or_null<ConstantOp>(op))return c.getValue().getZExtValue();
    if(auto z=dyn_cast_or_null<BitsPrimOp>(op)) return (eval(z.getInput())>>z.getLo())&((uint64_t(1)<<(z.getHi()-z.getLo()+1))-1);
    if(isa_and_nonnull<AndPrimOp>(op))return eval(op->getOperand(0))&eval(op->getOperand(1));
    if(isa_and_nonnull<DShrPrimOp>(op)) {auto shift=eval(op->getOperand(1));return shift>=64?0:eval(op->getOperand(0))>>shift;}
    if(isa_and_nonnull<DShlPrimOp>(op))return eval(op->getOperand(0))<<eval(op->getOperand(1));
    if(isa_and_nonnull<MuxPrimOp>(op))return eval(op->getOperand(0))?eval(op->getOperand(1)):eval(op->getOperand(2));
    throw std::runtime_error("unexpected decoder operation");
  };
  llvm::StringMap<Value> decoded;
  for(auto f:m.getOps<SubfieldOp>())if(f.getInput()==arg("decoded"))decoded[f.getFieldName()]=f.getResult();
  std::mt19937_64 rng(0x4452414d);
  for(unsigned i=0;i<12000;++i) {
    values.clear();uint64_t raw=rng()&((uint64_t(1)<<35)-1);values[address]=raw;
    for(auto stem:{"bank","rank","row"}) {
      unsigned width=StringRef(stem)=="bank"?3:StringRef(stem)=="rank"?2:26;
      unsigned offsets[]={0,1,13,18,34,35,63,0x80000000u,0xffffffffu};
      unsigned offset=i%2?offsets[(i/2)%9]:unsigned(rng());
      uint64_t mask=rng()&((uint64_t(1)<<width)-1);
      values[arg(std::string(stem)+"AddrOffset")]=offset;
      values[arg(std::string(stem)+"AddrMask")]=mask;
      uint64_t expected=offset>=35?0:(raw>>offset)&mask;
      require(eval(decoded.lookup(std::string(stem)+"Addr"))==expected,"full-width address decode changed");
      if(width!=26)require(eval(decoded.lookup(std::string(stem)+"AddrOH"))==(uint64_t(1)<<expected),"onehot address changed");
    }
    auto mask=values.lookup(arg("rankAddrMask"));
    require(eval(arg("ranksInUse"))==(mask>=2?15:mask==1?3:1),"rank mask priority changed");
  }
}
int main(int argc,char **argv) {
 try {
  require(argc==1||argc==3,"usage: test [10|20 output.mlir]");
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  unsigned selected=argc==3?std::stoul(argv[1]):0;
  for(unsigned capacity:{10,20}) {
    if(selected&&capacity!=selected)continue;
    auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
    auto c=*root->getOps<CircuitOp>().begin();FModuleOp helper;std::string error;
    require(succeeded(goldengate::materializeFASEDFRFCFSController(c,capacity,helper,error)),error);
    require(succeeded(verify(*root)),"invalid controller IR");checkDecode(helper);
    unsigned instances=0;for(auto i:helper.getOps<InstanceOp>())++instances;
    require(instances==3,"controller must own all three timing stages");
    auto saved=print(root->getOperation());auto sentinel=helper;
    require(failed(goldengate::materializeFASEDFRFCFSController(c,capacity,sentinel,error)),"duplicate accepted");
    require(saved==print(root->getOperation())&&sentinel==helper,"collision mutated circuit");
    require(failed(goldengate::materializeFASEDFRFCFSController(c,11,sentinel,error)),"invalid capacity accepted");
    require(saved==print(root->getOperation()),"invalid capacity mutated circuit");
    for(auto name:{std::string("GGFASEDFRFCFSController")+std::to_string(capacity),
        std::string("GGFASEDUnifiedTransactionScheduler")+std::to_string(capacity),
        std::string("GGFASEDDRAMBackend"),std::string("GGFASEDFRFCFSCommandScheduler"),
        std::string("GGFASEDRankStateTracker"),std::string("GGFASEDBankStateTracker"),
        std::string("GGFASEDRefreshUnit"),std::string("GGFASEDCollapsingBuffer")}) {
      for(bool external:{false,true}) {
        std::string text="module { firrtl.circuit \"Top\" { firrtl.module @Top() {} ";
        text+=external?"firrtl.extmodule @":"firrtl.module @";text+=name;
        text+=external?"() } }":"() {} } }";
        auto collision=parseSourceString<ModuleOp>(text,&ctx);require(bool(collision),"collision fixture parse");
        auto circ=*collision->getOps<CircuitOp>().begin();auto before=print(collision->getOperation());
        require(failed(goldengate::materializeFASEDFRFCFSController(circ,capacity,sentinel,error)),"dependency collision accepted");
        require(before==print(collision->getOperation())&&sentinel==helper,"dependency failure partially mutated circuit/result");
      }
    }
    if(argc==3){c.setName(helper.getName());std::error_code ec;llvm::raw_fd_ostream os(argv[2],ec);require(!ec,"output failed");root->print(os);os<<"\n";}
  }
  require(!selected||selected==10||selected==20,"unsupported selected capacity");
  llvm::outs()<<"FRFCFS controller: both capacities, composed IR, 24000 address/mask vectors, atomic failures\n";return 0;
 }catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
}
