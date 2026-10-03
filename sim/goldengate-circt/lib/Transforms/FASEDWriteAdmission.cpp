// See LICENSE for license details.
// Oracle: TimingModel.scala SplitTransactionModel AW/W ready := ~pending.full.
// Required invariants: uninstantiated write-retirement top, exact recorded
// 35/64/4-bit requests and four-bit pending-counter observations.
// Annotations consumed/produced: none; copied top references transfer to the
// wrapper, including ready fields whose identities now denote driven outputs.
// IR mutations: wrap the top and close AW/W reverse ready with NotPrimOps.
// Analyses required/preserved: no cached analyses. The predecessor's pending
// counter and target B retirement bindings remain in the instantiated module.
// Output: AW/W ready are combinational inversions of the respective full flag.
// No reset, fire, queue-ready or host-ready mask. All request payloads and AR
// reverse ready pass through. AW/W ready remain observable but cannot be driven
// from the external boundary; only those two bundle flips change.
#include "goldengate/FASEDWriteAdmission.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::bindFASEDWriteAdmission(CircuitOp circuit,
                                                std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGFASEDWriteAdmissionWrapper";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (circuit.getName() != "GGFASEDWriteRetirementWrapper" || !raw)
    return reject("FASED write admission requires the active retirement wrapper and annotations");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName)
      return reject("FASED write admission wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  if (!inner) return reject("FASED write admission needs the retirement module");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED write admission needs an uninstantiated top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> es;
    for (auto [n,w] : fields) es.push_back({b.getStringAttr(n), false, uint(w)});
    return BundleType::get(ctx, es);
  };
  auto channel = [&](BundleType bits, bool reverseReady) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"),reverseReady,bit},
        {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bits}});
  };
  auto address = payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
      {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto requests = [&](bool reverseWriteReady) {
    return BundleType::get(ctx, {{b.getStringAttr("aw"),false,channel(address,reverseWriteReady)},
      {b.getStringAttr("w"),false,channel(data,reverseWriteReady)},
      {b.getStringAttr("ar"),false,channel(address,true)}});
  };
  auto pending = payload({{"awValue",4},{"wValue",4},{"awFull",1},{"wFull",1}});
  auto port = [&](llvm::StringRef n, Type t)->std::optional<unsigned> {
    for (auto [i,p] : llvm::enumerate(inner.getPorts()))
      if (p.name == n && p.type == t && p.direction == Direction::Out) return i;
    return std::nullopt;
  };
  auto req = port("fased_timing_requests", requests(true));
  auto counts = port("fased_pending_writes", pending);
  if (!req || !counts) return reject("FASED write admission needs exact request and pending-counter boundaries");
  auto ports = inner.getPorts();
  ports[*req].type = requests(false);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc,inner,"sim");
  auto field = [&](Value v,llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc,v,n); };
  auto connect = [&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  for (auto [i,p] : llvm::enumerate(ports)) {
    if (i == *req) continue;
    Value v = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,
                       p.direction==Direction::In?v:sim.getResult(i));
  }
  Value outer = wrapper.getBodyBlock()->getArgument(*req), innerReq = sim.getResult(*req);
  for (llvm::StringRef n : {"aw", "w"}) {
    Value out = field(outer,n), in = field(innerReq,n);
    for (auto leaf : {"valid", "bits"}) connect(field(out,leaf),field(in,leaf));
    Value full = field(sim.getResult(*counts),n=="aw"?"awFull":"wFull");
    Value ready = b.create<NotPrimOp>(loc,full);
    connect(field(in,"ready"),ready); connect(field(out,"ready"),ready);
  }
  b.create<ConnectOp>(loc,field(outer,"ar"),field(innerReq,"ar"));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),
      modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr)->Attribute {
    if (auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue(); if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));
        for(auto p:ports)if(n==p.name) {suffix="|"+wrapperName.str()+">"+ref.str();break;}
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
