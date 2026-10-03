// See LICENSE for license details.
// Oracle: FPGATop AXI4Xbar, Xbar.scala mapInputIds/fanout and the recorded
// AXI4Xbar B cone. One output sink makes response arbitration combinational.
// Required input invariants: uninstantiated GGHostMemoryWriteWrapper; exact
//   AW/W master, remaining FASED B/AR/R and LoadMem B boundaries; recorded
//   35/64/4 FASED constructor and its unique two-instance wrapper bindings.
// Annotations consumed: none. Annotations produced: none.
// IR mutations: add B demultiplexer and a wrapper joining host AW/W/B;
//   retarget copied ports and AW/W leaves; retain consumed B identities inside.
// Analyses required: port, constructor and local SSA connectivity inspection.
// Analyses preserved: existing module bodies, bridge keys and annotation classes.
// Output invariants: LoadMem ID 16, FASED IDs 0..15 (trimmed to four bits);
//   unmatched IDs 17..31 are blocked; ready is independent of valid;
//   backpressure follows only the selected sink; AR/R remain separate.
#include "goldengate/HostMemoryWriteResponses.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::routeHostMemoryWriteResponses(CircuitOp circuit,
                                                        std::string &error) {
  constexpr StringLiteral oldName="GGHostMemoryWriteWrapper";
  constexpr StringLiteral newName="GGHostMemoryWriteResponseWrapper";
  constexpr StringLiteral helperName="GGHostMemoryWriteResponseRouter";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner,engine,arbiter;unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==newName||n==helperName)return reject("host write response router already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==oldName)inner=f;
    if(n=="GGHostMemoryWriteArbiter")arbiter=f;
    if(auto key=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(key.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!=oldName||!inner||!arbiter||!engine||engines!=1||!raw)
    return reject("host write response router requires the AW/W wrapper and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  if(engine.getName()!="GGFASEDTokenEngine"||!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||
     !has(widths,"idBits",4)||!has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("host write response router supports the recorded LoadMem ID 16 and FASED IDs 0..15");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fs)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,BundleType::get(ctx,es)}});
  };
  auto address=[&](unsigned id){return token({{"id",id},{"qos",4},{"prot",3},{"cache",4},{"lock",1},
    {"burst",2},{"size",3},{"len",8},{"addr",34}});};
  auto data=token({{"strb",8},{"last",1},{"data",64}});
  auto response=[&](unsigned id){return token({{"id",id},{"resp",2}});};
  auto write=BundleType::get(ctx,{{b.getStringAttr("aw"),false,address(5)},{b.getStringAttr("w"),false,data}});
  auto rest=BundleType::get(ctx,{{b.getStringAttr("ar"),false,address(4)},
    {b.getStringAttr("r"),true,token({{"id",4},{"last",1},{"data",64},{"resp",2}})}});
  SmallVector<BundleType::BundleElement> fs{{b.getStringAttr("b"),true,response(4)}};
  llvm::append_range(fs,rest.getElements());auto remaining=BundleType::get(ctx,fs);
  SmallVector<BundleType::BundleElement> ws(write.getElements());ws.push_back({b.getStringAttr("b"),true,response(5)});
  auto master=BundleType::get(ctx,ws);
  std::map<std::string,unsigned> old,copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  auto port=[&](StringRef n,Type t,Direction d)->std::optional<unsigned>{
    auto it=old.find(n.str());if(it==old.end())return std::nullopt;
    auto p=inner.getPorts()[it->second];if(p.type!=t||p.direction!=d)return std::nullopt;return it->second;
  };
  auto mem=port("fased_host_mem",remaining,Direction::Out),w=port("host_mem_write",write,Direction::Out);
  auto lr=port("loadmem_mem_b_ready",uint(1),Direction::Out),lv=port("loadmem_mem_b_valid",uint(1),Direction::In);
  auto clock=port("hostClock",ClockType::get(ctx),Direction::In),reset=port("hostReset",uint(1),Direction::In);
  if(!mem||!w||!lr||!lv||!clock||!reset)
    return reject("host write response router requires exact LoadMem B, FASED B/AR/R and host AW/W ports");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp sim,arb;unsigned instances=0;
  for(auto i:inner.getOps<InstanceOp>()) {
    ++instances;if(i.getName()=="sim"&&i.getModuleName()=="GGFASEDAddressTranslationWrapper")sim=i;
    if(i.getName()=="write_arbiter"&&i.getModuleName()==arbiter.getName())arb=i;
  }
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  auto fieldIs=[](Value v,Value root,StringRef n){auto f=v.getDefiningOp<SubfieldOp>();return f&&f.getInput()==root&&f.getFieldName()==n;};
  auto simPort=[&](StringRef n)->Value {
    if(!sim)return {};for(auto [i,p]:llvm::enumerate(sim.getPortNames()))if(cast<StringAttr>(p).getValue()==n)return sim.getResult(i);return {};
  };
  unsigned wb=0,bb=0,rb=0,ab=0,lrb=0,lvb=0;
  if(sim&&arb&&arb.getNumResults()==5) {
    Value sm=simPort("fased_host_mem");
    for(auto c:inner.getOps<ConnectOp>()) {
      wb+=c.getDest()==arg(inner,*w)&&c.getSrc()==arb.getResult(4);
      bb+=fieldIs(c.getDest(),sm,"b")&&fieldIs(c.getSrc(),arg(inner,*mem),"b");
      rb+=fieldIs(c.getDest(),sm,"r")&&fieldIs(c.getSrc(),arg(inner,*mem),"r");
      ab+=fieldIs(c.getDest(),arg(inner,*mem),"ar")&&fieldIs(c.getSrc(),sm,"ar");
      lrb+=c.getDest()==arg(inner,*lr)&&c.getSrc()==simPort("loadmem_mem_b_ready");
      lvb+=c.getDest()==simPort("loadmem_mem_b_valid")&&c.getSrc()==arg(inner,*lv);
    }
  }
  if(used||instances!=2||!sim||!arb||wb!=1||bb!=1||rb!=1||ab!=1||lrb!=1||lvb!=1)
    return reject("host write response router requires the unique two-instance AW/W and B/AR/R bindings");
  // All validation precedes mutations; retained modules preserve consumed targets.
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto c=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto both=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto load=BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},{b.getStringAttr("valid"),false,uint(1)}});
  SmallVector<PortInfo> hp{{b.getStringAttr("host"),response(5),Direction::In},
    {b.getStringAttr("load"),load,Direction::Out},{b.getStringAttr("fased"),response(4),Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  Value host=arg(helper,0),ld=arg(helper,1),fd=arg(helper,2),id=f(f(host,"bits"),"id");
  Value selectLoad=b.create<EQPrimOp>(loc,id,b.create<ConstantOp>(loc,uint(5),APInt(5,16)));
  Value selectFASED=b.create<NotPrimOp>(loc,b.create<BitsPrimOp>(loc,id,4,4));
  c(f(ld,"valid"),both(f(host,"valid"),selectLoad));c(f(fd,"valid"),both(f(host,"valid"),selectFASED));
  c(f(f(fd,"bits"),"id"),b.create<BitsPrimOp>(loc,id,3,0));c(f(f(fd,"bits"),"resp"),f(f(host,"bits"),"resp"));
  c(f(host,"ready"),b.create<OrPrimOp>(loc,both(selectLoad,f(ld,"ready")),both(selectFASED,f(fd,"ready"))));
  std::set<std::string> consumed{"fased_host_mem","host_mem_write","loadmem_mem_b_ready","loadmem_mem_b_valid"};
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())){copied[p.name.getValue().str()]=ports.size();ports.push_back(p);}
  unsigned mi=ports.size();ports.push_back({b.getStringAttr("fased_host_mem"),rest,Direction::Out});
  unsigned wi=ports.size();ports.push_back({b.getStringAttr("host_mem_write"),master,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child=b.create<InstanceOp>(loc,inner,"sim"),router=b.create<InstanceOp>(loc,helper,"write_responses");
  for(auto [n,i]:copied) {
    auto p=inner.getPorts()[old.at(n)];Value v=child.getResult(old.at(n)),a=arg(wrapper,i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  for(auto ch:{"aw","w"})b.create<ConnectOp>(loc,f(arg(wrapper,wi),ch),f(child.getResult(*w),ch));
  b.create<ConnectOp>(loc,router.getResult(0),f(arg(wrapper,wi),"b"));
  c(f(router.getResult(1),"ready"),child.getResult(*lr));c(child.getResult(*lv),f(router.getResult(1),"valid"));
  b.create<ConnectOp>(loc,f(child.getResult(*mem),"b"),router.getResult(2));
  for(auto ch:{"ar","r"}) {
    bool request=StringRef(ch)=="ar";Value a=f(arg(wrapper,mi),ch),v=f(child.getResult(*mem),ch);
    b.create<ConnectOp>(loc,request?a:v,request?v:a);
  }
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto root=ref.take_front(ref.find_first_of(".[")).str();
        bool preserved=ref.starts_with("fased_host_mem.ar.")||ref.starts_with("fased_host_mem.r.")||
          ref.starts_with("host_mem_write.aw.")||ref.starts_with("host_mem_write.w.");
        if(copied.count(root)||preserved)suffix.replace(0,mp.size(),"|"+newName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(newName));return success();
}
