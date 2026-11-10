// See LICENSE for license details.
// Oracle: FPGATop.HostPortIOConnectChannels2Port. CSV only sinks wire tokens.
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <functional>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
namespace {
bool textIs(DictionaryAttr dict, StringRef field, StringRef expected) {
  auto value=dict?dict.getAs<StringAttr>(field):StringAttr();
  return value && value.getValue()==expected;
}
}
LogicalResult goldengate::bindAutoCounterBridgeHosts(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts, std::string &error) {
  auto reject=[&](StringRef message){error=message.str();return failure();};
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!raw)return reject("AutoCounter binding requires retained annotations");
  SmallVector<unsigned> constructorIndices;
  for(auto [i,attr]:llvm::enumerate(raw)) {
    auto d=dyn_cast<DictionaryAttr>(attr);
    if(textIs(d,"class",AnnotationClasses::BridgeIO) &&
       textIs(d,"widgetClass","midas.widgets.AutoCounterBridgeModule")) constructorIndices.push_back(i);
  }
  if(hosts.size()!=constructorIndices.size())return reject("AutoCounter binding requires the complete constructor-ordered host collection");
  if(hosts.empty())return success();
  constexpr StringLiteral wrapperName="GGAutoCounterBridgeHostWrapper";
  FModuleOp inner;
  for(auto module:circuit.getOps<FModuleLike>()) {
    if(module.getModuleName()==wrapperName)return reject("AutoCounter binding wrapper already exists");
    if(module.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(module.getOperation());
  }
  if(!inner)return reject("AutoCounter binding needs an active FIRRTL top");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used)return reject("AutoCounter binding needs an uninstantiated top");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto bit=UIntType::get(ctx,1);auto clock=ClockType::get(ctx);
  std::optional<unsigned> hostClock,hostReset;
  llvm::StringSet<> portNames,hostNames,globalNames;
  for(auto [i,port]:llvm::enumerate(inner.getPorts())) {
    portNames.insert(port.name.getValue());
    if(port.name=="hostClock"&&port.type==clock&&port.direction==Direction::In)hostClock=i;
    if(port.name=="hostReset"&&port.type==bit&&port.direction==Direction::In)hostReset=i;
  }
  if(!hostClock||!hostReset)return reject("AutoCounter binding needs hostClock/hostReset inputs");
  struct Leaf {std::string name;unsigned port,annotation,field;};
  struct Binding {FModuleOp host;unsigned annotation;DictionaryAttr clock;SmallVector<Leaf> leaves;};
  SmallVector<Binding> bindings;std::set<unsigned> consumed;
  const char *expectedNames[]={"hostClock","hostReset","hValid","fromHostReady","hBits","hReady","fromHostValid","ctrl"};
  for(auto [index,item]:llvm::enumerate(hosts)) {
    FModuleOp host=item;
    if(!host||host->getParentOp()!=circuit||host==inner||!hostNames.insert(host.getName()).second||host.getNumPorts()!=8)
      return reject("AutoCounter binding needs distinct local hosts");
    auto info=host->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");
    auto bridge=cast<DictionaryAttr>(raw[constructorIndices[index]]);
    auto key=bridge.getAs<DictionaryAttr>("widgetConstructorKey");
    auto target=bridge.getAs<StringAttr>("target");auto mapping=bridge.getAs<DictionaryAttr>("channelMapping");
    auto clockInfo=bridge.getAs<DictionaryAttr>("clockInfo");
    auto clockName=clockInfo?clockInfo.getAs<StringAttr>("name"):StringAttr();
    auto mult=clockInfo?clockInfo.getAs<IntegerAttr>("multiplier"):IntegerAttr();
    auto div=clockInfo?clockInfo.getAs<IntegerAttr>("divisor"):IntegerAttr();
    if(!info||!key||!target||info.get("parameters")!=key||info.get("bridgeTarget")!=target||
       !textIs(key,"class","midas.widgets.AutoCounterParameters")||!mapping||
       !clockName||clockName.getValue().empty()||!mult||mult.getInt()<=0||mult.getInt()>UINT32_MAX||
       !div||div.getInt()<=0||div.getInt()>UINT32_MAX)
      return reject("AutoCounter binding constructor order, parameters, mapping or rational clock mismatch");
    auto bag=dyn_cast<BundleType>(host.getPortType(4));auto events=key.getAs<ArrayAttr>("eventMetadata");
    auto trigger=key.getAs<StringAttr>("triggerName"),reset=key.getAs<StringAttr>("resetPortName");
    if(!bag||!bag.isPassive()||!events||events.empty()||!trigger||!reset||
       bag.getNumElements()!=events.size()+2||mapping.size()!=bag.getNumElements()||
       bag.getElement(0).name!=trigger||bag.getElement(0).type!=bit||
       bag.getElement(1).name!=reset||bag.getElement(1).type!=bit)
      return reject("AutoCounter binding payload does not match the constructor");
    for(auto [i,attr]:llvm::enumerate(events)) {
      auto event=dyn_cast<DictionaryAttr>(attr);auto name=event?event.getAs<StringAttr>("portName"):StringAttr();
      auto width=event?event.getAs<IntegerAttr>("width"):IntegerAttr();auto op=event?event.getAs<DictionaryAttr>("opType"):DictionaryAttr();
      if(!name||!width||width.getInt()<=0||width.getInt()>(1<<20)||
         bag.getElement(i+2).name!=name||bag.getElement(i+2).type!=UIntType::get(ctx,width.getInt())||
         (!textIs(op,"class",AnnotationClasses::AutoCounterAccumulate)&&!textIs(op,"class",AnnotationClasses::AutoCounterIdentity)))
        return reject("AutoCounter binding requires typed event fields and operations");
    }
    const auto hostPorts = host.getPorts();
    for(unsigned i=0;i<8;++i) {
      const auto &port=hostPorts[i];auto expected=i==0?Type(clock):Type(bit);
      auto direction=(i==5||i==6)?Direction::Out:Direction::In;
      if(port.name!=expectedNames[i]||port.direction!=direction||
         ((i!=4&&i!=7)&&port.type!=expected))return reject("AutoCounter binding host port contract mismatch");
    }
    auto axi=dyn_cast<BundleType>(host.getPortType(7));auto addressBits=info.getAs<IntegerAttr>("addressBits");
    auto idBits=info.getAs<IntegerAttr>("idBits");
    if(!axi||!addressBits||(addressBits.getInt()!=25&&addressBits.getInt()!=32)||!idBits||idBits.getInt()!=12)
      return reject("AutoCounter binding needs the supported local control transport");
    for(auto name:{"aw","ar"}) {
      auto channel=axi.getElement(name);auto token=channel?dyn_cast<BundleType>(channel->type):BundleType();
      auto bits=token?token.getElement("bits"):std::nullopt;auto payload=bits?dyn_cast<BundleType>(bits->type):BundleType();
      auto addr=payload?payload.getElement("addr"):std::nullopt;auto id=payload?payload.getElement("id"):std::nullopt;
      if(!addr||addr->type!=UIntType::get(ctx,addressBits.getInt())||!id||id->type!=UIntType::get(ctx,12))
        return reject("AutoCounter binding local control address/ID width mismatch");
    }
    bool instantiated=false;circuit.walk([&](InstanceOp i){instantiated|=i.getModuleName()==host.getName();});
    if(instantiated)return reject("AutoCounter binding host is already instantiated");
    Binding binding{host,constructorIndices[index],clockInfo,{}};Attribute channelClock;bool haveClock=false;
    for(auto [field,element]:llvm::enumerate(bag.getElements())) {
      auto global=mapping.getAs<StringAttr>(element.name);
      if(!global||!globalNames.insert(global.getValue()).second)return reject("AutoCounter binding channel mapping is missing or shared");
      DictionaryAttr channel;unsigned annotation=0;
      for(auto [i,attr]:llvm::enumerate(raw)) {
        auto d=dyn_cast<DictionaryAttr>(attr);
        if(textIs(d,"class",AnnotationClasses::ChannelConnection)&&d.getAs<StringAttr>("globalName")==global) {
          if(channel)return reject("AutoCounter binding channel annotation is ambiguous");channel=d;annotation=i;
        }
      }
      auto channelInfo=channel?channel.getAs<DictionaryAttr>("channelInfo"):DictionaryAttr();
      auto latency=channelInfo?channelInfo.getAs<IntegerAttr>("latency"):IntegerAttr();
      auto sources=channel?channel.getAs<ArrayAttr>("sources"):ArrayAttr();auto sinks=channel?channel.getAs<ArrayAttr>("sinks"):ArrayAttr();
      if(!textIs(channelInfo,"class",AnnotationClasses::PipeChannel)||!latency||latency.getInt()!=0||
         !sources||sources.size()!=1||(sinks&&!sinks.empty()))return reject("AutoCounter binding requires latency-zero outgoing pipe channels");
      if(haveClock&&channel.get("clock")!=channelClock)return reject("AutoCounter fields cross target clock domains");
      channelClock=channel.get("clock");haveClock=true;
      auto source=dyn_cast<StringAttr>(sources[0]);auto resolved=source?resolveAnnotationTarget(circuit,source.getValue(),error):std::nullopt;
      if(!resolved||resolved->module!=inner||!resolved->port)return reject("AutoCounter channel source must resolve to an active top port");
      unsigned port=*resolved->port;auto token=dyn_cast<BundleType>(inner.getPortType(port));
      auto expected=BundleType::get(ctx,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,element.type}});
      if(!token||token!=expected||inner.getPortDirection(port)!=Direction::Out||
         resolved->fieldID!=token.getFieldID(*token.getElementIndex("bits"))||!consumed.insert(port).second)
        return reject("AutoCounter source must be a distinct typed Decoupled bits output");
      binding.leaves.push_back({element.name.getValue().str(),port,annotation,unsigned(field)});
    }
    if(!portNames.insert("autocounter_"+std::to_string(index)+"_ctrl").second)return reject("AutoCounter exposed control port already exists");
    bindings.push_back(std::move(binding));
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));FModuleOp stagedInner;
  for(auto m:staged->getOps<FModuleOp>())if(m.getName()==inner.getName())stagedInner=m;
  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))if(!consumed.count(i)){copied.push_back(i);ports.push_back(p);}
  for(auto [i,binding]:llvm::enumerate(bindings))ports.push_back({b.getStringAttr("autocounter_"+std::to_string(i)+"_ctrl"),binding.host.getPortType(7),Direction::In});
  auto loc=circuit.getLoc();b.setInsertionPointToEnd(staged->getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());auto sim=b.create<InstanceOp>(loc,stagedInner,"sim");
  auto connect=[&](Value d,Value s){b.create<ConnectOp>(loc,d,s);};
  auto field=[&](Value v,StringRef name)->Value{return b.create<SubfieldOp>(loc,v,name);};
  auto both=[&](Value a,Value c)->Value{return b.create<AndPrimOp>(loc,a,c);};
  auto outer=[&](unsigned i){return wrapper.getArgument(llvm::find(copied,i)-copied.begin());};
  for(auto i:copied)connect(inner.getPortDirection(i)==Direction::In?sim.getResult(i):outer(i),inner.getPortDirection(i)==Direction::In?outer(i):sim.getResult(i));
  SmallVector<Attribute> summaries;
  for(auto [i,binding]:llvm::enumerate(bindings)) {
    FModuleOp host;for(auto m:staged->getOps<FModuleOp>())if(m.getName()==binding.host.getName())host=m;
    auto instance=b.create<InstanceOp>(loc,host,"AutoCounterBridgeModule_"+std::to_string(i));
    connect(instance.getResult(0),outer(*hostClock));connect(instance.getResult(1),outer(*hostReset));
    Value one=b.create<ConstantOp>(loc,bit,APInt(1,1));Value valid=one;
    for(auto &leaf:binding.leaves)valid=both(valid,field(sim.getResult(leaf.port),"valid"));
    connect(instance.getResult(2),valid);connect(instance.getResult(3),one);
    for(auto [j,leaf]:llvm::enumerate(binding.leaves)) {
      auto token=sim.getResult(leaf.port);connect(b.create<SubfieldOp>(loc,instance.getResult(4),leaf.field),field(token,"bits"));
      Value ready=instance.getResult(5);
      for(auto [k,other]:llvm::enumerate(binding.leaves))if(j!=k)ready=both(ready,field(sim.getResult(other.port),"valid"));
      connect(field(token,"ready"),ready);
    }
    connect(instance.getResult(7),wrapper.getArgument(copied.size()+i));
    summaries.push_back(b.getDictionaryAttr({b.getNamedAttr("hostModule",host.getNameAttr()),
        b.getNamedAttr("widgetName",b.getStringAttr("AutoCounterBridgeModule_"+std::to_string(i))),
        b.getNamedAttr("controlPort",ports[copied.size()+i].name),b.getNamedAttr("clockInfo",binding.clock)}));
  }
  wrapper->setAttr("goldengate.autocounterHostBindings",b.getArrayAttr(summaries));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str();
  std::string modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute attr)->Attribute {
    if(auto s=dyn_cast<StringAttr>(attr)) {
      auto value=s.getValue();if(value==oldPrefix)return b.getStringAttr(newPrefix);
      if(!value.consume_front(oldPrefix+"|"))return attr;std::string suffix="|"+value.str();StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto name=ref.take_front(ref.find_first_of(".["));
        for(auto i:copied)if(name==inner.getPortName(i)){suffix.replace(0,modulePrefix.size(),"|"+wrapperName.str()+">");break;}
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto a=dyn_cast<ArrayAttr>(attr)){SmallVector<Attribute> result;for(auto v:a)result.push_back(retarget(v));return b.getArrayAttr(result);}
    if(auto d=dyn_cast<DictionaryAttr>(attr)){NamedAttrList result;for(auto v:d)result.set(v.getName(),retarget(v.getValue()));return result.getDictionary(ctx);}
    return attr;
  };
  SmallVector<Attribute> annotations;for(auto attr:raw)annotations.push_back(retarget(attr));
  for(auto &binding:bindings) {
    auto target=newPrefix+"|"+binding.host.getName().str()+">hBits";
    NamedAttrList bridge(cast<DictionaryAttr>(annotations[binding.annotation]));bridge.set("target",b.getStringAttr(target));
    annotations[binding.annotation]=bridge.getDictionary(ctx);
    for(auto &leaf:binding.leaves) {
      NamedAttrList channel(cast<DictionaryAttr>(annotations[leaf.annotation]));channel.set("sinks",b.getArrayAttr({b.getStringAttr(target+"."+leaf.name)}));
      annotations[leaf.annotation]=channel.getDictionary(ctx);
    }
  }
  staged->setName(wrapperName);(*staged)->setAttr("rawAnnotations",b.getArrayAttr(annotations));
  if(failed(verify(*staged)))return reject("AutoCounter binding produced invalid staged FIRRTL");
  wrapper->moveBefore(circuit.getBodyBlock(),circuit.getBodyBlock()->end());
  circuit.setName(wrapperName);circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));return success();
}

