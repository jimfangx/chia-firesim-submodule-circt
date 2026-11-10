// See LICENSE for license details.
// Oracle: Widget.HasWidgets.genCtrlIO / NastiRecursiveInterconnect, actual CSV
// widgets and Widget.genHeader. Compose existing helpers rather than duplicating
// local AXI adapters or inventing stand-alone driver addresses.
#include "goldengate/AutoCounterControl.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlReadDispatch.h"
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlReadArbiter.h"
#include "goldengate/ControlWriteArbiter.h"
#include "goldengate/ControlWriteTracker.h"
#include "goldengate/SimulationMasterControl.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/BitVector.h"
#include <set>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::assembleAutoCounterControl(
    CircuitOp circuit, ArrayRef<FModuleOp> hosts,
    ArrayRef<ControlMMIORegion> regions, std::string &header,
    std::string &error) {
  auto reject=[&](StringRef why){error=why.str();return failure();};
  if(circuit.getName()!="GGAutoCounterBridgeHostWrapper"||hosts.empty())
    return reject("CSV control requires the bound host top and complete nonempty hosts");
  for(auto module:circuit.getOps<FModuleOp>())if(module.getName()==circuit.getName())
    for(auto port:module.getPorts())if(port.name.getValue()=="ctrl"||port.name.getValue().starts_with("ctrl_"))
      return reject("CSV global control reserves the ctrl routing namespace");
  SmallVector<ControlMMIOWidget> catalog;
  if(failed(deriveAutoCounterControlMMIOCatalog(circuit,hosts,catalog,error)))return failure();
  if(regions.size()!=catalog.size())
    return reject("CSV control requires exactly the implemented CSV regions; unrelated banks must be bound by a platform assembly");
  std::set<std::string> expected;
  for(auto &widget:catalog)expected.insert(widget.name.str());
  for(auto &region:regions)if(!expected.erase(region.name.str()))
    return reject("CSV control region has an unknown or duplicate widget");
  if(!expected.empty())return reject("CSV control region omits an implemented widget");
  auto info=hosts.front()->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");
  unsigned addressBits=info.getAs<IntegerAttr>("addressBits").getInt();
  for(auto host:hosts) {
    auto other=host->getAttrOfType<DictionaryAttr>("goldengate.autocounterHost");
    auto address=other.getAs<IntegerAttr>("addressBits");
    auto ids=other.getAs<IntegerAttr>("idBits");
    if(!address||!ids||address.getInt()!=addressBits||ids.getInt()!=12)
      return reject("CSV global control requires uniform address width and twelve-bit IDs");
  }
  // Header validates exact bank sizes, all bounds, permissions and overlaps.
  // Nothing is published until the complete connected network verifies.
  std::string nextHeader;
  if(failed(prepareAutoCounterBridgeHeader(circuit,hosts,regions,nextHeader,error)))return failure();
  SmallVector<ControlWidgetPort> ports;
  for(auto [i,widget]:llvm::enumerate(catalog))
    ports.push_back({widget.name.str(),"autocounter_"+std::to_string(i)+"_ctrl"});
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  auto stage=[&](StringRef name,auto action)->LogicalResult {
    if(failed(action())){error=name.str()+": "+error;return failure();}
    return success();
  };
  if(failed(stage("error slave",[&]{return addControlErrorSlave(*staged,addressBits,12,"GGAutoCounterBridgeHostWrapper",error);}))||
     failed(stage("address decode",[&]{return addControlAddressDecode(*staged,addressBits,regions,error);}))||
     failed(stage("AW/W route",[&]{return addControlWriteRoute(*staged,error);}))||
     failed(stage("write dispatch",[&]{return addControlWriteDispatch(*staged,error);}))||
     failed(stage("widget writes",[&]{return bindControlWidgetWrites(*staged,ports,error);}))||
     failed(stage("read dispatch",[&]{return addControlReadDispatch(*staged,error);}))||
     failed(stage("read tracker",[&]{return addControlReadTracker(*staged,error);}))||
     failed(stage("read arbiter",[&]{return addControlReadArbiter(*staged,error);}))||
     failed(stage("write arbiter",[&]{return addControlWriteArbiter(*staged,error);}))||
     failed(stage("write tracker",[&]{return addControlWriteTracker(*staged,error);}))||
     failed(stage("master",[&]{return bindControlMaster(*staged,"GGControlWriteTrackerWrapper",error);})) )return failure();
  FModuleOp top;
  for(auto module:staged->getOps<FModuleOp>())if(module.getName()==staged->getName())top=module;
  if(!top)return reject("CSV global control is missing its final top");
  unsigned masters=0;
  llvm::BitVector diagnostics(top.getNumPorts());
  SmallVector<Operation *> diagnosticConnections;
  for(auto [index,port]:llvm::enumerate(top.getPorts())) {
    auto name=port.name.getValue();
    if(name=="ctrl"){++masters;continue;}
    if(name.starts_with("autocounter_"))
      return reject("CSV global control left an unbound local slave");
    if(!name.starts_with("ctrl_"))continue;
    // The generic stages retain monitor outputs for staged unit tests. Close
    // these only after every real request/response input has been consumed.
    // Their source nets remain in the inner network; no state/logic is removed.
    if(port.direction!=Direction::Out)
      return reject("CSV global control left an unbound routing input");
    auto argument=top.getArgument(index);
    for(auto &use:argument.getUses()) {
      auto *operation=use.getOwner();
      if(auto connect=dyn_cast<ConnectOp>(operation)) {
        if(connect.getDest()!=argument)return reject("CSV control monitor output has a source use");
      }else if(auto connect=dyn_cast<StrictConnectOp>(operation)) {
        if(connect.getDest()!=argument)return reject("CSV control monitor output has a source use");
      }else return reject("CSV control monitor output has a non-connect use");
      diagnosticConnections.push_back(operation);
    }
    diagnostics.set(index);
  }
  for(auto *connection:diagnosticConnections)connection->erase();
  top.erasePorts(diagnostics);
  if(masters!=1)return reject("CSV global control failed to produce exactly one master");
  Builder builder(circuit.getContext());
  top->setAttr("goldengate.autocounterGlobalControl",builder.getUnitAttr());
  if(failed(verify(*staged)))return reject("CSV global control produced invalid FIRRTL");
  circuit->setAttrs((*staged)->getAttrs());
  auto &destination=circuit.getBodyBlock()->getOperations();
  destination.clear();
  destination.splice(destination.end(),staged->getBodyBlock()->getOperations());
  header=std::move(nextHeader);
  return success();
}
