// See LICENSE for license details.
// Oracle: LatencyBandwidthPipe.scala 77-88 and its SFC Queue_16.
// Ten entries with empty flow-through, no full-queue pipe bypass. Queue
// state, RAM writes and reset run on the enabled model clock (targetFire).
// The model's unsigned releaseCycle <= tCycle comparison gates retirement.
// AR acceptance enqueues independently of capacity; the SFC overflow assertion
// diagnoses missing upstream admission control. Preserve Scala read len even
// though AXI4Releaser doesn't use it and SFC optimizes its storage away.
#include "goldengate/FASEDReadLatency.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDReadLatency(CircuitOp circuit,
                                             std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDReadLatency10";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDReadLatencyWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (circuit.getName() != "GGFASEDTimingCycleWrapper")
    return reject("FASED read latency requires the active timing cycle wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("FASED read latency helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED read latency needs top, engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto has = [&](llvm::StringRef n, int w) {
    auto a = widths ? widths.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == w;
  };
  if (!has("addrBits", 35) || !has("dataBits", 64) || !has("idBits", 4))
    return reject("FASED read latency requires the recorded 35/64/4-bit profile");
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
      {b.getStringAttr("w"),false,decoupled(data)}, {b.getStringAttr("ar"),false,decoupled(address)}});
  auto nextRead = decoupled(payload({{"id",4},{"len",8}}));
  auto enqueue = decoupled(payload({{"releaseCycle",64},{"id",4},{"len",8}}));
  const llvm::StringRef names[]{"hostClock", "fased_model_reset", "fased_tfire", "fased_timing_cycle",
      "fased_read_release_cycle", "fased_next_read", "fased_timing_requests"};
  const Type types[]{ClockType::get(ctx), bit, bit, uint(64), uint(64), nextRead, requests};
  const Direction dirs[]{Direction::In, Direction::Out, Direction::Out, Direction::Out,
      Direction::Out, Direction::In, Direction::Out};
  unsigned indices[7];
  for (unsigned j = 0; j < 7; ++j) {
    std::optional<unsigned> i;
    for (auto [n, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.type == types[j] && p.direction == dirs[j]) i = n;
    if (!i) return reject("FASED read latency needs exact clock/reset/fire/cycle/deadline/metadata/AR boundaries");
    indices[j] = *i;
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED read latency needs an uninstantiated top");
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"), types[0], Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}, {b.getStringAttr("targetFire"), bit, Direction::In},
      {b.getStringAttr("tCycle"), uint(64), Direction::In},
      {b.getStringAttr("enq"), enqueue, Direction::In}, {b.getStringAttr("deq"), nextRead, Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(ctx, Convention::Internal), hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value a, Value z)->Value { return b.create<AndPrimOp>(loc, a, z); };
  auto neg = [&](Value a)->Value { return b.create<NotPrimOp>(loc, a); };
  auto mux = [&](Value c, Value a, Value z)->Value { return b.create<MuxPrimOp>(loc, c, a, z); };
  auto constant = [&](unsigned w, uint64_t n)->Value { return b.create<ConstantOp>(loc, uint(w), APInt(w,n)); };
  Value enabledReset = both(arg(1),arg(2));
  auto reg = [&](unsigned w, llvm::StringRef n)->Value {
    return b.create<RegResetOp>(loc,uint(w),arg(0),enabledReset,constant(w,0),n).getResult();
  };
  Value ep = reg(4,"enq_ptr_value"), dp = reg(4,"deq_ptr_value"), mf = reg(1,"maybe_full");
  Value equal = b.create<EQPrimOp>(loc,ep,dp), empty = both(equal,neg(mf)), full = both(equal,mf);
  Value ready = neg(full), ev = field(arg(4),"valid"), dr = field(arg(5),"ready"), eb = field(arg(4),"bits");
  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(10,uint(76),MemOp::PortKind::Read),
      MemOp::getTypeForPort(10,uint(76),MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"),b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc,memoryTypes,0,1,10,RUWAttr::Undefined,memoryNames,"ram");
  Value reader=ram.getResult(0),writer=ram.getResult(1);
  connect(field(reader,"clk"),arg(0));connect(field(reader,"en"),constant(1,1));connect(field(reader,"addr"),dp);
  Value packed=field(reader,"data");
  Value deadline=mux(empty,field(eb,"releaseCycle"),b.create<BitsPrimOp>(loc,packed,75,12));
  Value id=mux(empty,field(eb,"id"),b.create<BitsPrimOp>(loc,packed,11,8));
  Value len=mux(empty,field(eb,"len"),b.create<BitsPrimOp>(loc,packed,7,0));
  Value due=b.create<LEQPrimOp>(loc,deadline,arg(3));
  Value valid=both(b.create<OrPrimOp>(loc,ev,neg(empty)),due);
  connect(field(arg(4),"ready"),ready);connect(field(arg(5),"valid"),valid);
  connect(field(field(arg(5),"bits"),"id"),id);connect(field(field(arg(5),"bits"),"len"),len);
  Value push=both(both(ready,ev),neg(both(empty,both(dr,due))));
  Value pop=both(neg(empty),both(valid,dr));
  auto increment = [&](Value pointer)->Value {
    return mux(b.create<EQPrimOp>(loc,pointer,constant(4,9)),constant(4,0),
      b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,pointer,constant(4,1)),3,0));
  };
  connect(ep,mux(both(arg(2),push),increment(ep),ep));connect(dp,mux(both(arg(2),pop),increment(dp),dp));
  connect(mf,mux(both(arg(2),b.create<XorPrimOp>(loc,push,pop)),push,mf));
  connect(field(writer,"clk"),arg(0));connect(field(writer,"en"),both(arg(2),push));
  connect(field(writer,"addr"),ep);connect(field(writer,"mask"),constant(1,1));
  connect(field(writer,"data"),b.create<CatPrimOp>(loc,b.create<CatPrimOp>(loc,field(eb,"releaseCycle"),field(eb,"id")),field(eb,"len")));
  Value permitted=b.create<OrPrimOp>(loc,ready,neg(ev));
  b.create<AssertOp>(loc,arg(0),permitted,both(arg(2),neg(arg(1))),
      "LBP read latency pipe would overflow.",ValueRange{},"");

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts()))
    if (i != indices[4] && i != indices[5]) { copied.push_back(i); ports.push_back(p); }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),queue=b.create<InstanceOp>(loc,helper,"readLatency");
  for (auto [j,i] : llvm::enumerate(copied)) {
    Value v=wrapper.getBodyBlock()->getArgument(j);auto p=inner.getPorts()[i];
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,p.direction==Direction::In?v:sim.getResult(i));
  }
  connect(queue.getResult(0),wrapper.getBodyBlock()->getArgument(llvm::find(copied,indices[0])-copied.begin()));
  for(unsigned j=1;j<4;++j)connect(queue.getResult(j),sim.getResult(indices[j]));
  Value ar=field(sim.getResult(indices[6]),"ar"),enq=queue.getResult(4),bits=field(enq,"bits");
  connect(field(enq,"valid"),both(field(ar,"ready"),field(ar,"valid")));
  connect(field(bits,"releaseCycle"),sim.getResult(indices[4]));
  for(auto n:{"id","len"})connect(field(bits,n),field(field(ar,"bits"),n));
  b.create<ConnectOp>(loc,sim.getResult(indices[5]),queue.getResult(5));
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
