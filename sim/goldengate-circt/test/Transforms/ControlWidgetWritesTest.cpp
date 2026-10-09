// See LICENSE for license details.
#include "goldengate/ControlWidgetWrites.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,llvm::StringRef s){if(!ok)throw std::runtime_error(s.str());}
std::string dump(ModuleOp m){std::string s;llvm::raw_string_ostream out(s);m.print(out);return s;}
constexpr const char *oldTop="GGControlWriteDispatchWrapper",*newTop="GGControlWidgetWriteWrapper";
const std::pair<unsigned,const char*> slaves[]{{2,"tracerv_ctrl"},{4,"loadmem_ctrl"},
  {5,"peekPokeBridge_ctrl"},{6,"uartBridge_ctrl"},{7,"clockBridge_ctrl"},
  {9,"resetBridge_ctrl"},{10,"cpuStream_ctrl"}};
std::vector<std::string> allocation(unsigned layout) {
  std::vector<std::string> names{"BlockDevBridgeModule_0","FASEDMemoryTimingModel_0",
    "TracerVBridgeModule_0","TSIBridgeModule_0","LoadMemWidget_0","PeekPokeBridgeModule_0",
    "UARTBridgeModule_0","ClockBridgeModule_0","SimulationMaster_0",
    "ResetPulseBridgeModule_0","CPUManagedStreamEngine_0"};
  if(layout==1)names.insert(names.begin()+5,{"PrintBridgeModule_0","PrintBridgeModule_1"});
  if(layout==2)std::reverse(names.begin(),names.end());
  return names;
}
unsigned allocatedSlave(unsigned baseline,unsigned layout) {
  auto names=allocation(layout), original=allocation(0);
  return std::find(names.begin(),names.end(),original[baseline])-names.begin();
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0,unsigned layout=0) {
  std::string address="bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>";
  std::string data="bundle<data: uint<32>, last: uint<1>, id: uint<12>, strb: uint<4>, user: uint<1>>";
  auto token=[](std::string bits){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: "+bits+">";};
  std::string control="!firrtl.bundle<aw: "+token(address)+", w: "+token(data)+", b flip: "+token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>")+", ar: "+token(address)+", r flip: "+token("bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>")+">";
  std::string text="module { firrtl.circuit \""+std::string(oldTop)+"\" { firrtl.module @"+oldTop+"(in %other: !firrtl.uint<8>";
  for(auto [slave,port]:slaves) {
    text+=", "+std::string(bad==1&&slave==7?"out":"in")+" %"+port+": "+control;
  }
  if(layout==1)for(auto port:{"print0_ctrl","print1_ctrl"})text+=", in %"+std::string(port)+": "+control;
  auto names=allocation(layout);
  for(unsigned slave=0;slave<names.size();++slave) {
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
  SmallVector<Attribute> rows;
  for(unsigned i=0;i<(bad==6?10:names.size());++i) {
    auto name=names[i];
    if(bad==7&&i==7)name="WrongWidget";
    if(bad==12&&i==3)name=names[0]; // Duplicate an unbound widget, too.
    if(bad==13&&i==3)name="";
    NamedAttrList row;
    if(!(bad==16&&i==3))row.set("name",b.getStringAttr(name));
    if(!(bad==17&&i==3))row.set("slave",bad==18&&i==3?
      b.getIntegerAttr(b.getIntegerType(128),APInt(128,1).shl(100)):
      b.getI32IntegerAttr(bad==14&&i==3?-1:bad==8&&i==7?6:i));
    rows.push_back(bad==15&&i==3?Attribute(b.getStringAttr("bad")):row.getDictionary(&ctx));
  }
  if(bad==19)rows.clear();
  if(bad==20)while(rows.size()<64)rows.push_back(rows[0]);
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
  SmallVector<Attribute> annos;
  for(auto ref:{"other","clockBridge_ctrl","clockBridge_ctrl.aw.bits.addr","clockBridge_ctrl.w.valid",
                "clockBridge_ctrl.ar.bits.addr","clockBridge_ctrl.b.ready","clockBridge_ctrl.r.bits.data",
                "ctrl_write_dispatch_slave_7_aw_valid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("target",b.getStringAttr("~"+std::string(oldTop)+"|"+oldTop+">"+ref))}));
  auto dispatchTarget=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
    b.getNamedAttr("target",b.getStringAttr("~"+std::string(oldTop)+"|"+oldTop+
      ">ctrl_write_dispatch_slave_"+std::to_string(allocatedSlave(7,layout))+"_aw_valid"))});
  annos.back()=dispatchTarget;
  if(bad!=9)c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if(bad==10) {
    b.setInsertionPointToEnd(c.getBodyBlock());auto top=*c.getOps<FModuleOp>().begin();
    auto parent=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("Parent"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(parent.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");
  }
  if(bad==11)c.setNameAttr(b.getStringAttr("WrongTop"));
  return root;
}
void test(MLIRContext &ctx,unsigned layout) {
  auto root=fixture(ctx,0,layout);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  std::vector<std::string> originalTargets;
  for(auto a:c->getAttrOfType<ArrayAttr>("rawAnnotations"))
    originalTargets.push_back(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().str());
  require(succeeded(layout ? goldengate::bindControlWidgetWrites(c,error) :
                            goldengate::bindRocketControlWidgetWrites(c,error)),error);
  require(succeeded(verify(*root)),"widget write IR invalid");
  FModuleOp top;for(auto m:c.getOps<FModuleOp>())if(m.getName()==newTop)top=m;
  require(bool(top),"wrapper missing");
  // The AR/B/R fields keep their bidirectional contract. AW/W no longer
  // accept unrelated external transactions alongside the internal dispatcher.
  for(auto [baseline,port]:slaves) {
    unsigned slave=allocatedSlave(baseline,layout);
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
  require(top.getNumPorts()==(layout==1?75:55),"shared metadata or unbound ports changed");
  auto bindings=top->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
  require(bindings&&bindings.size()==7,"binding catalog missing");
  auto names=allocation(layout);
  for(auto [i,entry]:llvm::enumerate(bindings)) {
    auto row=cast<DictionaryAttr>(entry);
    unsigned slave=allocatedSlave(slaves[i].first,layout);
    require(row.getAs<StringAttr>("name").getValue()==names[slave]&&
      row.getAs<StringAttr>("port").getValue()==slaves[i].second&&
      row.getAs<IntegerAttr>("slave").getInt()==slave,"binding catalog lost allocated identity");
  }
  // Unimplemented widgets retain their full dispatch boundary. Adding Print
  // allocations must not accidentally consume their requests or readiness.
  for(unsigned slave=0;slave<names.size();++slave) {
    bool bound=false;for(auto [baseline,port]:slaves)bound|=slave==allocatedSlave(baseline,layout);
    if(bound)continue;
    unsigned count=0;for(auto p:top.getPorts())
      count+=p.name.getValue().starts_with("ctrl_write_dispatch_slave_"+std::to_string(slave)+"_");
    require(count==9,"unbound dispatch lane lost");
  }
  if(layout==1)for(auto name:{"print0_ctrl","print1_ctrl"}) {
    bool full=false;for(auto p:top.getPorts())if(p.name==name)
      full=cast<BundleType>(p.type).getNumElements()==5;
    require(full,"unbound Print control bundle changed");
  }
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  unsigned i=0;
  for(auto a:annos) {
    auto target=cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();
    auto original=StringRef(originalTargets[i]);
    auto suffix=original.drop_front(original.find('>'));
    auto module=(i==0||i==4||i==5||i==6)?newTop:oldTop;
    require(target=="~"+std::string(newTop)+"|"+module+suffix.str(),"annotation target lost during partial bundle transfer");++i;
  }
  // Each newly internalized scalar has exactly one driver. Detect accidental
  // aggregate-plus-leaf drivers on the consumed AW/W interfaces as well.
  std::map<std::string,unsigned> sinks;
  std::function<std::string(Value)> key=[&](Value v)->std::string {
    if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
    if(auto instance=v.getDefiningOp<InstanceOp>())
      return "sim."+instance.getPortNameStr(cast<OpResult>(v).getResultNumber()).str();
    if(auto argument=dyn_cast<BlockArgument>(v))
      return "top."+top.getPorts()[argument.getArgNumber()].name.getValue().str();
    throw std::runtime_error("unexpected binding value");
  };
  std::set<std::pair<std::string,std::string>> copiedConnections;
  for(auto x:top.getOps<ConnectOp>())copiedConnections.emplace(key(x.getDest()),key(x.getSrc()));
  for(auto p:top.getPorts())if(p.name.getValue().starts_with("ctrl_write_dispatch_slave_")) {
    auto name=p.name.getValue().str();
    auto dest=(p.direction==Direction::In?"sim.":"top.")+name;
    auto source=(p.direction==Direction::In?"top.":"sim.")+name;
    require(copiedConnections.count({dest,source}),"unbound dispatch connection changed");
  }
  std::map<std::string,std::string> actual,expected;
  for(auto x:top.getOps<StrictConnectOp>()) {
    require(++sinks[key(x.getDest())]==1,"multiple scalar drivers");
    actual.emplace(key(x.getDest()),key(x.getSrc()));
  }
  for(auto [baseline,port]:slaves)for(auto channel:{"aw","w"}) {
    std::string prefix="sim.ctrl_write_dispatch_slave_"+std::to_string(allocatedSlave(baseline,layout))+"_"+channel;
    std::string widget="sim."+std::string(port)+"."+channel;
    expected[prefix+"_ready"]=widget+".ready";
    expected[widget+".valid"]=prefix+"_valid";
    const char *address[]{"addr","len","size","burst","lock","cache","prot","qos","region","id","user"};
    const char *data[]{"data","last","id","strb","user"};
    for(auto field:std::string(channel)=="aw"?ArrayRef<const char*>(address):ArrayRef<const char*>(data)) {
      bool dispatched=std::string(channel)=="aw"?
        std::string(field)=="addr"||std::string(field)=="len"||std::string(field)=="id":
        std::string(field)=="data"||std::string(field)=="last";
      expected[widget+".bits."+field]=dispatched?prefix+"_bits_"+field:
        "top.ctrl_write_dispatch_master_"+std::string(channel)+"_bits_"+field;
    }
  }
  require(actual==expected&&actual.size()==140,"widget requests/readiness wired to incorrect allocation");
  auto good=dump(*root);require(failed(goldengate::bindControlWidgetWrites(c,error)),"repeat accepted");
  require(dump(*root)==good,"repeat rejection mutated IR");
  if(layout)return;
  for(unsigned bad=1;bad<=20;++bad) {
    auto m=fixture(ctx,bad);auto circuit=*m->getOps<CircuitOp>().begin();auto before=dump(*m);
    require(failed(goldengate::bindControlWidgetWrites(circuit,error)),"malformed contract accepted");
    require(dump(*m)==before,"rejection mutated IR");
  }
}
void explicitCatalogRejections(MLIRContext &ctx) {
  {
    auto root = fixture(ctx, 0, 1); auto c = *root->getOps<CircuitOp>().begin();
    std::string error; auto before = dump(*root);
    require(failed(goldengate::bindRocketControlWidgetWrites(c, error)) && !error.empty() &&
        dump(*root) == before, "Rocket request binding accepted allocated but uninstantiated Print banks");
  }
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(ctx, 0, 1); auto c = *root->getOps<CircuitOp>().begin();
    SmallVector<goldengate::ControlWidgetPort> widgets{{"PrintBridgeModule_0", "print0_ctrl"},
                                                     {"PrintBridgeModule_1", "print1_ctrl"}};
    if (bad == 0) widgets.clear();
    if (bad == 1) widgets[1].widget = widgets[0].widget;
    if (bad == 2) widgets[1].port = widgets[0].port;
    if (bad == 3) widgets[1].widget.clear();
    if (bad == 4) widgets[1].port.clear();
    if (bad == 5) widgets[1].widget = "unallocated";
    std::string error; auto before = dump(*root);
    require(failed(goldengate::bindControlWidgetWrites(c, widgets, error)) && !error.empty() &&
        dump(*root) == before, "explicit widget binding rejection must preserve IR");
  }
}
}
int main() {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{for(unsigned layout=0;layout<3;++layout)test(ctx,layout); explicitCatalogRejections(ctx);
    llvm::outs()<<"Widget writes: 420 exact scalar bindings across baseline, two added Print banks, and reversed allocations; partial targets and 30 atomic rejections passed\n";
    return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
