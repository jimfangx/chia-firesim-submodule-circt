// See LICENSE for license details.
// Oracle: IngressUnit.scala xaction_order, Util.scala DualQueue, SFC Queue_10.
// Requires active uninstantiated credit wrapper, qualified queue reset,
// accepted AR pulse, completed-write status and recorded maxFlight=10 profile.
// Preserves all ports/annotations; adds transaction-order dequeue and enqueue
// readiness. Read=true and write=false, read precedes write on dual enqueue.
// Host issue policy will consume the dequeue. Enqueue readiness is observable
// but does not gate already accepted requests, matching the Scala ingress.
// Two 10-entry asynchronous one-bit RAM banks; reset flushes pointers only.
#include "goldengate/FASEDIngressOrder.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addFASEDIngressOrder(CircuitOp circuit,std::string &error) {
  constexpr llvm::StringLiteral helperName="GGFASEDIngressOrder20";
  constexpr llvm::StringLiteral wrapperName="GGFASEDIngressOrderWrapper";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!="GGFASEDIngressCreditsWrapper")
    return reject("FASED ingress order needs the active credit wrapper");
  FModuleOp inner,engine;
  for(auto m:circuit.getOps<FModuleLike>()){
    if(m.getModuleName()==helperName||m.getModuleName()==wrapperName)
      return reject("FASED ingress order helper or wrapper already exists");
    if(m.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getModuleName()=="GGFASEDTokenEngine")engine=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!inner||!engine||!raw)return reject("FASED ingress order needs top, engine and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge=key?key.getAs<DictionaryAttr>("axi4Edge"):DictionaryAttr();
  auto flight=edge?edge.getAs<IntegerAttr>("maxFlight"):IntegerAttr();
  if(!flight||flight.getInt()!=10)return reject("FASED ingress order supports recorded maxFlight=10 profile");
  auto *context=circuit.getContext();OpBuilder b(context);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(context,w,false);};auto bit=uint(1);
  auto status=BundleType::get(context,{{b.getStringAttr("awValue"),false,uint(4)},
      {b.getStringAttr("wValue"),false,uint(4)},{b.getStringAttr("awEmpty"),false,bit},
      {b.getStringAttr("wEmpty"),false,bit},{b.getStringAttr("writeReqDone"),false,bit}});
  const llvm::StringRef required[]{"hostClock","fased_ingress_reset","fased_ingress_ar_enq_fire","fased_ingress_credits"};
  const Type types[]{ClockType::get(context),bit,bit,status};unsigned indices[4];
  for(unsigned j=0;j<4;++j){
    std::optional<unsigned> index;
    for(auto [i,p]:llvm::enumerate(inner.getPorts()))
      if(p.name==required[j]&&p.type==types[j]&&p.direction==(j==0?Direction::In:Direction::Out))index=i;
    if(!index)return reject("FASED ingress order needs exact clock, reset, AR pulse and credit status");
    indices[j]=*index;
  }
  for(auto p:inner.getPorts())if(p.name=="fased_ingress_order"||p.name=="fased_ingress_order_ready")
    return reject("FASED ingress order boundary already exists");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used)return reject("FASED ingress order needs uninstantiated top");
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp;
  for(auto n:{"clock","reset","enqA_valid","enqB_valid","deq_ready"})
    hp.push_back({b.getStringAttr(n),n==llvm::StringRef("clock")?Type(ClockType::get(context)):Type(bit),Direction::In});
  for(auto n:{"enqA_ready","enqB_ready","deq_valid","deq_bits"})hp.push_back({b.getStringAttr(n),bit,Direction::Out});
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(context,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto constant=[&](unsigned w,unsigned n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto both=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto neg=[&](Value v)->Value{return b.create<NotPrimOp>(loc,v);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  auto reg=[&](unsigned w,llvm::StringRef n)->Value{return b.create<RegResetOp>(loc,uint(w),arg(0),arg(1),constant(w,0),n).getResult();};
  Value enqPointer=reg(1,"enqPointer"),deqPointer=reg(1,"deqPointer");
  Value swap=b.create<XorPrimOp>(loc,enqPointer,neg(arg(2)));
  struct Bank {Value ready,valid,data;};
  auto bank=[&](llvm::StringRef name,Value valid,Value data,Value ready){
    Value ep=reg(4,(name+"_enq_ptr_value").str()),dp=reg(4,(name+"_deq_ptr_value").str());
    Value mf=reg(1,(name+"_maybe_full").str());Value equal=b.create<EQPrimOp>(loc,ep,dp);
    Value er=neg(both(equal,mf)),dv=neg(both(equal,neg(mf)));
    Value push=both(er,valid),pop=both(dv,ready);
    auto inc=[&](Value v)->Value{
      Value plus=b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,v,constant(4,1)),3,0);
      return mux(b.create<EQPrimOp>(loc,v,constant(4,9)),constant(4,0),plus);
    };
    connect(ep,mux(push,inc(ep),ep));connect(dp,mux(pop,inc(dp),dp));
    connect(mf,mux(b.create<XorPrimOp>(loc,push,pop),push,mf));
    SmallVector<Type> mt{MemOp::getTypeForPort(10,bit,MemOp::PortKind::Read),MemOp::getTypeForPort(10,bit,MemOp::PortKind::Write)};
    SmallVector<Attribute> mn{b.getStringAttr("read"),b.getStringAttr("write")};
    auto ram=b.create<MemOp>(loc,mt,0,1,10,RUWAttr::Undefined,mn,(name+"_ram").str());
    auto rd=ram.getResult(0),wr=ram.getResult(1);
    connect(field(rd,"clk"),arg(0));connect(field(rd,"en"),constant(1,1));connect(field(rd,"addr"),dp);
    connect(field(wr,"clk"),arg(0));connect(field(wr,"en"),push);connect(field(wr,"mask"),constant(1,1));
    connect(field(wr,"addr"),ep);connect(field(wr,"data"),data);
    return Bank{er,dv,field(rd,"data")};
  };
  Bank a=bank("qA",mux(swap,arg(3),arg(2)),neg(swap),both(neg(deqPointer),arg(4)));
  Bank z=bank("qB",mux(swap,arg(2),arg(3)),swap,both(deqPointer,arg(4)));
  Value ar=mux(swap,z.ready,a.ready),br=mux(swap,a.ready,z.ready);
  Value dv=mux(deqPointer,z.valid,a.valid),data=mux(deqPointer,z.data,a.data);
  connect(arg(5),ar);connect(arg(6),br);connect(arg(7),dv);connect(arg(8),data);
  Value single=b.create<XorPrimOp>(loc,both(ar,arg(2)),both(br,arg(3)));
  connect(enqPointer,mux(single,neg(enqPointer),enqPointer));
  connect(deqPointer,mux(both(arg(4),dv),neg(deqPointer),deqPointer));

  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts())){copied.push_back(i);ports.push_back(p);}
  unsigned added=ports.size();
  auto token=BundleType::get(context,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bit}});
  auto readiness=BundleType::get(context,{{b.getStringAttr("read"),false,bit},{b.getStringAttr("write"),false,bit}});
  ports.push_back({b.getStringAttr("fased_ingress_order"),token,Direction::Out});
  ports.push_back({b.getStringAttr("fased_ingress_order_ready"),readiness,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());auto sim=b.create<InstanceOp>(loc,inner,"sim");
  auto order=b.create<InstanceOp>(loc,helper,"xactionOrder");
  auto outer=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):outer(i),p.direction==Direction::In?outer(i):sim.getResult(i));
  connect(order.getResult(0),outer(indices[0]));connect(order.getResult(1),sim.getResult(indices[1]));
  connect(order.getResult(2),sim.getResult(indices[2]));connect(order.getResult(3),field(sim.getResult(indices[3]),"writeReqDone"));
  connect(order.getResult(4),field(outer(added),"ready"));
  connect(field(outer(added),"valid"),order.getResult(7));connect(field(outer(added),"bits"),order.getResult(8));
  connect(field(outer(added+1),"read"),order.getResult(5));connect(field(outer(added+1),"write"),order.getResult(6));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue();
      if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
