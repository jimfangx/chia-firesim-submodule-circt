// See LICENSE for license details.
// Oracle: IngressUnit.scala host issue policy and do_hwrite_data_reg.
// Requires uninstantiated order wrapper and exact Rocket queues/status/host
// handshake boundaries. All validation precedes mutation.
// Consumes raw AW/W/AR dequeue, order dequeue and host transaction boundary.
// Produces gated host requests, connects actual acceptance to queue retirement,
// order retirement and host outstanding counters; exposes only response-side
// handshake inputs. Host issue is independent of targetFire and reset level.
// AW acceptance wins over accepted final W in the strict write-data register.
// Qualified ingress reset clears that register, but never suppresses requests.
#include "goldengate/FASEDIngressIssue.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addFASEDIngressIssue(CircuitOp circuit,std::string &error) {
  constexpr llvm::StringLiteral helperName="GGFASEDIngressIssue";
  constexpr llvm::StringLiteral wrapperName="GGFASEDIngressIssueWrapper";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!="GGFASEDIngressOrderWrapper")return reject("FASED ingress issue needs active order wrapper");
  FModuleOp inner,engine;
  for(auto m:circuit.getOps<FModuleLike>()){
    if(m.getModuleName()==helperName||m.getModuleName()==wrapperName)return reject("FASED ingress issue helper or wrapper already exists");
    if(m.getModuleName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getModuleName()=="GGFASEDTokenEngine")engine=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!inner||!engine||!raw)return reject("FASED ingress issue needs top, engine and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge=key?key.getAs<DictionaryAttr>("axi4Edge"):DictionaryAttr();
  auto flight=edge?edge.getAs<IntegerAttr>("maxFlight"):IntegerAttr();
  if(!flight||flight.getInt()!=10)return reject("FASED ingress issue supports recorded maxFlight=10 profile");
  auto *context=circuit.getContext();OpBuilder b(context);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(context,w,false);};auto bit=uint(1);
  auto bundle=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){
    SmallVector<BundleType::BundleElement> e;for(auto [n,w]:fields)e.push_back({b.getStringAttr(n),false,uint(w)});return BundleType::get(context,e);
  };
  auto address=bundle({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=bundle({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto token=[&](FIRRTLBaseType t){return BundleType::get(context,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,t}});};
  auto status=bundle({{"awValue",4},{"wValue",4},{"awEmpty",1},{"wEmpty",1},{"writeReqDone",1}});
  auto transactions=bundle({{"arReady",1},{"arValid",1},{"rReady",1},{"rValid",1},{"rLast",1},{"awReady",1},{"awValid",1},{"bReady",1},{"bValid",1}});
  const llvm::StringRef required[]{"hostClock","fased_ingress_reset","fased_ingress_relaxed","fased_host_mem_idle","fased_host_read_inflight","fased_ingress_credits","fased_ingress_order","fased_ingress_aw_deq","fased_ingress_w_deq","fased_ingress_ar_deq","fased_host_transactions"};
  const Type types[]{ClockType::get(context),bit,bit,bit,bit,status,token(bit),token(address),token(data),token(address),transactions};unsigned indices[11];
  for(unsigned j=0;j<11;++j){std::optional<unsigned> index;
    auto direction=(j==0||j==2||j==10)?Direction::In:Direction::Out;
    for(auto [i,p]:llvm::enumerate(inner.getPorts()))if(p.name==required[j]&&p.type==types[j]&&p.direction==direction)index=i;
    if(!index)return reject("FASED ingress issue needs exact clock, reset, policy, status, queue and transaction boundaries");indices[j]=*index;
  }
  for(auto p:inner.getPorts())if(p.name=="fased_host_requests"||p.name=="fased_host_responses")return reject("FASED ingress issue boundary already exists");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});if(used)return reject("FASED ingress issue needs uninstantiated top");
  auto requests=BundleType::get(context,{{b.getStringAttr("aw"),false,token(address)},{b.getStringAttr("w"),false,token(data)},{b.getStringAttr("ar"),false,token(address)}});
  auto responses=bundle({{"rReady",1},{"rValid",1},{"rLast",1},{"bReady",1},{"bValid",1}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp;
  for(auto n:{"clock","reset","relaxed","hostMemIdle","hostReadInflight","awEmpty","wEmpty"})hp.push_back({b.getStringAttr(n),n==llvm::StringRef("clock")?Type(ClockType::get(context)):Type(bit),Direction::In});
  hp.push_back({b.getStringAttr("order"),token(bit),Direction::In});hp.push_back({b.getStringAttr("aw"),token(address),Direction::In});hp.push_back({b.getStringAttr("w"),token(data),Direction::In});hp.push_back({b.getStringAttr("ar"),token(address),Direction::In});hp.push_back({b.getStringAttr("requests"),requests,Direction::Out});
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(context,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto both=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto either=[&](Value a,Value z)->Value{return b.create<OrPrimOp>(loc,a,z);};
  auto neg=[&](Value v)->Value{return b.create<NotPrimOp>(loc,v);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  Value zero=b.create<ConstantOp>(loc,bit,APInt(1,0)),one=b.create<ConstantOp>(loc,bit,APInt(1,1));
  Value writeData=b.create<RegResetOp>(loc,bit,arg(0),arg(1),zero,"do_hwrite_data_reg").getResult();
  Value read=either(arg(2),both(both(either(arg(3),arg(4)),field(arg(7),"valid")),field(arg(7),"bits")));
  Value write=mux(arg(2),neg(arg(5)),both(both(arg(3),field(arg(7),"valid")),neg(field(arg(7),"bits"))));
  Value writeBeat=mux(arg(2),neg(arg(6)),writeData);
  auto gate=[&](unsigned i,llvm::StringRef n,Value enable){
    Value request=field(arg(11),n);connect(field(request,"valid"),both(enable,field(arg(i),"valid")));connect(field(arg(i),"ready"),both(enable,field(request,"ready")));
    b.create<ConnectOp>(loc,field(request,"bits"),field(arg(i),"bits"));
    return both(field(request,"valid"),field(request,"ready"));
  };
  Value awFire=gate(8,"aw",write),wFire=gate(9,"w",writeBeat),arFire=gate(10,"ar",read);
  connect(field(arg(7),"ready"),either(awFire,arFire));
  connect(writeData,mux(awFire,one,mux(both(wFire,field(field(arg(9),"bits"),"last")),zero,writeData)));

  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))if(i!=indices[6]&&i!=indices[7]&&i!=indices[8]&&i!=indices[9]&&i!=indices[10]){copied.push_back(i);ports.push_back(p);}
  unsigned added=ports.size();ports.push_back({b.getStringAttr("fased_host_requests"),requests,Direction::Out});ports.push_back({b.getStringAttr("fased_host_responses"),responses,Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());auto sim=b.create<InstanceOp>(loc,inner,"sim");auto issue=b.create<InstanceOp>(loc,helper,"ingressIssue");
  auto outer=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(llvm::find(copied,i)-copied.begin());};auto extra=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(added+i);};
  for(auto [j,i]:llvm::enumerate(copied)){auto p=inner.getPorts()[i];Value v=wrapper.getBodyBlock()->getArgument(j);b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,p.direction==Direction::In?v:sim.getResult(i));}
  connect(issue.getResult(0),outer(indices[0]));connect(issue.getResult(1),sim.getResult(indices[1]));connect(issue.getResult(2),outer(indices[2]));connect(issue.getResult(3),sim.getResult(indices[3]));connect(issue.getResult(4),sim.getResult(indices[4]));
  connect(issue.getResult(5),field(sim.getResult(indices[5]),"awEmpty"));connect(issue.getResult(6),field(sim.getResult(indices[5]),"wEmpty"));
  for(unsigned j=7;j<=10;++j)b.create<ConnectOp>(loc,issue.getResult(j),sim.getResult(indices[j-1]));
  b.create<ConnectOp>(loc,extra(0),issue.getResult(11));
  for(auto n:{"rReady","rValid","rLast","bReady","bValid"})connect(field(sim.getResult(indices[10]),n),field(extra(1),n));
  for(auto [n,ch,leaf]:{std::tuple<llvm::StringRef,llvm::StringRef,llvm::StringRef>{"arReady","ar","ready"},{"arValid","ar","valid"},{"awReady","aw","ready"},{"awValid","aw","valid"}})
    connect(field(sim.getResult(indices[10]),n),field(field(extra(0),ch),leaf));
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
      // Transfer consumed transaction leaves to the actual request/response
      // boundary. Whole bundles and raw queue identities stay on inner sim.
      llvm::StringRef transactionRef(suffix);
      if (transactionRef.consume_front("|" + inner.getName().str() + ">fased_host_transactions.")) {
        std::string transactionLeaf = transactionRef.str();
        for (auto n : {"rReady","rValid","rLast","bReady","bValid"})
          if (transactionLeaf == n) suffix = "|" + wrapperName.str() + ">fased_host_responses." + std::string(n);
        for (auto [n,ch,leaf] : {std::tuple<llvm::StringRef,llvm::StringRef,llvm::StringRef>{"arReady","ar","ready"},{"arValid","ar","valid"},{"awReady","aw","ready"},{"awValid","aw","valid"}})
          if (transactionLeaf == n) suffix = "|" + wrapperName.str() + ">fased_host_requests." + ch.str() + "." + leaf.str();
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
