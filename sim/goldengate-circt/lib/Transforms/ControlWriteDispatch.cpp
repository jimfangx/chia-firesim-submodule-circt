// See LICENSE for license details.
// Port NastiRouter's AW/W dispatch as FIRRTL mux, bit-select and connect ops.
#include "goldengate/ControlWriteDispatch.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addControlWriteDispatch(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName="GGControlWriteDispatchWrapper",helperName="GGControlWriteDispatch";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!="GGControlWriteRouteWrapper")return reject("control write dispatch requires the write route wrapper");
  FModuleOp inner,decoder;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()==wrapperName||m.getModuleName()==helperName)return reject("control write dispatch helper or wrapper exists");
    if(m.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getModuleName()=="GGControlAddressDecode")decoder=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto catalog=decoder?decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions"):ArrayAttr();
  if(!inner||!raw||!catalog||catalog.size()!=11)return reject("control write dispatch needs retained annotations and eleven decoded regions");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  struct Required {const char *name;unsigned width;Direction direction;};
  const Required required[]{
    {"ctrl_decode_aw_route",11,Direction::Out},{"ctrl_decode_aw_addr",25,Direction::In},
    {"ctrl_write_route_w_route",11,Direction::Out},{"ctrl_write_route_aw_slave_valid",1,Direction::Out},
    {"ctrl_write_route_w_slave_valid",1,Direction::Out},{"ctrl_write_route_w_last",1,Direction::In},
    {"ctrl_write_route_aw_slave_ready",1,Direction::In},{"ctrl_write_route_w_slave_ready",1,Direction::In},
    {"ctrl_error_aw_ready",1,Direction::Out},{"ctrl_error_w_ready",1,Direction::Out},
    {"ctrl_error_aw_valid",1,Direction::In},{"ctrl_error_aw_bits_addr",25,Direction::In},
    {"ctrl_error_aw_bits_id",12,Direction::In},{"ctrl_error_w_valid",1,Direction::In},
    {"ctrl_error_w_bits_last",1,Direction::In}};
  std::map<std::string,unsigned> old;
  for(auto [i,p]:llvm::enumerate(inner.getPorts())) {
    if(p.name.getValue().starts_with("ctrl_write_dispatch_"))return reject("control write dispatch boundary exists");
    old.emplace(p.name.getValue().str(),i);
  }
  for(auto r:required) {
    auto found=old.find(r.name);if(found==old.end())return reject("control write dispatch is missing a route/error boundary");
    auto p=inner.getPorts()[found->second];
    if(p.type!=uint(r.width)||p.direction!=r.direction)return reject("control write dispatch requires exact U250 boundary types and directions");
  }
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used)return reject("control write dispatch requires an uninstantiated top");
  const std::set<std::string> consumed{"ctrl_write_route_aw_slave_ready","ctrl_write_route_w_slave_ready",
    "ctrl_error_aw_valid","ctrl_error_aw_bits_addr","ctrl_error_aw_bits_id","ctrl_error_w_valid","ctrl_error_w_bits_last"};
  SmallVector<PortInfo> hp;std::map<std::string,unsigned> hi;
  auto port=[&](std::string name,unsigned w,Direction d){hi[name]=hp.size();hp.push_back({b.getStringAttr(name),uint(w),d});};
  for(auto n:{"aw_route","w_route"})port(n,11,Direction::In);
  for(auto n:{"aw_valid","w_valid"})port(n,1,Direction::In);
  port("master_aw_bits_addr",25,Direction::In);port("master_aw_bits_len",8,Direction::In);port("master_aw_bits_id",12,Direction::In);
  port("master_w_bits_data",32,Direction::In);port("master_w_bits_last",1,Direction::In);
  for(unsigned i=0;i<12;++i)for(auto ch:{"aw","w"})port((i==11?"err_slave":"slave_"+std::to_string(i))+std::string("_")+ch+"_ready",1,Direction::In);
  for(auto n:{"aw_ready","w_ready"})port(n,1,Direction::Out);
  for(unsigned i=0;i<12;++i) {
    std::string prefix=(i==11?"err_slave":"slave_"+std::to_string(i))+std::string("_");
    port(prefix+"aw_valid",1,Direction::Out);port(prefix+"aw_bits_addr",25,Direction::Out);
    port(prefix+"aw_bits_len",8,Direction::Out);port(prefix+"aw_bits_id",12,Direction::Out);
    port(prefix+"w_valid",1,Direction::Out);port(prefix+"w_bits_data",32,Direction::Out);port(prefix+"w_bits_last",1,Direction::Out);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  helper->setAttr("goldengate.controlRegions",catalog);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](const std::string &n){return helper.getBodyBlock()->getArgument(hi.at(n));};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  Value zero=b.create<ConstantOp>(loc,uint(1),APInt(1,0));
  for(auto ch:{"aw","w"}) {
    Value route=arg(std::string(ch)+"_route"),valid=arg(std::string(ch)+"_valid"),ready=zero;
    // Chisel's ordered when assignments give the highest numbered matching
    // slave priority. Preserve that behavior even for malformed multi-bit routes.
    for(unsigned i=0;i<12;++i) {
      std::string prefix=(i==11?"err_slave":"slave_"+std::to_string(i))+std::string("_")+ch;
      Value selected=i==11?Value(b.create<EQPrimOp>(loc,route,b.create<ConstantOp>(loc,uint(11),APInt(11,0)))):Value(b.create<BitsPrimOp>(loc,route,i,i));
      ready=b.create<MuxPrimOp>(loc,selected,arg(prefix+"_ready"),ready);
      connect(arg(prefix+"_valid"),b.create<AndPrimOp>(loc,valid,selected));
      for(auto field:{"addr","len","id","data","last"})
        if ((std::string(ch)=="aw") == (std::string(field)!="data" && std::string(field)!="last"))
          connect(arg(prefix+"_bits_"+field),arg(std::string("master_")+ch+"_bits_"+field));
    }
    connect(arg(std::string(ch)+"_ready"),ready);
  }
  SmallVector<PortInfo> ports;std::map<std::string,unsigned> copied,external;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())) {
    copied[p.name.getValue().str()]=ports.size();ports.push_back(p);
  }
  const std::set<std::string> exposedInputs{"master_aw_bits_len","master_aw_bits_id","master_w_bits_data"};
  for(auto p:hp) {
    auto n=p.name.getValue().str();
    if(exposedInputs.count(n)||n.rfind("slave_",0)==0) {
      external[n]=ports.size();p.name=b.getStringAttr("ctrl_write_dispatch_"+n);ports.push_back(p);
    }
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),dispatch=b.create<InstanceOp>(loc,helper,"controlWriteDispatch");
  auto topArg=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  for(auto [n,index]:copied) {
    Value v=sim.getResult(old.at(n)),outside=topArg(index);
    if(inner.getPorts()[old.at(n)].direction==Direction::In)b.create<ConnectOp>(loc,v,outside);
    else b.create<ConnectOp>(loc,outside,v);
  }
  const std::pair<const char*,const char*> live[]{
    {"aw_route","ctrl_decode_aw_route"},{"w_route","ctrl_write_route_w_route"},
    {"aw_valid","ctrl_write_route_aw_slave_valid"},{"w_valid","ctrl_write_route_w_slave_valid"},
    {"err_slave_aw_ready","ctrl_error_aw_ready"},{"err_slave_w_ready","ctrl_error_w_ready"}};
  for(auto [h,n]:live)connect(dispatch.getResult(hi.at(h)),sim.getResult(old.at(n)));
  connect(dispatch.getResult(hi.at("master_aw_bits_addr")),topArg(copied.at("ctrl_decode_aw_addr")));
  connect(dispatch.getResult(hi.at("master_w_bits_last")),topArg(copied.at("ctrl_write_route_w_last")));
  const std::pair<const char*,const char*> sinks[]{
    {"aw_ready","ctrl_write_route_aw_slave_ready"},{"w_ready","ctrl_write_route_w_slave_ready"},
    {"err_slave_aw_valid","ctrl_error_aw_valid"},{"err_slave_aw_bits_addr","ctrl_error_aw_bits_addr"},
    {"err_slave_aw_bits_id","ctrl_error_aw_bits_id"},{"err_slave_w_valid","ctrl_error_w_valid"},
    {"err_slave_w_bits_last","ctrl_error_w_bits_last"}};
  for(auto [h,n]:sinks)connect(sim.getResult(old.at(n)),dispatch.getResult(hi.at(h)));
  for(auto [n,index]:external) {
    Value v=dispatch.getResult(hi.at(n)),outside=topArg(index);
    if(hp[hi.at(n)].direction==Direction::In)connect(v,outside);else connect(outside,v);
  }
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),mp="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return a;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(mp)&&copied.count(ref.take_front(ref.find_first_of(".[")).str()))suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto arr=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> values;for(auto x:arr)values.push_back(retarget(x));return b.getArrayAttr(values);}
    if(auto dict=dyn_cast<DictionaryAttr>(a)){NamedAttrList values;for(auto x:dict)values.set(x.getName(),retarget(x.getValue()));return values.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(wrapperName));return success();
}
