// See LICENSE for license details.
// Input: uninstantiated widget MCRFile wrapper and the recorded U250 catalog.
// Widget.scala attaches SimulationMaster/TSI/BlockDev/FASED at indices 8/3/0/1.
// Consume AW/W/AR dispatch and B/R arbiter boundaries using FIRRTL operations.
// The existing arbiters already retire tracker entries on accepted responses;
// their ready signals, not response valid alone, return to the MCRFile.
// Retained annotations: copied ports transfer; consumed targets stay in inner.
// Output: internally connected widget slave; other unported slaves stay explicit.
// All catalog, identity, type and direction checks precede mutation. No analyses
// or annotation classes are consumed; retained targets follow wrapper identity.
// These bindings add no state and preserve host clock/reset and tracker wiring.
#include "goldengate/SimulationMasterControl.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

namespace {
struct WidgetBinding {
  llvm::StringRef inputName, wrapperName, controlPort, catalogName, slaveAttr;
  unsigned slaveIndex;
  uint64_t start, size;
};
LogicalResult bindWidgetControl(CircuitOp circuit, const WidgetBinding &spec,
                                std::string &error) {
  auto wrapperName = spec.wrapperName;
  const std::string index = std::to_string(spec.slaveIndex);
  auto reject = [&](llvm::StringRef s) { error = spec.catalogName.str()+": "+s.str(); return failure(); };
  if (circuit.getName() != spec.inputName)
    return reject("widget binding requires its MCRFile wrapper");
  FModuleOp inner, decoder;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName) return reject("widget binding wrapper exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGControlAddressDecode") decoder = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto regions = decoder ? decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions") : ArrayAttr();
  if (!inner || !raw || !regions || regions.size() != 11)
    return reject("widget binding needs retained annotations and eleven regions");
  auto row = dyn_cast<DictionaryAttr>(regions[spec.slaveIndex]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto slave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  if (!name || name != spec.catalogName || !slave || slave.getInt() != spec.slaveIndex ||
      !start || start.getInt() != spec.start || !size || size.getInt() != spec.size)
    return reject("widget allocation differs from the recorded U250 catalog");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("widget binding requires an uninstantiated top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  struct Field { const char *name; unsigned width; };
  const Field addresses[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
    {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
  const Field data[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
  const Field responses[]{{"resp",2},{"id",12},{"user",1}};
  const Field reads[]{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}};
  auto token = [&](ArrayRef<Field> fields) {
    SmallVector<BundleType::BundleElement> payload;
    for (auto f : fields) payload.push_back({b.getStringAttr(f.name),false,uint(f.width)});
    return BundleType::get(ctx, {{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},
      {b.getStringAttr("bits"),false,BundleType::get(ctx,payload)}});
  };
  auto control = BundleType::get(ctx, {{b.getStringAttr("aw"),false,token(addresses)},
    {b.getStringAttr("w"),false,token(data)}, {b.getStringAttr("b"),true,token(responses)},
    {b.getStringAttr("ar"),false,token(addresses)}, {b.getStringAttr("r"),true,token(reads)}});
  std::map<std::string,unsigned> old, copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) old.emplace(p.name.getValue().str(),i);
  std::set<std::string> consumed;
  auto required = [&](std::string n, Type t, Direction d, bool consume = true) {
    auto it = old.find(n);
    if (it == old.end() || inner.getPorts()[it->second].type != t || inner.getPorts()[it->second].direction != d) return false;
    if (consume) consumed.insert(n);
    return true;
  };
  if (!required(spec.controlPort.str(),control,Direction::In) ||
      !required("ctrl_read_dispatch_slave_"+index+"_ar",token(addresses),Direction::Out))
    return reject("widget binding requires exact control and AR bundles");
  auto dispatched = [](llvm::StringRef ch, llvm::StringRef f) {
    return ch == "aw" ? f == "addr" || f == "len" || f == "id" : f == "data" || f == "last";
  };
  for (auto ch : {"aw","w"}) {
    std::string prefix = "ctrl_write_dispatch_slave_"+index+"_" + std::string(ch);
    if (!required(prefix+"_ready",uint(1),Direction::In) || !required(prefix+"_valid",uint(1),Direction::Out))
      return reject("widget binding is missing AW/W handshakes");
    for (auto f : ch == StringRef("aw") ? ArrayRef<Field>(addresses) : ArrayRef<Field>(data)) {
      bool local = dispatched(ch,f.name);
      std::string n = local ? prefix+"_bits_"+f.name : "ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name;
      if (!required(n,uint(f.width),local ? Direction::Out : Direction::In,local))
        return reject("widget binding is missing request payload or shared metadata");
    }
  }
  for (auto ch : {"r","b"}) {
    std::string prefix = ch == StringRef("r") ? "ctrl_read_arb_in_"+index : "ctrl_write_arb_in_"+index;
    if (!required(prefix+"_ready",uint(1),Direction::Out) || !required(prefix+"_valid",uint(1),Direction::In))
      return reject("widget binding is missing response handshakes");
    for (auto f : ch == StringRef("r") ? ArrayRef<Field>(reads) : ArrayRef<Field>(responses))
      if (!required(prefix+"_bits_"+f.name,uint(f.width),Direction::In))
        return reject("widget binding is missing a response field");
  }
  // Every check above precedes mutation, including shared metadata validation.
  SmallVector<PortInfo> ports;
  for (auto p : inner.getPorts()) if (!consumed.count(p.name.getValue().str())) {
    copied[p.name.getValue().str()] = ports.size(); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  wrapper->setAttr(spec.slaveAttr,b.getI32IntegerAttr(spec.slaveIndex));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc,inner,"sim");
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc,v,n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc,d,s); };
  for (auto [n,i] : copied) {
    Value v = sim.getResult(old.at(n)), a = wrapper.getBodyBlock()->getArgument(i);
    auto d = inner.getPorts()[old.at(n)].direction;
    b.create<ConnectOp>(loc,d == Direction::In ? v : a,d == Direction::In ? a : v);
  }
  Value ctrl = sim.getResult(old.at(spec.controlPort.str()));
  b.create<ConnectOp>(loc,field(ctrl,"ar"),sim.getResult(old.at("ctrl_read_dispatch_slave_"+index+"_ar")));
  for (auto ch : {"aw","w"}) {
    Value channel = field(ctrl,ch), bits = field(channel,"bits");
    std::string prefix = "ctrl_write_dispatch_slave_"+index+"_"+std::string(ch);
    connect(sim.getResult(old.at(prefix+"_ready")),field(channel,"ready"));
    connect(field(channel,"valid"),sim.getResult(old.at(prefix+"_valid")));
    for (auto f : ch == StringRef("aw") ? ArrayRef<Field>(addresses) : ArrayRef<Field>(data)) {
      std::string n = dispatched(ch,f.name) ? prefix+"_bits_"+f.name : "ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name;
      connect(field(bits,f.name),sim.getResult(old.at(n)));
    }
  }
  for (auto ch : {"r","b"}) {
    Value channel = field(ctrl,ch), bits = field(channel,"bits");
    std::string prefix = ch == StringRef("r") ? "ctrl_read_arb_in_"+index : "ctrl_write_arb_in_"+index;
    connect(field(channel,"ready"),sim.getResult(old.at(prefix+"_ready")));
    connect(sim.getResult(old.at(prefix+"_valid")),field(channel,"valid"));
    for (auto f : ch == StringRef("r") ? ArrayRef<Field>(reads) : ArrayRef<Field>(responses))
      connect(sim.getResult(old.at(prefix+"_bits_"+f.name)),field(bits,f.name));
  }
  std::string op = "~"+circuit.getName().str(), np = "~"+wrapperName.str(), mp = "|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue(); if (v == op) return b.getStringAttr(np);
      if (!v.consume_front(op+"|")) return a;
      std::string suffix = "|"+v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(mp) && copied.count(ref.take_front(ref.find_first_of(".[")).str()))
        suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      return b.getStringAttr(np+suffix);
    }
    if (auto xs = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> out; for (auto x : xs) out.push_back(retarget(x)); return b.getArrayAttr(out); }
    if (auto xs = dyn_cast<DictionaryAttr>(a)) { NamedAttrList out; for (auto x : xs) out.set(x.getName(),retarget(x.getValue())); return out.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw)); circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}

} // namespace

