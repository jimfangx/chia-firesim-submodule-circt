// See LICENSE for license details.
#include "goldengate/FASEDLLCModel.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok,StringRef text){if(!ok)throw std::runtime_error(text.str());}
static std::string print(Operation *op){std::string s;llvm::raw_string_ostream os(s);op->print(os);return s;}
int main(int argc,char **argv) {
 try {
  require(argc<=2,"usage: test [output.mlir]");
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
  auto circuit=*root->getOps<CircuitOp>().begin();auto top=*circuit.getOps<FModuleOp>().begin();
  auto original=print(top);FModuleOp helper;std::string error;
  require(succeeded(goldengate::materializeFASEDLLCModel(circuit,helper,error)),error);
  require(succeeded(verify(*root)),"invalid LLC composed IR");require(print(top)==original&&circuit.getName()=="Top","changed original circuit");
  unsigned children=0,unreset=0,resets=0;for(auto i:helper.getOps<InstanceOp>())++children;
  for(auto r:helper.getOps<RegOp>())++unreset;for(auto r:helper.getOps<RegResetOp>())++resets;
  require(children==2,"missing metadata/MSHR composition");
  require(unreset==24,"four depth-two three-field queues must preserve unreset payload");
  require(resets==18,"four queue pointer/full groups, state/busy and four counters required");
  for(auto p:helper.getPorts())if(p.getName()=="arAddr"||p.getName()=="awAddr"||p.getName()=="memArAddr"||p.getName()=="memAwAddr")
    require(*cast<UIntType>(p.type).getWidth()==35,"address width changed");
  auto sentinel=helper;
  for(auto symbol:{"GGFASEDLLCModel","GGFASEDLLCMetadata","GGFASEDLLCMSHRTable"})for(bool external:{false,true}) {
    std::string text="module { firrtl.circuit \"Top\" { firrtl.module @Top() {} ";
    text+=external?"firrtl.extmodule @":"firrtl.module @";text+=symbol;text+=external?"() } }":"() {} } }";
    auto collision=parseSourceString<ModuleOp>(text,&ctx);auto c=*collision->getOps<CircuitOp>().begin();
    auto before=print(collision->getOperation());
    require(failed(goldengate::materializeFASEDLLCModel(c,sentinel,error)),"dependency collision accepted");
    require(before==print(collision->getOperation())&&sentinel==helper,"dependency collision partially mutated circuit");
  }
  if(argc==2){circuit.setName(helper.getName());std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"output file failed");root->print(os);os<<"\n";}
  llvm::outs()<<"LLC model: verified exact queue payload/reset geometry, dependency composition and atomic collisions\n";return 0;
 }catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
}
