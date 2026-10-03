// See LICENSE for license details.
// Oracle: FPGATopImp clock/reset, WidgetMMIO, mem and cpu_managed_axi4 IO.
// Requires: uninstantiated GGCPUStreamPlatformWrapper, exact recorded U250
// AXI4 port contracts, uniquely forwarded platform ports, retained annotations.
// Annotations consumed: none. Produces HostClockSource on FPGATop.clock.
// Transferred: the five public port targets (hostClock/hostReset renamed).
// Preserved: every other target on the retained inner module, all old bodies.
// Mutation: one FPGATop module/instance and five ordinary aggregate connects.
// Diagnostic outputs remain internal; no input, protocol field, state, clock
// gate or reset rule is removed. No new buffering or target-cycle logic.
#include "goldengate/FPGATopShell.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
namespace {
using Contract = std::map<std::string, std::pair<unsigned, bool>>;
// bool is the leaf's input direction after resolving all bundle flips.
bool flatten(Type t, std::string path, bool input, Contract &out) {
  if (auto bundle = dyn_cast<BundleType>(t)) {
    for (auto f : bundle.getElements())
      if (!flatten(f.type, path + "." + f.name.getValue().str(),
                   input != f.isFlip, out)) return false;
    return true;
  }
  auto u = dyn_cast<UIntType>(t);
  return u && u.getWidth() && out.emplace(path, std::make_pair(*u.getWidth(), input)).second;
}
Contract expected(StringRef root) {
  Contract out;
  using Fields = ArrayRef<std::pair<StringRef, unsigned>>;
  bool master = root == "mem_0";
  auto channel = [&](StringRef n, Fields fields) {
    bool request = n == "aw" || n == "w" || n == "ar";
    bool input = request != master;
    auto prefix = root.str() + "." + n.str();
    out[prefix + ".ready"] = {1, !input};
    out[prefix + ".valid"] = {1, input};
    for (auto [f,w] : fields) out[prefix + ".bits." + f.str()] = {w,input};
  };
  if (root == "ctrl") {
    const std::pair<StringRef,unsigned> address[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},{"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
    channel("aw",address); channel("ar",address);
    channel("w",{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}});
    channel("b",{{"resp",2},{"id",12},{"user",1}});
    channel("r",{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}});
  } else {
    const std::pair<StringRef,unsigned> address[]{{"id",16},{"addr",master?34u:64u},{"len",8},{"size",3},{"burst",2},{"lock",1},{"cache",4},{"prot",3},{"qos",4}};
    channel("aw",address); channel("ar",address);
    channel("w",{{"data",master?64u:512u},{"strb",master?8u:64u},{"last",1}});
    channel("b",{{"id",16},{"resp",2}});
    channel("r",{{"id",16},{"data",master?64u:512u},{"resp",2},{"last",1}});
  }
  return out;
}
} // namespace
LogicalResult goldengate::assembleFPGATopShell(CircuitOp circuit,
                                              std::string &error) {
  constexpr StringLiteral oldName="GGCPUStreamPlatformWrapper", newName="FPGATop";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner;
  for (auto m:circuit.getOps<FModuleLike>()) {
    if (m.getModuleName()==newName) return reject("FPGATop shell already exists");
    if (m.getModuleName()==oldName) inner=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (circuit.getName()!=oldName||!inner||!raw)
    return reject("FPGATop shell requires the assembled CPU/memory/control boundary and annotations");
  std::map<std::string,unsigned> old;
  const std::map<std::string,std::string> publicNames{{"hostClock","clock"},{"hostReset","reset"},{"ctrl","ctrl"},{"mem_0","mem_0"},{"cpu_managed_axi4","cpu_managed_axi4"}};
  const std::set<std::string> clocks{"peekPokeBridge_clock","resetBridge_clock","ep_clock","ep_1_clock","ep_2_clock","tracerv_tiletrace_clock","ep_3_clock"};
  for (auto [i,p]:llvm::enumerate(inner.getPorts())) {
    auto n=p.name.getValue(); if (!old.emplace(n.str(),i).second) return reject("duplicate platform port name");
    if (publicNames.count(n.str())) {
      if (n=="hostClock") {
        if (!isa<ClockType>(p.type)||p.direction!=Direction::In) return reject("invalid host clock port");
      } else if (n=="hostReset") {
        auto u=dyn_cast<UIntType>(p.type);
        if (!u||u.getWidth()!=1||p.direction!=Direction::In) return reject("invalid host reset port");
      } else {
        Contract actual;
        if (!isa<BundleType>(p.type)||!flatten(p.type,n.str(),p.direction==Direction::In,actual)||actual!=expected(n))
          return reject("platform AXI4 fields have unsupported widths, directions or field names");
      }
    } else {
      if (p.direction!=Direction::Out) return reject("FPGATop shell would hide an input port");
      if (clocks.count(n.str())) {
        if (!isa<ClockType>(p.type)) return reject("invalid retained bridge clock");
      } else {
        Contract diagnostic;
        if (!(n.starts_with("ctrl_")||n.starts_with("blockdev_")||n.starts_with("fased_"))||
            !flatten(p.type,n.str(),false,diagnostic)) return reject("unrecognized diagnostic output");
        for (auto [name,contract]:diagnostic) if (contract.second) return reject("diagnostic bundle contains an input leaf");
      }
    }
  }
  for (auto [n,name]:publicNames) if (!old.count(n)) return reject("missing platform shell port");
  bool used=false; circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp sim,cpu; unsigned instances=0;
  for (auto i:inner.getOps<InstanceOp>()) {
    ++instances;
    if (i.getName()=="sim"&&i.getModuleName()=="GGHostMemoryPlatformWrapper") sim=i;
    if (i.getName()=="cpu_port"&&i.getModuleName()=="GGCPUStreamPortAdapter") cpu=i;
  }
  if (used||instances!=2||!sim||!cpu) return reject("FPGATop shell requires the unique CPU platform wrapper");
  for (auto [n,name]:publicNames) {
    auto instance=n=="cpu_managed_axi4"?cpu:sim; Value v;
    for (auto [i,p]:llvm::enumerate(instance.getPortNames()))
      if (cast<StringAttr>(p).getValue()==n) v=instance.getResult(i);
    auto p=inner.getPorts()[old.at(n)]; Value a=inner.getBodyBlock()->getArgument(old.at(n));
    if (!v||v.getType()!=a.getType()||!a.hasOneUse()) return reject("platform port requires one exact child forwarding use");
    Value dest=p.direction==Direction::In?v:a,src=p.direction==Direction::In?a:v;
    unsigned count=0;
    for (auto c:inner.getOps<ConnectOp>()) count+=c.getDest()==dest&&c.getSrc()==src;
    if (count!=1) return reject("platform port requires a unique direct child connection");
  }
  unsigned hostClock=0,hostReset=0;
  for (auto a:raw) {
    auto d=dyn_cast<DictionaryAttr>(a); auto cls=d?d.getAs<StringAttr>("class"):StringAttr();
    if (!cls) return reject("malformed platform annotation");
    if (cls.getValue()==AnnotationClasses::HostClockSource) return reject("platform host clock source already exists");
    if (cls.getValue()==AnnotationClasses::HostClock||cls.getValue()==AnnotationClasses::HostReset) {
      bool clock=cls.getValue()==AnnotationClasses::HostClock;
      auto t=d.getAs<StringAttr>("target");
      if (!t||t.getValue()!="~"+oldName.str()+"|"+oldName.str()+">"+(clock?"hostClock":"hostReset"))
        return reject("FAME host clock/reset must identify the active platform inputs");
      (clock?hostClock:hostReset)++;
    }
  }
  if (hostClock!=1||hostReset!=1) return reject("platform shell requires one FAME host clock and reset identity");
  // All rejections precede mutation. The retained diagnostics have no new
  // consumers; normal CIRCT lowering can remove unused logic later.
  auto *ctx=circuit.getContext(); OpBuilder b(ctx); auto loc=circuit.getLoc();
  SmallVector<PortInfo> ports;
  for (auto n:{"hostClock","hostReset","ctrl","mem_0","cpu_managed_axi4"}) {
    auto p=inner.getPorts()[old.at(n)]; p.name=b.getStringAttr(publicNames.at(n)); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock()); auto instance=b.create<InstanceOp>(loc,inner,"sim");
  const StringRef inputNames[]{"hostClock","hostReset","ctrl","mem_0","cpu_managed_axi4"};
  for (auto [i,n]:llvm::enumerate(inputNames)) {
    auto p=ports[i]; Value a=wrapper.getBodyBlock()->getArgument(i),v=instance.getResult(old.at(n.str()));
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if (auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue(); if (v==op) return b.getStringAttr(np); if (!v.consume_front(op+"|")) return a;
      std::string suffix="|"+v.str(); StringRef ref(suffix);
      if (ref.consume_front(mp)) {
        auto end=ref.find_first_of(".["); auto root=ref.take_front(end).str();
        auto it=publicNames.find(root);
        if (it!=publicNames.end()) suffix="|"+newName.str()+">"+it->second+ref.drop_front(root.size()).str();
      }
      return b.getStringAttr(np+suffix);
    }
    if (auto xs=dyn_cast<ArrayAttr>(a)) {SmallVector<Attribute> out;for (auto x:xs) out.push_back(retarget(x));return b.getArrayAttr(out);}
    if (auto xs=dyn_cast<DictionaryAttr>(a)) {NamedAttrList out;for (auto x:xs) out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  auto transferred=cast<ArrayAttr>(retarget(raw)); SmallVector<Attribute> annotations(transferred.begin(),transferred.end());
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::HostClockSource)),b.getNamedAttr("target",b.getStringAttr("~FPGATop|FPGATop>clock"))}));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations)); circuit.setNameAttr(b.getStringAttr(newName));
  return success();
}