LogicalResult goldengate::bindSimulationMasterControl(CircuitOp circuit,
                                                     std::string &error) {
  return bindWidgetControl(circuit,
      {"GGSimulationMasterControlWrapper", "GGSimulationMasterBoundWrapper",
       "simulationMaster_ctrl", "SimulationMaster_0",
       "goldengate.simulationMasterSlave", 8, 0x220, 0x10}, error);
}

LogicalResult goldengate::bindTSIBridgeControl(CircuitOp circuit,
                                              std::string &error) {
  return bindWidgetControl(circuit,
      {"GGTSIBridgeControlWrapper", "GGTSIBridgeBoundWrapper",
       "tsiBridge_ctrl", "TSIBridgeModule_0", "goldengate.tsiSlave",
       3, 0x140, 0x40}, error);
}

// Requires: uninstantiated GGBlockDevBridgeControlWrapper, exact Nasti and
// dispatcher/arbiter ports, and the eleven-region U250 control catalog.
// Consumes: BlockDev control and slave-0 dispatch/response boundary ports;
// no annotation classes are consumed or produced. Copied targets transfer,
// while targets of internalized ports retain the inner module identity.
// Mutates: adds a stateless FIRRTL wrapper connecting all five AXI channels.
// Analyses required: none. Preserves: inner state, clocks, channels, constructor
// metadata and existing tracker retirement through arbiter ready signals.
// Output: internally bound slave 0 at [0, 128); other boundaries remain explicit.
LogicalResult goldengate::bindBlockDevBridgeControl(CircuitOp circuit,
                                                  std::string &error) {
  return bindWidgetControl(circuit,
      {"GGBlockDevBridgeControlWrapper", "GGBlockDevBridgeBoundWrapper",
       "blockdevBridge_ctrl", "BlockDevBridgeModule_0", "goldengate.blockdevSlave",
       0, 0x0, 0x80}, error);
}

