// See LICENSE for license details.
// Oracle: IngressUnit.scala wCredits/awCredits/write_req_done and Lib.scala.
// Requires: uninstantiated AR ingress wrapper, exact enqueue pulses/dequeue
// channels, qualified ingress reset, retained annotations, maxFlight=10.
// Preserves all existing ports; adds relaxed ordering input and credit status.
// Host issue gates and transaction-order FIFO are subsequent transforms. Their
// accepted AW/final-W dequeue handshakes feed the relaxed retirement rules.
// Two host-clocked four-bit counters saturate at ten; strict mode consumes
// both credits on a completed write. Completion uses pre-edge counter values
// and remains combinational during reset, matching the SFC ordering enqueue.
#include "goldengate/FASEDIngressCredits.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addFASEDIngressCredits(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDIngressCredits";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDIngressCreditsWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDIngressARQueueWrapper")
    return reject("FASED ingress credits require the active AR ingress wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("FASED ingress credit helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED ingress credits need top, engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!flight || flight.getInt() != 10)
    return reject("FASED ingress credits support the recorded maxFlight=10 profile");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1), countType = uint(4);
  auto bundle = [&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields) {
    SmallVector<BundleType::BundleElement> elements;
    for (auto [n,w] : fields) elements.push_back({b.getStringAttr(n), false, uint(w)});
    return BundleType::get(context, elements);
  };
  auto address = bundle({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},
      {"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = bundle({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto token = [&](BundleType payload) { return BundleType::get(context,
      {{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},
       {b.getStringAttr("bits"),false,payload}}); };
  const llvm::StringRef required[]{"hostClock","fased_ingress_reset",
      "fased_ingress_aw_enq_fire","fased_ingress_w_last_fire",
      "fased_ingress_aw_deq","fased_ingress_w_deq"};
  const Type types[]{ClockType::get(context),bit,bit,bit,token(address),token(data)};
  unsigned indices[6];
  for (unsigned j=0;j<6;++j) {
    std::optional<unsigned> index;
    for (auto [i,p] : llvm::enumerate(inner.getPorts()))
      if (p.name == required[j] && p.type == types[j] &&
          p.direction == (j==0 ? Direction::In : Direction::Out)) index=i;
    if (!index) return reject("FASED ingress credits need exact clock, reset, enqueue pulses and dequeue channels");
    indices[j]=*index;
  }
  for (auto p : inner.getPorts())
    if (p.name == "fased_ingress_relaxed" || p.name == "fased_ingress_credits")
      return reject("FASED ingress credit boundary already exists");
  bool used=false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName()==inner.getName(); });
  if (used) return reject("FASED ingress credits need an uninstantiated top");
  auto status = BundleType::get(context, {{b.getStringAttr("awValue"),false,countType},
      {b.getStringAttr("wValue"),false,countType},{b.getStringAttr("awEmpty"),false,bit},
      {b.getStringAttr("wEmpty"),false,bit},{b.getStringAttr("writeReqDone"),false,bit}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(context),Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("relaxed"),bit,Direction::In},
      {b.getStringAttr("awEnqFire"),bit,Direction::In},{b.getStringAttr("wLastEnqFire"),bit,Direction::In},
      {b.getStringAttr("awDeqFire"),bit,Direction::In},{b.getStringAttr("wLastDeqFire"),bit,Direction::In},
      {b.getStringAttr("status"),status,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(context,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto constant=[&](unsigned n)->Value {return b.create<ConstantOp>(loc,countType,APInt(4,n));};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto both=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto neg=[&](Value v)->Value{return b.create<NotPrimOp>(loc,v);};
  auto either=[&](Value a,Value z)->Value{return b.create<OrPrimOp>(loc,a,z);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  Value aw=b.create<RegResetOp>(loc,countType,arg(0),arg(1),constant(0),"awCredits").getResult();
  Value w=b.create<RegResetOp>(loc,countType,arg(0),arg(1),constant(0),"wCredits").getResult();
  Value awEmpty=b.create<EQPrimOp>(loc,aw,constant(0)), wEmpty=b.create<EQPrimOp>(loc,w,constant(0));
  Value done=either(either(both(b.create<LTPrimOp>(loc,w,aw),arg(3)),
                          both(b.create<LTPrimOp>(loc,aw,w),arg(4))),both(arg(3),arg(4)));
  auto update=[&](Value value,Value inc,Value dec,Value empty){
    Value full=b.create<GEQPrimOp>(loc,value,constant(10));
    Value up=both(both(inc,neg(dec)),neg(full)), down=both(both(neg(inc),dec),neg(empty));
    Value plus=b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,value,constant(1)),3,0);
    Value minus=b.create<BitsPrimOp>(loc,b.create<SubPrimOp>(loc,value,constant(1)),3,0);
    connect(value,mux(up,plus,mux(down,minus,value)));
  };
  update(aw,arg(4),mux(arg(2),arg(5),done),awEmpty);
  update(w,arg(3),mux(arg(2),arg(6),done),wEmpty);
  connect(field(arg(7),"awValue"),aw);connect(field(arg(7),"wValue"),w);
  connect(field(arg(7),"awEmpty"),awEmpty);connect(field(arg(7),"wEmpty"),wEmpty);
  connect(field(arg(7),"writeReqDone"),done);

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) {copied.push_back(i);ports.push_back(p);}
  unsigned added=ports.size();
  ports.push_back({b.getStringAttr("fased_ingress_relaxed"),bit,Direction::In});
  ports.push_back({b.getStringAttr("fased_ingress_credits"),status,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim");
  auto credits=b.create<InstanceOp>(loc,helper,"ingressCredits");
  auto outer=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  for (auto [i,p] : llvm::enumerate(inner.getPorts()))
    b.create<ConnectOp>(loc,p.direction==Direction::In ? sim.getResult(i) : outer(i),
                           p.direction==Direction::In ? outer(i) : sim.getResult(i));
  connect(credits.getResult(0),outer(indices[0]));connect(credits.getResult(1),sim.getResult(indices[1]));
  connect(credits.getResult(2),outer(added));
  connect(credits.getResult(3),sim.getResult(indices[2]));connect(credits.getResult(4),sim.getResult(indices[3]));
  auto accepted=[&](unsigned index){return both(field(outer(index),"valid"),field(outer(index),"ready"));};
  connect(credits.getResult(5),accepted(indices[4]));
  connect(credits.getResult(6),both(accepted(indices[5]),field(field(outer(indices[5]),"bits"),"last")));
  b.create<ConnectOp>(loc,outer(added+1),credits.getResult(7));
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
