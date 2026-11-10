// See LICENSE for license details.
#include "goldengate/InstrumentationControl.h"
#include "goldengate/PrintBridgePayload.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <stdexcept>
using namespace mlir; using namespace circt::firrtl;
namespace {
void require(bool ok,const std::string &why){if(!ok)throw std::runtime_error(why);}
std::string dump(Operation *op){std::string result;llvm::raw_string_ostream out(result);op->print(out);return result;}
struct Fixture {
  OwningOpRef<ModuleOp> root; CircuitOp circuit;
  Fixture(MLIRContext &ctx,StringRef path) {
    llvm::SourceMgr source;
    for(auto suffix:{".fir",".anno.json"}){auto file=llvm::MemoryBuffer::getFile(path.str()+suffix);require(bool(file),"missing fixture");source.AddNewSourceBuffer(std::move(*file),llvm::SMLoc());}
    DefaultTimingManager timing;auto scope=timing.getRootScope();FIRParserOptions options;options.numAnnotationFiles=1;
    root=importFIRFile(source,&ctx,scope,options);require(bool(root),"import mixed fixture");circuit=*root->getOps<CircuitOp>().begin();
  }
};
void positive(MLIRContext &ctx,StringRef path,unsigned width) {
  Fixture f(ctx,path);std::string header="old",decoder="old",peer="old",error;
  auto before=f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::compileInstrumentationControl(f.circuit,width,12,header,decoder,peer,error)),error);
  require(succeeded(verify(*f.root)),"mixed verifier");
  require(f.circuit.getName()=="GGSelectedPrintCPUStreamPlatformWrapper","complete CPU transport top");
  FModuleOp top,allocation;
  for(auto module:f.circuit.getOps<FModuleOp>()){if(module.getName()==f.circuit.getName())top=module;if(module.getName()=="GGControlAddressDecode")allocation=module;}
  unsigned ctrl=0,cpu=0;for(auto port:top.getPorts()){auto n=port.name.getValue();ctrl+=n=="ctrl";cpu+=n=="cpu_managed_axi4";require(!n.starts_with("ctrl_")&&!n.starts_with("autocounter_")&&!n.starts_with("print_")&&!n.starts_with("cpu_stream_"),"unbound local port remains");}
  require(ctrl==1&&cpu==1&&top->hasAttr("goldengate.instrumentationGlobalControl"),"missing shared control or complete CPU port");
  auto regions=allocation->getAttrOfType<ArrayAttr>("goldengate.controlRegions");require(regions&&regions.size()==4,"incomplete mixed bank allocation");
  const char *names[]={"AutoCounterBridgeModule_0","AutoCounterBridgeModule_1","PrintBridgeModule_0","CPUManagedStreamEngine_0"};
  const unsigned starts[]={0,64,128,160},sizes[]={64,64,32,4};
  for(auto [i,attr]:llvm::enumerate(regions)){auto row=cast<DictionaryAttr>(attr);require(row.getAs<StringAttr>("name")==names[i]&&row.getAs<IntegerAttr>("start").getInt()==starts[i]&&row.getAs<IntegerAttr>("size").getInt()==sizes[i],"complete SFC registration/size allocation differs");}
  require(StringRef(header).count("new autocounter_t")==2&&StringRef(header).contains("make_print_bridge_0")&&StringRef(header).contains("128ULL")&&StringRef(header).contains("160ULL"),"mixed allocated constructors missing");
  require(StringRef(decoder).contains("signed=%d")&&StringRef(decoder).contains("extra=%x")&&StringRef(peer).contains("PRINTBRIDGEMODULE_0_to_cpu_stream"),"record format order or CPU map missing");
  require(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations").size()==before.size()+2,"constructor/channel/output annotation inventory differs");
  auto ir=dump(*f.root),h=header,d=decoder,p=peer;
  require(failed(goldengate::compileInstrumentationControl(f.circuit,width,12,header,decoder,peer,error))&&dump(*f.root)==ir&&header==h&&decoder==d&&peer==p,"repeat assembly changed IR or collateral");
}
void negativeAssembly(MLIRContext &ctx,StringRef path) {
  for (unsigned bad=0;bad<4;++bad) {
    Fixture f(ctx,path);SmallVector<FModuleOp> csv,payloads,stages,controls,prints;std::string error;
    require(succeeded(goldengate::materializeAutoCounterBridgeHosts(f.circuit,32,12,csv,error)),error);
    require(succeeded(goldengate::bindAutoCounterBridgeHosts(f.circuit,csv,error)),error);
    require(succeeded(goldengate::materializePrintBridgePayloads(f.circuit,payloads,error)),error);
    require(succeeded(goldengate::materializePrintBridgeTokenStages(f.circuit,payloads,stages,error)),error);
    require(succeeded(goldengate::materializePrintBridgeControls(f.circuit,payloads,stages,controls,error)),error);
    require(succeeded(goldengate::materializePrintBridgeHosts(f.circuit,controls,32,12,prints,error)),error);
    require(succeeded(goldengate::bindPrintBridgeHosts(f.circuit,prints,error)),error);
    if(bad==0)csv[0]=FModuleOp{};
    if(bad==1)prints[0]=FModuleOp{};
    if(bad==2)std::swap(csv[0],csv[1]);
    if(bad==3)for(auto module:f.circuit.getOps<FModuleOp>())if(module.getName()==f.circuit.getName())module->setAttr("sym_name",StringAttr::get(&ctx,"MissingActiveTop"));
    auto ir=dump(*f.root);std::string h="header",d="decoder",p="peer";
    require(failed(goldengate::assembleInstrumentationControl(f.circuit,csv,prints,h,d,p,error))&&!error.empty()&&dump(*f.root)==ir&&h=="header"&&d=="decoder"&&p=="peer","null/reordered/missing-top assembly changed IR/collateral");
  }
}
void negative(MLIRContext &ctx,StringRef path) {
  for(unsigned bad=0;bad<4;++bad){Fixture f(ctx,path);Builder b(&ctx);auto raw=llvm::to_vector(f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations"));
    if(bad<3){for(auto &attr:raw){auto row=cast<DictionaryAttr>(attr);auto kind=row.getAs<StringAttr>("widgetClass");if(!kind||kind!="midas.widgets.PrintBridgeModule")continue;NamedAttrList changed(row);
      if(bad==0)changed.set("widgetClass",b.getStringAttr("test.UnimplementedBridge"));
      if(bad==1)changed.set("channelMapping",b.getDictionaryAttr({}));
      if(bad==2){NamedAttrList key(row.getAs<DictionaryAttr>("widgetConstructorKey"));key.set("printPorts",b.getArrayAttr({}));changed.set("widgetConstructorKey",key.getDictionary(&ctx));}
      attr=changed.getDictionary(&ctx);break;}
      f.circuit->setAttr("rawAnnotations",b.getArrayAttr(raw));
    }else{OpBuilder op(&ctx);op.setInsertionPointToEnd(f.circuit.getBodyBlock());op.create<FModuleOp>(f.circuit.getLoc(),op.getStringAttr("GGCPUStreamPortAdapter"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
    std::string h="header",d="decoder",p="peer",error;auto ir=dump(*f.root);
    require(failed(goldengate::compileInstrumentationControl(f.circuit,32,12,h,d,p,error))&&!error.empty()&&dump(*f.root)==ir&&h=="header"&&d=="decoder"&&p=="peer","invalid early/late mixed composition changed IR/collateral");
  }
}
}
int main(int argc,char **argv){try{require(argc==2,"usage fixture-basename");MLIRContext ctx;ctx.getOrLoadDialect<FIRRTLDialect>();ctx.getOrLoadDialect<circt::hw::HWDialect>();for(unsigned width:{25u,32u})positive(ctx,argv[1],width);negative(ctx,argv[1]);negativeAssembly(ctx,argv[1]);llvm::outs()<<"PASS complete enabled CSV + Print + CPU shared control/driver allocation, F1/F2 widths and eight atomic rejections\n";}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
