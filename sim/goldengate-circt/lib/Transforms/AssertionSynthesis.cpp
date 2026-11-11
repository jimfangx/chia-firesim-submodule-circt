// See LICENSE for license details.
// Oracle: midas.passes.AssertionSynthesis. Native AssertOp is accepted directly
// instead of requiring SFC's early assert -> printf/stop conversion.
#include "goldengate/AssertionSynthesis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/PrintWiring.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/APSInt.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;
namespace {
struct Event { Value failure, clock; std::string message; };
struct Route { unsigned event, port=0, childPort=0; SmallVector<Operation*> path; };
struct Routes { FModuleOp module; unsigned oldPorts; SmallVector<Route> routes; SmallVector<std::pair<unsigned,PortInfo>> added; };
void reserveNames(FModuleOp module, circt::Namespace &names) {
  for (auto name : module.getPortNamesAttr()) names.newName(cast<StringAttr>(name).getValue());
  module.walk([&](Operation *op) { if (auto name=op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue()); });
}
LogicalResult implement(CircuitOp circuit, unsigned &count, std::string &error) {
  auto reject=[&](StringRef message) { error=message.str(); return failure(); };
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("assertion synthesis needs retained annotations");
  std::set<std::pair<std::string,std::string>> excludes;
  for (auto attr:raw) if (auto anno=dyn_cast<DictionaryAttr>(attr)) {
    auto cls=anno.getAs<StringAttr>("class");
    if (!cls || cls!="midas.targetutils.ExcludeInstanceAssertsAnnotation") continue;
    StringAttr parent, instance;
    if (auto target=anno.getAs<ArrayAttr>("target"); target && target.size()==2) {
      parent=dyn_cast<StringAttr>(target[0]); instance=dyn_cast<StringAttr>(target[1]);
    } else if (auto target=anno.getAs<DictionaryAttr>("target")) {
      parent=target.getAs<StringAttr>("_1"); instance=target.getAs<StringAttr>("_2");
    }
    if (!parent || !instance) return reject("invalid ExcludeInstanceAssertsAnnotation tuple");
    excludes.insert({parent.getValue().str(),instance.getValue().str()});
  }
  OpBuilder b(circuit.getContext());
  SmallVector<Event> events;
  llvm::DenseMap<Operation*,SmallVector<unsigned>> local;
  SmallVector<Operation*> erase;
  std::set<std::string> removedPrints;
  for (auto module:circuit.getOps<FModuleOp>()) {
    circt::Namespace names; reserveNames(module,names);
    bool invalid=false;
    module.walk([&](Operation *op) {
      Value clock, condition; std::string message;
      if (auto assertion=dyn_cast<AssertOp>(op)) {
        clock=assertion.getClock(); message=assertion.getMessage().str();
        b.setInsertionPoint(op);
        condition=b.create<AndPrimOp>(op->getLoc(), assertion.getEnable(),
            b.create<NotPrimOp>(op->getLoc(),assertion.getPredicate()));
      } else if (auto stop=dyn_cast<StopOp>(op)) {
        if (!stop.getExitCode()) return;
        if (auto c=stop.getCond().getDefiningOp<ConstantOp>(); c && c.getValue().isZero()) return;
        clock=stop.getClock(); condition=stop.getCond();
        message="stop("+std::to_string(stop.getExitCode())+")";
        // Match only the legacy diagnostic on the exact same clock and enable.
        // Independent print statements are never consumed.
        for (auto print:module.getOps<PrintFOp>())
          if (print.getClock()==clock && print.getCond()==condition) {
            message=print.getFormatString().str(); erase.push_back(print);
            removedPrints.insert("~"+circuit.getName().str()+"|"+module.getName().str()+">"+print.getName().str());
          }
      } else return;
      if (op->getBlock()!=module.getBodyBlock() || !isa<ClockType>(clock.getType())) { invalid=true; return; }
      b.setInsertionPoint(op);
      auto node=b.create<NodeOp>(op->getLoc(),condition,b.getStringAttr(names.newName("assert_"+std::to_string(local[module].size()))));
      local[module].push_back(events.size());
      events.push_back({node.getResult(),clock,std::move(message)}); erase.push_back(op);
    });
    if (invalid) return reject("assertions require module-scope clocks and enables after ExpandWhens");
  }
  if (events.empty()) { count=0; return success(); }
  std::map<Operation*,Routes> modules; SmallVector<Operation*> order;
  SmallVector<std::pair<InstanceOp,Operation*>> uses; FModuleOp top;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *root=graph.lookup(b.getStringAttr(circuit.getName()));
    if (!root || !root->noUses() || !(top=dyn_cast<FModuleOp>(root->getModule().getOperation())))
      return reject("assertions require an uninstantiated internal top");
    llvm::DenseSet<Operation*> active;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode*)> plan=[&](auto *current)->LogicalResult {
      auto module=dyn_cast<FModuleOp>(current->getModule().getOperation()); if (!module) return success();
      auto *key=module.getOperation(); if (modules.count(key)) return success();
      if (!active.insert(key).second) return reject("recursive assertion hierarchy");
      Routes info{module,unsigned(module.getNumPorts()),{}, {}};
      for (auto i:local[key]) info.routes.push_back({i});
      // Local assertions precede children. Distinct instances of a shared module
      // keep distinct bits and messages even when the failure SSA is shared.
      for (auto *record:*current) {
        if (failed(plan(record->getTarget()))) return failure();
        auto instance=record->template getInstance<InstanceOp>();
        auto child=modules.find(record->getTarget()->getModule().getOperation());
        if (child==modules.end() || child->second.routes.empty()) continue;
        if (!instance || instance->getBlock()!=module.getBodyBlock()) return reject("assertions require module-scope instances");
        if (excludes.count({module.getName().str(),instance.getName().str()})) continue;
        for (auto route:child->second.routes) { route.childPort=route.port; route.path.insert(route.path.begin(),instance); info.routes.push_back(std::move(route)); }
      }
      circt::Namespace names; reserveNames(module,names);
      for (auto [i,route]:llvm::enumerate(info.routes)) {
        route.port=info.oldPorts+i;
        info.added.push_back({info.oldPorts,PortInfo(b.getStringAttr(names.newName("midasAssert")),UIntType::get(b.getContext(),1),Direction::Out)});
      }
      active.erase(key); modules.emplace(key,std::move(info)); order.push_back(key); return success();
    };
    for (auto module:circuit.getOps<FModuleOp>()) if (failed(plan(graph.lookup(module)))) return failure();
    for (auto &[key,info]:modules) if (!info.routes.empty()) for (auto *record:graph.lookup(info.module)->uses()) {
      auto instance=record->getInstance<InstanceOp>(); auto parent=instance?instance->getParentOfType<FModuleOp>():FModuleOp();
      if (!instance || !parent || instance->getBlock()!=parent.getBodyBlock()) return reject("incompatible assertion instance use");
      uses.push_back({instance,key});
    }
  }
  auto &routes=modules.at(top).routes;
  SmallVector<goldengate::PrintStub> clockSources;
  SmallVector<goldengate::WiredPrint> clockRoutes;
  for (auto &event:events) clockSources.push_back({{}, {},event.clock,{}, {},{}});
  for (auto &route:routes) {
    SmallVector<InstanceOp> path; std::string absolute=top.getName().str();
    for (auto *op:route.path) { auto instance=cast<InstanceOp>(op); path.push_back(instance); absolute+="."+instance.getName().str(); }
    clockRoutes.push_back({route.event,std::move(path),{},absolute,{}});
  }
  SmallVector<goldengate::PrintClockSource> clocks;
  if (failed(goldengate::analyzePrintClockSources(circuit,clockSources,clockRoutes,clocks,error,true))) return failure();
  // Only children need per-assertion export ports. The top packs the local
  // values and child exports directly into its per-domain bridge outputs.
  for (auto *key:order) { auto &info=modules.at(key); if (info.module!=top && !info.added.empty()) info.module.insertPorts(info.added); }
  llvm::DenseMap<Operation*,InstanceOp> replacements;
  for (auto [instance,key]:uses) {
    auto &info=modules.at(key); auto next=instance.cloneAndInsertPorts(info.added);
    for (auto attr:instance->getAttrs()) if (!next->hasAttr(attr.getName())) next->setAttr(attr.getName(),attr.getValue());
    for (unsigned i=0;i<info.oldPorts;++i) instance.getResult(i).replaceAllUsesWith(next.getResult(i));
    replacements[instance]=next; instance.erase();
  }
  for (auto *key:order) {
    auto &info=modules.at(key); b.setInsertionPointToEnd(info.module.getBodyBlock());
    if (info.module==top) continue;
    for (auto &route:info.routes) {
      Value value=events[route.event].failure;
      if (!route.path.empty()) value=replacements.lookup(route.path.front()).getResult(route.childPort);
      b.create<ConnectOp>(top.getLoc(),info.module.getArgument(route.port),value);
    }
  }
  struct Domain { Value clock; SmallVector<unsigned> routes; std::string asserts, reset, name; };
  std::map<std::string,Domain> domains;
  for (auto clock:clocks) { auto &domain=domains[clock.sourceTarget]; domain.clock=clock.source; domain.routes.push_back(clock.routeIndex); }
  circt::Namespace names; reserveNames(top,names); unsigned oldPorts=top.getNumPorts();
  SmallVector<std::pair<unsigned,PortInfo>> ports;
  for (auto &[key,domain]:domains) {
    domain.asserts=names.newName("midasAsserts"); domain.reset=names.newName("midasAsserts_globalResetCondition"); domain.name=names.newName("midasAsserts_clock");
    ports.push_back({oldPorts,PortInfo(b.getStringAttr(domain.asserts),UIntType::get(b.getContext(),domain.routes.size()),Direction::Out)});
    ports.push_back({oldPorts,PortInfo(b.getStringAttr(domain.reset),UIntType::get(b.getContext(),1),Direction::Out)});
    ports.push_back({oldPorts,PortInfo(b.getStringAttr(domain.name),ClockType::get(b.getContext()),Direction::Out)});
  }
  top.insertPorts(ports); b.setInsertionPointToEnd(top.getBodyBlock());
  SmallVector<Attribute> annotations;
  for (auto attr:raw) {
    auto anno=dyn_cast<DictionaryAttr>(attr); auto cls=anno?anno.getAs<StringAttr>("class"):StringAttr();
    auto target=anno?anno.getAs<StringAttr>("target"):StringAttr();
    if (cls==goldengate::AnnotationClasses::SynthPrintf && target && removedPrints.count(target.getValue().str())) continue;
    annotations.push_back(attr);
  }
  auto prefix="~"+circuit.getName().str()+"|"+top.getName().str()+">";
  auto channel=[&](StringRef port,StringRef clock) {
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName",b.getStringAttr(port)),
      b.getNamedAttr("channelInfo",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),b.getNamedAttr("latency",b.getI64IntegerAttr(0))})),
      b.getNamedAttr("clock",b.getStringAttr(prefix+clock.str())),
      b.getNamedAttr("sources",b.getArrayAttr({b.getStringAttr(prefix+port.str())}))}));
  };
  unsigned cursor=oldPorts; count=0;
  for (auto &[key,domain]:domains) {
    Value packed; SmallVector<Attribute> messages;
    for (auto i:domain.routes) {
      auto &route=routes[i];
      Value bit=events[route.event].failure;
      if (!route.path.empty()) bit=replacements.lookup(route.path.front()).getResult(route.childPort);
      if (packed) packed=b.create<CatPrimOp>(top.getLoc(),bit,packed);
      else packed=bit;
      auto &event=events[routes[i].event];
      auto module=event.failure.getParentBlock()->getParentOp();
      messages.push_back(b.getStringAttr("module: "+cast<FModuleOp>(module).getName().str()+", path: "+clockRoutes[i].absoluteSource+"]\n"+event.message));
      ++count;
    }
    b.create<ConnectOp>(top.getLoc(),top.getArgument(cursor++),packed);
    auto zero=b.create<ConstantOp>(top.getLoc(),UIntType::get(b.getContext(),1),APInt(1,0));
    b.create<ConnectOp>(top.getLoc(),top.getArgument(cursor++),zero.getResult());
    b.create<ConnectOp>(top.getLoc(),top.getArgument(cursor++),domain.clock);
    channel(domain.asserts,domain.name); channel(domain.reset,domain.name);
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::GlobalResetSink)),b.getNamedAttr("target",b.getStringAttr(prefix+domain.reset))}));
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),b.getNamedAttr("target",b.getStringAttr(prefix+domain.asserts)),
      b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.AssertBridgeModule")),
      b.getNamedAttr("channelMapping",b.getDictionaryAttr({b.getNamedAttr(domain.asserts,b.getStringAttr(domain.asserts)),b.getNamedAttr(domain.reset,b.getStringAttr(domain.reset))})),
      b.getNamedAttr("widgetConstructorKey",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("midas.widgets.AssertBridgeParameters")),
        b.getNamedAttr("assertPortName",b.getStringAttr(domain.asserts)),b.getNamedAttr("resetPortName",b.getStringAttr(domain.reset)),b.getNamedAttr("assertMessages",b.getArrayAttr(messages))}))}));
  }
  llvm::DenseSet<Operation*> erased;
  for (auto *op:erase) if (erased.insert(op).second) op->erase();
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return success();
}
}
LogicalResult goldengate::synthesizeAssertions(CircuitOp circuit,unsigned &count,std::string &error) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone())); unsigned next=0;
  if (failed(implement(*staged,next,error))) return failure();
  if (failed(verify(*staged))) { error="assertion synthesis produced invalid staged FIRRTL"; return failure(); }
  circuit->setAttrs((*staged)->getAttrs()); auto &dest=circuit.getBodyBlock()->getOperations();
  dest.clear(); dest.splice(dest.end(),staged->getBodyBlock()->getOperations()); count=next; return success();
}
