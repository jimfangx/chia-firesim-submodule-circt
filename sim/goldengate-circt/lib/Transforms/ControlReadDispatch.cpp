// See LICENSE for license details.
// Port junctions.nasti.scala's AR DecoupledHelper and Widget.scala's slave
// binding with CIRCT FIRRTL operations. Requests and tracker enqueues fire
// together, even under independent slave and response-tracker backpressure.
#include "goldengate/ControlReadDispatch.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addControlReadDispatch(CircuitOp circuit,
                                                std::string &error) {
  constexpr llvm::StringLiteral wrapperName="GGControlReadDispatchWrapper",helperName="GGControlReadDispatch";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!="GGControlWidgetWriteWrapper")return reject("control read dispatch requires the widget write wrapper");
  FModuleOp inner,decoder;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()==wrapperName||m.getModuleName()==helperName)return reject("control read dispatch module exists");
    if(m.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getModuleName()=="GGControlAddressDecode")decoder=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto regions=decoder?decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions"):ArrayAttr();
  auto bindings=inner?inner->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings"):ArrayAttr();
  if(!inner||!raw||!regions||regions.size()!=11||!bindings||bindings.size()!=7)
    return reject("control read dispatch needs retained annotations, regions and seven write bindings");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used)return reject("control read dispatch requires an uninstantiated top");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto bundle=[&](std::initializer_list<BundleType::BundleElement> xs){return BundleType::get(ctx,xs);};
  auto token=[&](FIRRTLBaseType bits){return bundle({{b.getStringAttr("ready"),true,uint(1)},
    {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,bits}});};
  struct Field {const char *name;unsigned width;};
  const Field addressFields[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
    {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
  SmallVector<BundleType::BundleElement> address;
  for(auto f:addressFields)address.push_back({b.getStringAttr(f.name),false,uint(f.width)});
  auto arType=token(BundleType::get(ctx,address));
  auto bType=token(bundle({{b.getStringAttr("resp"),false,uint(2)},
    {b.getStringAttr("id"),false,uint(12)},{b.getStringAttr("user"),false,uint(1)}}));
  auto rType=token(bundle({{b.getStringAttr("resp"),false,uint(2)},
    {b.getStringAttr("data"),false,uint(32)},{b.getStringAttr("last"),false,uint(1)},
    {b.getStringAttr("id"),false,uint(12)},{b.getStringAttr("user"),false,uint(1)}}));
  auto controlType=bundle({{b.getStringAttr("b"),true,bType},
    {b.getStringAttr("ar"),false,arType},{b.getStringAttr("r"),true,rType}});
  auto remainingType=bundle({{b.getStringAttr("b"),true,bType},{b.getStringAttr("r"),true,rType}});
  std::map<std::string,unsigned> old,copied,hi,exposed,widgets;
  std::set<unsigned> allocated;
  for(auto [i,p]:llvm::enumerate(inner.getPorts())) {
    if(p.name.getValue().starts_with("ctrl_read_dispatch_"))return reject("control read dispatch boundary exists");
    old.emplace(p.name.getValue().str(),i);
  }
  struct Required {const char *name;unsigned width;Direction dir;};
  const Required required[]{{"ctrl_decode_ar_route",11,Direction::Out},
    {"ctrl_decode_ar_target",4,Direction::Out},{"ctrl_decode_ar_addr",25,Direction::In},
    {"ctrl_error_ar_ready",1,Direction::Out},{"ctrl_error_ar_valid",1,Direction::In},
    {"ctrl_error_ar_bits_addr",25,Direction::In},{"ctrl_error_ar_bits_len",8,Direction::In},
    {"ctrl_error_ar_bits_id",12,Direction::In}};
  for(auto r:required) {
    auto it=old.find(r.name);
    if(it==old.end()||inner.getPorts()[it->second].type!=uint(r.width)||inner.getPorts()[it->second].direction!=r.dir)
      return reject("control read dispatch requires exact U250 decoder/error boundaries");
  }
  for(auto a:bindings) {
    auto d=dyn_cast<DictionaryAttr>(a);auto name=d?d.getAs<StringAttr>("name"):StringAttr();
    auto port=d?d.getAs<StringAttr>("port"):StringAttr();auto slave=d?d.getAs<IntegerAttr>("slave"):IntegerAttr();
    if(!name||!port||!slave||slave.getInt()<0||slave.getInt()>=11||
       !allocated.insert(slave.getInt()).second||widgets.count(port.getValue().str()))
      return reject("control read dispatch has invalid or duplicate widget bindings");
    auto row=dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
    auto rn=row?row.getAs<StringAttr>("name"):StringAttr();auto ri=row?row.getAs<IntegerAttr>("slave"):IntegerAttr();
    auto it=old.find(port.getValue().str());
    if(!rn||rn!=name||!ri||ri.getInt()!=slave.getInt()||it==old.end()||
       inner.getPorts()[it->second].type!=controlType||inner.getPorts()[it->second].direction!=Direction::In)
      return reject("control read dispatch widget catalog or control bundle differs");
    widgets[port.getValue().str()]=slave.getInt();
  }
  const std::set<std::string> consumed{"ctrl_decode_ar_addr","ctrl_error_ar_valid",
    "ctrl_error_ar_bits_addr","ctrl_error_ar_bits_len","ctrl_error_ar_bits_id"};
  SmallVector<PortInfo> hp;
  auto port=[&](std::string n,Type t,Direction d){hi[n]=hp.size();hp.push_back({b.getStringAttr(n),t,d});};
  port("route",uint(11),Direction::In);port("target",uint(4),Direction::In);
  port("tracker_ready",uint(1),Direction::In);port("master_ar",arType,Direction::In);
  for(unsigned i=0;i<12;++i)port(i==11?"err_slave_ar":"slave_"+std::to_string(i)+"_ar",arType,Direction::Out);
  port("track_valid",uint(1),Direction::Out);port("track_tag",uint(12),Direction::Out);port("track_target",uint(4),Direction::Out);
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())) {
    std::string n=p.name.getValue().str();copied[n]=ports.size();if(widgets.count(n))p.type=remainingType;ports.push_back(p);
  }
  for(auto p:hp) {
    auto n=p.name.getValue().str();bool expose=n=="master_ar"||n=="tracker_ready"||n.rfind("track_",0)==0;
    for(unsigned i=0;i<11;++i)if(!allocated.count(i)&&n=="slave_"+std::to_string(i)+"_ar")expose=true;
    if(expose){exposed[n]=ports.size();p.name=b.getStringAttr("ctrl_read_dispatch_"+n);ports.push_back(p);}
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](llvm::StringRef n){return helper.getBodyBlock()->getArgument(hi.at(n.str()));};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  Value master=arg("master_ar"),valid=field(master,"valid"),bits=field(master,"bits"),route=arg("route");
  Value dispatched=b.create<AndPrimOp>(loc,valid,arg("tracker_ready"));
  Value ready=b.create<ConstantOp>(loc,uint(1),APInt(1,0));
  for(unsigned i=0;i<12;++i) {
    Value slave=arg(i==11?"err_slave_ar":"slave_"+std::to_string(i)+"_ar");
    Value selected=i==11?Value(b.create<EQPrimOp>(loc,route,b.create<ConstantOp>(loc,uint(11),APInt(11,0)))):
      Value(b.create<BitsPrimOp>(loc,route,i,i));
    ready=b.create<MuxPrimOp>(loc,selected,field(slave,"ready"),ready);
    connect(field(slave,"valid"),b.create<AndPrimOp>(loc,dispatched,selected));
    Value slaveBits=field(slave,"bits");
    for(auto f:addressFields)connect(field(slaveBits,f.name),field(bits,f.name));
  }
  // DecoupledHelper excludes the destination's own ready/valid condition:
  // no request reaches a slave without tracker capacity; no tracker entry
  // is offered without a ready slave. Both accept on the same master fire.
  connect(field(master,"ready"),b.create<AndPrimOp>(loc,arg("tracker_ready"),ready));
  connect(arg("track_valid"),b.create<AndPrimOp>(loc,valid,ready));
  connect(arg("track_tag"),field(bits,"id"));connect(arg("track_target"),arg("target"));
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  wrapper->setAttr("goldengate.controlReadBindings",bindings);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),dispatch=b.create<InstanceOp>(loc,helper,"controlReadDispatch");
  auto topArg=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  for(auto [n,i]:copied) {
    Value a=sim.getResult(old.at(n)),z=topArg(i);
    if(widgets.count(n))for(auto ch:{"b","r"})b.create<ConnectOp>(loc,field(z,ch),field(a,ch));
    else {auto d=inner.getPorts()[old.at(n)].direction;b.create<ConnectOp>(loc,d==Direction::In?a:z,d==Direction::In?z:a);}
  }
  connect(dispatch.getResult(hi.at("route")),sim.getResult(old.at("ctrl_decode_ar_route")));
  connect(dispatch.getResult(hi.at("target")),sim.getResult(old.at("ctrl_decode_ar_target")));
  for(auto [n,i]:exposed) {
    auto v=dispatch.getResult(hi.at(n));auto d=hp[hi.at(n)].direction;
    b.create<ConnectOp>(loc,d==Direction::In?v:topArg(i),d==Direction::In?topArg(i):v);
  }
  Value externalMaster=topArg(exposed.at("master_ar"));
  connect(sim.getResult(old.at("ctrl_decode_ar_addr")),field(field(externalMaster,"bits"),"addr"));
  for(auto [n,i]:widgets)b.create<ConnectOp>(loc,field(sim.getResult(old.at(n)),"ar"),dispatch.getResult(hi.at("slave_"+std::to_string(i)+"_ar")));
  Value err=dispatch.getResult(hi.at("err_slave_ar")),errBits=field(err,"bits");
  connect(field(err,"ready"),sim.getResult(old.at("ctrl_error_ar_ready")));
  connect(sim.getResult(old.at("ctrl_error_ar_valid")),field(err,"valid"));
  for(auto n:{"addr","len","id"})connect(sim.getResult(old.at("ctrl_error_ar_bits_"+std::string(n))),field(errBits,n));
  std::string op="~"+circuit.getName().str(),np="~"+wrapperName.str(),mp="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto n=ref.take_front(ref.find_first_of(".[")).str();bool transfer=copied.count(n);
        if(widgets.count(n)){ref=ref.drop_front(n.size());transfer=ref==".b"||ref.starts_with(".b.")||ref==".r"||ref.starts_with(".r.");}
        if(transfer)suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(wrapperName));return success();
}
