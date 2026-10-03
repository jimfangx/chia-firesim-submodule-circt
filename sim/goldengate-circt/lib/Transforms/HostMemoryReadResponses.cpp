// See LICENSE for license details.
// Oracle: FPGATop AXI4Xbar, Xbar.scala mapInputIds/fanout and recorded R cone.
// Required input invariants: uninstantiated GGHostMemoryReadWrapper, recorded
//   35/64/4 FASED constructor, exact AW/W/B/AR host master, remaining FASED R
//   and LoadMem R-data boundaries, unique two-instance AR-wrapper bindings.
// Annotations consumed: none. Annotations produced: none.
// IR mutations: add combinational R router; wrap and join all host AXI channels.
//   Copied targets and AW/W/B/AR leaves transfer; consumed R and original
//   aggregate identities remain on retained inner modules.
// Analyses required: constructor, port types/directions, local SSA connectivity.
// Analyses preserved: existing bodies, annotation classes and bridge keys.
// Output invariants: ID16 goes to LoadMem, IDs0..15 to FASED (trim to four bits);
//   IDs17..31 stall; ready independent of valid; payload broadcast without
//   gating; data/status/last stay stable if the host holds them during stalls.
#include "goldengate/HostMemoryReadResponses.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <optional>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::routeHostMemoryReadResponses(CircuitOp circuit,
                                                      std::string &error) {
  constexpr StringLiteral oldName="GGHostMemoryReadWrapper";
  constexpr StringLiteral newName="GGHostMemoryReadResponseWrapper";
  constexpr StringLiteral helperName="GGHostMemoryReadResponseRouter";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner,engine,arbiter;unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==newName||n==helperName)return reject("host read response router already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==oldName)inner=f;
    if(n=="GGHostMemoryReadArbiter")arbiter=f;
    if(auto key=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(key.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!=oldName||!inner||!arbiter||!engine||engines!=1||!raw)
    return reject("host read response router requires the AR wrapper and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  if(engine.getName()!="GGFASEDTokenEngine"||!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||
     !has(widths,"idBits",4)||!has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("host read response router supports recorded LoadMem ID16 and FASED IDs0..15");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fs)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,BundleType::get(ctx,es)}});
  };
  auto address=token({{"id",5},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",34}});
  auto read=[&](unsigned id){return token({{"id",id},{"last",1},{"data",64},{"resp",2}});};
  auto remaining=BundleType::get(ctx,{{b.getStringAttr("r"),true,read(4)}});
  auto partial=BundleType::get(ctx,{{b.getStringAttr("aw"),false,address},
    {b.getStringAttr("w"),false,token({{"strb",8},{"last",1},{"data",64}})},
    {b.getStringAttr("b"),true,token({{"id",5},{"resp",2}})},
    {b.getStringAttr("ar"),false,address}});
  SmallVector<BundleType::BundleElement> fs(partial.getElements());fs.push_back({b.getStringAttr("r"),true,read(5)});
  auto master=BundleType::get(ctx,fs);
  std::map<std::string,unsigned> old,copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  auto port=[&](StringRef n,Type t,Direction d)->std::optional<unsigned>{
    auto it=old.find(n.str());if(it==old.end())return std::nullopt;
    auto p=inner.getPorts()[it->second];if(p.type!=t||p.direction!=d)return std::nullopt;return it->second;
  };
  auto mem=port("fased_host_mem",remaining,Direction::Out),host=port("host_mem",partial,Direction::Out);
  auto lr=port("loadmem_mem_r_ready",uint(1),Direction::Out),lv=port("loadmem_mem_r_valid",uint(1),Direction::In);
  auto ld=port("loadmem_mem_r_bits_data",uint(64),Direction::In);
  auto clock=port("hostClock",ClockType::get(ctx),Direction::In),reset=port("hostReset",uint(1),Direction::In);
  if(!mem||!host||!lr||!lv||!ld||!clock||!reset)
    return reject("host read response router requires exact LoadMem R data, FASED R, host master and clock/reset ports");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp sim,arb;unsigned instances=0;
  for(auto i:inner.getOps<InstanceOp>()) {
    ++instances;if(i.getName()=="sim"&&i.getModuleName()=="GGHostMemoryWriteResponseWrapper")sim=i;
    if(i.getName()=="read_arbiter"&&i.getModuleName()==arbiter.getName())arb=i;
  }
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  auto fieldIs=[](Value v,Value root,StringRef n){auto f=v.getDefiningOp<SubfieldOp>();return f&&f.getInput()==root&&f.getFieldName()==n;};
  auto simPort=[&](StringRef n)->Value {
    if(!sim)return {};for(auto [i,p]:llvm::enumerate(sim.getPortNames()))if(cast<StringAttr>(p).getValue()==n)return sim.getResult(i);return {};
  };
  unsigned awb=0,wb=0,bb=0,rb=0,ab=0,lrb=0,lvb=0,ldb=0,cb=0,rstb=0,acb=0,arstb=0,afb=0;
  if(sim&&arb&&arb.getNumResults()==5) {
    Value sm=simPort("fased_host_mem"),sw=simPort("host_mem_write");
    for(auto c:inner.getOps<ConnectOp>()) {
      awb+=fieldIs(c.getDest(),arg(inner,*host),"aw")&&fieldIs(c.getSrc(),sw,"aw");
      wb+=fieldIs(c.getDest(),arg(inner,*host),"w")&&fieldIs(c.getSrc(),sw,"w");
      bb+=fieldIs(c.getDest(),sw,"b")&&fieldIs(c.getSrc(),arg(inner,*host),"b");
      rb+=fieldIs(c.getDest(),sm,"r")&&fieldIs(c.getSrc(),arg(inner,*mem),"r");
      ab+=fieldIs(c.getDest(),arg(inner,*host),"ar")&&c.getSrc()==arb.getResult(4);
      afb+=c.getDest()==arb.getResult(3)&&fieldIs(c.getSrc(),sm,"ar");
      lrb+=c.getDest()==arg(inner,*lr)&&c.getSrc()==simPort("loadmem_mem_r_ready");
      lvb+=c.getDest()==simPort("loadmem_mem_r_valid")&&c.getSrc()==arg(inner,*lv);
      ldb+=c.getDest()==simPort("loadmem_mem_r_bits_data")&&c.getSrc()==arg(inner,*ld);
      cb+=c.getDest()==simPort("hostClock")&&c.getSrc()==arg(inner,*clock);
      rstb+=c.getDest()==simPort("hostReset")&&c.getSrc()==arg(inner,*reset);
    }
    for(auto c:inner.getOps<StrictConnectOp>()) {
      acb+=c.getDest()==arb.getResult(0)&&c.getSrc()==arg(inner,*clock);
      arstb+=c.getDest()==arb.getResult(1)&&c.getSrc()==arg(inner,*reset);
    }
  }
  if(used||instances!=2||!sim||!arb||awb!=1||wb!=1||bb!=1||rb!=1||ab!=1||afb!=1||
     lrb!=1||lvb!=1||ldb!=1||cb!=1||rstb!=1||acb!=1||arstb!=1)
    return reject("host read response router requires unique two-instance host, FASED R, LoadMem data and clock bindings");
  // All validation precedes mutations. Retained modules keep consumed targets.
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto c=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto both=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto load=token({{"data",64}});
  SmallVector<PortInfo> hp{{b.getStringAttr("host"),read(5),Direction::In},
    {b.getStringAttr("load"),load,Direction::Out},{b.getStringAttr("fased"),read(4),Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  Value h=arg(helper,0),l=arg(helper,1),fd=arg(helper,2),id=f(f(h,"bits"),"id");
  Value selectLoad=b.create<EQPrimOp>(loc,id,b.create<ConstantOp>(loc,uint(5),APInt(5,16)));
  Value selectFASED=b.create<NotPrimOp>(loc,b.create<BitsPrimOp>(loc,id,4,4));
  c(f(l,"valid"),both(f(h,"valid"),selectLoad));c(f(fd,"valid"),both(f(h,"valid"),selectFASED));
  c(f(f(l,"bits"),"data"),f(f(h,"bits"),"data"));
  c(f(f(fd,"bits"),"id"),b.create<BitsPrimOp>(loc,id,3,0));
  for(auto n:{"last","data","resp"})c(f(f(fd,"bits"),n),f(f(h,"bits"),n));
  c(f(h,"ready"),b.create<OrPrimOp>(loc,both(selectLoad,f(l,"ready")),both(selectFASED,f(fd,"ready"))));
  std::set<std::string> consumed{"fased_host_mem","host_mem","loadmem_mem_r_ready","loadmem_mem_r_valid","loadmem_mem_r_bits_data"};
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())){copied[p.name.getValue().str()]=ports.size();ports.push_back(p);}
  unsigned hi=ports.size();ports.push_back({b.getStringAttr("host_mem"),master,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child=b.create<InstanceOp>(loc,inner,"sim"),router=b.create<InstanceOp>(loc,helper,"read_responses");
  for(auto [n,i]:copied) {
    auto p=inner.getPorts()[old.at(n)];Value v=child.getResult(old.at(n)),a=arg(wrapper,i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  for(auto ch:{"aw","w","b","ar"}) {
    bool response=StringRef(ch)=="b";Value a=f(arg(wrapper,hi),ch),v=f(child.getResult(*host),ch);
    b.create<ConnectOp>(loc,response?v:a,response?a:v);
  }
  b.create<ConnectOp>(loc,router.getResult(0),f(arg(wrapper,hi),"r"));
  c(f(router.getResult(1),"ready"),child.getResult(*lr));c(child.getResult(*lv),f(router.getResult(1),"valid"));
  c(child.getResult(*ld),f(f(router.getResult(1),"bits"),"data"));
  b.create<ConnectOp>(loc,f(child.getResult(*mem),"r"),router.getResult(2));
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto root=ref.take_front(ref.find_first_of(".[")).str();
        bool preserved=ref.starts_with("host_mem.aw.")||ref.starts_with("host_mem.w.")||
          ref.starts_with("host_mem.b.")||ref.starts_with("host_mem.ar.");
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
