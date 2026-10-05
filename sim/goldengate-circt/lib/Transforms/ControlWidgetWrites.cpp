// See LICENSE for license details.
// Widget.scala binds each widget's io.ctrl to its allocated Nasti slave.
// Connect requests and readiness through FIRRTL operations; preserve the
// independent AW/W routes and the MCRFile's independent request capture.
#include "goldengate/ControlWidgetWrites.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

namespace {
struct WidgetPort { const char *widget; const char *port; };
constexpr WidgetPort widgetPorts[]{
  {"TracerVBridgeModule_0", "tracerv_ctrl"},
  {"LoadMemWidget_0", "loadmem_ctrl"},
  {"PeekPokeBridgeModule_0", "peekPokeBridge_ctrl"},
  {"UARTBridgeModule_0", "uartBridge_ctrl"},
  {"ClockBridgeModule_0", "clockBridge_ctrl"},
  {"ResetPulseBridgeModule_0", "resetBridge_ctrl"},
  {"CPUManagedStreamEngine_0", "cpuStream_ctrl"}};
struct Binding { const char *widget; const char *port; unsigned slave; };
struct Field { const char *name; unsigned width; };
constexpr Field addressFields[]{
  {"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
  {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
constexpr Field dataFields[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
bool dispatched(llvm::StringRef channel, llvm::StringRef field) {
  return channel == "aw" ? field == "addr" || field == "len" || field == "id"
                         : field == "data" || field == "last";
}
}

LogicalResult goldengate::bindControlWidgetWrites(CircuitOp circuit,
                                                 std::string &error) {
  constexpr llvm::StringLiteral wrapperName="GGControlWidgetWriteWrapper";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!="GGControlWriteDispatchWrapper")
    return reject("widget writes require the control write dispatch wrapper");
  FModuleOp inner,decoder;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()==wrapperName)return reject("widget write wrapper exists");
    if(m.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getModuleName()=="GGControlAddressDecode")decoder=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto regions=decoder?decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions"):ArrayAttr();
  if(!inner||!raw||!regions||regions.empty()||regions.size()>63)
    return reject("widget writes require retained annotations and 1..63 decoded regions");
  // Widget.scala zips sortedWidgets with the interconnect slaves. Resolve
  // identity through the decoder's allocation rather than assuming the
  // baseline ordering: adding a register bank can shift existing slaves.
  std::map<std::string,unsigned> allocated;
  for(auto [i,row]:llvm::enumerate(regions)) {
    auto region=dyn_cast<DictionaryAttr>(row);
    auto name=region?region.getAs<StringAttr>("name"):StringAttr();
    auto index=region?region.getAs<IntegerAttr>("slave"):IntegerAttr();
    if(!name||name.getValue().empty()||!index||index.getValue().isNegative()||
       index.getValue().getActiveBits()>32||index.getValue().getZExtValue()!=i||
       !allocated.emplace(name.getValue().str(),i).second)
      return reject("widget write allocation needs unique names and ordered slave indices");
  }
  SmallVector<Binding> bindings;
  for(auto widget:widgetPorts) {
    auto found=allocated.find(widget.widget);
    if(found==allocated.end())return reject("widget write allocation is missing an implemented widget");
    bindings.push_back({widget.widget,widget.port,found->second});
  }
  bool instantiated=false;
  circuit.walk([&](InstanceOp i){instantiated|=i.getModuleName()==inner.getName();});
  if(instantiated)return reject("widget writes require an uninstantiated top");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto payload=[&](ArrayRef<Field> fields) {
    SmallVector<BundleType::BundleElement> elems;
    for(auto f:fields)elems.push_back({b.getStringAttr(f.name),false,uint(f.width)});
    return BundleType::get(ctx,elems);
  };
  auto token=[&](FIRRTLBaseType bits) {
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,bits}});
  };
  const Field bfields[]{{"resp",2},{"id",12},{"user",1}};
  const Field rfields[]{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}};
  auto controlType=BundleType::get(ctx,{
    {b.getStringAttr("aw"),false,token(payload(addressFields))},
    {b.getStringAttr("w"),false,token(payload(dataFields))},
    {b.getStringAttr("b"),true,token(payload(bfields))},
    {b.getStringAttr("ar"),false,token(payload(addressFields))},
    {b.getStringAttr("r"),true,token(payload(rfields))}});
  SmallVector<BundleType::BundleElement> remaining;
  for(auto e:controlType.getElements())if(e.name!="aw"&&e.name!="w")remaining.push_back(e);
  auto remainingType=BundleType::get(ctx,remaining);
  std::map<std::string,unsigned> old,copied,metadata;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old.emplace(p.name.getValue().str(),i);
  std::set<std::string> consumed,controls;
  // Complete preflight before adding any operation or changing annotations.
  for(auto binding:bindings) {
    auto found=old.find(binding.port);
    if(found==old.end()||inner.getPorts()[found->second].type!=controlType||
       inner.getPorts()[found->second].direction!=Direction::In)
      return reject("widget writes require exact U250 Nasti control bundles");
    controls.insert(binding.port);
    std::string prefix="ctrl_write_dispatch_slave_"+std::to_string(binding.slave)+"_";
    const Field fields[]{{"aw_ready",1},{"w_ready",1},{"aw_valid",1},
      {"aw_bits_addr",25},{"aw_bits_len",8},{"aw_bits_id",12},
      {"w_valid",1},{"w_bits_data",32},{"w_bits_last",1}};
    for(auto f:fields) {
      auto n=prefix+f.name;auto it=old.find(n);
      auto dir=StringRef(f.name).ends_with("ready")?Direction::In:Direction::Out;
      if(it==old.end()||inner.getPorts()[it->second].type!=uint(f.width)||
         inner.getPorts()[it->second].direction!=dir)
        return reject("widget writes are missing an exact dispatch slave boundary");
      consumed.insert(n);
    }
  }
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())) {
    auto n=p.name.getValue().str();copied[n]=ports.size();
    if(controls.count(n))p.type=remainingType;
    ports.push_back(p);
  }
  for(auto ch:{"aw","w"})for(auto f:ch==StringRef("aw")?ArrayRef<Field>(addressFields):ArrayRef<Field>(dataFields)) {
    if(dispatched(ch,f.name))continue;
    std::string n="ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name;
    if(old.count(n))return reject("widget write metadata boundary already exists");
    metadata[n]=ports.size();ports.push_back({b.getStringAttr(n),uint(f.width),Direction::In});
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  SmallVector<Attribute> catalog;
  for(auto x:bindings)catalog.push_back(b.getDictionaryAttr({
    b.getNamedAttr("name",b.getStringAttr(x.widget)),b.getNamedAttr("port",b.getStringAttr(x.port)),
    b.getNamedAttr("slave",b.getI32IntegerAttr(x.slave))}));
  wrapper->setAttr("goldengate.controlWriteBindings",b.getArrayAttr(catalog));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim");
  auto arg=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  for(auto [n,i]:copied) {
    Value v=sim.getResult(old.at(n)),external=arg(i);
    if(controls.count(n)) {
      for(auto e:remaining) {
        Value a=field(v,e.name),z=field(external,e.name);
        // ConnectOp observes all nested flips, including response ready.
        b.create<ConnectOp>(loc,e.isFlip?z:a,e.isFlip?a:z);
      }
    } else {
      auto dir=inner.getPorts()[old.at(n)].direction;
      b.create<ConnectOp>(loc,dir==Direction::In?v:external,dir==Direction::In?external:v);
    }
  }
  for(auto binding:bindings)for(auto ch:{"aw","w"}) {
    std::string prefix="ctrl_write_dispatch_slave_"+std::to_string(binding.slave)+"_"+ch;
    Value channel=field(sim.getResult(old.at(binding.port)),ch);
    connect(sim.getResult(old.at(prefix+"_ready")),field(channel,"ready"));
    connect(field(channel,"valid"),sim.getResult(old.at(prefix+"_valid")));
    Value bits=field(channel,"bits");
    for(auto f:ch==StringRef("aw")?ArrayRef<Field>(addressFields):ArrayRef<Field>(dataFields)) {
      Value source=dispatched(ch,f.name)?sim.getResult(old.at(prefix+"_bits_"+f.name)):
        arg(metadata.at("ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name));
      connect(field(bits,f.name),source);
    }
  }
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),mp="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return a;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto port=ref.take_front(ref.find_first_of(".[")).str();
        bool transfer=copied.count(port);
        if(controls.count(port)) {
          ref=ref.drop_front(port.size());
          transfer=ref.starts_with(".ar.")||ref==".ar"||ref.starts_with(".b.")||ref==".b"||ref.starts_with(".r.")||ref==".r";
        }
        // Whole control-bundle and consumed AW/W targets still resolve in
        // the inner module. Only surviving AR/B/R fields move to the new top.
        if(transfer)suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto arr=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> xs;for(auto x:arr)xs.push_back(retarget(x));return b.getArrayAttr(xs);}
    if(auto dict=dyn_cast<DictionaryAttr>(a)){NamedAttrList xs;for(auto x:dict)xs.set(x.getName(),retarget(x.getValue()));return xs.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(wrapperName));return success();
}
