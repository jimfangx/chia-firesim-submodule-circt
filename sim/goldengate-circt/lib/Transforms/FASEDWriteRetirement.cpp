// See LICENSE for license details.
// Oracle: TimingModel.scala pendingAWReq/pendingWReq.dec := tNasti.b.fire;
// Util.scala AXI4Releaser io.b.valid := currentWrite.valid.
// Requires: uninstantiated write-pairing top, unique recorded sim chain to the
// response releaser, exact one-bit retirement input and Decoupled B response.
// Consumes/produces annotations: none. Copied targets move to the new top;
// the consumed retirement target stays on the inner module's connected input.
// Mutations: append six internal observation outputs and clone their instances;
// wrap the top to feed the observed B handshake into both pending counters.
// Analyses required/preserved: no cached analyses; hierarchy uses are checked
// before mutation. Existing module, port and instance identities are preserved.
// Output: retirement is valid && ready, independent of reset and targetFire.
// The pending counters already gate state updates/reset on targetFire. Host B
// acknowledgements and egress request acceptance are separate transactions.
#include "goldengate/FASEDWriteRetirement.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::bindFASEDWriteRetirement(CircuitOp circuit,
                                                 std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGFASEDWriteRetirementWrapper";
  constexpr llvm::StringLiteral observationName = "fased_accepted_b_fire";
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (circuit.getName() != "GGFASEDWritePairingWrapper" || !raw)
    return reject("FASED write retirement requires the active pairing wrapper and annotations");
  const llvm::StringRef names[]{"GGFASEDWritePairingWrapper", "GGFASEDTimingAWQueueWrapper",
      "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper",
      "GGFASEDTimingCycleWrapper", "GGFASEDResponseReleaserWrapper"};
  SmallVector<FModuleOp> modules;
  for (auto name : names) {
    FModuleOp found;
    for (auto m : circuit.getOps<FModuleLike>()) {
      if (m.getModuleName() == wrapperName)
        return reject("FASED write retirement wrapper already exists");
      if (m.getModuleName() == name) found = dyn_cast<FModuleOp>(m.getOperation());
    }
    if (!found) return reject("FASED write retirement needs the recorded response hierarchy");
    for (auto p : found.getPorts()) if (p.name == observationName)
      return reject("FASED write retirement observation already exists");
    modules.push_back(found);
  }
  SmallVector<InstanceOp> chain;
  for (unsigned j = 0; j < modules.size(); ++j) {
    SmallVector<InstanceOp> uses;
    circuit.walk([&](InstanceOp i) { if (i.getModuleName() == names[j]) uses.push_back(i); });
    if (j == 0 ? !uses.empty() : uses.size() != 1)
      return reject("FASED write retirement needs unique response instances and an uninstantiated top");
    if (j && (uses[0]->getParentOfType<FModuleOp>() != modules[j-1] ||
              uses[0].getInstanceName() != "sim"))
      return reject("FASED write retirement needs the recorded sim chain");
    if (j) chain.push_back(uses[0]);
  }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); }; auto bit = uint(1);
  auto port = [&](FModuleOp m, llvm::StringRef n, Type t, Direction d)->std::optional<unsigned> {
    for (auto [i,p] : llvm::enumerate(m.getPorts()))
      if (p.name == n && p.type == t && p.direction == d) return i;
    return std::nullopt;
  };
  auto retirement = port(modules[0], "fased_target_b_fire", bit, Direction::In);
  InstanceOp releaser;
  for (auto i : modules.back().getOps<InstanceOp>())
    if (i.getInstanceName() == "releaser" && i.getModuleName() == "GGFASEDResponseReleaser") releaser = i;
  auto bits = BundleType::get(ctx, {{b.getStringAttr("user"),false,bit},
      {b.getStringAttr("id"),false,uint(4)}, {b.getStringAttr("resp"),false,uint(2)}});
  auto response = BundleType::get(ctx, {{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit}, {b.getStringAttr("bits"),false,bits}});
  std::optional<unsigned> responseIndex;
  if (releaser) for (unsigned i=0; i<releaser.getNumResults(); ++i)
    if (releaser.getPortNameStr(i) == "b" && releaser.getResult(i).getType() == response &&
        releaser.getPortDirection(i) == Direction::Out) responseIndex = i;
  if (!retirement || !responseIndex)
    return reject("FASED write retirement needs exact pending-counter input and releaser B response");
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(modules[0].getPorts()))
    if (i != *retirement) { copied.push_back(i); ports.push_back(p); }

  // Append from the bottom up so old result indices and all existing uses stay
  // intact. Only the final wrapper closes the feedback into the old top input.
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
  Value ready = b.create<SubfieldOp>(loc,responseValue,"ready");
  Value valid = b.create<SubfieldOp>(loc,responseValue,"valid");
  connect(modules.back().getBodyBlock()->getArgument(modules.back().getNumPorts()-1),
          b.create<AndPrimOp>(loc,ready,valid));
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),modules[0].getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc,modules[0],"sim");
  for (auto [j,i] : llvm::enumerate(copied)) {
    Value v=wrapper.getBodyBlock()->getArgument(j); auto p=ports[j];
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,
                       p.direction==Direction::In?v:sim.getResult(i));
  }
  connect(sim.getResult(*retirement),sim.getResult(sim.getNumResults()-1));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),
      modulePrefix="|"+modules[0].getName().str()+">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr)->Attribute {
    if (auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue(); if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));
        for(auto i:copied)if(n==modules[0].getPortName(i)) {suffix="|"+wrapperName.str()+">"+ref.str();break;}
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