// Requires: uninstantiated GGFASEDBridgeControlWrapper, exact Nasti and
// dispatcher/arbiter ports, and the eleven-region U250 control catalog.
// Consumes: FASED control and slave-1 dispatch/response boundary ports.
// No annotation classes are consumed or produced. Copied targets transfer;
// targets of internalized ports retain their inner module identity.
// Mutates: adds a stateless FIRRTL wrapper connecting all five AXI channels.
// Analyses required: none. Preserves: inner timing state, host clocks/reset,
// channels, constructor metadata and tracker retirement on accepted responses.
// Output: internally bound slave 1 at [128, 256); other boundaries stay explicit.
LogicalResult goldengate::bindFASEDBridgeControl(CircuitOp circuit,
                                               std::string &error) {
  return bindWidgetControl(circuit,
      {"GGFASEDBridgeControlWrapper", "GGFASEDBridgeBoundWrapper",
       "fasedBridge_ctrl", "FASEDMemoryTimingModel_0", "goldengate.fasedSlave",
       1, 0x80, 0x80}, error);
}

// Requires: uninstantiated GGFASEDBridgeBoundWrapper and exact U250 master
// request/response boundaries. Widget.scala connects the host master to the
// recursive interconnect; the existing route/tracker helpers supply readiness.
// Consumes: 32 scalar boundaries and the aggregate AR master boundary.
// Mutates: creates one stateless wrapper with the full host-facing ctrl bundle.
// No analyses or annotation classes are consumed. Copied targets transfer;
// consumed targets stay on the inner module. Preserves all inner state/clocks,
// bridge metadata and response acceptance through the existing arbiters.
LogicalResult goldengate::bindControlMaster(CircuitOp circuit,
                                          std::string &error) {
  constexpr llvm::StringLiteral inputName="GGFASEDBridgeBoundWrapper";
  constexpr llvm::StringLiteral wrapperName="GGControlMasterWrapper";
  auto reject=[&](llvm::StringRef s){error=s.str();return failure();};
  if(circuit.getName()!=inputName)return reject("control master requires all widget bindings");
  FModuleOp inner;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()==wrapperName)return reject("control master wrapper exists");
    if(m.getModuleName()==inputName)inner=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!inner||!raw)return reject("control master requires retained annotations and active top");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inputName;});
  if(used)return reject("control master requires an uninstantiated top");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  struct Field {const char *name;unsigned width;};
  const Field addresses[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
    {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
  const Field data[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
  const Field responses[]{{"resp",2},{"id",12},{"user",1}};
  const Field reads[]{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}};
  auto token=[&](ArrayRef<Field> fs){
    SmallVector<BundleType::BundleElement> fields;
    for(auto f:fs)fields.push_back({b.getStringAttr(f.name),false,uint(f.width)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},
      {b.getStringAttr("bits"),false,BundleType::get(ctx,fields)}});
  };
  auto arType=token(addresses);
  auto control=BundleType::get(ctx,{{b.getStringAttr("aw"),false,arType},
    {b.getStringAttr("w"),false,token(data)},{b.getStringAttr("b"),true,token(responses)},
    {b.getStringAttr("ar"),false,arType},{b.getStringAttr("r"),true,token(reads)}});
  struct Wire {std::string boundary,channel,leaf;unsigned width;Direction direction;};
  SmallVector<Wire> wires;
  for(auto ch:{"aw","w"}) {
    auto prefix="ctrl_write_route_"+std::string(ch);
    wires.push_back({prefix+"_ready",ch,"ready",1,Direction::Out});
    wires.push_back({prefix+"_valid",ch,"valid",1,Direction::In});
    for(auto f:ch==StringRef("aw")?ArrayRef<Field>(addresses):ArrayRef<Field>(data)) {
      std::string n="ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+f.name;
      if(ch==StringRef("aw")&&StringRef(f.name)=="addr")n="ctrl_decode_aw_addr";
      if(ch==StringRef("w")&&StringRef(f.name)=="last")n="ctrl_write_route_w_last";
      wires.push_back({n,ch,"bits."+std::string(f.name),f.width,Direction::In});
    }
  }
  for(auto ch:{"b","r"}) {
    auto prefix="ctrl_"+std::string(ch==StringRef("r")?"read":"write")+"_arb_out";
    wires.push_back({prefix+"_ready",ch,"ready",1,Direction::In});
    wires.push_back({prefix+"_valid",ch,"valid",1,Direction::Out});
    for(auto f:ch==StringRef("r")?ArrayRef<Field>(reads):ArrayRef<Field>(responses))
      wires.push_back({prefix+"_bits_"+f.name,ch,"bits."+std::string(f.name),f.width,Direction::Out});
  }
  std::map<std::string,unsigned> old,copied;std::set<std::string> consumed;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  if(old.count("ctrl"))return reject("host control port exists");
  auto exact=[&](std::string n,Type t,Direction d){
    auto i=old.find(n);
    if(i==old.end()||inner.getPorts()[i->second].type!=t||inner.getPorts()[i->second].direction!=d)return false;
    consumed.insert(n);return true;
  };
  for(auto &w:wires)if(!exact(w.boundary,uint(w.width),w.direction))
    return reject("control master has a missing or incompatible scalar boundary");
  if(!exact("ctrl_read_dispatch_master_ar",arType,Direction::In))
    return reject("control master requires the full AR bundle");
  // No mutation until every request/response field has been checked.
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())) {
    copied[p.name.getValue().str()]=ports.size();ports.push_back(p);
  }
  unsigned controlIndex=ports.size();ports.push_back({b.getStringAttr("ctrl"),control,Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());auto sim=b.create<InstanceOp>(loc,inner,"sim");
  for(auto [n,i]:copied) {
    Value v=sim.getResult(old.at(n)),a=wrapper.getBodyBlock()->getArgument(i);
    auto d=inner.getPorts()[old.at(n)].direction;
    b.create<ConnectOp>(loc,d==Direction::In?v:a,d==Direction::In?a:v);
  }
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  Value ctrl=wrapper.getBodyBlock()->getArgument(controlIndex);
  for(auto &w:wires) {
    Value a=field(ctrl,w.channel),v=sim.getResult(old.at(w.boundary));
    llvm::StringRef leaf(w.leaf);
    if(leaf.consume_front("bits."))a=field(field(a,"bits"),leaf);else a=field(a,leaf);
    b.create<StrictConnectOp>(loc,w.direction==Direction::In?v:a,w.direction==Direction::In?a:v);
  }
  b.create<ConnectOp>(loc,sim.getResult(old.at("ctrl_read_dispatch_master_ar")),field(ctrl,"ar"));
  std::string op="~"+inputName.str(),np="~"+wrapperName.str(),mp="|"+inputName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(mp)&&copied.count(ref.take_front(ref.find_first_of(".[")).str()))
        suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(wrapperName));return success();
}
