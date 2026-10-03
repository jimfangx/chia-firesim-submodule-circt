// See LICENSE for license details.
// Oracle: FPGATop AXI4Buffer_1 and Queue_29/23/31/33 in immutable SFC RTL.
// Default BufferParams: depth two, no flow and no pipe on all five channels.
#include "goldengate/FASEDHostMemoryBuffer.h"
#include "mlir/IR/Builders.h"
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDHostMemoryBuffer(CircuitOp circuit,
                                                  std::string &error) {
  constexpr StringLiteral topName="GGFASEDAddressTranslationWrapper";
  constexpr StringLiteral helperName="GGFASEDHostMemoryBuffer";
  const StringRef queueNames[]{"GGFASEDMemoryAddressQueue2", "GGFASEDMemoryWriteQueue2",
    "GGFASEDMemoryAckQueue2", "GGFASEDMemoryReadQueue2"};
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp top,inner,translation,engine;
  unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==helperName || llvm::is_contained(queueNames,n))return reject("FASED host memory buffer already exists");
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
    return reject("FASED host memory buffer requires the active translation boundary and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  if(!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||!has(widths,"idBits",4)||
     !has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("FASED host memory buffer supports the recorded sixteen-ID eight-beat profile");
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
      {"burst",2},{"size",3},{"len",8},{"addr",34}});
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
  auto mem=port(top,"fased_host_mem",master,Direction::Out);
  auto clock=port(top,"hostClock",ClockType::get(ctx),Direction::In);
  auto reset=port(top,"hostReset",uint(1),Direction::In);
  if(!mem || !clock || !reset || translation.getNumPorts()!=4 ||
     port(translation,"clock",ClockType::get(ctx),Direction::In)!=std::optional<unsigned>(0) ||
     port(translation,"reset",uint(1),Direction::In)!=std::optional<unsigned>(1) ||
     port(translation,"out",master,Direction::Out)!=std::optional<unsigned>(3))
    return reject("FASED host memory buffer requires exact host clock/reset and post-translation master");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==topName;});
  InstanceOp sim,trans,deint;unsigned instances=0;
  for(auto i:top.getOps<InstanceOp>()) {
    ++instances;
    if(i.getName()=="sim"&&i.getModuleName()==inner.getName())sim=i;
    if(i.getName()=="deinterleaver"&&i.getModuleName()=="GGFASEDReadDeinterleaver")deint=i;
    if(i.getName()=="translation"&&i.getModuleName()==translation.getName())trans=i;
  }
  if(used || instances!=3 || !sim || !trans || !deint)
    return reject("FASED host memory buffer requires the uninstantiated three-instance translation top");
  ConnectOp boundary;
  for(auto c:top.getOps<ConnectOp>())
    if(c.getDest()==top.getBodyBlock()->getArgument(*mem)&&c.getSrc()==trans.getResult(3))boundary=c;
  unsigned clk=0,rst=0;
  for(auto c:top.getOps<StrictConnectOp>()) {
    clk+=c.getDest()==trans.getResult(0)&&c.getSrc()==top.getBodyBlock()->getArgument(*clock);
    rst+=c.getDest()==trans.getResult(1)&&c.getSrc()==top.getBodyBlock()->getArgument(*reset);
  }
  if(!boundary || !trans.getResult(3).hasOneUse() || !top.getBodyBlock()->getArgument(*mem).hasOneUse() || clk!=1 || rst!=1)
    return reject("FASED host memory buffer needs the unique direct memory connection and host clock/reset binding");

  // Rejections precede mutation. Buffer the translated master in place so
  // all pre-existing module and annotation identities remain unchanged.
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto c=[&](Value d,Value v){b.create<StrictConnectOp>(loc,d,v);};
  auto k=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto mux=[&](Value p,Value y,Value n)->Value{return b.create<MuxPrimOp>(loc,p,y,n);};
  auto both=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto inv=[&](Value x)->Value{return b.create<NotPrimOp>(loc,x);};
  SmallVector<FModuleOp> queues;
  for(unsigned j=0;j<4;++j) {
    auto response=cast<BundleType>(master.getElements()[j==3?4:j].type);
    auto payload=cast<BundleType>(response.getElements()[2].type);
    unsigned width=0;
    for(auto e:payload.getElements())width+=*cast<UIntType>(e.type).getWidth();
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    SmallVector<PortInfo> qp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
      {b.getStringAttr("reset"),uint(1),Direction::In},
      {b.getStringAttr("enq"),response,Direction::In},{b.getStringAttr("deq"),response,Direction::Out}};
    auto queue=b.create<FModuleOp>(loc,b.getStringAttr(queueNames[j]),ConventionAttr::get(ctx,Convention::Internal),qp);
    queues.push_back(queue);b.setInsertionPointToStart(queue.getBodyBlock());
    auto reg=[&](StringRef name)->Value {
      return b.create<RegResetOp>(loc,uint(1),arg(queue,0),arg(queue,1),k(1,0),name).getResult();
    };
    Value ep=reg("enq_ptr_value"),dp=reg("deq_ptr_value"),mf=reg("maybe_full");
    Value eq=b.create<EQPrimOp>(loc,ep,dp),ready=inv(both(eq,mf)),valid=inv(both(eq,inv(mf)));
    Value enq=arg(queue,2),deq=arg(queue,3);
    Value push=both(ready,f(enq,"valid")),pop=both(valid,f(deq,"ready"));
    c(f(enq,"ready"),ready);c(f(deq,"valid"),valid);
    c(ep,mux(push,inv(ep),ep));c(dp,mux(pop,inv(dp),dp));
    c(mf,mux(b.create<XorPrimOp>(loc,push,pop),push,mf));
    SmallVector<Type> mt{MemOp::getTypeForPort(2,uint(width),MemOp::PortKind::Read),
                        MemOp::getTypeForPort(2,uint(width),MemOp::PortKind::Write)};
    SmallVector<Attribute> mn{b.getStringAttr("read"),b.getStringAttr("write")};
    auto ram=b.create<MemOp>(loc,mt,0,1,2,RUWAttr::Undefined,mn,"ram");
    Value rd=ram.getResult(0),wr=ram.getResult(1);
    c(f(rd,"clk"),arg(queue,0));c(f(rd,"en"),k(1,1));c(f(rd,"addr"),dp);
    // As in Chisel Queue, reset clears occupancy, not RAM or its write enable.
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
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},
    {b.getStringAttr("in"),master,Direction::In},{b.getStringAttr("out"),master,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  // AW and AR share a queue definition, but have independent state instances.
  const unsigned queueIndex[]{0,1,2,0,3};
  for(auto [j,ch]:llvm::enumerate(master.getElements())) {
    auto q=b.create<InstanceOp>(loc,queues[queueIndex[j]],ch.name.getValue().str()+"_queue");
    c(q.getResult(0),arg(helper,0));c(q.getResult(1),arg(helper,1));
    bool request=!ch.isFlip;
    Value enq=f(arg(helper,request?2:3),ch.name),deq=f(arg(helper,request?3:2),ch.name);
    c(f(q.getResult(2),"valid"),f(enq,"valid"));
    c(f(enq,"ready"),f(q.getResult(2),"ready"));
    c(f(deq,"valid"),f(q.getResult(3),"valid"));
    c(f(q.getResult(3),"ready"),f(deq,"ready"));
    auto payload=cast<BundleType>(cast<BundleType>(ch.type).getElements()[2].type);
    for(auto e:payload.getElements()) {
      c(f(f(q.getResult(2),"bits"),e.name),f(f(enq,"bits"),e.name));
      c(f(f(deq,"bits"),e.name),f(f(q.getResult(3),"bits"),e.name));
    }
  }
  b.setInsertionPoint(boundary);
  auto buffer=b.create<InstanceOp>(loc,helper,"memory_buffer");
  c(buffer.getResult(0),top.getBodyBlock()->getArgument(*clock));
  c(buffer.getResult(1),top.getBodyBlock()->getArgument(*reset));
  b.create<ConnectOp>(loc,buffer.getResult(2),trans.getResult(3));
  b.create<ConnectOp>(loc,top.getBodyBlock()->getArgument(*mem),buffer.getResult(3));
  boundary.erase();return success();
}
