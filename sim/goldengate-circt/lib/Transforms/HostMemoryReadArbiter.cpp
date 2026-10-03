// See LICENSE for license details.
// Oracle: FPGATop's two-input, one-output AXI4Xbar AR arbitration,
// Xbar.scala returnWinner and TLArbiter.roundRobin; recorded SFC AXI4Xbar.
// Required input invariants: uninstantiated GGHostMemoryWriteResponseWrapper;
//   recorded 35/64/4 FASED constructor; exact LoadMem AR and FASED AR/R ports,
//   host AW/W/B, host clock/reset and unique two-instance boundary bindings.
// Annotations consumed: none. Annotations produced: none.
// IR mutations: add host-clock AR arbiter; wrap and join AW/W/B/AR in host_mem.
//   Copied targets transfer; consumed AR targets and old aggregate identities
//   remain on retained modules; old host_mem_write field targets rename host_mem.
// Analyses required: constructor, ports and local SSA connectivity.
// Analyses preserved: old module bodies, bridge keys and annotation classes.
// Output invariants: locked selection through stalls; priority rotation at idle
//   selection; LoadMem ID16/len0/size3/burst1; FASED IDs zero-extend to five bits.
//   Reset is synchronous on the host clock; R responses retain their boundary.
#include "goldengate/HostMemoryReadArbiter.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <optional>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addHostMemoryReadArbiter(CircuitOp circuit,
                                                    std::string &error) {
  constexpr StringLiteral oldName="GGHostMemoryWriteResponseWrapper";
  constexpr StringLiteral newName="GGHostMemoryReadWrapper";
  constexpr StringLiteral helperName="GGHostMemoryReadArbiter";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner,engine,arbiter;unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==newName||n==helperName)return reject("host memory read arbiter already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==oldName)inner=f;
    if(n=="GGHostMemoryWriteResponseRouter")arbiter=f;
    if(auto key=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(key.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!=oldName||!inner||!arbiter||!engine||engines!=1||!raw)
    return reject("host memory read arbiter requires the AW/W/B wrapper and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  if(engine.getName()!="GGFASEDTokenEngine"||!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||
     !has(widths,"idBits",4)||!has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("host memory read arbiter supports the recorded LoadMem ID 16 and FASED IDs 0..15");
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
  auto write=BundleType::get(ctx,{{b.getStringAttr("aw"),false,address(5)},
    {b.getStringAttr("w"),false,data},{b.getStringAttr("b"),true,response(5)}});
  auto rest=BundleType::get(ctx,{{b.getStringAttr("r"),true,token({{"id",4},{"last",1},{"data",64},{"resp",2}})}});
  SmallVector<BundleType::BundleElement> fs{{b.getStringAttr("ar"),false,address(4)}};
  llvm::append_range(fs,rest.getElements());auto remaining=BundleType::get(ctx,fs);
  SmallVector<BundleType::BundleElement> ws(write.getElements());ws.push_back({b.getStringAttr("ar"),false,address(5)});
  auto master=BundleType::get(ctx,ws);
  std::map<std::string,unsigned> old,copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  auto port=[&](StringRef n,Type t,Direction d)->std::optional<unsigned>{
    auto it=old.find(n.str());if(it==old.end())return std::nullopt;
    auto p=inner.getPorts()[it->second];if(p.type!=t||p.direction!=d)return std::nullopt;return it->second;
  };
  auto mem=port("fased_host_mem",remaining,Direction::Out),w=port("host_mem_write",write,Direction::Out);
  auto lr=port("loadmem_mem_ar_ready",uint(1),Direction::In),lv=port("loadmem_mem_ar_valid",uint(1),Direction::Out);
  auto la=port("loadmem_mem_ar_bits_addr",uint(34),Direction::Out);
  auto clock=port("hostClock",ClockType::get(ctx),Direction::In),reset=port("hostReset",uint(1),Direction::In);
  if(!mem||!w||!lr||!lv||!la||!clock||!reset||old.count("host_mem"))
    return reject("host memory read arbiter requires exact LoadMem AR, FASED AR/R, AW/W/B and host clock/reset ports");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp sim,router;unsigned instances=0;
  for(auto i:inner.getOps<InstanceOp>()) {
    ++instances;if(i.getName()=="sim"&&i.getModuleName()=="GGHostMemoryWriteWrapper")sim=i;
    if(i.getName()=="write_responses"&&i.getModuleName()==arbiter.getName())router=i;
  }
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  auto fieldIs=[](Value v,Value root,StringRef n){auto f=v.getDefiningOp<SubfieldOp>();return f&&f.getInput()==root&&f.getFieldName()==n;};
  auto simPort=[&](StringRef n)->Value {
    if(!sim)return {};for(auto [i,p]:llvm::enumerate(sim.getPortNames()))if(cast<StringAttr>(p).getValue()==n)return sim.getResult(i);return {};
  };
  unsigned awb=0,wb=0,bb=0,rb=0,ab=0,lrb=0,lvb=0,lab=0,cb=0,rstb=0;
  if(sim&&router&&router.getNumResults()==3) {
    Value sm=simPort("fased_host_mem"),sw=simPort("host_mem_write");
    for(auto c:inner.getOps<ConnectOp>()) {
      awb+=fieldIs(c.getDest(),arg(inner,*w),"aw")&&fieldIs(c.getSrc(),sw,"aw");
      wb+=fieldIs(c.getDest(),arg(inner,*w),"w")&&fieldIs(c.getSrc(),sw,"w");
      bb+=c.getDest()==router.getResult(0)&&fieldIs(c.getSrc(),arg(inner,*w),"b");
      rb+=fieldIs(c.getDest(),sm,"r")&&fieldIs(c.getSrc(),arg(inner,*mem),"r");
      ab+=fieldIs(c.getDest(),arg(inner,*mem),"ar")&&fieldIs(c.getSrc(),sm,"ar");
      lrb+=c.getDest()==simPort("loadmem_mem_ar_ready")&&c.getSrc()==arg(inner,*lr);
      lvb+=c.getDest()==arg(inner,*lv)&&c.getSrc()==simPort("loadmem_mem_ar_valid");
      lab+=c.getDest()==arg(inner,*la)&&c.getSrc()==simPort("loadmem_mem_ar_bits_addr");
      cb+=c.getDest()==simPort("hostClock")&&c.getSrc()==arg(inner,*clock);
      rstb+=c.getDest()==simPort("hostReset")&&c.getSrc()==arg(inner,*reset);
    }
  }
  if(used||instances!=2||!sim||!router||awb!=1||wb!=1||bb!=1||rb!=1||ab!=1||lrb!=1||lvb!=1||lab!=1||cb!=1||rstb!=1)
    return reject("host memory read arbiter requires the unique two-instance AW/W/B, AR/R and host-clock bindings");
  // All contract checks precede mutations.
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto c=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto k=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto mux=[&](Value p,Value y,Value n)->Value{return b.create<MuxPrimOp>(loc,p,y,n);};
  auto both=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto either=[&](Value x,Value y)->Value{return b.create<OrPrimOp>(loc,x,y);};
  auto inv=[&](Value x)->Value{return b.create<NotPrimOp>(loc,x);};
  auto cat=[&](Value x,Value y)->Value{return b.create<CatPrimOp>(loc,x,y);};
  auto bits=[&](Value x,unsigned hi,unsigned lo)->Value{return b.create<BitsPrimOp>(loc,x,hi,lo);};
  auto load=token({{"addr",34}});
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},{b.getStringAttr("load"),load,Direction::In},
    {b.getStringAttr("fased"),address(4),Direction::In},{b.getStringAttr("out"),address(5),Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto reg=[&](StringRef n,unsigned w,uint64_t init)->Value{
    return b.create<RegResetOp>(loc,uint(w),arg(helper,0),arg(helper,1),k(w,init),n).getResult();};
  Value idle=reg("idle",1,1),state0=reg("state_0",1,0),state1=reg("state_1",1,0),mask=reg("readys_mask",2,3);
  Value a0=arg(helper,2),a1=arg(helper,3),ao=arg(helper,4),v0=f(a0,"valid"),v1=f(a1,"valid");
  Value any=either(v0,v1),vs=cat(v1,v0);
  // Width-two SFC roundRobin/rightOR, including rotation on idle selection
  // before acceptance and readys (not winner) gating each input's ready.
  Value filter=cat(both(vs,inv(mask)),vs);
  Value spread=either(filter,cat(k(1,0),bits(filter,3,1)));
  Value unready=either(cat(k(1,0),bits(spread,3,1)),cat(mask,k(2,0)));
  Value readys=inv(both(bits(unready,3,2),bits(unready,1,0))),winner=both(readys,vs);
  c(mask,mux(both(idle,any),either(winner,cat(bits(winner,0,0),k(1,0))),mask));
  Value s0=mux(idle,bits(winner,0,0),state0),s1=mux(idle,bits(winner,1,1),state1);
  c(state0,s0);c(state1,s1);
  Value av=mux(idle,any,either(both(state0,v0),both(state1,v1))),ready=f(ao,"ready");
  c(idle,mux(both(av,ready),k(1,1),mux(any,k(1,0),idle)));
  c(f(ao,"valid"),av);
  c(f(a0,"ready"),both(ready,mux(idle,bits(readys,0,0),state0)));
  c(f(a1,"ready"),both(ready,mux(idle,bits(readys,1,1),state1)));
  auto select=[&](Value p,Value x,Value r,Value y,unsigned w){return either(mux(p,x,k(w,0)),mux(r,y,k(w,0)));};
  for(auto e:cast<BundleType>(address(5).getElements()[2].type).getElements()) {
    auto n=e.name.getValue();unsigned w=*cast<UIntType>(e.type).getWidth();
    Value x=n=="addr"?f(f(a0,"bits"),n):k(w,n=="id"?16:n=="size"?3:n=="burst"?1:0);
    Value y=f(f(a1,"bits"),n);if(n=="id")y=cat(k(1,0),y);
    c(f(f(ao,"bits"),n),select(s0,x,s1,y,w));
  }
  Value enabled=inv(arg(helper,1));
  b.create<AssertOp>(loc,arg(helper,0),inv(both(bits(winner,0,0),bits(winner,1,1))),enabled,
    "AXI4 AR arbiter has multiple winners",ValueRange{},"one_winner");
  b.create<AssertOp>(loc,arg(helper,0),either(inv(any),b.create<NEQPrimOp>(loc,winner,k(2,0))),enabled,
    "AXI4 AR arbiter has no winner",ValueRange{},"some_winner");
  std::set<std::string> consumed{"fased_host_mem","host_mem_write","loadmem_mem_ar_ready","loadmem_mem_ar_valid","loadmem_mem_ar_bits_addr"};
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())){copied[p.name.getValue().str()]=ports.size();ports.push_back(p);}
  unsigned mi=ports.size();ports.push_back({b.getStringAttr("fased_host_mem"),rest,Direction::Out});
  unsigned wi=ports.size();ports.push_back({b.getStringAttr("host_mem"),master,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child=b.create<InstanceOp>(loc,inner,"sim"),arb=b.create<InstanceOp>(loc,helper,"read_arbiter");
  for(auto [n,i]:copied) {
    auto p=inner.getPorts()[old.at(n)];Value v=child.getResult(old.at(n)),a=arg(wrapper,i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  for(auto ch:{"aw","w","b"}) {
    bool response=StringRef(ch)=="b";Value a=f(arg(wrapper,wi),ch),v=f(child.getResult(*w),ch);
    b.create<ConnectOp>(loc,response?v:a,response?a:v);
  }
  c(arb.getResult(0),arg(wrapper,copied.at("hostClock")));c(arb.getResult(1),arg(wrapper,copied.at("hostReset")));
  c(child.getResult(*lr),f(arb.getResult(2),"ready"));c(f(arb.getResult(2),"valid"),child.getResult(*lv));
  c(f(f(arb.getResult(2),"bits"),"addr"),child.getResult(*la));
  b.create<ConnectOp>(loc,arb.getResult(3),f(child.getResult(*mem),"ar"));
  b.create<ConnectOp>(loc,f(arg(wrapper,wi),"ar"),arb.getResult(4));
  b.create<ConnectOp>(loc,f(child.getResult(*mem),"r"),f(arg(wrapper,mi),"r"));
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto root=ref.take_front(ref.find_first_of(".[")).str();
        if(ref.starts_with("host_mem_write.")) {
          suffix="|"+newName.str()+">host_mem"+ref.drop_front(StringRef("host_mem_write").size()).str();
        } else if(copied.count(root)||ref.starts_with("fased_host_mem.r."))
          suffix.replace(0,mp.size(),"|"+newName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(newName));return success();
}
