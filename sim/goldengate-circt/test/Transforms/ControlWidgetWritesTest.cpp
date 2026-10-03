// See LICENSE for license details.
#include "goldengate/ControlWidgetWrites.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,llvm::StringRef s){if(!ok)throw std::runtime_error(s.str());}
std::string dump(ModuleOp m){std::string s;llvm::raw_string_ostream out(s);m.print(out);return s;}
constexpr const char *oldTop="GGControlWriteDispatchWrapper",*newTop="GGControlWidgetWriteWrapper";
const std::pair<unsigned,const char*> slaves[]{{2,"tracerv_ctrl"},{4,"loadmem_ctrl"},
  {5,"peekPokeBridge_ctrl"},{6,"uartBridge_ctrl"},{7,"clockBridge_ctrl"},
  {9,"resetBridge_ctrl"},{10,"cpuStream_ctrl"}};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0) {
  std::string address="bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>";
  std::string data="bundle<data: uint<32>, last: uint<1>, id: uint<12>, strb: uint<4>, user: uint<1>>";
  auto token=[](std::string bits){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: "+bits+">";};
  std::string control="!firrtl.bundle<aw: "+token(address)+", w: "+token(data)+", b flip: "+token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>")+", ar: "+token(address)+", r flip: "+token("bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>")+">";
  std::string text="module { firrtl.circuit \""+std::string(oldTop)+"\" { firrtl.module @"+oldTop+"(in %other: !firrtl.uint<8>";
  for(auto [slave,port]:slaves) {
    text+=", "+std::string(bad==1&&slave==7?"out":"in")+" %"+port+": "+control;
    const std::pair<const char*,unsigned> fields[]{{"aw_ready",1},{"w_ready",1},{"aw_valid",1},{"aw_bits_addr",25},{"aw_bits_len",8},{"aw_bits_id",12},{"w_valid",1},{"w_bits_data",32},{"w_bits_last",1}};
    for(auto [field,width]:fields) {
      if(bad==2&&slave==10&&std::string(field)=="w_ready")continue;
      bool ready=StringRef(field).ends_with("ready");
      if(bad==3&&slave==7&&std::string(field)=="aw_bits_id")width=11;
      if(bad==4&&slave==7&&std::string(field)=="w_valid")ready=true;
      text+=", "+std::string(ready?"in":"out")+" %ctrl_write_dispatch_slave_"+std::to_string(slave)+"_"+field+": !firrtl.uint<"+std::to_string(width)+">";
    }
  }
  if(bad==5)text+=", in %ctrl_write_dispatch_master_aw_bits_user: !firrtl.uint<1>";
  text+=") {} firrtl.module @GGControlAddressDecode() {} } }";
  auto root=parseSourceString<ModuleOp>(text,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  auto decoder=*std::next(c.getOps<FModuleOp>().begin());
  const char *names[]{"BlockDevBridgeModule_0","FASEDMemoryTimingModel_0","TracerVBridgeModule_0","TSIBridgeModule_0","LoadMemWidget_0","PeekPokeBridgeModule_0","UARTBridgeModule_0","ClockBridgeModule_0","SimulationMaster_0","ResetPulseBridgeModule_0","CPUManagedStreamEngine_0"};
  SmallVector<Attribute> rows;
  for(unsigned i=0;i<(bad==6?10:11);++i)rows.push_back(b.getDictionaryAttr({
    b.getNamedAttr("name",b.getStringAttr(bad==7&&i==7?"WrongWidget":names[i])),
    b.getNamedAttr("slave",b.getI32IntegerAttr(bad==8&&i==7?6:i))}));
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
  SmallVector<Attribute> annos;
  for(auto ref:{"other","clockBridge_ctrl","clockBridge_ctrl.aw.bits.addr","clockBridge_ctrl.w.valid",
                "clockBridge_ctrl.ar.bits.addr","clockBridge_ctrl.b.ready","clockBridge_ctrl.r.bits.data",
                "ctrl_write_dispatch_slave_7_aw_valid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("target",b.getStringAttr("~"+std::string(oldTop)+"|"+oldTop+">"+ref))}));
  if(bad!=9)c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if(bad==10) {
    b.setInsertionPointToEnd(c.getBodyBlock());auto top=*c.getOps<FModuleOp>().begin();
    auto parent=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("Parent"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(parent.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");
  }
  if(bad==11)c.setNameAttr(b.getStringAttr("WrongTop"));
  return root;
}
void test(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::bindControlWidgetWrites(c,error)),error);
  require(succeeded(verify(*root)),"widget write IR invalid");
  FModuleOp top;for(auto m:c.getOps<FModuleOp>())if(m.getName()==newTop)top=m;
  require(bool(top),"wrapper missing");
  // The AR/B/R fields keep their bidirectional contract. AW/W no longer
  // accept unrelated external transactions alongside the internal dispatcher.
  for(auto [slave,port]:slaves) {
    bool found=false;
    for(auto p:top.getPorts()) {
      require(!p.name.getValue().starts_with("ctrl_write_dispatch_slave_"+std::to_string(slave)+"_"),"consumed dispatch port exposed");
      if(p.name==port) {
        auto type=cast<BundleType>(p.type);found=true;
        require(type.getNumElements()==3&&!type.getElement("aw")&&!type.getElement("w")&&
          type.getElement("ar")&&type.getElement("b")->isFlip&&type.getElement("r")->isFlip,"remaining control bundle changed");
      }
    }
    require(found,"remaining control port missing");
  }
  require(top.getNumPorts()==19,"metadata is not shared across the seven widgets");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  unsigned i=0;
  for(auto a:annos) {
    auto target=cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();
    auto suffix=target.drop_front(target.find('>'));
    auto module=(i==0||i==4||i==5||i==6)?newTop:oldTop;
    require(target=="~"+std::string(newTop)+"|"+module+suffix.str(),"annotation target lost during partial bundle transfer");++i;
  }
  // Each newly internalized scalar has exactly one driver. Detect accidental
  // aggregate-plus-leaf drivers on the consumed AW/W interfaces as well.
  std::map<std::string,unsigned> sinks;
  std::function<std::string(Value)> key=[&](Value v)->std::string {
    if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  };
  for(auto x:top.getOps<StrictConnectOp>())require(++sinks[key(x.getDest())]==1,"multiple scalar drivers");
  require(sinks.size()==140,"incomplete widget request/ready binding");
  auto good=dump(*root);require(failed(goldengate::bindControlWidgetWrites(c,error)),"repeat accepted");
  require(dump(*root)==good,"repeat rejection mutated IR");
  for(unsigned bad=1;bad<=11;++bad) {
    auto m=fixture(ctx,bad);auto circuit=*m->getOps<CircuitOp>().begin();auto before=dump(*m);
    require(failed(goldengate::bindControlWidgetWrites(circuit,error)),"malformed contract accepted");
    require(dump(*m)==before,"rejection mutated IR");
  }
  llvm::outs()<<"Widget writes: seven bindings, partial bundle targets, shared metadata, and twelve atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{test(ctx);return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
