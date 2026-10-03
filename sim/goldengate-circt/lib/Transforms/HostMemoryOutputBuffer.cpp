// See LICENSE for license details.
// Oracle: FPGATop.axi4buf (AXI4Buffer), Queue_22/23/24/26 in SFC RTL.
// Input: recorded Rocket constructor, complete host_mem at the uninstantiated
// GGHostMemoryReadResponseWrapper, five unique channel bindings.
// Mutation: insert host-clock two-entry, no-flow/no-pipe queues on AW/W/B/AR/R.
// Annotations: none consumed or produced; all module/port identities retained.
// Reset clears occupancy; RAM writes remain enabled on an accepted reset edge.
#include "goldengate/HostMemoryOutputBuffer.h"
#include "mlir/IR/Builders.h"
#include <array>
#include <optional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addHostMemoryOutputBuffer(CircuitOp circuit,
                                                    std::string &error) {
  constexpr StringLiteral topName="GGHostMemoryReadResponseWrapper";
  constexpr StringLiteral helperName="GGHostMemoryOutputBuffer";
  const StringRef queueNames[]{"GGHostMemoryAddressQueue2", "GGHostMemoryWriteQueue2",
    "GGHostMemoryAckQueue2", "GGHostMemoryReadQueue2"};
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp top,engine;unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();
    if(n==helperName || llvm::is_contained(queueNames,n))return reject("host memory output buffer already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==topName)top=f;
    if(auto k=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
      if(k.getAs<DictionaryAttr>("axi4Edge")){engine=f;++engines;}
  }
  if(circuit.getName()!=topName || !top || !engine || engines!=1 ||
     engine.getName()!="GGFASEDTokenEngine" || !circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("host memory output buffer requires the R-response boundary and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths=key.getAs<DictionaryAttr>("axi4Widths"),edge=key.getAs<DictionaryAttr>("axi4Edge");
  auto has=[](DictionaryAttr d,StringRef n,int v){auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  if(!has(widths,"addrBits",35)||!has(widths,"dataBits",64)||!has(widths,"idBits",4)||
     !has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10))
    return reject("host memory output buffer supports the recorded Rocket profile");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
    SmallVector<BundleType::BundleElement> es;
    for(auto [n,w]:fs)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},
      {b.getStringAttr("bits"),false,BundleType::get(ctx,es)}});
  };
  auto address=token({{"id",5},{"qos",4},{"prot",3},{"cache",4},{"lock",1},
      {"burst",2},{"size",3},{"len",8},{"addr",34}});
  auto master=BundleType::get(ctx,{{b.getStringAttr("aw"),false,address},
    {b.getStringAttr("w"),false,token({{"strb",8},{"last",1},{"data",64}})},
    {b.getStringAttr("b"),true,token({{"id",5},{"resp",2}})},
    {b.getStringAttr("ar"),false,address},
    {b.getStringAttr("r"),true,token({{"id",5},{"last",1},{"data",64},{"resp",2}})}});
  auto port=[&](StringRef n,Type t,Direction d)->std::optional<unsigned>{
    for(auto [i,p]:llvm::enumerate(top.getPorts()))
      if(p.name==n&&p.type==t&&p.direction==d)return i;
    return std::nullopt;
  };
  auto mem=port("host_mem",master,Direction::Out);
  auto clock=port("hostClock",ClockType::get(ctx),Direction::In);
  auto reset=port("hostReset",uint(1),Direction::In);
  if(!mem || !clock || !reset)
    return reject("host memory output buffer requires exact five-bit host master and clock/reset ports");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==topName;});
  InstanceOp sim,router;unsigned instances=0;
  for(auto i:top.getOps<InstanceOp>()) {
    ++instances;
    if(i.getName()=="sim"&&i.getModuleName()=="GGHostMemoryReadWrapper")sim=i;
    if(i.getName()=="read_responses"&&i.getModuleName()=="GGHostMemoryReadResponseRouter")router=i;
  }
  if(used||instances!=2||!sim||!router||router.getNumResults()!=3)
    return reject("host memory output buffer requires the unique two-instance response top");
  auto simPort=[&](StringRef n)->Value {
    for(auto [i,p]:llvm::enumerate(sim.getPortNames()))
      if(cast<StringAttr>(p).getValue()==n)return sim.getResult(i);
    return {};
  };
  auto fieldIs=[](Value v,Value root,StringRef n){auto f=v.getDefiningOp<SubfieldOp>();return f&&f.getInput()==root&&f.getFieldName()==n;};
  Value host=top.getBodyBlock()->getArgument(*mem), sm=simPort("host_mem");
  std::array<SubfieldOp,5> fields;unsigned count=0;
  for(auto &use:host.getUses()) {
    auto f=dyn_cast<SubfieldOp>(use.getOwner());
    if(!f||!f.getResult().hasOneUse())return reject("host memory channel must have one direct boundary use");
    auto index=master.getElementIndex(f.getFieldName());
    if(!index||fields[*index])return reject("duplicate host memory channel boundary");
    fields[*index]=f;++count;
  }
  unsigned bindings=0,clocks=0,resets=0;
  for(auto c:top.getOps<ConnectOp>()) {
    for(unsigned j:{0u,1u,3u})
      bindings+=fieldIs(c.getDest(),host,master.getElements()[j].name)&&fieldIs(c.getSrc(),sm,master.getElements()[j].name);
    bindings+=fieldIs(c.getDest(),sm,"b")&&fieldIs(c.getSrc(),host,"b");
    bindings+=c.getDest()==router.getResult(0)&&fieldIs(c.getSrc(),host,"r");
    clocks+=c.getDest()==simPort("hostClock")&&c.getSrc()==top.getBodyBlock()->getArgument(*clock);
    resets+=c.getDest()==simPort("hostReset")&&c.getSrc()==top.getBodyBlock()->getArgument(*reset);
  }
  if(count!=5||bindings!=5||clocks!=1||resets!=1)
    return reject("host memory output buffer requires all five direct host bindings and sim clock/reset");
  // Rejections precede mutation. Buffer the unified master in place so
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
  b.setInsertionPointToStart(top.getBodyBlock());
  auto buffer=b.create<InstanceOp>(loc,helper,"host_memory_buffer");
  c(buffer.getResult(0),top.getBodyBlock()->getArgument(*clock));
  c(buffer.getResult(1),top.getBodyBlock()->getArgument(*reset));
  b.create<ConnectOp>(loc,host,buffer.getResult(3));
  // Existing directional channel connects now bind the buffer input. Responses
  // flow toward the router; requests flow toward the output buffer queues.
  for(auto field:fields)field->setOperand(0,buffer.getResult(2));
  return success();
}
