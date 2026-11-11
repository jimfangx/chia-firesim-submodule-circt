// See LICENSE for license details.
#include "goldengate/AssertionBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
void require(bool v,const std::string& message){if(!v)throw std::runtime_error(message);}
std::string dump(Operation *op){std::string s;llvm::raw_string_ostream out(s);op->print(out);return s;}
std::string checkBinding(MLIRContext& ctx) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
  auto circuit=*root->getOps<CircuitOp>().begin();auto top=*circuit.getOps<FModuleOp>().begin();OpBuilder b(&ctx);
  auto bit=UIntType::get(&ctx,1);
  auto token=[&](unsigned n){return BundleType::get(&ctx,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,UIntType::get(&ctx,n)}});};
  SmallVector<std::pair<unsigned,PortInfo>> ports{{0,{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In}},
    {0,{b.getStringAttr("hostReset"),bit,Direction::In}},{0,{b.getStringAttr("assertToken"),token(3),Direction::Out}},
    {0,{b.getStringAttr("resetToken"),token(1),Direction::Out}}};
  top.insertPorts(ports);
  auto key=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.widgets.AssertBridgeParameters")),
    b.getNamedAttr("assertPortName",b.getStringAttr("asserts")),b.getNamedAttr("resetPortName",b.getStringAttr("globalReset")),
    b.getNamedAttr("assertMessages",b.getArrayAttr({b.getStringAttr("assert zero\n"),b.getStringAttr("assert one\n"),b.getStringAttr("assert two\n")}))});
  auto clock=b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("clock")),b.getNamedAttr("multiplier",b.getI64IntegerAttr(1)),b.getNamedAttr("divisor",b.getI64IntegerAttr(1))});
  SmallVector<Attribute> annotations;
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("target",b.getStringAttr("~Top|Top>asserts")),b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.AssertBridgeModule")),
    b.getNamedAttr("widgetConstructorKey",key),b.getNamedAttr("clockInfo",clock),
    b.getNamedAttr("channelMapping",b.getDictionaryAttr({b.getNamedAttr("asserts",b.getStringAttr("assertsChannel")),b.getNamedAttr("globalReset",b.getStringAttr("resetChannel"))}))}));
  const char* globals[]{"assertsChannel","resetChannel"};const char* targets[]{"assertToken","resetToken"};
  for(unsigned i=0;i<2;++i)annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
    b.getNamedAttr("globalName",b.getStringAttr(globals[i])),b.getNamedAttr("channelInfo",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),b.getNamedAttr("latency",b.getI64IntegerAttr(0))})),
    b.getNamedAttr("clock",b.getStringAttr("~Top|Top>hostClock")),b.getNamedAttr("sources",b.getArrayAttr({b.getStringAttr(std::string("~Top|Top>")+targets[i]+".bits")}))}));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));std::string error;SmallVector<FModuleOp> hosts,discovered;
  require(succeeded(goldengate::materializeAssertionBridgeHosts(circuit,32,12,hosts,error)),error);
  require(succeeded(goldengate::bindAssertionBridgeHosts(circuit,hosts,error)),error);
  require(succeeded(verify(*root)),"bound IR verification");
  require(succeeded(goldengate::discoverBoundAssertionBridgeHosts(circuit,discovered,error))&&discovered==hosts,error);
  SmallVector<goldengate::ControlMMIOWidget> widgets;
  require(succeeded(goldengate::deriveAssertionControlMMIOCatalog(circuit,hosts,widgets,error))&&widgets.size()==1&&widgets[0].registerCount==6,error);
  SmallVector<goldengate::ControlMMIORegion> regions{{"AssertBridgeModule_0",128,32}};std::string header;
  require(succeeded(goldengate::prepareAssertionBridgeHeader(circuit,hosts,regions,header,error)),error);
  require(header.find("new synthesized_assertions_t")!=std::string::npos&&header.find(".enable = 148ULL")!=std::string::npos&&header.find("assert zero")!=std::string::npos,"driver constructor addresses/messages");
  auto original=header;regions[0].size=64;
  require(failed(goldengate::prepareAssertionBridgeHeader(circuit,hosts,regions,header,error))&&header==original,"bad header allocation must preserve output");
  return original;
}

int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.getOrLoadDialect<FIRRTLDialect>();ctx.getOrLoadDialect<circt::hw::HWDialect>();
    for(unsigned bits:{25u,32u}) {
      auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
      require(bool(root),"parse fixture");auto circuit=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
      auto key=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.widgets.AssertBridgeParameters")),
        b.getNamedAttr("assertPortName",b.getStringAttr("asserts")),b.getNamedAttr("resetPortName",b.getStringAttr("globalReset")),
        b.getNamedAttr("assertMessages",b.getArrayAttr({b.getStringAttr("assert zero\n"),b.getStringAttr("assert one\n"),b.getStringAttr("assert two\n")}))});
      auto bridge=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),b.getNamedAttr("target",b.getStringAttr("~Top|Top>asserts")),b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.AssertBridgeModule")),b.getNamedAttr("widgetConstructorKey",key)});
      circuit->setAttr("rawAnnotations",b.getArrayAttr({bridge}));
      SmallVector<FModuleOp> hosts;std::string error;
      require(succeeded(goldengate::materializeAssertionBridgeHosts(circuit,bits,12,hosts,error)),error);
      require(hosts.size()==1&&succeeded(verify(*root)),"invalid assertion hardware");
      auto info=hosts[0]->getAttrOfType<DictionaryAttr>("goldengate.assertionHost");
      require(info&&info.getAs<IntegerAttr>("queueDepth").getInt()==2,"queue depth");
      auto before=dump(*root);auto count=hosts.size();
      require(failed(goldengate::materializeAssertionBridgeHosts(circuit,bits,12,hosts,error))&&hosts.size()==count&&dump(*root)==before,"module collision must be atomic");
      if(argc>=2&&bits==32) {
        circuit.setName("GGAssertionBridgeBank");
        circuit->removeAttr("rawAnnotations");
        std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"open output");root->print(out);
      }
    }
    auto header=checkBinding(ctx);
    if(argc>=3){std::error_code ec;llvm::raw_fd_ostream out(argv[2],ec);require(!ec,"open header output");out<<header;}
    llvm::outs()<<"PASS native assertion bridge hardware, token binding, MMIO/header and atomic rejection\n";
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