// Oracle: AutoCounterBridgeModule.genHeader / Widget.genConstructor. Header
// metadata is derived from the same materialized bank that services the bus.
// Allocation is supplied by the platform, never guessed from constructor order.
namespace {
struct CSVHeaderWidget {
  goldengate::ControlMMIOWidget widget;
  DictionaryAttr parameters, clock;
  ArrayAttr registers;
  unsigned addressBits;
};
LogicalResult collectCSVHeaderWidgets(CircuitOp circuit, ArrayRef<FModuleOp> hosts,
    SmallVectorImpl<CSVHeaderWidget> &result, std::string &error) {
  auto reject=[&](StringRef why){error=why.str();return failure();};
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!raw)return reject("AutoCounter catalog needs retained annotations");
  FModuleOp wrapper;
  for(auto m:circuit.getOps<FModuleOp>())if(m->hasAttr("goldengate.autocounterHostBindings")) {
    if(wrapper)return reject("AutoCounter catalog has ambiguous host binding registries");
    wrapper=m;
  }
  auto bindings=wrapper?wrapper->getAttrOfType<ArrayAttr>("goldengate.autocounterHostBindings"):ArrayAttr();
  unsigned constructors=0;
  for(auto a:raw){auto d=dyn_cast<DictionaryAttr>(a);if(textIs(d,"class",goldengate::AnnotationClasses::BridgeIO)&&textIs(d,"widgetClass","midas.widgets.AutoCounterBridgeModule"))++constructors;}
  if(hosts.size()!=constructors||(!hosts.empty()&&(!bindings||bindings.size()!=hosts.size())))
    return reject("AutoCounter catalog requires every bound host in constructor order");
  llvm::StringSet<> names;
  SmallVector<CSVHeaderWidget> next;
  for(auto [i,item]:llvm::enumerate(hosts)) {
    FModuleOp host=item;
    auto info=host?host->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost"):DictionaryAttr();
    auto row=bindings?dyn_cast<DictionaryAttr>(bindings[i]):DictionaryAttr();
    auto key=info?info.getAs<DictionaryAttr>("parameters"):DictionaryAttr();
    auto bankName=info?info.getAs<StringAttr>("bankModule"):StringAttr();
    auto mcr=info?info.getAs<StringAttr>("mcrModule"):StringAttr();
    auto addressBits=info?info.getAs<IntegerAttr>("addressBits"):IntegerAttr();
    auto name=row?row.getAs<StringAttr>("widgetName"):StringAttr();
    auto control=row?row.getAs<StringAttr>("controlPort"):StringAttr();
    auto clock=row?row.getAs<DictionaryAttr>("clockInfo"):DictionaryAttr();
    if(!host||host->getParentOp()!=circuit||!row||row.getAs<StringAttr>("hostModule")!=host.getName()||
       !name||name.getValue()!="AutoCounterBridgeModule_"+std::to_string(i)||!names.insert(name.getValue()).second||
       !control||control.getValue()!="autocounter_"+std::to_string(i)+"_ctrl"||
       !textIs(key,"class","midas.widgets.AutoCounterParameters")||!bankName||!mcr||!addressBits||
       (addressBits.getInt()!=25&&addressBits.getInt()!=32))
      return reject("AutoCounter catalog host identity, order or control width mismatch");
    unsigned instances=0;InstanceOp bound;
    for(auto instance:wrapper.getOps<InstanceOp>())if(instance.getModuleName()==host.getName()&&instance.getName()==name){++instances;bound=instance;}
    if(instances!=1||host.getNumPorts()!=8)return reject("AutoCounter catalog host is not uniquely bound");
    std::optional<unsigned> controlIndex;
    for(auto [portIndex,port]:llvm::enumerate(wrapper.getPorts()))if(port.name==control)controlIndex=portIndex;
    if(!controlIndex||wrapper.getPortDirection(*controlIndex)!=Direction::In||wrapper.getPortType(*controlIndex)!=host.getPortType(7))
      return reject("AutoCounter catalog exposed control type mismatch");
    unsigned controlConnections=0;
    for(auto connect:wrapper.getOps<ConnectOp>())if(connect.getDest()==bound.getResult(7)&&connect.getSrc()==wrapper.getArgument(*controlIndex))++controlConnections;
    if(controlConnections!=1)return reject("AutoCounter catalog control is not bound to its host");
    DictionaryAttr bridge;
    for(auto a:raw) {
      auto d=dyn_cast<DictionaryAttr>(a);
      if(!textIs(d,"class",goldengate::AnnotationClasses::BridgeIO)||!textIs(d,"widgetClass","midas.widgets.AutoCounterBridgeModule"))continue;
      auto target=d.getAs<StringAttr>("target");auto resolved=target?goldengate::resolveAnnotationTarget(circuit,target.getValue(),error):std::nullopt;
      if(!resolved||resolved->module!=host||!resolved->port||*resolved->port!=4||resolved->fieldID!=uint64_t(0))continue;
      if(bridge)return reject("AutoCounter catalog bridge identity is ambiguous");bridge=d;
    }
    auto clockName=clock?clock.getAs<StringAttr>("name"):StringAttr();
    auto mult=clock?clock.getAs<IntegerAttr>("multiplier"):IntegerAttr();auto div=clock?clock.getAs<IntegerAttr>("divisor"):IntegerAttr();
    if(!bridge||bridge.getAs<DictionaryAttr>("widgetConstructorKey")!=key||bridge.getAs<DictionaryAttr>("clockInfo")!=clock||
       !clockName||clockName.getValue().empty()||clockName.getValue().contains('\0')||
       !mult||mult.getInt()<=0||mult.getInt()>UINT32_MAX||!div||div.getInt()<=0||div.getInt()>UINT32_MAX)
      return reject("AutoCounter catalog requires matching constructor and rational clock");
    FModuleOp bank;
    for(auto m:circuit.getOps<FModuleOp>())if(m.getName()==bankName)bank=m;
    auto registers=bank?bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"):ArrayAttr();
    auto events=key.getAs<ArrayAttr>("eventMetadata");
    auto payload=host.getNumPorts()==8?dyn_cast<BundleType>(host.getPortType(4)):BundleType();
    if(!bank||bank->getAttrOfType<DictionaryAttr>("goldengate.autocounterParameters")!=key||
       !events||events.empty()||!registers||registers.size()!=2*(events.size()+1)+7||
       !payload||payload.getNumElements()!=events.size()+2||
       payload.getElement(0).name!=key.getAs<StringAttr>("triggerName")||
       payload.getElement(1).name!=key.getAs<StringAttr>("resetPortName"))
      return reject("AutoCounter catalog bank or payload differs from constructor");
    SmallVector<std::string> labels{"local_cycle"};llvm::StringSet<> uniqueLabels;uniqueLabels.insert("local_cycle");
    for(auto [j,attr]:llvm::enumerate(events)) {
      auto e=dyn_cast<DictionaryAttr>(attr);auto label=e?e.getAs<StringAttr>("label"):StringAttr();
      auto desc=e?e.getAs<StringAttr>("description"):StringAttr();auto width=e?e.getAs<IntegerAttr>("width"):IntegerAttr();
      auto op=e?e.getAs<DictionaryAttr>("opType"):DictionaryAttr();
      if(!label||label.getValue().empty()||label.getValue().contains('\0')||!uniqueLabels.insert(label.getValue()).second||
         !desc||desc.getValue().contains('\0')||!width||width.getInt()<=0||width.getInt()>(1<<20)||
         payload.getElement(j+2).name!=e.getAs<StringAttr>("portName")||payload.getElement(j+2).type!=UIntType::get(circuit.getContext(),width.getInt())||
         (!textIs(op,"class",goldengate::AnnotationClasses::AutoCounterAccumulate)&&!textIs(op,"class",goldengate::AnnotationClasses::AutoCounterIdentity)))
        return reject("AutoCounter catalog event label, description, type or operation mismatch");
      labels.push_back(label.getValue().str());
    }
    SmallVector<std::string> expected;
    for(auto &label:labels){expected.push_back("autocounter_low_"+label);expected.push_back("autocounter_high_"+label);}
    for(auto n:{"cycles_low","cycles_high","readrate_low","readrate_high","init_done","countersready","readdone"})expected.push_back(n);
    for(auto [j,a]:llvm::enumerate(registers)) {
      auto reg=dyn_cast<DictionaryAttr>(a);auto read=reg?reg.getAs<BoolAttr>("readable"):BoolAttr();auto write=reg?reg.getAs<BoolAttr>("writeable"):BoolAttr();auto offset=reg?reg.getAs<IntegerAttr>("offset"):IntegerAttr();
      unsigned fixed=2*labels.size();bool isWrite=j>=fixed+2&&j<=fixed+4;
      if(!reg||reg.getAs<StringAttr>("name")!=expected[j]||!read||!write||!offset||offset.getInt()!=int64_t(j*4)||
         read.getValue()!=!isWrite||write.getValue()!=(isWrite||j==fixed+6))
        return reject("AutoCounter catalog register name/order/permission mismatch");
    }
    goldengate::ControlMMIOWidget widget;
    if(failed(goldengate::deriveControlMMIOWidget(circuit,name.getValue(),mcr.getValue(),{bankName.getValue()},widget,error)))return failure();
    next.push_back({widget,key,clock,registers,unsigned(addressBits.getInt())});
  }
  result.append(next.begin(),next.end());return success();
}
void csvLiteral(llvm::raw_ostream &out,StringRef value) {
  out<<'"';for(unsigned char ch:value) {
    if(ch=='"'||ch=='\\')out<<'\\'<<char(ch);
    else if(ch>=32&&ch<127&&ch!='?')out<<char(ch);
    else out<<'\\'<<char('0'+((ch>>6)&7))<<char('0'+((ch>>3)&7))<<char('0'+(ch&7));
  }out<<'"';
}
}
LogicalResult goldengate::discoverBoundAutoCounterBridgeHosts(CircuitOp circuit,
    SmallVectorImpl<FModuleOp> &hosts, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  FModuleOp bound;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module->hasAttr("goldengate.autocounterHostBindings")) {
      if (bound) return reject("AutoCounter platform requires one bound host registry");
      bound = module;
    }
  SmallVector<FModuleOp> next;
  if (bound) {
    auto registry = bound->getAttrOfType<ArrayAttr>("goldengate.autocounterHostBindings");
    if (!registry || registry.empty()) return reject("AutoCounter platform has an empty host registry");
    for (auto attr : registry) {
      auto row = dyn_cast<DictionaryAttr>(attr);
      auto name = row ? row.getAs<StringAttr>("hostModule") : StringAttr();
      FModuleOp host;
      if (name) for (auto module : circuit.getOps<FModuleOp>())
        if (module.getName() == name) host = module;
      if (!host) return reject("AutoCounter platform registry refers to a missing host");
      next.push_back(host);
    }
  }
  SmallVector<CSVHeaderWidget> checked;
  if (failed(collectCSVHeaderWidgets(circuit, next, checked, error))) return failure();
  for (auto module : circuit.getOps<FModuleOp>())
    if (module->hasAttr("goldengate.autocounterHost") && !llvm::is_contained(next, module))
      return reject("AutoCounter platform would omit a materialized host");
  hosts.assign(next.begin(), next.end());
  return success();
}

