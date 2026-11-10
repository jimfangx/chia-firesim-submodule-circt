// See LICENSE for license details.
// Oracle: AutoCounterTransform.implementViaBridge and BridgeTopWiring.
#include "goldengate/AutoCounterSynthesis.h"
#include "goldengate/AutoCounterAnalysis.h"
#include "goldengate/AutoCounterResetGate.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/PrintWiring.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/ConvertUTF.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
namespace {
struct EventInfo { goldengate::AutoCounterEvent event; StringAttr description, operation; };
struct Route { unsigned event,port=0,childPort=0;SmallVector<Operation*> path;StringAttr name; };
struct ModuleRoutes {FModuleOp module;unsigned oldPorts;SmallVector<Route> routes;SmallVector<std::pair<unsigned,PortInfo>> added;};
// BridgeTopWiring constructs localToAbsSource with Scala 2.13.10's immutable
// HashMap.map, keyed by pathless ReferenceTarget. Its CHAMP iterator emits a
// node's scalar payloads before descending into its children. This order is
// observable in AutoCounterParameters.eventMetadata and thus in counter MMIO
// addresses. Keep the oracle's ordering rather than sorting the exported names.
// All arithmetic below deliberately wraps as JVM Int arithmetic.
uint32_t rotate(uint32_t value, unsigned bits) {
  return (value << bits) | (value >> (32 - bits));
}
uint32_t mix(uint32_t hash, uint32_t data) {
  return rotate(hash ^ (rotate(data * 0xcc9e2d51u, 15) * 0x1b873593u), 13) * 5 + 0xe6546b64u;
}
uint32_t finish(uint32_t hash, unsigned length) {
  hash ^= length;
  hash = (hash ^ (hash >> 16)) * 0x85ebca6bu;
  hash = (hash ^ (hash >> 13)) * 0xc2b2ae35u;
  return hash ^ (hash >> 16);
}
uint32_t javaHash(StringRef text) {
  SmallVector<llvm::UTF16> units;
  // FIRRTL names are UTF-8. Hash UTF-16 code units as java.lang.String does,
  // including surrogate pairs, instead of hashing their UTF-8 bytes.
  bool valid = llvm::convertUTF8ToUTF16String(text, units);
  assert(valid && "FIRRTL name must be valid UTF-8");
  (void)valid;
  uint32_t hash = 0;
  for (auto unit : units) hash = hash * 31 + unit;
  return hash;
}
uint32_t productHash(StringRef prefix, std::initializer_list<uint32_t> parts) {
  uint32_t hash = mix(0xcafebabeu, javaHash(prefix));
  for (auto part : parts) hash = mix(hash, part);
  return finish(hash, parts.size());
}
uint32_t improve(uint32_t hash) {
  // scala.collection.Hashing.improve, used by immutable.HashMap.
  hash += ~(hash << 9);
  hash ^= hash >> 14;
  hash += hash << 4;
  return hash ^ (hash >> 10);
}
uint32_t referenceHash(StringRef circuit, StringRef module, StringRef ref) {
  const uint32_t emptySeq = finish(javaHash("Seq"), 0);
  return improve(productHash("ReferenceTarget", {javaHash(circuit),
      javaHash(module), emptySeq, javaHash(ref), emptySeq}));
}
SmallVector<unsigned> champOrder(ArrayRef<uint32_t> hashes,
                                ArrayRef<unsigned> indices) {
  SmallVector<unsigned> ordered;
  std::function<void(ArrayRef<unsigned>, unsigned)> visit =
      [&](ArrayRef<unsigned> group, unsigned shift) {
    if (shift >= 32) {
      // A full-hash collision preserves insertion order in the collision node.
      ordered.append(group.begin(), group.end());
      return;
    }
    std::array<SmallVector<unsigned>, 32> buckets;
    for (auto index : group) buckets[(hashes[index] >> shift) & 31].push_back(index);
    for (auto &bucket : buckets) if (bucket.size() == 1) ordered.push_back(bucket[0]);
    for (auto &bucket : buckets) if (bucket.size() > 1) visit(bucket, shift + 5);
  };
  visit(indices, 0);
  return ordered;
}
SmallVector<unsigned> sourceOrder(ArrayRef<EventInfo> events,
                                 ArrayRef<Route> routes, StringRef circuit) {
  SmallVector<uint32_t> components, references;
  for (auto event : events) {
    auto module = event.event.module;
    auto ref = StringRef(event.event.target).split('>').second;
    const auto mod = productHash("ModuleName", {javaHash(module.getName()),
        productHash("CircuitName", {javaHash(circuit)})});
    components.push_back(improve(productHash("ComponentName", {javaHash(ref), mod})));
    references.push_back(referenceHash(circuit, module.getName(), ref));
  }
  // TopWiring's source mapping follows local declaration order and then child
  // instance order. First group by ComponentName; map those groups to pathless
  // ReferenceTarget keys. Both are immutable HashMaps, including for <=4 keys.
  // The first traversal also supplies collision-node insertion order to the
  // second map, so deliberate Java String hash collisions remain well-defined.
  SmallVector<unsigned> indices;
  llvm::DenseSet<unsigned> seen;
  for (auto route : routes)
    if (seen.insert(route.event).second) indices.push_back(route.event);
  auto componentOrder = champOrder(components, indices);
  return champOrder(references, componentOrder);
}
void reserveNames(FModuleOp module,circt::Namespace &names) {
  for(auto name:module.getPortNamesAttr())names.newName(cast<StringAttr>(name).getValue());
  module.walk([&](Operation *op){if(auto name=op->getAttrOfType<StringAttr>("name"))names.newName(name.getValue());});
}
LogicalResult implement(CircuitOp circuit,ArrayRef<StringRef> coverModules,unsigned &count,std::string &error) {
  auto reject=[&](StringRef message){error=message.str();return failure();};
  SmallVector<goldengate::AutoCounterEvent> selected;
  if(failed(goldengate::analyzeSelectedAutoCounterEvents(circuit,coverModules,selected,error)))return failure();
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");OpBuilder b(circuit.getContext());
  SmallVector<EventInfo> events;
  for(auto event:selected) {
    auto annotation=cast<DictionaryAttr>(raw[event.annotationIndex]);auto description=annotation.getAs<StringAttr>("description");
    auto op=annotation.getAs<DictionaryAttr>("opType");auto operation=op?op.getAs<StringAttr>("class"):StringAttr();
    if(!annotation.get("opType"))operation=b.getStringAttr(goldengate::AnnotationClasses::AutoCounterAccumulate);
    auto type=dyn_cast<UIntType>(event.event.getType());auto reset=dyn_cast<UIntType>(event.reset.getType());
    if(!description||!operation||(operation!=goldengate::AnnotationClasses::AutoCounterAccumulate&&operation!=goldengate::AnnotationClasses::AutoCounterIdentity)||
       !type||!type.getWidth()||*type.getWidth()==0||*type.getWidth()>(1<<20)||!reset||reset.getWidth()!=1||event.label.empty())
      return reject("CSV AutoCounter needs typed operations, known UInt events, descriptions and synchronous UInt<1> resets");
    events.push_back({event,description,operation});
  }
  // Never mix a partially executed BridgeTopWiring transform into this stage.
  for(auto attr:raw)if(auto anno=dyn_cast<DictionaryAttr>(attr))if(auto cls=anno.getAs<StringAttr>("class"))
    if(cls==goldengate::AnnotationClasses::BridgeTopWiring||cls==goldengate::AnnotationClasses::BridgeTopWiringOutput||
       cls=="firrtl.transforms.TopWiringAnnotation"||cls=="firrtl.transforms.TopWiringOutputFilesAnnotation")
      return reject("CSV AutoCounter needs a clean target top-wiring boundary");
  if(events.empty()){unsigned removed=0;if(failed(goldengate::dropDisabledAutoCounterAnnotations(circuit,removed,error)))return failure();count=0;return success();}
  if(failed(goldengate::gateAutoCounterEventsWithReset(circuit,selected,error)))return failure();
  selected.clear();if(failed(goldengate::analyzeSelectedAutoCounterEvents(circuit,coverModules,selected,error)))return failure();
  for(auto [i,event]:llvm::enumerate(selected))events[i].event=event;
  // Plan typed scalar exports through every native instance path. Shared module
  // definitions expand once, and every use receives their changed signature.
  std::map<Operation*,ModuleRoutes> modules;SmallVector<Operation*> order;
  SmallVector<std::pair<InstanceOp,Operation*>> uses;FModuleOp top;
  {
    circt::igraph::InstanceGraph graph(circuit);auto *node=graph.lookup(b.getStringAttr(circuit.getName()));
    if(!node||!node->noUses()||!(top=dyn_cast<FModuleOp>(node->getModule().getOperation())))return reject("CSV AutoCounter requires an uninstantiated internal top");
    llvm::DenseMap<Operation*,SmallVector<unsigned>> local;
    for(auto [i,event]:llvm::enumerate(events))local[event.event.module].push_back(i);
    llvm::DenseSet<Operation*> active;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode*)> plan=[&](circt::igraph::InstanceGraphNode *current)->LogicalResult {
      auto module=dyn_cast<FModuleOp>(current->getModule().getOperation());if(!module)return success();auto *key=module.getOperation();
      if(modules.count(key))return success();if(!active.insert(key).second)return reject("recursive CSV AutoCounter target hierarchy");
      ModuleRoutes info{module,unsigned(module.getNumPorts()),{}, {}};
      for(auto i:local[key])info.routes.push_back({i,0,0,{}, {}});
      for(auto *record:*current) {
        if(failed(plan(record->getTarget())))return failure();auto child=modules.find(record->getTarget()->getModule().getOperation());
        if(child==modules.end()||child->second.routes.empty())continue;auto instance=record->getInstance<InstanceOp>();
        if(!instance||instance->getBlock()!=module.getBodyBlock())return reject("CSV AutoCounter needs module-scope FIRRTL instances");
        for(auto route:child->second.routes){route.childPort=route.port;route.path.insert(route.path.begin(),instance.getOperation());info.routes.push_back(std::move(route));}
      }
      circt::Namespace names;reserveNames(module,names);
      for(auto [i,route]:llvm::enumerate(info.routes)) {
        std::string suggested="autocounter_";for(auto *instance:route.path)suggested+=cast<InstanceOp>(instance).getName().str()+"_";
        suggested+=StringRef(events[route.event].event.target).split('>').second.str();route.port=info.oldPorts+i;route.name=b.getStringAttr(names.newName(suggested));
        info.added.push_back({info.oldPorts,PortInfo(route.name,events[route.event].event.event.getType(),Direction::Out)});
      }
      active.erase(key);modules.emplace(key,std::move(info));order.push_back(key);return success();
    };
    for(auto module:circuit.getOps<FModuleOp>())if(failed(plan(graph.lookup(module))))return failure();
    llvm::DenseSet<unsigned> reachable;for(auto &route:modules.at(top).routes)reachable.insert(route.event);
    if(reachable.size()!=events.size())return reject("CSV AutoCounter selected source has no path from active top");
    for(auto &[key,info]:modules)if(!info.routes.empty())for(auto *record:graph.lookup(info.module)->uses()) {
      auto instance=record->getInstance<InstanceOp>();auto parent=instance?instance->getParentOfType<FModuleOp>():FModuleOp();
      if(!instance||!parent||instance.getNumResults()!=info.oldPorts||instance->getBlock()!=parent.getBodyBlock())return reject("CSV AutoCounter source has an incompatible instance use");
      uses.push_back({instance,key});
    }
  }
  auto &topRoutes=modules.at(top).routes;
  // FindClockSources is common to Print and CSV. Its adapter records carry
  // real native clock SSA/path values; no printf or bundle is synthesized.
  SmallVector<goldengate::PrintStub> clockSources;SmallVector<goldengate::WiredPrint> clockRoutes;
  for(auto &event:events)clockSources.push_back({{}, {},event.event.clock,event.event.target,event.event.clockTarget,{}});
  for(auto &route:topRoutes) {
    std::string absolute="~"+circuit.getName().str()+"|"+top.getName().str();SmallVector<InstanceOp> path;
    for(auto *old:route.path){auto instance=cast<InstanceOp>(old);path.push_back(instance);absolute+="/"+instance.getName().str()+":"+instance.getModuleName().str();}
    absolute+=">"+StringRef(events[route.event].event.target).split('>').second.str();clockRoutes.push_back({route.event,std::move(path),{},absolute,{}});
  }
  SmallVector<goldengate::PrintClockSource> clocks;
  if(failed(goldengate::analyzePrintClockSources(circuit,clockSources,clockRoutes,clocks,error)))return failure();
  if(clocks.size()!=topRoutes.size())return reject("CSV AutoCounter clock analysis omitted a route");
  for(auto *key:order){auto &info=modules.at(key);if(!info.added.empty())info.module.insertPorts(info.added);}
  llvm::DenseMap<Operation*,InstanceOp> replacements;
  for(auto [instance,key]:uses){auto &info=modules.at(key);auto next=instance.cloneAndInsertPorts(info.added);
    for(auto attr:instance->getAttrs())if(!next->hasAttr(attr.getName()))next->setAttr(attr.getName(),attr.getValue());
    for(unsigned i=0;i<info.oldPorts;++i)instance.getResult(i).replaceAllUsesWith(next.getResult(i));replacements[instance]=next;instance.erase();}
  for(auto *key:order){auto &info=modules.at(key);b.setInsertionPointToEnd(info.module.getBodyBlock());
    for(auto &route:info.routes){Value value=events[route.event].event.event;if(!route.path.empty())value=replacements.lookup(route.path.front()).getResult(route.childPort);
      b.create<StrictConnectOp>(top.getLoc(),info.module.getArgument(route.port),value);}}
  circt::Namespace names;reserveNames(top,names);auto bit=UIntType::get(circuit.getContext(),1);
  std::string topPrefix="~"+circuit.getName().str()+"|"+top.getName().str()+">";
  // Groups sort by source Clock name, matching implementViaBridge. Within
  // each domain preserve BridgeTopWiring's pathless ReferenceTarget CHAMP
  // iteration, then its top-wiring instance order for that local source.
  struct Domain{Value source;std::string clock;SmallVector<unsigned> routes;};std::map<std::string,Domain> domains;
  SmallVector<SmallVector<unsigned>> sourceRoutes(events.size());
  for(auto [i,route]:llvm::enumerate(topRoutes))sourceRoutes[route.event].push_back(i);
  for(auto source:sourceOrder(events,topRoutes,circuit.getName()))for(auto i:sourceRoutes[source]) {
    auto &clock=clocks[i];auto [it,inserted]=domains.try_emplace(clock.sourceTarget);it->second.source=clock.source;it->second.routes.push_back(i);
  }
  SmallVector<std::pair<unsigned,PortInfo>> extra;unsigned oldPorts=top.getNumPorts();
  for(auto &[source,domain]:domains){domain.clock=names.newName("autocounter_"+StringRef(source).split('>').second.str());extra.push_back({oldPorts,PortInfo(b.getStringAttr(domain.clock),ClockType::get(circuit.getContext()),Direction::Out)});}
  struct Added{std::string trigger,node,reset;};SmallVector<Added> added;
  for(auto &[source,domain]:domains){Added row{names.newName("autocounter_triggerEnable").str(),names.newName("autocounter_triggerEnable_node").str(),names.newName("autocounter_globalResetCondition").str()};
    extra.push_back({oldPorts,PortInfo(b.getStringAttr(row.trigger),bit,Direction::Out)});extra.push_back({oldPorts,PortInfo(b.getStringAttr(row.reset),bit,Direction::Out)});added.push_back(std::move(row));}
  top.insertPorts(extra);b.setInsertionPointToEnd(top.getBodyBlock());
  auto one=b.create<ConstantOp>(top.getLoc(),bit,APInt(1,1));auto zero=b.create<ConstantOp>(top.getLoc(),bit,APInt(1,0));
  unsigned cursor=oldPorts;for(auto &[source,domain]:domains)b.create<StrictConnectOp>(top.getLoc(),top.getArgument(cursor++),domain.source);
  unsigned removed=0;if(failed(goldengate::dropDisabledAutoCounterAnnotations(circuit,removed,error)))return failure();
  raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> annotations(raw.begin(),raw.end());std::set<std::string> channelNames;
  for(auto a:raw)if(auto d=dyn_cast<DictionaryAttr>(a))if(auto c=d.getAs<StringAttr>("class");c&&c==goldengate::AnnotationClasses::ChannelConnection){auto name=d.getAs<StringAttr>("globalName");if(!name||!channelNames.insert(name.getValue().str()).second)return reject("CSV AutoCounter found duplicate or malformed existing channels");}
  auto channel=[&](StringRef name,StringRef clock)->DictionaryAttr {
    return b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),b.getNamedAttr("globalName",b.getStringAttr(name)),
      b.getNamedAttr("channelInfo",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),b.getNamedAttr("latency",b.getI64IntegerAttr(0))})),
      b.getNamedAttr("clock",b.getStringAttr(clock)),b.getNamedAttr("sources",b.getArrayAttr({b.getStringAttr(topPrefix+name.str())}))});};
  unsigned domainIndex=0;
  for(auto &[source,domain]:domains) {
    auto row=added[domainIndex++];auto triggerNode=b.create<NodeOp>(top.getLoc(),one.getResult(),b.getStringAttr(row.node));
    b.create<StrictConnectOp>(top.getLoc(),top.getArgument(cursor++),triggerNode.getResult());b.create<StrictConnectOp>(top.getLoc(),top.getArgument(cursor++),zero.getResult());
    NamedAttrList mapping;mapping.set(row.trigger,b.getStringAttr(row.trigger));mapping.set(row.reset,b.getStringAttr(row.reset));SmallVector<Attribute> metadata,channels;
    for(auto index:domain.routes) {
      auto &route=topRoutes[index];auto &event=events[route.event];auto port=route.name.getValue();std::string label=circuit.getName().str();
      for(auto *old:route.path)label+="_"+replacements.lookup(old).getName().str();label+="_"+event.event.label;
      metadata.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.widgets.EventMetadata")),b.getNamedAttr("portName",route.name),b.getNamedAttr("label",b.getStringAttr(label)),
        b.getNamedAttr("description",event.description),b.getNamedAttr("width",b.getI64IntegerAttr(*cast<UIntType>(event.event.event.getType()).getWidth())),
        b.getNamedAttr("opType",b.getDictionaryAttr({b.getNamedAttr("class",event.operation)}))}));
      mapping.set(port,b.getStringAttr(port));channels.push_back(channel(port,topPrefix+domain.clock));
    }
    if(!channelNames.insert(row.trigger).second||!channelNames.insert(row.reset).second)return reject("CSV AutoCounter predicate channel collides with existing channel");
    for(auto index:domain.routes)if(!channelNames.insert(topRoutes[index].name.getValue().str()).second)return reject("CSV AutoCounter event channel collides with existing channel");
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),b.getNamedAttr("target",b.getStringAttr(topPrefix+"autocounter")),
      b.getNamedAttr("channelMapping",mapping.getDictionary(circuit.getContext())),b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.AutoCounterBridgeModule")),
      b.getNamedAttr("widgetConstructorKey",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.widgets.AutoCounterParameters")),b.getNamedAttr("eventMetadata",b.getArrayAttr(metadata)),b.getNamedAttr("triggerName",b.getStringAttr(row.trigger)),b.getNamedAttr("resetPortName",b.getStringAttr(row.reset))}))}));
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::InternalTriggerSink)),b.getNamedAttr("target",b.getStringAttr(topPrefix+row.node)),b.getNamedAttr("clock",b.getStringAttr(topPrefix+domain.clock))}));
    annotations.push_back(channel(row.trigger,topPrefix+domain.clock));annotations.push_back(channel(row.reset,topPrefix+domain.clock));
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::GlobalResetSink)),b.getNamedAttr("target",b.getStringAttr(topPrefix+row.reset))}));
    annotations.append(channels.begin(),channels.end());
  }
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));count=domains.size();return success();
}
}
LogicalResult goldengate::synthesizeEnabledAutoCounterCSV(CircuitOp circuit,ArrayRef<StringRef> coverModules,unsigned &synthesized,std::string &error) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));unsigned count=0;
  if(failed(implement(*staged,coverModules,count,error)))return failure();
  if(failed(verify(*staged))){error="CSV AutoCounter synthesis produced invalid staged FIRRTL";return failure();}
  circuit->setAttrs((*staged)->getAttrs());auto &destination=circuit.getBodyBlock()->getOperations();destination.clear();destination.splice(destination.end(),staged->getBodyBlock()->getOperations());
  synthesized=count;return success();
}
