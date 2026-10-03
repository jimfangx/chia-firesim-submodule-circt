// See LICENSE for license details.
// Oracle: TimingModel.scala 108-114 and 236-239, widgets/Lib.scala 71-86.
// Required invariants: uninstantiated GGFASEDTimingAWQueueWrapper, recorded
// 35/64/4-bit no-LLC model, exact clock/reset/fire/request/pairing boundaries.
// Annotations consumed/produced: none; copied targets move to the new top;
// consumed pairing targets remain on the instantiated inner module.
// IR mutations: two four-bit saturating counters and combinational AW/W pairing;
// bind accepted AW and last W handshakes and retire both on target B.fire.
// Analyses required/preserved: retained constructor, no cached analyses.
// Output invariants: counter state/reset advance only on targetFire. Simultaneous
// inc/dec hold; lowering the runtime maximum does not clamp existing state.
// Pairing uses pre-edge counts and is not gated by full, reset or targetFire.
// Target B.fire and runtime maximum remain explicit boundaries. Request-ready
// admission is unchanged; observed full flags will drive it in a later pass.
#include "goldengate/FASEDWritePairing.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDWritePairing(CircuitOp circuit,
                                             std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDWritePairing";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDWritePairingWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDTimingAWQueueWrapper")
    return reject("FASED write pairing requires the active timing AW queue wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("FASED write pairing helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED write pairing needs top, engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto has = [&](llvm::StringRef n, int w) {
    auto a = widths ? widths.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == w;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("FASED write pairing requires the recorded 35/64/4-bit profile");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> es;
    for (auto [n, w] : fields) es.push_back({b.getStringAttr(n), false, uint(w)});
    return BundleType::get(ctx, es);
  };
  auto decoupled = [&](BundleType bits) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bits}});
  };
  auto address = payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
      {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto requests = BundleType::get(ctx, {{b.getStringAttr("aw"),false,decoupled(address)},
      {b.getStringAttr("w"),false,decoupled(data)},{b.getStringAttr("ar"),false,decoupled(address)}});
  const llvm::StringRef names[]{"hostClock", "fased_model_reset", "fased_tfire",
      "fased_timing_requests", "fased_write_pair_complete"};
  const Type types[]{ClockType::get(ctx), bit, bit, requests, bit};
  const Direction dirs[]{Direction::In, Direction::Out, Direction::Out, Direction::Out, Direction::In};
  unsigned indices[5];
  for (unsigned j = 0; j < 5; ++j) {
    std::optional<unsigned> i;
    for (auto [n, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.type == types[j] && p.direction == dirs[j]) i = n;
    if (!i) return reject("FASED write pairing needs exact model clock/reset/fire/request/completion boundaries");
    indices[j] = *i;
  }
  for (auto p : inner.getPorts())
    if (p.name == "fased_target_b_fire" || p.name == "fased_write_max_reqs" || p.name == "fased_pending_writes")
      return reject("FASED write pairing boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED write pairing needs an uninstantiated top");
  auto pending = payload({{"awValue",4},{"wValue",4},{"awFull",1},{"wFull",1}});
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"), types[0], Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("targetFire"), bit, Direction::In},
      {b.getStringAttr("awFire"), bit, Direction::In}, {b.getStringAttr("wLastFire"), bit, Direction::In},
      {b.getStringAttr("bFire"), bit, Direction::In}, {b.getStringAttr("writeMaxReqs"), uint(4), Direction::In},
      {b.getStringAttr("pairComplete"), bit, Direction::Out}, {b.getStringAttr("pending"), pending, Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(ctx, Convention::Internal), hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value a, Value z)->Value { return b.create<AndPrimOp>(loc, a, z); };
  auto either = [&](Value a, Value z)->Value { return b.create<OrPrimOp>(loc, a, z); };
  auto neg = [&](Value a)->Value { return b.create<NotPrimOp>(loc, a); };
  auto mux = [&](Value c, Value a, Value z)->Value { return b.create<MuxPrimOp>(loc, c, a, z); };
  auto constant = [&](unsigned w, uint64_t n)->Value { return b.create<ConstantOp>(loc, uint(w), APInt(w,n)); };
  Value enabledReset = both(arg(1),arg(2));
  SmallVector<Value> values;
  for (unsigned i=0; i<2; ++i) {
    Value value = b.create<RegResetOp>(loc,uint(4),arg(0),enabledReset,constant(4,0),
        i ? "pendingW" : "pendingAW").getResult();
    values.push_back(value);
    Value inc=arg(3+i), dec=arg(5), full=b.create<GEQPrimOp>(loc,value,arg(6));
    Value empty=b.create<EQPrimOp>(loc,value,constant(4,0));
    Value up=both(both(inc,neg(dec)),neg(full));
    Value down=both(both(neg(inc),dec),neg(empty));
    Value plus=b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,value,constant(4,1)),3,0);
    Value minus=b.create<BitsPrimOp>(loc,b.create<SubPrimOp>(loc,value,constant(4,1)),3,0);
    connect(value,mux(arg(2),mux(up,plus,mux(down,minus,value)),value));
    connect(field(arg(8),i ? "wValue" : "awValue"),value);
    connect(field(arg(8),i ? "wFull" : "awFull"),full);
  }
  Value wAhead=b.create<GTPrimOp>(loc,values[1],values[0]);
  Value awAhead=b.create<LTPrimOp>(loc,values[1],values[0]);
  connect(arg(7),either(either(both(wAhead,arg(3)),both(awAhead,arg(4))),both(arg(3),arg(4))));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts()))
    if (i != indices[4]) { copied.push_back(i); ports.push_back(p); }
  unsigned added = ports.size();
  ports.push_back({b.getStringAttr("fased_target_b_fire"),bit,Direction::In});
  ports.push_back({b.getStringAttr("fased_write_max_reqs"),uint(4),Direction::In});
  ports.push_back({b.getStringAttr("fased_pending_writes"),pending,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),pairing=b.create<InstanceOp>(loc,helper,"pairing");
  for (auto [j,i] : llvm::enumerate(copied)) {
    Value v=wrapper.getBodyBlock()->getArgument(j);auto p=inner.getPorts()[i];
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,p.direction==Direction::In?v:sim.getResult(i));
  }
  connect(pairing.getResult(0),wrapper.getBodyBlock()->getArgument(llvm::find(copied,indices[0])-copied.begin()));
  for(unsigned j=1;j<3;++j)connect(pairing.getResult(j),sim.getResult(indices[j]));
  Value requestsOut=sim.getResult(indices[3]);
  auto accepted = [&](llvm::StringRef n)->Value {
    Value channel=field(requestsOut,n); return both(field(channel,"ready"),field(channel,"valid"));
  };
  connect(pairing.getResult(3),accepted("aw"));
  connect(pairing.getResult(4),both(accepted("w"),field(field(field(requestsOut,"w"),"bits"),"last")));
  connect(pairing.getResult(5),wrapper.getBodyBlock()->getArgument(added));
  connect(pairing.getResult(6),wrapper.getBodyBlock()->getArgument(added+1));
  connect(sim.getResult(indices[4]),pairing.getResult(7));
  connect(wrapper.getBodyBlock()->getArgument(added+2),pairing.getResult(8));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute attr)->Attribute {
    if(auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));
        for(auto i:copied)if(n==inner.getPortName(i)){suffix="|"+wrapperName.str()+">"+ref.str();break;}
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
