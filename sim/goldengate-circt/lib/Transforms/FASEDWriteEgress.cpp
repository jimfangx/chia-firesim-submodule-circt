// See LICENSE for license details.
// Oracle: EgressUnit.scala WriteEgress, idReuse=1/2 constructor profiles.
// Host B always accepts; its response metadata is discarded except the ID.
// Counters wrap at log2Up(idReuse+1) bits. haveAck samples OLD nonzero counts on request start or
// retry, including host clocks without targetFire. Retry selects the current
// ID even on a simultaneous new request. Same-ID enqueue/retirement cancel.
// Only valid, haveAck and counters reset; the request ID remains unreset.
// Timing-model requests and target payload remain explicit pending lowering.
// Required input invariants: uninstantiated completed read scheduler boundary,
// exact host/target response ports and recorded 35/64/4-bit constructor.
// Annotations consumed: none; rawAnnotations is required and transferred.
// Annotations produced: renamed circuit/copied ports and mapped response leaves.
// IR mutations: append shared WriteEgress helper and wrapper; retain prior bodies.
// Analyses required: constructor, port shape, symbol collision and top-use checks.
// Analyses preserved: prior module bodies/ports; circuit hierarchy is extended.
// Output invariants: host B always accepts, counters wrap, old-counter retry
// sampling and qualified reset/fire match SFC; request ID remains unreset.
#include "goldengate/FASEDWriteEgress.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDWriteEgress(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDWriteEgress";
  bool isSelectedPrint = circuit.getName() == "GGSelectedPrintFASEDReadSchedulerWrapper";
  llvm::StringRef wrapperName = isSelectedPrint ? "GGSelectedPrintFASEDWriteEgressWrapper"
                                        : "GGFASEDWriteEgressWrapper";
  auto reject = [&](llvm::StringRef s) { error=s.str();return failure(); };
  if (!isSelectedPrint && circuit.getName() != "GGFASEDReadSchedulerWrapper")
    return reject("FASED write egress requires the active read scheduler wrapper");
  FModuleOp inner,engine;
  for (auto m:circuit.getOps<FModuleLike>()) {
    if (m.getModuleName()==helperName || m.getModuleName()==wrapperName)
      return reject("FASED write egress helper or wrapper already exists");
    if (m.getModuleName()==circuit.getName()) inner=dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName()=="GGFASEDTokenEngine") engine=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED write egress needs top, engine and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge=key?key.getAs<DictionaryAttr>("axi4Edge"):DictionaryAttr();
  auto widths=key?key.getAs<DictionaryAttr>("axi4Widths"):DictionaryAttr();
  auto has=[&](DictionaryAttr d,llvm::StringRef n,int v) {
    auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getValue().getBitWidth()<=64&&a.getInt()==v;
  };
  auto reuse=edge?edge.getAs<IntegerAttr>("idReuse"):IntegerAttr();
  auto flight=edge?edge.getAs<IntegerAttr>("maxFlight"):IntegerAttr();
  // maxRequests/maxReqLength do not enter the SFC WriteEgress logic.
  // Its acknowledgment width uses maxReqsPerId directly, without clamping
  // to maxFlight; maxFlight=1 still needs two bits when idReuse=2.
  if (!has(edge,"maxWriteTransfer",8)||!reuse||reuse.getValue().getBitWidth()>64||
      (reuse.getInt()!=1&&reuse.getInt()!=2)||!flight||
      flight.getValue().getBitWidth()>64||flight.getInt()<=0||
      !has(widths,"addrBits",35)||!has(widths,"dataBits",64)||!has(widths,"idBits",4))
    return reject("FASED write egress supports 35/64/4-bit, 8-beat, one or two requests per ID with positive maxFlight");
  unsigned counterWidth=reuse.getInt()==1?1:2;
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w) {return UIntType::get(ctx,w,false);};auto bit=uint(1);
  auto bundle=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> es) {
    SmallVector<BundleType::BundleElement> fields;
    for(auto [n,w]:es) fields.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,fields);
  };
  auto flat=bundle({{"bReady",1},{"bValid",1}});
  const llvm::StringRef names[]{"hostClock","fased_egress_reset","fased_tfire","fased_write_egress_valid","fased_host_write_responses"};
  const Type types[]{ClockType::get(ctx),bit,bit,bit,flat};
  const Direction dirs[]{Direction::In,Direction::Out,Direction::Out,Direction::In,Direction::In};
  unsigned indices[5];
  for(unsigned j=0;j<5;++j) {
    std::optional<unsigned> i;
    for(auto [n,p]:llvm::enumerate(inner.getPorts()))
      if(p.name==names[j]&&p.type==types[j]&&p.direction==dirs[j]) i=n;
    if(!i) return reject("FASED write egress needs exact clock/reset/fire/valid/host response boundaries");
    indices[j]=*i;
  }
  for(auto p:inner.getPorts())
    if(p.name=="fased_host_write_response"||p.name=="fased_write_egress_req"||p.name=="fased_write_egress_resp")
      return reject("FASED write egress boundary already exists");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used) return reject("FASED write egress needs an uninstantiated top");
  auto req=BundleType::get(ctx,{{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(4)}});
  auto write=bundle({{"user",1},{"id",4},{"resp",2}});
  auto token=BundleType::get(ctx,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,write}});
  auto resp=BundleType::get(ctx,{{b.getStringAttr("tReady"),true,bit},{b.getStringAttr("hValid"),false,bit},{b.getStringAttr("tBits"),false,write}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),types[0],Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("targetFire"),bit,Direction::In},
      {b.getStringAttr("req"),req,Direction::In},{b.getStringAttr("enq"),token,Direction::In},
      {b.getStringAttr("resp"),resp,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto constant=[&](unsigned w,uint64_t v)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,v));};
  auto both=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto either=[&](Value a,Value z)->Value{return b.create<OrPrimOp>(loc,a,z);};
  auto neg=[&](Value a)->Value{return b.create<NotPrimOp>(loc,a);};
  auto eq=[&](Value a,Value z)->Value{return b.create<EQPrimOp>(loc,a,z);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  auto reg=[&](llvm::StringRef name,unsigned width=1)->Value {
    return b.create<RegResetOp>(loc,uint(width),arg(0),arg(1),constant(width,0),name).getResult();
  };
  Value valid=reg("currReqReg_valid"),ack=reg("haveAck");
  Value id=b.create<RegOp>(loc,uint(4),arg(0),"currReqReg_bits").getResult();
  SmallVector<Value> counters;
  for(unsigned i=0;i<16;++i) counters.push_back(reg("ackCounters_"+std::to_string(i),counterWidth));
  Value start=both(arg(2),field(arg(3),"valid"));
  Value retire=both(both(both(arg(2),valid),ack),field(arg(5),"tReady"));
  Value retry=both(valid,neg(ack)),deqId=mux(retry,id,field(arg(3),"bits"));
  Value selected=counters.front();
  for(unsigned i=1;i<16;++i) selected=mux(eq(deqId,constant(4,i)),counters[i],selected);
  if(counterWidth>1) selected=neg(eq(selected,constant(counterWidth,0)));
  connect(valid,mux(start,constant(1,1),mux(retire,constant(1,0),valid)));
  connect(id,mux(start,field(arg(3),"bits"),id));
  connect(ack,mux(either(retry,start),selected,ack));
  Value enq=field(arg(4),"valid"),enqId=field(field(arg(4),"bits"),"id");
  Value cancel=both(both(retire,enq),eq(id,enqId));
  for(unsigned i=0;i<16;++i) {
    if(counterWidth==1) {
      Value change=both(neg(cancel),either(both(enq,eq(enqId,constant(4,i))),both(retire,eq(id,constant(4,i)))));
      // Width one: increment and decrement both toggle, without saturation.
      connect(counters[i],mux(change,neg(counters[i]),counters[i]));
    } else {
      Value increment=b.create<AddPrimOp>(loc,counters[i],constant(1,1));
      Value decrement=b.create<SubPrimOp>(loc,counters[i],constant(1,1));
      // FIRRTL arithmetic widens; assignment truncation wraps at the register
      // width, including overflow and underflow. Do not saturate at idReuse.
      increment=b.create<BitsPrimOp>(loc,increment,counterWidth-1,0);
      decrement=b.create<BitsPrimOp>(loc,decrement,counterWidth-1,0);
      Value next=mux(both(enq,eq(enqId,constant(4,i))),increment,
                     mux(both(retire,eq(id,constant(4,i))),decrement,counters[i]));
      connect(counters[i],mux(cancel,counters[i],next));
    }
  }
  connect(field(arg(4),"ready"),constant(1,1));
  connect(field(arg(5),"hValid"),either(neg(valid),ack));
  Value out=field(arg(5),"tBits");connect(field(out,"id"),id);
  connect(field(out,"resp"),constant(2,0));connect(field(out,"user"),constant(1,0));

  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))
    if(i!=indices[3]&&i!=indices[4]) {copied.push_back(i);ports.push_back(p);}
  unsigned added=ports.size();
  ports.push_back({b.getStringAttr("fased_host_write_response"),token,Direction::In});
  ports.push_back({b.getStringAttr("fased_write_egress_req"),req,Direction::In});
  ports.push_back({b.getStringAttr("fased_write_egress_resp"),resp,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),egress=b.create<InstanceOp>(loc,helper,"writeEgress");
  for(auto [j,i]:llvm::enumerate(copied)) {
    auto p=inner.getPorts()[i];Value v=wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,p.direction==Direction::In?v:sim.getResult(i));
  }
  auto extra=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(added+i);};
  connect(egress.getResult(0),wrapper.getBodyBlock()->getArgument(llvm::find(copied,indices[0])-copied.begin()));
  connect(egress.getResult(1),sim.getResult(indices[1]));connect(egress.getResult(2),sim.getResult(indices[2]));
  connect(egress.getResult(3),extra(1));b.create<ConnectOp>(loc,egress.getResult(4),extra(0));
  b.create<ConnectOp>(loc,extra(2),egress.getResult(5));
  connect(sim.getResult(indices[3]),field(egress.getResult(5),"hValid"));
  connect(field(sim.getResult(indices[4]),"bReady"),field(extra(0),"ready"));
  connect(field(sim.getResult(indices[4]),"bValid"),field(extra(0),"valid"));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute attr)->Attribute {
    if(auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);if(!v.consume_front(oldPrefix+"|"))return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));
        for(auto i:copied)if(n==inner.getPortName(i)){suffix="|"+wrapperName.str()+">"+ref.str();break;}
        if(ref=="fased_write_egress_valid")suffix="|"+wrapperName.str()+">fased_write_egress_resp.hValid";
        if(ref=="fased_host_write_responses.bReady")suffix="|"+wrapperName.str()+">fased_host_write_response.ready";
        if(ref=="fased_host_write_responses.bValid")suffix="|"+wrapperName.str()+">fased_host_write_response.valid";
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto a=dyn_cast<ArrayAttr>(attr)){SmallVector<Attribute> values;for(auto v:a)values.push_back(retarget(v));return b.getArrayAttr(values);}
    if(auto d=dyn_cast<DictionaryAttr>(attr)){NamedAttrList values;for(auto v:d)values.set(v.getName(),retarget(v.getValue()));return values.getDictionary(ctx);}
    return attr;
  };
  SmallVector<Attribute> annotations;for(auto a:raw)annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));circuit.setName(wrapperName);return success();
}