LogicalResult goldengate::deriveAutoCounterControlMMIOCatalog(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts,SmallVectorImpl<ControlMMIOWidget> &widgets,std::string &error) {
  SmallVector<CSVHeaderWidget> collected;
  if(failed(collectCSVHeaderWidgets(circuit,hosts,collected,error)))return failure();
  SmallVector<ControlMMIOWidget> next;for(auto &item:collected)next.push_back(item.widget);
  widgets.assign(next.begin(),next.end());return success();
}
LogicalResult goldengate::prepareAutoCounterBridgeHeader(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts,ArrayRef<ControlMMIORegion> regions,std::string &header,std::string &error) {
  SmallVector<CSVHeaderWidget> widgets;
  if(failed(collectCSVHeaderWidgets(circuit,hosts,widgets,error)))return failure();
  auto reject=[&](StringRef why){error=why.str();return failure();};
  // Validate the entire caller allocation, including unrelated regions, so
  // duplicate identities, out-of-range banks and overlaps cannot hide here.
  llvm::StringSet<> names;
  for(auto [i,r]:llvm::enumerate(regions)) {
    if(r.name.empty()||!names.insert(r.name).second||!r.size||(r.size&(r.size-1))||r.start%r.size||
       r.start>UINT32_MAX||r.size>(uint64_t(1)<<32)-r.start)return reject("AutoCounter header invalid global MMIO region");
    for(unsigned j=0;j<i;++j)if(r.start<regions[j].start+regions[j].size&&regions[j].start<r.start+r.size)
      return reject("AutoCounter header global MMIO regions overlap");
  }
  std::string result;llvm::raw_string_ostream out(result);
  out<<"#ifdef GET_INCLUDES\n#include \"bridges/autocounter.h\"\n#endif // GET_INCLUDES\n";
  out<<"#ifdef GET_SUBSTRUCT_CHECKS\n";
  const char *fixedNames[]={"cycles_low","cycles_high","readrate_low","readrate_high","init_done","countersready","readdone"};
  if(!widgets.empty()) {
    for(auto [i,n]:llvm::enumerate(fixedNames))out<<"static_assert(offsetof(AUTOCOUNTERBRIDGEMODULE_struct, "<<n<<") == "<<i<<" * sizeof(uint64_t), \"invalid "<<n<<"\");\n";
    out<<"static_assert(sizeof(AUTOCOUNTERBRIDGEMODULE_struct) == 7 * sizeof(uint64_t), \"invalid structure\");\n";
  }
  out<<"#endif // GET_SUBSTRUCT_CHECKS\n#ifdef GET_BRIDGE_CONSTRUCTOR\n";
  for(auto [index,w]:llvm::enumerate(widgets)) {
    const ControlMMIORegion *region=nullptr;for(auto &r:regions)if(r.name==w.widget.name)region=&r;
    uint64_t size=4;while(size<w.widget.registerCount*4)size<<=1;
    if(!region||region->size!=size||region->start+region->size>(uint64_t(1)<<w.addressBits))
      return reject("AutoCounter header needs the matching implemented MMIO region");
    auto address=[&](unsigned word){return region->start+4*word;};
    out<<"registry.add_widget(new autocounter_t(simif, AddressMap{\n";
    for(bool readable:{true,false}) {
      out<<"std::vector<std::pair<std::string, uint32_t>>{\n";
      for(auto [j,a]:llvm::enumerate(w.registers)) {
        auto r=cast<DictionaryAttr>(a);if(!r.getAs<BoolAttr>(readable?"readable":"writeable").getValue())continue;
        out<<"{";csvLiteral(out,r.getAs<StringAttr>("name").getValue());out<<", "<<address(j)<<"U},\n";
      }
      out<<"}"<<(readable?",":"")<<"\n";
    }
    auto events=w.parameters.getAs<ArrayAttr>("eventMetadata");unsigned fixed=2*(events.size()+1);
    out<<"}, AUTOCOUNTERBRIDGEMODULE_struct{\n";
    for(auto [j,n]:llvm::enumerate(fixedNames))out<<"."<<n<<" = "<<address(fixed+j)<<"ULL,\n";
    out<<"}, "<<index<<"U, args, std::vector<autocounter_t::Counter>{\n";
    for(unsigned j=0;j<=events.size();++j) {
      auto event=j?cast<DictionaryAttr>(events[j-1]):DictionaryAttr();
      auto op=event?event.getAs<DictionaryAttr>("opType"):DictionaryAttr();
      out<<"autocounter_t::Counter{.type = ";csvLiteral(out,!event||textIs(op,"class",AnnotationClasses::AutoCounterAccumulate)?"Accumulate":"Identity");
      out<<", .event_label = ";csvLiteral(out,event?event.getAs<StringAttr>("label").getValue():"local_cycle");
      out<<", .event_msg = ";csvLiteral(out,event?event.getAs<StringAttr>("description").getValue():"Clock cycles elapsed in the local domain.");
      out<<", .bit_width = "<<(event?event.getAs<IntegerAttr>("width").getInt():1)<<"U, .accumulator_width = 64U, .event_addr_hi = "<<address(2*j+1)<<"U, .event_addr_lo = "<<address(2*j)<<"U},\n";
    }
    out<<"}, ClockInfo{";csvLiteral(out,w.clock.getAs<StringAttr>("name").getValue());
    out<<", "<<w.clock.getAs<IntegerAttr>("multiplier").getInt()<<"U, "<<w.clock.getAs<IntegerAttr>("divisor").getInt()<<"U}));\n";
  }
  out<<"#endif // GET_BRIDGE_CONSTRUCTOR\n";out.flush();header=std::move(result);return success();
}
