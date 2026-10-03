// See LICENSE for license details.
// Oracle: FPGATop.scala mem IO <> memAXI4Nodes, recorded FPGATop mem_0 cone.
// Requires: recorded Rocket constructor, uninstantiated buffered host master,
// exact 34/64/5 internal AXI interface and unique output/clock/reset bindings.
// Mutation: wrap the retained top and expose a 34/64/16 mem_0 interface.
// Requests zero extend IDs; responses truncate IDs without validity gating.
// Other payload and handshake leaves pass through. No state is introduced.
// Annotations: none consumed/produced; copied targets transfer to the wrapper,
// consumed host_mem targets retain their existing module/port identity.
#include "goldengate/HostMemoryPort.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <optional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::specializeHostMemoryPort(CircuitOp circuit,
                                                  std::string &error) {
  constexpr StringLiteral oldName="GGHostMemoryReadResponseWrapper";
  constexpr StringLiteral newName="GGHostMemoryPlatformWrapper";
  constexpr StringLiteral helperName="GGHostMemoryPortAdapter";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner,engine,bufferModule;unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==newName||n==helperName)return reject("host memory platform adapter already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==oldName)inner=f;
    if(n=="GGHostMemoryOutputBuffer")bufferModule=f;
    if(auto key=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(key.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!=oldName||!inner||!bufferModule||!engine||engines!=1||!raw)
    return reject("host memory platform adapter requires the buffered Rocket boundary and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  if(engine.getName()!="GGFASEDTokenEngine"||!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||
     !has(widths,"idBits",4)||!has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("host memory platform adapter supports the recorded Rocket profile");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fs)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,BundleType::get(ctx,es)}});
  };
  auto master=[&](unsigned id){
    auto address=token({{"id",id},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",34}});
    return BundleType::get(ctx,{{b.getStringAttr("aw"),false,address},
      {b.getStringAttr("w"),false,token({{"strb",8},{"last",1},{"data",64}})},
      {b.getStringAttr("b"),true,token({{"id",id},{"resp",2}})},
      {b.getStringAttr("ar"),false,address},
      {b.getStringAttr("r"),true,token({{"id",id},{"last",1},{"data",64},{"resp",2}})}});
  };
  auto internal=master(5),external=master(16);
  std::map<std::string,unsigned> old,copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  auto port=[&](StringRef n,Type t,Direction d)->std::optional<unsigned>{
    auto it=old.find(n.str());if(it==old.end())return std::nullopt;
    auto p=inner.getPorts()[it->second];if(p.type!=t||p.direction!=d)return std::nullopt;return it->second;
  };
  auto mem=port("host_mem",internal,Direction::Out);
  auto clock=port("hostClock",ClockType::get(ctx),Direction::In),reset=port("hostReset",uint(1),Direction::In);
  if(!mem||!clock||!reset||old.count("mem_0"))
    return reject("host memory platform adapter requires exact five-bit host master and clock/reset ports");
  auto bp=bufferModule.getPorts();
  if(bp.size()!=4||bp[0].name!="clock"||bp[0].type!=ClockType::get(ctx)||bp[0].direction!=Direction::In||
     bp[1].name!="reset"||bp[1].type!=uint(1)||bp[1].direction!=Direction::In||
     bp[2].name!="in"||bp[2].type!=internal||bp[2].direction!=Direction::In||
     bp[3].name!="out"||bp[3].type!=internal||bp[3].direction!=Direction::Out)
    return reject("host memory platform adapter requires the recorded output buffer interface");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp sim,router,buffer;unsigned instances=0;
  for(auto i:inner.getOps<InstanceOp>()) {
    ++instances;if(i.getName()=="sim"&&i.getModuleName()=="GGHostMemoryReadWrapper")sim=i;
    if(i.getName()=="read_responses"&&i.getModuleName()=="GGHostMemoryReadResponseRouter")router=i;
    if(i.getName()=="host_memory_buffer"&&i.getModuleName()==bufferModule.getName())buffer=i;
  }
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  unsigned out=0,cb=0,rb=0;
  if(buffer&&buffer.getNumResults()==4) {
    for(auto c:inner.getOps<ConnectOp>())out+=c.getDest()==arg(inner,*mem)&&c.getSrc()==buffer.getResult(3);
    for(auto c:inner.getOps<StrictConnectOp>()) {
      cb+=c.getDest()==buffer.getResult(0)&&c.getSrc()==arg(inner,*clock);
      rb+=c.getDest()==buffer.getResult(1)&&c.getSrc()==arg(inner,*reset);
    }
  }
  if(used||instances!=3||!sim||!router||!buffer||out!=1||cb!=1||rb!=1||!arg(inner,*mem).hasOneUse()||!buffer.getResult(3).hasOneUse())
    return reject("host memory platform adapter requires unique buffered output and host clock/reset bindings");
  // Validation precedes mutation. Keep every original module body intact.
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("in"),internal,Direction::In},
    {b.getStringAttr("out"),external,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  for(auto ch:internal.getElements()) {
    Value enq=field(arg(helper,ch.isFlip?1:0),ch.name),deq=field(arg(helper,ch.isFlip?0:1),ch.name);
    connect(field(enq,"ready"),field(deq,"ready"));
    connect(field(deq,"valid"),field(enq,"valid"));
    auto payload=cast<BundleType>(cast<BundleType>(ch.type).getElements()[2].type);
    for(auto e:payload.getElements()) {
      Value value=field(field(enq,"bits"),e.name);
      if(e.name=="id") {
        if(ch.isFlip)value=b.create<BitsPrimOp>(loc,value,4,0);
        else value=b.create<PadPrimOp>(loc,value,16);
      }
      connect(field(field(deq,"bits"),e.name),value);
    }
  }
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(p.name!="host_mem"){copied[p.name.getValue().str()]=ports.size();ports.push_back(p);}
  unsigned mi=ports.size();ports.push_back({b.getStringAttr("mem_0"),external,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child=b.create<InstanceOp>(loc,inner,"sim"),adapter=b.create<InstanceOp>(loc,helper,"memory_port");
  for(auto [n,i]:copied) {
    auto p=inner.getPorts()[old.at(n)];Value v=child.getResult(old.at(n)),a=arg(wrapper,i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  b.create<ConnectOp>(loc,adapter.getResult(0),child.getResult(*mem));
  b.create<ConnectOp>(loc,arg(wrapper,mi),adapter.getResult(1));
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto root=ref.take_front(ref.find_first_of(".[")).str();
        if(copied.count(root))suffix.replace(0,mp.size(),"|"+newName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(newName));return success();
}
