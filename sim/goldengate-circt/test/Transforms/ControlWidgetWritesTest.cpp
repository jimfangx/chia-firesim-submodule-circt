// See LICENSE for license details.
#include "goldengate/ControlWidgetWrites.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/IRMapping.h"
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
  if(layout==3)names.insert(names.begin()+7,"SimpleNICBridgeModule_0");
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
  if(layout==3)text+=", in %nicBridge_ctrl: "+control;
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
OwningOpRef<ModuleOp> nicFixture(MLIRContext &ctx) {
  auto root=fixture(ctx,0,3);auto c=*root->getOps<CircuitOp>().begin();
  auto top=*c.getOps<FModuleOp>().begin();OpBuilder b(&ctx);auto loc=c.getLoc();
  unsigned port=0;while(top.getPortName(port)!="nicBridge_ctrl")++port;
  auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};
  auto token=BundleType::get(&ctx,{{b.getStringAttr("ready"),true,uint(1)},
    {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,uint(32)}});
  auto lanes=FVectorType::get(token,6);
  auto mcr=BundleType::get(&ctx,{{b.getStringAttr("read"),false,lanes},
    {b.getStringAttr("write"),true,lanes},{b.getStringAttr("wstrb"),true,uint(4)}});
  b.setInsertionPointToEnd(c.getBodyBlock());
  SmallVector<PortInfo> adapterPorts{{b.getStringAttr("clock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},
    {b.getStringAttr("nasti"),top.getPortType(port),Direction::In},
    {b.getStringAttr("mcr"),mcr,Direction::In}};
  auto adapter=b.create<FModuleOp>(loc,b.getStringAttr("GGNICMCRFile"),top.getConventionAttr(),adapterPorts);
  auto bank=b.create<FModuleOp>(loc,b.getStringAttr("GGNICMMIOBank"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  SmallVector<Attribute> rows;
  for(unsigned i=0;i<6;++i)rows.push_back(b.getDictionaryAttr({
    b.getNamedAttr("name",b.getStringAttr("register"+std::to_string(i))),
    b.getNamedAttr("offset",b.getI32IntegerAttr(i*4)),
    b.getNamedAttr("readable",b.getBoolAttr(i==5)),
    b.getNamedAttr("writeable",b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
  SmallVector<PortInfo> wrapperPorts{{b.getStringAttr("nicBridge_ctrl"),top.getPortType(port),Direction::In}};
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr("GGNICBridgeControlWrapper"),top.getConventionAttr(),wrapperPorts);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto cr=b.create<InstanceOp>(loc,adapter,"crFile");
  b.create<ConnectOp>(loc,cr.getResult(2),wrapper.getArgument(0));
  b.setInsertionPointToStart(top.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,wrapper,"sim");
  b.create<ConnectOp>(loc,sim.getResult(0),top.getArgument(port));
  return root;
}
void nicTest(MLIRContext &ctx) {
  for(unsigned bad=0;bad<=10;++bad) {
    auto root=nicFixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    FModuleOp top,decoder,adapter,wrapper,bank;
    for(auto m:c.getOps<FModuleOp>()) {
      if(m.getName()==oldTop)top=m;
      if(m.getName()=="GGControlAddressDecode")decoder=m;
      if(m.getName()=="GGNICMCRFile")adapter=m;
      if(m.getName()=="GGNICBridgeControlWrapper")wrapper=m;
      if(m.getName()=="GGNICMMIOBank")bank=m;
    }
    OpBuilder b(&ctx);
    if(bad==1)(*top.getOps<ConnectOp>().begin()).erase();
    if(bad==2)(*wrapper.getOps<ConnectOp>().begin()).erase();
    if(bad==3)(*wrapper.getOps<InstanceOp>().begin()).setName("otherBank");
    if(bad==4) {
      auto connect=*top.getOps<ConnectOp>().begin();b.setInsertionPointAfter(connect);
      b.create<ConnectOp>(connect.getLoc(),connect.getDest(),connect.getSrc());
    }
    if(bad==5)bank->removeAttr("goldengate.mmioRegisters");
    if(bad==6) {
      SmallVector<Attribute> rows(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters").begin(),bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters").end());
      rows.pop_back();bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
    }
    if(bad==7) {SmallVector<bool> ds(adapter.getPortDirections().begin(),adapter.getPortDirections().end());ds[2]=true;adapter.setPortDirections(ds);}
    if(bad==8) {SmallVector<bool> ds(adapter.getPortDirections().begin(),adapter.getPortDirections().end());ds[3]=true;adapter.setPortDirections(ds);}
    if(bad==9||bad==10) {
      SmallVector<Attribute> rows(decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions").begin(),decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions").end());
      NamedAttrList row(cast<DictionaryAttr>(rows[7]));row.set("name",b.getStringAttr(bad==9?"SimpleNICBridgeModule_1":"otherWidget"));rows[7]=row.getDictionary(&ctx);
      decoder->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
    }
    auto before=dump(*root);std::string error;
    auto result=goldengate::bindRocketControlWidgetWrites(c,error);
    if(bad) {
      require(failed(result)&&!error.empty()&&dump(*root)==before,"NIC identity rejection mutated or accepted IR");
      continue;
    }
    require(succeeded(result),error);require(succeeded(verify(*root)),"NIC write binding IR invalid");
    for(auto m:c.getOps<FModuleOp>())if(m.getName()==newTop) {
      auto rows=m->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
      require(rows.size()==8,"NIC write bank not included");
      auto nic=cast<DictionaryAttr>(rows[7]);
      require(nic.getAs<StringAttr>("name").getValue()=="SimpleNICBridgeModule_0"&&
        nic.getAs<StringAttr>("port").getValue()=="nicBridge_ctrl"&&
        nic.getAs<IntegerAttr>("slave").getInt()==7,"NIC allocated slave differs");
      unsigned connections=std::distance(m.getOps<StrictConnectOp>().begin(),m.getOps<StrictConnectOp>().end());
      require(connections==160,"eight banks require 160 scalar request/ready bindings");
      for(auto p:m.getPorts())if(p.name=="nicBridge_ctrl")
        require(cast<BundleType>(p.type).getNumElements()==3,"NIC AW/W remains external");
    }
  }
}
// Isolate the production NIC binding operations, adapter and registers. The
// accepted dispatch signals are test inputs; no request/ready expression is
// reconstructed. AR/B/R remain the same explicit external boundary as in IR.
void exportNICBinding(CircuitOp production, StringRef path) {
  FModuleOp bound,inner,bank,adapter;
  for(auto m:production.getOps<FModuleOp>()) {
    if(m.getName()==newTop)bound=m;
    if(m.getName()==oldTop)inner=m;
    if(m.getName()=="GGNICMMIOBank")bank=m;
    if(m.getName()=="GGNICMCRFile")adapter=m;
  }
  require(bound&&inner&&bank&&adapter,"missing production NIC binding modules");
  require(bank.getNumPorts()==11&&adapter.getNumPorts()==4,"NIC bank shape changed");
  auto rows=bound->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
  unsigned slave=63;
  for(auto attr:rows) {
    auto row=cast<DictionaryAttr>(attr);
    if(row.getAs<StringAttr>("name").getValue()=="SimpleNICBridgeModule_0")
      slave=row.getAs<IntegerAttr>("slave").getInt();
  }
  require(slave<63,"NIC binding identity missing");
  std::string prefix="ctrl_write_dispatch_slave_"+std::to_string(slave)+"_";
  OpBuilder b(production.getContext());auto loc=production.getLoc();
  auto root=ModuleOp::create(loc);b.setInsertionPointToEnd(root.getBody());
  auto c=b.create<CircuitOp>(loc,b.getStringAttr("GGNICControlWriteBindingTest"));
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto nativeBank=cast<FModuleOp>(b.insert(bank->clone()));
  auto nativeAdapter=cast<FModuleOp>(b.insert(adapter->clone()));
  auto ports=bank.getPorts();ports.pop_back();
  ports[0].name=b.getStringAttr("hostClock");ports[1].name=b.getStringAttr("hostReset");
  std::map<unsigned,unsigned> copied;
  for(unsigned p=0;p<bound.getNumPorts();++p) {
    auto name=bound.getPortName(p);
    bool metadata=name.starts_with("ctrl_write_dispatch_master_")&&
      name!="ctrl_write_dispatch_master_aw_bits_len"&&
      name!="ctrl_write_dispatch_master_aw_bits_id"&&
      name!="ctrl_write_dispatch_master_w_bits_data";
    if(name=="nicBridge_ctrl"||metadata) {
      copied[p]=ports.size();ports.push_back(bound.getPorts()[p]);
    }
  }
  std::map<unsigned,unsigned> dispatch;
  for(unsigned p=0;p<inner.getNumPorts();++p)if(inner.getPortName(p).starts_with(prefix)) {
    auto pin=inner.getPorts()[p];pin.direction=pin.direction==Direction::In?Direction::Out:Direction::In;
    dispatch[p]=ports.size();ports.push_back(pin);
  }
  require(copied.size()==12&&dispatch.size()==9,"NIC slice lost shared metadata or dispatch fields");
  auto top=b.create<FModuleOp>(loc,b.getStringAttr("GGNICControlWriteBindingTest"),bank.getConventionAttr(),ports);
  b.setInsertionPointToStart(top.getBodyBlock());
  auto regs=b.create<InstanceOp>(loc,nativeBank,"nicRegisters");
  auto cr=b.create<InstanceOp>(loc,nativeAdapter,"crFile");
  for(unsigned p=0;p<10;++p) {
    bool input=ports[p].direction==Direction::In;
    b.create<StrictConnectOp>(loc,input?regs.getResult(p):top.getArgument(p),input?top.getArgument(p):regs.getResult(p));
  }
  b.create<StrictConnectOp>(loc,cr.getResult(0),top.getArgument(0));
  b.create<StrictConnectOp>(loc,cr.getResult(1),top.getArgument(1));
  b.create<ConnectOp>(loc,cr.getResult(3),regs.getResult(10));
  InstanceOp sim;for(auto i:bound.getOps<InstanceOp>())if(i.getName()=="sim")sim=i;
  require(bool(sim),"production binding instance missing");
  IRMapping mapping;
  for(auto [p,index]:copied)mapping.map(bound.getArgument(p),top.getArgument(index));
  for(auto [p,index]:dispatch)mapping.map(sim.getResult(p),top.getArgument(index));
  for(unsigned p=0;p<inner.getNumPorts();++p)if(inner.getPortName(p)=="nicBridge_ctrl")mapping.map(sim.getResult(p),cr.getResult(2));
  std::function<Value(Value)> copy=[&](Value v)->Value {
    if(auto mapped=mapping.lookupOrNull(v))return mapped;
    auto op=v.getDefiningOp<SubfieldOp>();require(bool(op),"NIC binding slice has an unexpected dependency");
    copy(op.getInput());b.clone(*op.getOperation(),mapping);return mapping.lookup(v);
  };
  std::function<bool(Value)> selected=[&](Value v)->bool {
    if(auto op=v.getDefiningOp<SubfieldOp>())return selected(op.getInput());
    if(auto result=dyn_cast<OpResult>(v))return result.getOwner()==sim.getOperation()&&
      (sim.getPortNameStr(result.getResultNumber())=="nicBridge_ctrl"||
       sim.getPortNameStr(result.getResultNumber()).starts_with(prefix));
    return false;
  };
  unsigned scalar=0,aggregate=0;
  for(auto &op:*bound.getBodyBlock()) {
    Value dest,src;
    if(auto x=dyn_cast<ConnectOp>(op)){dest=x.getDest();src=x.getSrc();}
    if(auto x=dyn_cast<StrictConnectOp>(op)){dest=x.getDest();src=x.getSrc();}
    if(!dest||(!selected(dest)&&!selected(src)))continue;
    copy(dest);copy(src);b.clone(op,mapping);
    scalar+=isa<StrictConnectOp>(op);aggregate+=isa<ConnectOp>(op);
  }
  require(scalar==20&&aggregate==3,"NIC binding slice is incomplete");
  require(succeeded(verify(root)),"isolated production NIC binding invalid");
  std::error_code ec;llvm::raw_fd_ostream out(path,ec);require(!ec,"cannot export binding");root.print(out);out<<'\n';
  root.erase();
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
int main(int argc,char **argv) {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{
    if(argc==3||argc==4) {
      auto root=parseSourceFile<ModuleOp>(argv[1],&ctx);require(bool(root),"production parse failed");
      require(succeeded(verify(*root)),"production input invalid");
      auto c=*root->getOps<CircuitOp>().begin();std::string error;
      auto result=goldengate::bindRocketControlWidgetWrites(c,error);require(succeeded(result),error);
      require(succeeded(verify(*root)),"production write binding invalid");
      std::error_code ec;llvm::raw_fd_ostream out(argv[2],ec);require(!ec,"cannot write production binding");
      root->print(out);out<<'\n';
      if(argc==4)exportNICBinding(c,argv[3]);
      llvm::outs()<<"Production NIC write binding verified\n";return 0;
    }
    for(unsigned layout=0;layout<3;++layout)test(ctx,layout); explicitCatalogRejections(ctx);nicTest(ctx);
    llvm::outs()<<"Widget writes: 420 exact scalar bindings plus 160 NIC bindings; partial targets and 40 atomic rejections passed\n";
    return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
