// See LICENSE for license details.
// Oracle: AXI4Deinterleaver.scala / Queue_34 in the immutable SFC RTL.
// Recorded 35/64/4 memory master, sixteen used IDs, eight beats per FIFO.
// Insert between FASED and translation, preserving the existing top/targets.
// Wait for complete bursts, choose the lowest pending ID, then hold that burst
// through stalls until accepted RLAST. Pending arbitration uses next counts.
// RAM is asynchronous read, synchronous write; reset flushes pointers only.
#include "goldengate/FASEDReadDeinterleaver.h"
#include "mlir/IR/Builders.h"
#include <map>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDReadDeinterleaver(CircuitOp circuit,
                                                  std::string &error) {
  constexpr StringLiteral topName="GGFASEDAddressTranslationWrapper";
  constexpr StringLiteral helperName="GGFASEDReadDeinterleaver";
  constexpr StringLiteral queueName="GGFASEDDeinterleaveQueue8";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp top,inner,translation,engine;
  unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==helperName || n==queueName)return reject("FASED deinterleaver already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==topName)top=f;
    if(n=="GGFASEDHostMemoryWrapper")inner=f;
    if(n=="GGFASEDAddressTranslation")translation=f;
    if(auto k=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(k.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  if(circuit.getName()!=topName || !top || !inner || !translation ||
     !engine || engines!=1 || engine.getName()!="GGFASEDTokenEngine" ||
     !circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("FASED deinterleaver requires the active translation boundary and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  if(!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||!has(widths,"idBits",4)||
     !has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("FASED deinterleaver supports the recorded sixteen-ID eight-beat profile");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
    SmallVector<BundleType::BundleElement> es;
    for(auto [n,w]:fs)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},
      {b.getStringAttr("bits"),false,BundleType::get(ctx,es)}});
  };
  auto address=token({{"id",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},
      {"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto master=BundleType::get(ctx,{{b.getStringAttr("aw"),false,address},
    {b.getStringAttr("w"),false,token({{"strb",8},{"last",1},{"data",64}})},
    {b.getStringAttr("b"),true,token({{"id",4},{"resp",2}})},
    {b.getStringAttr("ar"),false,address},
    {b.getStringAttr("r"),true,token({{"id",4},{"last",1},{"data",64},{"resp",2}})}});
  auto port=[&](FModuleOp m,StringRef n,Type t,Direction d)->std::optional<unsigned>{
    for(auto [i,p]:llvm::enumerate(m.getPorts()))
      if(p.name==n&&p.type==t&&p.direction==d)return i;
    return std::nullopt;
  };
  auto mem=port(inner,"fased_host_mem",master,Direction::Out);
  auto clock=port(top,"hostClock",ClockType::get(ctx),Direction::In);
  auto reset=port(top,"hostReset",uint(1),Direction::In);
  if(!mem || !clock || !reset || translation.getNumPorts()!=4 ||
     port(translation,"clock",ClockType::get(ctx),Direction::In)!=std::optional<unsigned>(0) ||
     port(translation,"reset",uint(1),Direction::In)!=std::optional<unsigned>(1) ||
     port(translation,"in",master,Direction::In)!=std::optional<unsigned>(2))
    return reject("FASED deinterleaver requires exact host clock/reset and pre-translation master");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==topName;});
  InstanceOp sim,trans;unsigned instances=0;
  for(auto i:top.getOps<InstanceOp>()) {
    ++instances;
    if(i.getName()=="sim"&&i.getModuleName()==inner.getName())sim=i;
    if(i.getName()=="translation"&&i.getModuleName()==translation.getName())trans=i;
  }
  if(used || instances!=2 || !sim || !trans)
    return reject("FASED deinterleaver requires the uninstantiated two-instance translation top");
  ConnectOp boundary;
  for(auto c:top.getOps<ConnectOp>())
    if(c.getDest()==trans.getResult(2)&&c.getSrc()==sim.getResult(*mem))boundary=c;
  unsigned clk=0,rst=0;
  for(auto c:top.getOps<StrictConnectOp>()) {
    clk+=c.getDest()==trans.getResult(0)&&c.getSrc()==top.getBodyBlock()->getArgument(*clock);
    rst+=c.getDest()==trans.getResult(1)&&c.getSrc()==top.getBodyBlock()->getArgument(*reset);
  }
  if(!boundary || !trans.getResult(2).hasOneUse() || !sim.getResult(*mem).hasOneUse() || clk!=1 || rst!=1)
    return reject("FASED deinterleaver needs the unique direct memory connection and host clock/reset binding");

  // All rejection checks precede mutation. Existing annotations/top ports stay
  // unchanged; only the unique internal master connection is replaced.
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto response=token({{"id",4},{"data",64},{"resp",2},{"last",1}});
  auto payload=cast<BundleType>(response.getElements()[2].type);
  SmallVector<PortInfo> qp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},
    {b.getStringAttr("enq"),response,Direction::In},{b.getStringAttr("deq"),response,Direction::Out}};
  auto queue=b.create<FModuleOp>(loc,b.getStringAttr(queueName),ConventionAttr::get(ctx,Convention::Internal),qp);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto c=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto k=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto mux=[&](Value p,Value y,Value n)->Value{return b.create<MuxPrimOp>(loc,p,y,n);};
  auto both=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto inv=[&](Value x)->Value{return b.create<NotPrimOp>(loc,x);};
  auto reg=[&](FModuleOp m,unsigned w,StringRef n)->Value {
    return b.create<RegResetOp>(loc,uint(w),arg(m,0),arg(m,1),k(w,0),n).getResult();
  };
  Value ep=reg(queue,3,"enq_ptr_value"),dp=reg(queue,3,"deq_ptr_value"),mf=reg(queue,1,"maybe_full");
  Value eq=b.create<EQPrimOp>(loc,ep,dp),ready=inv(both(eq,mf)),valid=inv(both(eq,inv(mf)));
  Value enq=arg(queue,2),deq=arg(queue,3);
  Value push=both(ready,f(enq,"valid")),pop=both(valid,f(deq,"ready"));
  c(f(enq,"ready"),ready);c(f(deq,"valid"),valid);
  auto incPtr=[&](Value v)->Value{return b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,v,k(3,1)),2,0);};
  c(ep,mux(push,incPtr(ep),ep));c(dp,mux(pop,incPtr(dp),dp));
  c(mf,mux(b.create<XorPrimOp>(loc,push,pop),push,mf));
  SmallVector<Type> mt{MemOp::getTypeForPort(8,uint(71),MemOp::PortKind::Read),
                      MemOp::getTypeForPort(8,uint(71),MemOp::PortKind::Write)};
  SmallVector<Attribute> mn{b.getStringAttr("read"),b.getStringAttr("write")};
  auto ram=b.create<MemOp>(loc,mt,0,1,8,RUWAttr::Undefined,mn,"ram");
  Value rd=ram.getResult(0),wr=ram.getResult(1);
  c(f(rd,"clk"),arg(queue,0));c(f(rd,"en"),k(1,1));c(f(rd,"addr"),dp);
  c(f(wr,"clk"),arg(queue,0));c(f(wr,"en"),push);c(f(wr,"addr"),ep);c(f(wr,"mask"),k(1,1));
  Value packed;for(auto e:payload.getElements()) {
    Value v=f(f(enq,"bits"),e.name);packed=packed?b.create<CatPrimOp>(loc,packed,v).getResult():v;
  }
  c(f(wr,"data"),packed);
  unsigned low=0;Value data=f(rd,"data");
  for(auto e:llvm::reverse(payload.getElements())) {
    unsigned w=*cast<UIntType>(e.type).getWidth();
    c(f(f(deq,"bits"),e.name),b.create<BitsPrimOp>(loc,data,low+w-1,low));low+=w;
  }

  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},
    {b.getStringAttr("in"),master,Direction::In},{b.getStringAttr("out"),master,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  Value in=arg(helper,2),out=arg(helper,3),ir=f(in,"r"),orr=f(out,"r");
  for(auto ch:{"aw","w","b","ar"}) {
    bool request=StringRef(ch)!="b";Value a=f(in,ch),z=f(out,ch);
    c(f(request?z:a,"valid"),f(request?a:z,"valid"));
    c(f(request?a:z,"ready"),f(request?z:a,"ready"));
    auto bits=cast<BundleType>(cast<BundleType>(a.getType()).getElements()[2].type);
    for(auto e:bits.getElements())c(f(f(request?z:a,"bits"),e.name),f(f(request?a:z,"bits"),e.name));
  }
  Value locked=reg(helper,1,"locked"),id=b.create<RegOp>(loc,uint(4),arg(helper,0),"deq_id").getResult();
  Value eid=f(f(orr,"bits"),"id");
  SmallVector<InstanceOp> qs;SmallVector<Value> selected;
  Value enqReady=k(1,0);
  std::map<std::string,Value> chosen;
  for(auto e:payload.getElements())chosen[e.name.getValue().str()]=k(*cast<UIntType>(e.type).getWidth(),0);
  for(unsigned j=0;j<16;++j) {
    auto q=b.create<InstanceOp>(loc,queue,"qs_queue_"+std::to_string(j));qs.push_back(q);
    c(q.getResult(0),arg(helper,0));c(q.getResult(1),arg(helper,1));
    Value es=b.create<EQPrimOp>(loc,eid,k(4,j)),ds=b.create<EQPrimOp>(loc,id,k(4,j));selected.push_back(ds);
    c(f(q.getResult(2),"valid"),both(es,f(orr,"valid")));
    for(auto e:payload.getElements())c(f(f(q.getResult(2),"bits"),e.name),f(f(orr,"bits"),e.name));
    enqReady=mux(es,f(q.getResult(2),"ready"),enqReady);
    for(auto e:payload.getElements()) {
      auto &v=chosen[e.name.getValue().str()];v=mux(ds,f(f(q.getResult(3),"bits"),e.name),v);
    }
  }
  c(f(orr,"ready"),enqReady);c(f(ir,"valid"),locked);
  for(auto e:payload.getElements())c(f(f(ir,"bits"),e.name),chosen.at(e.name.getValue().str()));
  Value inFire=both(locked,f(ir,"ready")),outFire=both(enqReady,f(orr,"valid"));
  Value done=both(inFire,chosen.at("last")),incomingLast=both(outFire,f(f(orr,"bits"),"last"));
  Value enabled=inv(arg(helper,1));SmallVector<Value> pending;
  for(unsigned j=0;j<16;++j) {
    c(f(qs[j].getResult(3),"ready"),both(selected[j],inFire));
    Value count=reg(helper,4,"pending_count_"+std::to_string(j));
    Value inc=both(b.create<EQPrimOp>(loc,eid,k(4,j)),incomingLast),dec=both(selected[j],done);
    Value sum=b.create<AddPrimOp>(loc,count,inc),diff=b.create<SubPrimOp>(loc,sum,dec);
    Value next=b.create<BitsPrimOp>(loc,diff,3,0);c(count,next);
    pending.push_back(b.create<NEQPrimOp>(loc,next,k(4,0)));
    Value nonzero=b.create<NEQPrimOp>(loc,count,k(4,0)),nonfull=b.create<NEQPrimOp>(loc,count,k(4,8));
    b.create<AssertOp>(loc,arg(helper,0),b.create<OrPrimOp>(loc,inv(dec),nonzero),enabled,
      "deinterleaver pending burst count underflow",ValueRange{},"");
    b.create<AssertOp>(loc,arg(helper,0),b.create<OrPrimOp>(loc,inv(inc),nonfull),enabled,
      "deinterleaver pending burst count overflow",ValueRange{},"");
  }
  Value winner=k(4,0),any=k(1,0);
  for(int j=15;j>=0;--j) {winner=mux(pending[j],k(4,j),winner);any=b.create<OrPrimOp>(loc,any,pending[j]);}
  Value select=b.create<OrPrimOp>(loc,inv(locked),done);
  c(locked,mux(select,any,locked));c(id,mux(select,winner,id));

  b.setInsertionPoint(boundary);
  auto deinterleaver=b.create<InstanceOp>(loc,helper,"deinterleaver");
  c(deinterleaver.getResult(0),top.getBodyBlock()->getArgument(*clock));
  c(deinterleaver.getResult(1),top.getBodyBlock()->getArgument(*reset));
  b.create<ConnectOp>(loc,deinterleaver.getResult(2),sim.getResult(*mem));
  b.create<ConnectOp>(loc,trans.getResult(2),deinterleaver.getResult(3));
  boundary.erase();return success();
}
