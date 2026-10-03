// See LICENSE for license details.
// Oracle: TimingModel.scala 104-106, 226, 229 and widgets/Lib.scala 71-86.
// Requires: uninstantiated write-admission top; unique eight-module sim chain;
// exact recorded 35/64/4-bit no-LLC requests, model clock/reset/fire and R response.
// Consumes/produces annotations: none; copied top references transfer to wrapper.
// Mutations: append internal accepted-final-R observations; create a four-bit
// pending-read counter and close AR ready with !full. Existing AW/W pass through.
// Analyses required/preserved: no cached analyses. Preflight checks all modified
// hierarchy uses before mutation; cloned instances retain custom attributes.
// Output: inc = AR.valid && AR.ready; dec = R.valid && R.ready && R.last.
// State/reset only advance on targetFire; simultaneous inc/dec hold. Runtime
// limit reductions do not clamp state. Ready has no reset/fire/host-ready mask.
// Runtime readMaxReqs remains an explicit boundary until FASED MMIO is ported.
#include "goldengate/FASEDReadAdmission.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::bindFASEDReadAdmission(CircuitOp circuit,
                                                 std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGFASEDReadAdmissionWrapper";
  constexpr llvm::StringLiteral observationName = "fased_accepted_r_last_fire";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (circuit.getName() != "GGFASEDWriteAdmissionWrapper" || !raw)
    return reject("FASED read admission requires the active write-admission wrapper and annotations");
  const llvm::StringRef names[]{"GGFASEDWriteAdmissionWrapper", "GGFASEDWriteRetirementWrapper",
      "GGFASEDWritePairingWrapper", "GGFASEDTimingAWQueueWrapper",
      "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
      "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
  SmallVector<FModuleOp> modules;
  for (auto name : names) {
    FModuleOp found;
    for (auto m : circuit.getOps<FModuleLike>()) {
      if (m.getModuleName() == wrapperName)
        return reject("FASED read admission wrapper already exists");
      if (m.getModuleName() == name) found = dyn_cast<FModuleOp>(m.getOperation());
    }
    if (!found) return reject("FASED read admission needs the recorded response hierarchy");
    for (auto p : found.getPorts()) if (p.name == observationName)
      return reject("FASED read admission observation already exists");
    modules.push_back(found);
  }
  SmallVector<InstanceOp> chain;
  for (unsigned j = 0; j < modules.size(); ++j) {
    SmallVector<InstanceOp> uses;
    circuit.walk([&](InstanceOp i) { if (i.getModuleName() == names[j]) uses.push_back(i); });
    if (j == 0 ? !uses.empty() : uses.size() != 1)
      return reject("FASED read admission needs unique response instances and an uninstantiated top");
    if (j && (uses[0]->getParentOfType<FModuleOp>() != modules[j-1] ||
              uses[0].getInstanceName() != "sim"))
      return reject("FASED read admission needs the recorded sim chain");
    if (j) chain.push_back(uses[0]);
  }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto port = [&](FModuleOp m, llvm::StringRef n, Type t, Direction d)->std::optional<unsigned> {
    for (auto [i,p] : llvm::enumerate(m.getPorts()))
      if (p.name == n && p.type == t && p.direction == d) return i;
    return std::nullopt;
  };
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields) {
    SmallVector<BundleType::BundleElement> es;
    for (auto [n,w]:fields) es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,es);
  };
  auto channel = [&](BundleType bits,bool flip) {
    return BundleType::get(ctx,{{b.getStringAttr("ready"),flip,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bits}});
  };
  auto address=payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
    {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto requests = [&](bool reverseReadReady) {
    return BundleType::get(ctx,{{b.getStringAttr("aw"),false,channel(address,false)},
      {b.getStringAttr("w"),false,channel(data,false)},
      {b.getStringAttr("ar"),false,channel(address,reverseReadReady)}});
  };
  auto clock=port(modules[0],"hostClock",ClockType::get(ctx),Direction::In);
  auto reset=port(modules[0],"fased_model_reset",bit,Direction::Out);
  auto fire=port(modules[0],"fased_tfire",bit,Direction::Out);
  auto req=port(modules[0],"fased_timing_requests",requests(true),Direction::Out);
  for (auto m:circuit.getOps<FModuleLike>())
    if(m.getModuleName()=="GGFASEDReadAdmission") return reject("FASED read admission helper already exists");
  for (auto p:modules[0].getPorts())
    if(p.name=="fased_read_max_reqs"||p.name=="fased_pending_reads")
      return reject("FASED read admission boundary already exists");
  InstanceOp releaser;
  for (auto i:modules.back().getOps<InstanceOp>())
    if(i.getInstanceName()=="releaser"&&i.getModuleName()=="GGFASEDResponseReleaser") releaser=i;
  auto response=channel(payload({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}}),true);
  std::optional<unsigned> responseIndex;
  if(releaser) for(unsigned i=0;i<releaser.getNumResults();++i)
    if(releaser.getPortNameStr(i)=="r"&&releaser.getResult(i).getType()==response&&
       releaser.getPortDirection(i)==Direction::Out) responseIndex=i;
  if(!clock||!reset||!fire||!req||!responseIndex)
    return reject("FASED read admission needs exact clock/reset/fire/requests and releaser R response");
  auto ports=modules[0].getPorts(); unsigned added=ports.size();
  ports[*req].type=requests(false);
  auto pending=payload({{"value",4},{"full",1}});
  ports.push_back({b.getStringAttr("fased_read_max_reqs"),uint(4),Direction::In});
  ports.push_back({b.getStringAttr("fased_pending_reads"),pending,Direction::Out});

  // Append bottom-up, preserving existing result indices, uses and metadata.
  for (unsigned j=modules.size(); j-- > 0;) {
    SmallVector<std::pair<unsigned,PortInfo>> added{{modules[j].getNumPorts(),
        PortInfo(b.getStringAttr(observationName),bit,Direction::Out)}};
    modules[j].insertPorts(added);
    if (j) {
      auto replacement = chain[j-1].cloneAndInsertPorts(added);
      // CIRCT rebuilds the instance's port attributes; retain other metadata
      // without replacing the rebuilt arrays for the appended output.
      for (auto attr : chain[j-1]->getAttrs())
        if (!replacement->hasAttr(attr.getName()))
          replacement->setAttr(attr.getName(), attr.getValue());
      for (unsigned i=0; i<chain[j-1].getNumResults(); ++i)
        chain[j-1].getResult(i).replaceAllUsesWith(replacement.getResult(i));
      chain[j-1].erase(); chain[j-1] = replacement;
    }
  }
  auto connect = [&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  for (unsigned j=0; j+1<modules.size(); ++j) {
    b.setInsertionPointToEnd(modules[j].getBodyBlock());
    connect(modules[j].getBodyBlock()->getArgument(modules[j].getNumPorts()-1),
            chain[j].getResult(chain[j].getNumResults()-1));
  }
  b.setInsertionPointToEnd(modules.back().getBodyBlock());
  Value responseValue = releaser.getResult(*responseIndex);
  auto field = [&](Value v,llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc,v,n); };
  auto both = [&](Value a,Value z)->Value { return b.create<AndPrimOp>(loc,a,z); };
  connect(modules.back().getBodyBlock()->getArgument(modules.back().getNumPorts()-1),
    both(both(field(responseValue,"ready"),field(responseValue,"valid")),
         field(field(responseValue,"bits"),"last")));

  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("targetFire"),bit,Direction::In},
    {b.getStringAttr("arValid"),bit,Direction::In},{b.getStringAttr("rLastFire"),bit,Direction::In},
    {b.getStringAttr("readMaxReqs"),uint(4),Direction::In},{b.getStringAttr("arReady"),bit,Direction::Out},
    {b.getStringAttr("pending"),pending,Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDReadAdmission"),
      ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto constant=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto neg=[&](Value v)->Value{return b.create<NotPrimOp>(loc,v);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  Value value=b.create<RegResetOp>(loc,uint(4),arg(0),both(arg(1),arg(2)),constant(4,0),"pendingReads").getResult();
  Value full=b.create<GEQPrimOp>(loc,value,arg(5));
  Value ready=neg(full),inc=both(arg(3),ready),dec=arg(4);
  Value empty=b.create<EQPrimOp>(loc,value,constant(4,0));
  Value up=both(both(inc,neg(dec)),neg(full));
  Value down=both(both(neg(inc),dec),neg(empty));
  Value plus=b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,value,constant(4,1)),3,0);
  Value minus=b.create<BitsPrimOp>(loc,b.create<SubPrimOp>(loc,value,constant(4,1)),3,0);
  connect(value,mux(arg(2),mux(up,plus,mux(down,minus,value)),value));
  connect(arg(6),ready); connect(field(arg(7),"value"),value); connect(field(arg(7),"full"),full);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),modules[0].getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,modules[0],"sim"),counter=b.create<InstanceOp>(loc,helper,"counter");
  for(unsigned i=0;i<added;++i) {
    if(i==*req)continue;
    Value v=wrapper.getBodyBlock()->getArgument(i);auto p=ports[i];
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,
                       p.direction==Direction::In?v:sim.getResult(i));
  }
  Value outer=wrapper.getBodyBlock()->getArgument(*req),innerReq=sim.getResult(*req);
  for(llvm::StringRef n:{"aw","w"})connect(field(outer,n),field(innerReq,n));
  Value out=field(outer,"ar"),in=field(innerReq,"ar");
  for(llvm::StringRef n:{"valid","bits"})connect(field(out,n),field(in,n));
  connect(field(out,"ready"),counter.getResult(6));connect(field(in,"ready"),counter.getResult(6));
  connect(counter.getResult(0),wrapper.getBodyBlock()->getArgument(*clock));
  connect(counter.getResult(1),sim.getResult(*reset));connect(counter.getResult(2),sim.getResult(*fire));
  connect(counter.getResult(3),field(in,"valid"));
  connect(counter.getResult(4),sim.getResult(sim.getNumResults()-1));
  connect(counter.getResult(5),wrapper.getBodyBlock()->getArgument(added));
  connect(wrapper.getBodyBlock()->getArgument(added+1),counter.getResult(7));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),
      modulePrefix="|"+modules[0].getName().str()+">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr)->Attribute {
    if (auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue(); if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));
        for(unsigned i=0;i<added;++i)if(n==modules[0].getPortName(i)) {suffix="|"+wrapperName.str()+">"+ref.str();break;}
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto a=dyn_cast<ArrayAttr>(attr)){SmallVector<Attribute> values;for(auto v:a)values.push_back(retarget(v));return b.getArrayAttr(values);}
    if(auto d=dyn_cast<DictionaryAttr>(attr)){NamedAttrList values;for(auto v:d)values.set(v.getName(),retarget(v.getValue()));return values.getDictionary(ctx);}
    return attr;
  };
  SmallVector<Attribute> annotations;for(auto a:raw)annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));circuit.setName(wrapperName);
  return success();
}
