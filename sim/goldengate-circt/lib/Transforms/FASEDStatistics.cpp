// See LICENSE for license details.
// Oracle: TimingModel.scala:151-167 and FASEDMemoryTimingModel MCR words 14-17.
// Required input invariants: recorded ten-flight 35/64/4-bit FASED profile,
// uninstantiated response-error top, unique sim chain to response releaser,
// exact model reset/fire and timing request interfaces.
// Annotations consumed: none.
// Annotations produced: none; retained top port targets explicitly retargeted.
// IR mutations: expose accepted target R beats through the response hierarchy,
// add four UInt<32> counters and decoded read-only MCR lanes at bytes 56-68.
// Analyses required: mapped FASED request admission and response release.
// Analyses preserved: existing channels, constructors and port identities.
// Output invariants: count AW/AR transactions and all W/R beats, wrapping at
// 32 bits; state/reset advance only on targetFire. Read-only assertions use
// host reset independently. Existing response and request drivers unchanged.
#include "goldengate/FASEDStatistics.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDStatistics(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGFASEDStatistics";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDStatisticsWrapper";
  constexpr llvm::StringLiteral controlName = "fased_statistics_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDResponseErrorsWrapper")
    return reject("FASED statistics require the active response-error wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("FASED statistics module or wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED statistics need a top and retained annotations");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!flight || flight.getInt() != 10)
    return reject("FASED statistics currently require the recorded ten-flight constructor");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  auto token = [&](FIRRTLBaseType payload) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> elements;
    for (auto [name, width] : fields) elements.push_back({b.getStringAttr(name), false, uint(width)});
    return BundleType::get(ctx, elements);
  };
  auto channel = [&](BundleType bits, bool flip) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), flip, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bits}});
  };
  auto address = payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
      {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto requests = BundleType::get(ctx, {{b.getStringAttr("aw"),false,channel(address,false)},
      {b.getStringAttr("w"),false,channel(data,false)}, {b.getStringAttr("ar"),false,channel(address,false)}});
  auto response = channel(payload({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}}), true);
  const llvm::StringRef required[]{"hostClock","hostReset","fased_model_reset","fased_tfire","fased_timing_requests"};
  const Type types[]{ClockType::get(ctx),bit,bit,bit,requests};
  const Direction directions[]{Direction::In,Direction::In,Direction::Out,Direction::Out,Direction::Out};
  unsigned indices[5];
  for (unsigned j=0; j<5; ++j) {
    std::optional<unsigned> index;
    for (auto [i,p] : llvm::enumerate(inner.getPorts()))
      if (p.name==required[j] && p.type==types[j] && p.direction==directions[j]) index=i;
    if (!index) return reject("FASED statistics need exact clock/reset/fire and timing request ports");
    indices[j]=*index;
  }
  constexpr llvm::StringLiteral observationName = "fased_accepted_r_fire";
  const llvm::StringRef hierarchy[]{"GGFASEDResponseErrorsWrapper", "GGFASEDFunctionalModelRegisterWrapper",
      "GGFASEDLatencyRegistersWrapper", "GGFASEDRequestLimitsWrapper", "GGFASEDReadAdmissionWrapper",
      "GGFASEDWriteAdmissionWrapper", "GGFASEDWriteRetirementWrapper", "GGFASEDWritePairingWrapper",
      "GGFASEDTimingAWQueueWrapper", "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
      "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
  SmallVector<FModuleOp> modules; SmallVector<InstanceOp> chain;
  for (auto name : hierarchy) {
    FModuleOp found;
    for (auto m : circuit.getOps<FModuleOp>()) if (m.getName()==name) found=m;
    if (!found) return reject("FASED statistics need the recorded response hierarchy");
    for (auto p : found.getPorts()) if (p.name==observationName || p.name==controlName)
      return reject("FASED statistics observation or MCR boundary already exists");
    SmallVector<InstanceOp> uses;
    circuit.walk([&](InstanceOp i) { if (i.getModuleName()==name) uses.push_back(i); });
    if (modules.empty() ? !uses.empty() : uses.size()!=1)
      return reject("FASED statistics need unique response instances and an uninstantiated top");
    if (!modules.empty()) {
      if (uses[0]->getParentOfType<FModuleOp>()!=modules.back() || uses[0].getName()!="sim")
        return reject("FASED statistics need the recorded sim instance chain");
      chain.push_back(uses[0]);
    }
    modules.push_back(found);
  }
  InstanceOp releaser; std::optional<unsigned> rIndex;
  for (auto i : modules.back().getOps<InstanceOp>())
    if (i.getName()=="releaser" && i.getModuleName()=="GGFASEDResponseReleaser") releaser=i;
  if (releaser) for (unsigned i=0; i<releaser.getNumResults(); ++i)
    if (releaser.getPortNameStr(i)=="r" && releaser.getResult(i).getType()==response &&
        releaser.getPortDirection(i)==Direction::Out) rIndex=i;
  if (!rIndex) return reject("FASED statistics need the exact releaser target R response");

  // All validation precedes mutations. The appended observation is consumed
  // internally by this wrapper, so the original top interface is preserved.
  auto originalPorts=inner.getPorts();
  for (unsigned j=modules.size(); j-- > 0;) {
    SmallVector<std::pair<unsigned,PortInfo>> added{{modules[j].getNumPorts(),
        PortInfo(b.getStringAttr(observationName),bit,Direction::Out)}};
    modules[j].insertPorts(added);
    if (j) {
      auto replacement=chain[j-1].cloneAndInsertPorts(added);
      for (auto attr : chain[j-1]->getAttrs())
        if (!replacement->hasAttr(attr.getName())) replacement->setAttr(attr.getName(),attr.getValue());
      for (unsigned i=0; i<chain[j-1].getNumResults(); ++i)
        chain[j-1].getResult(i).replaceAllUsesWith(replacement.getResult(i));
      chain[j-1].erase(); chain[j-1]=replacement;
    }
  }
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc,dest,src); };
  auto field = [&](Value v, llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc,v,n); };
  auto both = [&](Value a, Value z)->Value { return b.create<AndPrimOp>(loc,a,z); };
  for (unsigned j=0; j+1<modules.size(); ++j) {
    b.setInsertionPointToEnd(modules[j].getBodyBlock());
    connect(modules[j].getArguments().back(),chain[j].getResults().back());
  }
  b.setInsertionPointToEnd(modules.back().getBodyBlock());
  Value r=releaser.getResult(*rIndex);
  connect(modules.back().getArguments().back(),both(field(r,"ready"),field(r,"valid")));

  // Preflight is complete. Local lanes 0-3 map to global words 14-17.
  auto words = FVectorType::get(token(uint(32)), 4);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In}};
  for (auto name : {"hostReset","modelReset","targetFire","awFire","arFire","wFire","rFire"})
    bankPorts.push_back({b.getStringAttr(name),bit,Direction::In});
  bankPorts.push_back({b.getStringAttr("mcr"),mcr,Direction::Out});
  auto bank=b.create<FModuleOp>(loc,b.getStringAttr(bankName),
      ConventionAttr::get(ctx,Convention::Internal),bankPorts);
  const llvm::StringRef names[]{"totalWriteBeats","totalReadBeats","totalWrites","totalReads"};
  SmallVector<Attribute> registers;
  for (unsigned i=0;i<4;++i) registers.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr(names[i])),
      b.getNamedAttr("offset",b.getI32IntegerAttr(56+4*i)),
      b.getNamedAttr("readable",b.getBoolAttr(true)),
      b.getNamedAttr("writeable",b.getBoolAttr(false))}));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(8), group), i);
  };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value zero=b.create<ConstantOp>(loc,uint(32),APInt(32,0));
  Value increment=b.create<ConstantOp>(loc,uint(32),APInt(32,1));
  Value enabled=b.create<NotPrimOp>(loc,arg(1));
  Value modelReset=both(arg(2),arg(3));
  const unsigned events[]{6,7,4,5};
  for (unsigned i=0;i<4;++i) {
    Value reg=b.create<RegResetOp>(loc,uint(32),arg(0),modelReset,zero,names[i]).getResult();
    Value next=b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,reg,increment),31,0);
    connect(reg,b.create<MuxPrimOp>(loc,both(arg(3),arg(events[i])),next,reg));
    Value write=slot("write",i),read=slot("read",i);
    connect(field(read,"bits"),reg);
    connect(field(read,"valid"),one);connect(field(write,"ready"),one);
    Value permitted=b.create<NotPrimOp>(loc,field(write,"valid"));
    b.create<AssertOp>(loc,arg(0),permitted,enabled,
        "Register "+names[i].str()+" is read only",ValueRange{},"");
  }

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, port] : llvm::enumerate(originalPorts)) {
    copied.push_back(i); ports.push_back(port);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "statistics");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto port = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
                             port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0),outer(indices[0]));connect(mmio.getResult(1),outer(indices[1]));
  connect(mmio.getResult(2),sim.getResult(indices[2]));connect(mmio.getResult(3),sim.getResult(indices[3]));
  Value timing=sim.getResult(indices[4]);
  for (auto [name,index] : {std::pair<llvm::StringRef,unsigned>{"aw",4},{"ar",5},{"w",6}}) {
    Value channel=field(timing,name);
    connect(mmio.getResult(index),both(field(channel,"ready"),field(channel,"valid")));
  }
  connect(mmio.getResult(7),sim.getResults().back());
  b.create<ConnectOp>(loc,wrapper.getArguments().back(),mmio.getResult(8));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto local = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (local == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> vs; for (auto v : arr) vs.push_back(retarget(v)); return b.getArrayAttr(vs); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList vs; for (auto v : d) vs.set(v.getName(), retarget(v.getValue())); return vs.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(wrapperName);
  return success();
}
