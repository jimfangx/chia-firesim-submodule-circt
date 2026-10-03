// See LICENSE for license details.
// Oracle: FPGATop's two-input, one-output AXI4Xbar, Xbar.scala returnWinner
// and TLArbiter.roundRobin; SFC AXI4Xbar and Queue_21. Host-clock AW/W only.
// LoadMem IDs occupy range [16,17); FASED occupies [0,16). AW reserves a
// one-hot source in a depth-two flow queue before acceptance. The reservation
// persists through AW stalls and retires only with the last accepted W beat.
// AR and responses retain their existing boundaries for subsequent porting.
#include "goldengate/HostMemoryWriteArbiter.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addHostMemoryWriteArbiter(CircuitOp circuit,
                                                   std::string &error) {
  constexpr StringLiteral oldName="GGFASEDAddressTranslationWrapper";
  constexpr StringLiteral newName="GGHostMemoryWriteWrapper";
  constexpr StringLiteral helperName="GGHostMemoryWriteArbiter";
  constexpr StringLiteral queueName="GGHostMemoryWriteSourceQueue2";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner,engine,buffer;
  unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==newName||n==helperName||n==queueName)return reject("host memory write arbiter already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==oldName)inner=f;
    if(n=="GGFASEDHostMemoryBuffer")buffer=f;
    if(auto key=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(key.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!=oldName||!inner||!buffer||!engine||engines!=1||!raw)
    return reject("host memory write arbiter requires the buffered translation top and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  if(engine.getName()!="GGFASEDTokenEngine"||!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||
     !has(widths,"idBits",4)||!has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("host memory write arbiter supports the recorded LoadMem and sixteen-ID FASED profile");
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
  auto write=[&](unsigned id){return BundleType::get(ctx,{{b.getStringAttr("aw"),false,address(id)},
    {b.getStringAttr("w"),false,data}});};
  auto load=BundleType::get(ctx,{{b.getStringAttr("aw"),false,token({{"len",8},{"addr",34}})},
    {b.getStringAttr("w"),false,token({{"last",1},{"data",64}})}});
  auto rest=BundleType::get(ctx,{{b.getStringAttr("b"),true,token({{"id",4},{"resp",2}})},
    {b.getStringAttr("ar"),false,address(4)},
    {b.getStringAttr("r"),true,token({{"id",4},{"last",1},{"data",64},{"resp",2}})}});
  SmallVector<BundleType::BundleElement> me(write(4).getElements());
  llvm::append_range(me,rest.getElements());auto master=BundleType::get(ctx,me);
  std::map<std::string,unsigned> old,copied;std::set<std::string> consumed;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  auto port=[&](StringRef n,Type t,Direction d)->std::optional<unsigned>{
    auto it=old.find(n.str());if(it==old.end())return std::nullopt;
    auto p=inner.getPorts()[it->second];if(p.type!=t||p.direction!=d)return std::nullopt;return it->second;
  };
  auto mem=port("fased_host_mem",master,Direction::Out);
  auto clock=port("hostClock",ClockType::get(ctx),Direction::In),reset=port("hostReset",uint(1),Direction::In);
  if(!mem||!clock||!reset||old.count("host_mem_write"))return reject("host memory write arbiter needs exact clock/reset and translated AXI master");
  if(buffer.getNumPorts()!=4 || buffer.getPorts()[0].type!=ClockType::get(ctx) ||
     buffer.getPorts()[0].direction!=Direction::In || buffer.getPorts()[1].type!=uint(1) ||
     buffer.getPorts()[1].direction!=Direction::In || buffer.getPorts()[2].type!=master ||
     buffer.getPorts()[2].direction!=Direction::In || buffer.getPorts()[3].type!=master ||
     buffer.getPorts()[3].direction!=Direction::Out)
    return reject("host memory write arbiter needs the exact five-channel memory buffer");
  consumed.insert("fased_host_mem");
  struct Leaf {const char *name;unsigned width;Direction direction;};
  const Leaf ls[]{{"aw_ready",1,Direction::In},{"aw_valid",1,Direction::Out},
    {"aw_bits_addr",34,Direction::Out},{"aw_bits_len",8,Direction::Out},
    {"w_ready",1,Direction::In},{"w_valid",1,Direction::Out},
    {"w_bits_data",64,Direction::Out},{"w_bits_last",1,Direction::Out}};
  for(auto l:ls) {
    std::string n="loadmem_mem_"+std::string(l.name);
    if(!port(n,uint(l.width),l.direction))return reject("host memory write arbiter requires exact LoadMem AW/W ports");
    consumed.insert(n);
  }
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp buf;unsigned count=0;
  for(auto i:inner.getOps<InstanceOp>()){++count;if(i.getName()=="memory_buffer"&&i.getModuleName()==buffer.getName())buf=i;}
  ConnectOp boundary;for(auto c:inner.getOps<ConnectOp>())
    if(buf&&buf.getNumResults()==4&&c.getDest()==inner.getBodyBlock()->getArgument(*mem)&&c.getSrc()==buf.getResult(3))boundary=c;
  unsigned clockBindings=0,resetBindings=0;
  if(buf && buf.getNumResults()==4)for(auto conn:inner.getOps<StrictConnectOp>()) {
    clockBindings+=conn.getDest()==buf.getResult(0)&&conn.getSrc()==inner.getBodyBlock()->getArgument(*clock);
    resetBindings+=conn.getDest()==buf.getResult(1)&&conn.getSrc()==inner.getBodyBlock()->getArgument(*reset);
  }
  if(used||count!=4||!buf||buf.getNumResults()!=4||!boundary||!inner.getBodyBlock()->getArgument(*mem).hasOneUse()||clockBindings!=1||resetBindings!=1)
    return reject("host memory write arbiter requires the unique buffered master of an uninstantiated four-instance top");

  // All contract checks precede mutations. Inner modules and state are retained.
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto c=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto k=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto mux=[&](Value p,Value y,Value n)->Value{return b.create<MuxPrimOp>(loc,p,y,n);};
  auto both=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto either=[&](Value x,Value y)->Value{return b.create<OrPrimOp>(loc,x,y);};
  auto inv=[&](Value x)->Value{return b.create<NotPrimOp>(loc,x);};
  auto cat=[&](Value x,Value y)->Value{return b.create<CatPrimOp>(loc,x,y);};
  auto bits=[&](Value x,unsigned hi,unsigned lo)->Value{return b.create<BitsPrimOp>(loc,x,hi,lo);};
  auto sourceType=BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
    {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,uint(2)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> qp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},{b.getStringAttr("enq"),sourceType,Direction::In},
    {b.getStringAttr("deq"),sourceType,Direction::Out}};
  auto queue=b.create<FModuleOp>(loc,b.getStringAttr(queueName),ConventionAttr::get(ctx,Convention::Internal),qp);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto reg=[&](FModuleOp m,StringRef n,unsigned w,uint64_t init)->Value{
    return b.create<RegResetOp>(loc,uint(w),arg(m,0),arg(m,1),k(w,init),n).getResult();};
  Value ep=reg(queue,"enq_ptr_value",1,0),dp=reg(queue,"deq_ptr_value",1,0),mf=reg(queue,"maybe_full",1,0);
  Value eq=b.create<EQPrimOp>(loc,ep,dp),empty=both(eq,inv(mf)),ready=inv(both(eq,mf));
  Value enq=arg(queue,2),deq=arg(queue,3),ev=f(enq,"valid"),dr=f(deq,"ready");
  Value valid=either(ev,inv(empty));
  Value push=both(both(ready,ev),inv(both(empty,dr))),pop=both(both(valid,dr),inv(empty));
  c(f(enq,"ready"),ready);c(f(deq,"valid"),valid);
  c(ep,mux(push,inv(ep),ep));c(dp,mux(pop,inv(dp),dp));
  c(mf,mux(b.create<XorPrimOp>(loc,push,pop),push,mf));
  SmallVector<Type> mt{MemOp::getTypeForPort(2,uint(2),MemOp::PortKind::Read),
    MemOp::getTypeForPort(2,uint(2),MemOp::PortKind::Write)};
  SmallVector<Attribute> mn{b.getStringAttr("read"),b.getStringAttr("write")};
  auto ram=b.create<MemOp>(loc,mt,0,1,2,RUWAttr::Undefined,mn,"ram");
  Value rd=ram.getResult(0),wr=ram.getResult(1);
  c(f(rd,"clk"),arg(queue,0));c(f(rd,"en"),k(1,1));c(f(rd,"addr"),dp);
  c(f(wr,"clk"),arg(queue,0));c(f(wr,"en"),push);c(f(wr,"addr"),ep);c(f(wr,"mask"),k(1,1));
  c(f(wr,"data"),f(enq,"bits"));c(f(deq,"bits"),mux(empty,f(enq,"bits"),f(rd,"data")));
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},{b.getStringAttr("load"),load,Direction::In},
    {b.getStringAttr("fased"),write(4),Direction::In},{b.getStringAttr("out"),write(5),Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto q=b.create<InstanceOp>(loc,queue,"aw_sources");c(q.getResult(0),arg(helper,0));c(q.getResult(1),arg(helper,1));
  Value idle=reg(helper,"idle",1,1),state0=reg(helper,"state_0",1,0),state1=reg(helper,"state_1",1,0);
  Value mask=reg(helper,"readys_mask",2,3),latched=reg(helper,"latched",1,0);
  Value a0=f(arg(helper,2),"aw"),a1=f(arg(helper,3),"aw"),ao=f(arg(helper,4),"aw");
  Value v0=f(a0,"valid"),v1=f(a1,"valid"),any=either(v0,v1),vs=cat(v1,v0);
  // Exact width-two SFC rightOR(filter,4,2), then shift by one.
  Value filter=cat(both(vs,inv(mask)),vs);
  Value spread=either(filter,cat(k(1,0),bits(filter,3,1)));
  Value unready=either(cat(k(1,0),bits(spread,3,1)),cat(mask,k(2,0)));
  Value readys=inv(both(bits(unready,3,2),bits(unready,1,0))),winner=both(readys,vs);
  c(mask,mux(both(idle,any),either(winner,cat(bits(winner,0,0),k(1,0))),mask));
  Value s0=mux(idle,bits(winner,0,0),state0),s1=mux(idle,bits(winner,1,1),state1);
  c(state0,s0);c(state1,s1);
  Value av=mux(idle,any,either(both(state0,v0),both(state1,v1)));
  Value admitted=either(latched,f(q.getResult(2),"ready"));
  Value ar=both(f(ao,"ready"),admitted),af=both(av,ar);
  c(idle,mux(af,k(1,1),mux(any,k(1,0),idle)));
  c(f(ao,"valid"),both(av,admitted));
  c(f(a0,"ready"),both(ar,mux(idle,bits(readys,0,0),state0)));
  c(f(a1,"ready"),both(ar,mux(idle,bits(readys,1,1),state1)));
  Value qe=q.getResult(2),qd=q.getResult(3),qv=both(av,inv(latched));
  c(f(qe,"valid"),qv);c(f(qe,"bits"),cat(s1,s0));
  c(latched,mux(af,k(1,0),either(latched,both(qv,f(qe,"ready")))));
  auto select=[&](Value p,Value x,Value r,Value y,unsigned w){return either(mux(p,x,k(w,0)),mux(r,y,k(w,0)));};
  auto ab0=f(a0,"bits"),ab1=f(a1,"bits"),abo=f(ao,"bits");
  for(auto e:cast<BundleType>(address(5).getElements()[2].type).getElements()) {
    auto n=e.name.getValue();unsigned w=*cast<UIntType>(e.type).getWidth();
    Value x=n=="addr"||n=="len"?f(ab0,n):k(w,n=="id"?16:n=="size"?3:n=="burst"?1:0);
    Value y=f(ab1,n);if(n=="id")y=cat(k(1,0),y);
    c(f(abo,n),select(s0,x,s1,y,w));
  }
  Value w0=f(arg(helper,2),"w"),w1=f(arg(helper,3),"w"),wo=f(arg(helper,4),"w");
  Value d0=bits(f(qd,"bits"),0,0),d1=bits(f(qd,"bits"),1,1);
  Value wv=select(d0,f(w0,"valid"),d1,f(w1,"valid"),1);
  Value last=select(d0,f(f(w0,"bits"),"last"),d1,f(f(w1,"bits"),"last"),1);
  Value wrdy=both(f(wo,"ready"),f(qd,"valid"));
  c(f(wo,"valid"),both(wv,f(qd,"valid")));c(f(w0,"ready"),both(wrdy,d0));c(f(w1,"ready"),both(wrdy,d1));
  c(f(qd,"ready"),both(both(wv,last),f(wo,"ready")));
  for(auto e:cast<BundleType>(data.getElements()[2].type).getElements()) {
    auto n=e.name.getValue();unsigned w=*cast<UIntType>(e.type).getWidth();
    c(f(f(wo,"bits"),n),select(d0,n=="strb"?k(8,255):f(f(w0,"bits"),n),d1,f(f(w1,"bits"),n),w));
  }
  Value enabled=inv(arg(helper,1));
  b.create<AssertOp>(loc,arg(helper,0),inv(both(bits(winner,0,0),bits(winner,1,1))),enabled,
    "AXI4 AW arbiter has multiple winners",ValueRange{},"one_winner");
  b.create<AssertOp>(loc,arg(helper,0),either(inv(any),b.create<NEQPrimOp>(loc,winner,k(2,0))),enabled,
    "AXI4 AW arbiter has no winner",ValueRange{},"some_winner");

  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())){copied[p.name.getValue().str()]=ports.size();ports.push_back(p);}
  unsigned restIndex=ports.size();ports.push_back({b.getStringAttr("fased_host_mem"),rest,Direction::Out});
  unsigned writeIndex=ports.size();ports.push_back({b.getStringAttr("host_mem_write"),write(5),Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),arb=b.create<InstanceOp>(loc,helper,"write_arbiter");
  for(auto [n,i]:copied) {
    auto p=inner.getPorts()[old.at(n)];Value v=sim.getResult(old.at(n)),a=arg(wrapper,i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  c(arb.getResult(0),arg(wrapper,copied.at("hostClock")));c(arb.getResult(1),arg(wrapper,copied.at("hostReset")));
  for(auto l:ls) {
    StringRef n=l.name;auto split=n.find('_');auto ch=n.take_front(split);n=n.drop_front(split+1);
    Value leaf=f(arb.getResult(2),ch);
    if(n.consume_front("bits_"))leaf=f(f(leaf,"bits"),n);else leaf=f(leaf,n);
    Value v=sim.getResult(old.at("loadmem_mem_"+std::string(l.name)));
    c(l.direction==Direction::In?v:leaf,l.direction==Direction::In?leaf:v);
  }
  for(auto ch:{"aw","w"})b.create<ConnectOp>(loc,f(arb.getResult(3),ch),f(sim.getResult(*mem),ch));
  for(auto ch:{"b","ar","r"}) {
    bool request=StringRef(ch)=="ar";Value a=f(arg(wrapper,restIndex),ch),v=f(sim.getResult(*mem),ch);
    b.create<ConnectOp>(loc,request?a:v,request?v:a);
  }
  b.create<ConnectOp>(loc,arg(wrapper,writeIndex),arb.getResult(4));
  // Consumed leaf targets remain valid on the retained inner module; copied
  // ports and the remaining FASED channels retain their public top identity.
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);
      if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto root=ref.take_front(ref.find_first_of(".[")).str();
        bool remaining=ref.starts_with("fased_host_mem.b.")||ref.starts_with("fased_host_mem.ar.")||ref.starts_with("fased_host_mem.r.");
        if(copied.count(root)||remaining)suffix.replace(0,mp.size(),"|"+newName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(newName));return success();
}
