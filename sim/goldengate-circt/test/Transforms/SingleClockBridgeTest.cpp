// See LICENSE for license details.
#include "goldengate/SingleClockBridge.h"
#include "goldengate/RationalClockTokenGenerator.h"
#include "llvm/Support/FileSystem.h"
#include <fstream>
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <limits>
#include <map>
#include <random>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp circuit, llvm::StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned bad = 0, bool clockFirst = false,
                              ArrayRef<goldengate::RationalClockInfo> requested = {}) {
  unsigned lanes = requested.empty() ? 1 : requested.size();
  std::string tickPort = "in %ticks: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: vector<uint<1>, " + std::to_string(lanes) + ">>";
  std::string hostPorts = "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>";
  auto root = parseSourceString<ModuleOp>(
    "module { firrtl.circuit \"GGFAMEPipeWrapper\" { firrtl.module @GGFAMEPipeWrapper(" +
    (clockFirst ? tickPort + ", " + hostPorts : hostPorts + ", " + tickPort) +
    ", out %data: !firrtl.uint<8>) {} } }", &context);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto clk = b.getDictionaryAttr({
    b.getNamedAttr("name", b.getStringAttr("base")),
    b.getNamedAttr("multiplier", b.getI64IntegerAttr(bad == 2 ? 0 : 2)),
    b.getNamedAttr("divisor", b.getI64IntegerAttr(bad == 3 ? 0 : 2))});
  auto clocks = bad == 4 ? b.getArrayAttr({clk, clk}) : b.getArrayAttr({clk});
  SmallVector<Attribute> mfmrs{b.getI64IntegerAttr(bad == 7 ? 2 : 1)};
  if (!requested.empty()) {
    SmallVector<Attribute> values; mfmrs.clear();
    for (const auto &clock : requested) {
      values.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(clock.name)),
        b.getNamedAttr("multiplier", b.getI64IntegerAttr(clock.multiplier)),
        b.getNamedAttr("divisor", b.getI64IntegerAttr(clock.divisor))}));
      mfmrs.push_back(b.getI64IntegerAttr(clock.mfmr));
    }
    clocks = b.getArrayAttr(values);
  }
  auto key = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridges.ClockParameters")),
    b.getNamedAttr("clocks", clocks)});
  auto bridge = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("target", b.getStringAttr("~Original|Original>clockBridge")),
    b.getNamedAttr("widgetClass", b.getStringAttr("midas.widgets.ClockBridgeModule")),
    b.getNamedAttr("widgetConstructorKey", key),
    b.getNamedAttr("channelMapping", b.getDictionaryAttr({
      b.getNamedAttr("clocks", b.getStringAttr(bad == 5 ? "other" : "clockBridge_clocks"))}))});
  auto info = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::TargetClockChannel)),
    b.getNamedAttr("clockInfo", bad == 6 ? b.getArrayAttr({}) : clocks),
    b.getNamedAttr("perClockMFMR", b.getArrayAttr(mfmrs))});
  NamedAttrList channel;
  channel.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection));
  channel.set("globalName", b.getStringAttr("clockBridge_clocks"));
  channel.set("channelInfo", info);
  SmallVector<Attribute> sinks;
  for (unsigned i = 0; i < lanes; ++i)
    sinks.push_back(b.getStringAttr(bad == 8 ? "~GGFAMEPipeWrapper|GGFAMEPipeWrapper>ticks.valid" :
        "~GGFAMEPipeWrapper|GGFAMEPipeWrapper>ticks.bits[" + std::to_string(i) + "]"));
  channel.set("sinks", b.getArrayAttr(sinks));
  if (bad == 9) channel.set("sources", channel.get("sinks"));
  auto other = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DontTouch)),
    b.getNamedAttr("target", b.getStringAttr("~GGFAMEPipeWrapper|GGFAMEPipeWrapper>data"))});
  SmallVector<Attribute> annotations{channel.getDictionary(&context), other};
  if (bad != 1) annotations.push_back(bridge);
  if (bad == 10) annotations.push_back(bridge);
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (bad == 11) {
    auto inner = named(c, "GGFAMEPipeWrapper");
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto use = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Other"), inner.getConventionAttr(), ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(use.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), inner, "use");
  }
  if (bad == 12) {
    auto inner = named(c, "GGFAMEPipeWrapper");
    auto ports = inner.getPorts();
    ports[3].name = b.getStringAttr("clockBridge_mcr");
    inner.setPortNamesAttr(b.getArrayAttr(llvm::map_to_vector(ports,
        [](PortInfo p) -> Attribute { return p.name; })));
  }
  return root;
}

// Evaluate emitted FIRRTL, canonicalizing repeated accesses of bundle fields.
// State advances simultaneously; RegOp has no reset, RegResetOp has precedence.
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto i = v.getDefiningOp<SubindexOp>()) return key(i.getInput()) + "[" + std::to_string(i.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp();
    uint64_t n;
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) {
      unsigned w = bits.getHi() - bits.getLo() + 1;
      n = (eval(bits.getInput()) >> bits.getLo()) & (w == 64 ? ~uint64_t(0) : (uint64_t(1) << w) - 1);
    } else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto a : module.getOps<AssertOp>())
      require(!eval(a.getEnable()) || eval(a.getPredicate()), "register permission assertion fired");
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::addClockBridge(c, error)), error);
  require(succeeded(verify(*root)), "clock bridge IR verification failed");
  auto producer = named(c, "GGSingleClockBridge");
  Interpreter sim(producer);
  uint64_t host = 0, target = 0;
  // Shadows are deliberately unreset. Pick distinct arbitrary initial values
  // and prove host reset preserves them until an explicit latch transaction.
  uint64_t savedHost = 0x1122334455667788, savedTarget = 0xaabbccddeeff0099;
  for (auto r : producer.getOps<RegOp>())
    sim.state[r.getResult()] = r.getName() == "hCycle_mmreg" ? savedHost : savedTarget;
  std::mt19937_64 random(146);
  for (unsigned cycle = 0; cycle < 20000; ++cycle) {
    bool reset = cycle < 2 || cycle % 617 == 0;
    bool ready = cycle % 211 < 100 ? false : (random() & 1);
    if (cycle == 4000 || cycle == 19998) {
      host = target = cycle == 4000 ? 0xffffffff : std::numeric_limits<uint64_t>::max();
      for (auto r : producer.getOps<RegResetOp>()) sim.state[r.getResult()] = host;
      ready = true; reset = false;
    }
    sim.memo.clear();
    sim.memo[sim.key(sim.arg(0))] = 0;
    sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".ready"] = ready;
    auto mcrKey = sim.key(sim.arg(5));
    sim.memo[mcrKey + ".wstrb"] = random() & 15;
    bool valid[2]; uint32_t data[2];
    for (unsigned group = 0; group < 2; ++group) {
      valid[group] = random() & 1; data[group] = random();
      // Exercise reset plus latch, carry/overflow snapshots, and repeated writes.
      if ((cycle > 1 && cycle % 617 == 0) || cycle == 4000 || cycle >= 19998)
        valid[group] = true, data[group] = 1;
      if (cycle < 2) valid[group] = false; // Initial reset must not clear shadows.
      if (cycle == 2) valid[group] = false, data[group] = 1;
      if (cycle == 3) valid[group] = true, data[group] = 2; // Only bit zero latches.
      if (cycle == 4 || cycle == 5) valid[group] = true, data[group] = 1;
    }
    for (unsigned i = 0; i < 6; ++i) {
      auto read = mcrKey + ".read[" + std::to_string(i) + "]";
      auto write = mcrKey + ".write[" + std::to_string(i) + "]";
      bool latch = i % 3 == 2;
      sim.memo[read + ".ready"] = latch ? false : (random() & 1);
      sim.memo[write + ".valid"] = latch && valid[i / 3];
      sim.memo[write + ".bits"] = latch ? data[i / 3] : uint32_t(random());
      require(sim.eval(sim.drivers.at(read + ".valid")) == 1 &&
              sim.eval(sim.drivers.at(write + ".ready")) == 1, "MCR register handshake stalled");
      auto saved = i < 3 ? savedHost : savedTarget;
      uint32_t expected = latch ? 0 : uint32_t(saved >> (i % 3 * 32));
      require(sim.eval(sim.drivers.at(read + ".bits")) == expected,
              "MCR read changed without latch or split snapshot mismatch");
    }
    require(sim.eval(sim.arg(3)) == host, "host-cycle count mismatch");
    require(sim.eval(sim.arg(4)) == target, "accepted target-clock count mismatch");
    require(sim.eval(sim.drivers.at(sim.key(sim.arg(2)) + ".valid")) == 1 &&
            sim.eval(sim.drivers.at(sim.key(sim.arg(2)) + ".bits[0]")) == 1,
            "1:1 token changed during reset or backpressure");
    sim.edge();
    if (valid[0] && (data[0] & 1)) savedHost = host;
    if (valid[1] && (data[1] & 1)) savedTarget = target;
    host = reset ? 0 : host + 1; target = reset ? 0 : target + ready;
  }
  // Check each illegal access predicate and reset suppression without relying
  // on a textual count of assertions. Writes to RO / reads from WO must fail.
  unsigned assertions = 0;
  for (auto a : producer.getOps<AssertOp>()) {
    auto predicate = a.getPredicate().getDefiningOp<NotPrimOp>();
    require(bool(predicate), "missing permission predicate");
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = 0;
    sim.memo[sim.key(predicate.getInput())] = 1;
    require(sim.eval(a.getEnable()) == 1 && sim.eval(a.getPredicate()) == 0,
            "illegal access not rejected");
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = 1;
    require(sim.eval(a.getEnable()) == 0, "permission assertion enabled during reset");
    ++assertions;
  }
  require(assertions == 6, "missing MCR register permission checks");
}
void mapping(MLIRContext &context, bool clockFirst) {
  auto root = fixture(context, 0, clockFirst); auto c = *root->getOps<CircuitOp>().begin();
  auto original = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto info = cast<DictionaryAttr>(original[0]).get("channelInfo");
  auto ctor = cast<DictionaryAttr>(original[2]).get("widgetConstructorKey");
  std::string error;
  require(succeeded(goldengate::addClockBridge(c, error)), error);
  require(succeeded(verify(*root)), "mapped clock bridge invalid IR");
  auto outer = named(c, "GGClockBridgeWrapper");
  require(c.getName() == outer.getName() && outer.getPorts().size() == 4, "clock input not removed from new top");
  require(outer.getPortName(3) == "clockBridge_mcr" && outer.getPortDirection(3) == Direction::Out,
          "decoded MCR boundary missing");
  auto inner = named(c, "GGFAMEPipeWrapper"), producer = named(c, "GGSingleClockBridge");
  require(inner.getPorts().size() == 4 && producer->getAttr("goldengate.bridgeConstructor") == ctor,
          "inner interface or constructor changed");
  InstanceOp sim, bridge;
  for (auto i : outer.getOps<InstanceOp>()) (i.getModuleName() == inner.getName() ? sim : bridge) = i;
  require(sim && bridge, "wrapper has no simulator or producer instance");
  unsigned hostConnections = 0, tokenConnections = 0, mcrConnections = 0;
  for (auto connect : outer.getOps<StrictConnectOp>()) {
    auto port = cast<OpResult>(connect.getDest()).getResultNumber();
    require(connect.getDest().getDefiningOp() == bridge && port < 2, "wrong host connection");
    require(connect.getSrc() == outer.getBodyBlock()->getArgument(port), "host input indexing changed");
    ++hostConnections;
  }
  for (auto connect : outer.getOps<ConnectOp>()) {
    tokenConnections += connect.getDest() == sim.getResult(clockFirst ? 0 : 2) && connect.getSrc() == bridge.getResult(2);
    mcrConnections += connect.getDest() == outer.getBodyBlock()->getArgument(3) && connect.getSrc() == bridge.getResult(5);
  }
  require(hostConnections == 2 && tokenConnections == 1 && mcrConnections == 1, "clock producer is disconnected");
  auto regs = producer->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 6, "MCR map missing");
  for (auto [i, entry] : llvm::enumerate(regs)) {
    auto d = cast<DictionaryAttr>(entry);
    require(d.getAs<IntegerAttr>("offset").getInt() == i * 4 &&
            d.getAs<BoolAttr>("readable").getValue() == (i % 3 != 2) &&
            d.getAs<BoolAttr>("writeable").getValue() == (i % 3 == 2), "MCR offsets/permissions mismatch");
  }
  auto after = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto channel = cast<DictionaryAttr>(after[0]);
  require(channel.get("channelInfo") == info, "clock metadata changed");
  for (auto member : {"sinks", "sources"}) {
    auto spelling = cast<StringAttr>(channel.getAs<ArrayAttr>(member)[0]);
    auto resolved = goldengate::resolveAnnotationTarget(c, spelling, error);
    require(resolved && resolved->fieldID && resolved->module == (std::string(member) == "sinks" ? inner : producer),
            "clock endpoint identity does not resolve");
  }
  auto io = cast<DictionaryAttr>(after[2]);
  require(io.get("widgetConstructorKey") == ctor, "bridge constructor was lost");
  auto resolved = goldengate::resolveAnnotationTarget(c, io.getAs<StringAttr>("target"), error);
  require(resolved && resolved->module == producer, "bridge IO was not transferred");
  auto data = cast<DictionaryAttr>(after[1]).getAs<StringAttr>("target");
  require(data == "~GGClockBridgeWrapper|GGClockBridgeWrapper>data", "copied boundary identity was not transferred");
}
// Execute the actual producer operations, including rational countdowns,
// fastest-clock count and pre-edge decoded snapshots. Compare against both an
// independent event-time model here and full Scala ClockBridge observations.
void rationalBehavior(MLIRContext &context, StringRef outputDirectory) {
  using Ratios = std::vector<std::pair<unsigned,unsigned>>;
  const std::pair<const char *,Ratios> cases[]{
    {"single",{{2,2}}}, {"base-half",{{1,1},{1,2}}},
    {"two-three",{{1,2},{1,3}}}, {"three-two",{{1,3},{1,2}}},
    {"two-three-four",{{1,2},{1,3},{1,4}}},
    {"unreduced",{{2,4},{3,9},{4,16}}},
    {"equal",{{2,2},{3,3},{2147483647,2147483647}}},
    {"cross-product",{{2147483647,2147483646},{2147483647,1073741823}}},
    {"limit",{{1,1},{1,65535}}}};
  for (auto &[name,ratios] : cases) {
    SmallVector<goldengate::RationalClockInfo> clocks;
    for (auto [i,r] : llvm::enumerate(ratios))
      clocks.push_back({"clock"+std::to_string(i),r.first,r.second,1});
    std::string error;
    auto schedule = goldengate::analyzeRationalClockSchedule(clocks,error);
    require(bool(schedule),error);
    unsigned minimum = *llvm::min_element(schedule->periods);
    for (unsigned i=0;i<clocks.size();++i)
      clocks[i].mfmr=(schedule->periods[i]+minimum-1)/minimum;
    auto root=fixture(context,0,ratios.size()%2,clocks);
    auto c=*root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addClockBridge(c,error)),error);
    require(succeeded(verify(*root)),"rational bridge IR invalid");
    auto producer=named(c,"GGSingleClockBridge");
    auto info=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
    auto token=cast<BundleType>(producer.getPortType(2));
    auto bits=*token.getElementIndex("bits");
    auto vector=cast<FVectorType>(token.getElements()[bits].type);
    for (auto member : {"sources","sinks"}) {
      auto endpoints=info.getAs<ArrayAttr>(member);
      require(endpoints.size()==clocks.size(),"lost clock endpoints");
      for (auto [i,attr] : llvm::enumerate(endpoints)) {
        auto resolved=goldengate::resolveAnnotationTarget(c,cast<StringAttr>(attr),error);
        require(resolved && resolved->module==named(c,StringRef(member)=="sources" ?
            "GGSingleClockBridge":"GGFAMEPipeWrapper") &&
            resolved->fieldID==token.getFieldID(bits)+vector.getFieldID(i),
            "ordered rational endpoint identity mismatch");
      }
    }
    if (!outputDirectory.empty()) {
      std::error_code ec;
      llvm::raw_fd_ostream out(outputDirectory.str()+"/"+name+".bridge.mlir",ec);
      require(!ec,"cannot dump bridge IR"); root->print(out); out<<'\n';
      llvm::raw_fd_ostream rtl(outputDirectory.str()+"/"+name+".producer.mlir",ec);
      require(!ec,"cannot dump producer IR");
      rtl<<"module { firrtl.circuit \"GGSingleClockBridge\" {\n";
      producer->print(rtl); rtl<<"\n}}\n";
    }
    Interpreter sim(producer);
    unsigned fastest=0;
    for (unsigned i=1;i<ratios.size();++i)
      if (double(ratios[i].first)/ratios[i].second >= double(ratios[fastest].first)/ratios[fastest].second) fastest=i;
    // Structural check distinguishes last-lane ties even when tokens coincide.
    for (auto r:producer.getOps<RegResetOp>()) if (r.getName()=="tCycleFastest") {
      auto mux=sim.drivers.at(sim.key(r.getResult())).getDefiningOp<MuxPrimOp>();
      auto fire=mux.getSel().getDefiningOp<AndPrimOp>();
      if (ratios.size()>1) {
        auto eq=fire.getRhs().getDefiningOp<EQPrimOp>();
        require(eq && eq.getLhs().getDefiningOp<RegResetOp>().getName()==
            "timeToNextEdge_"+std::to_string(fastest),"fastest lane selection mismatch");
      }
    }
    if (!outputDirectory.empty()) {
      llvm::outs()<<"FASTEST "<<name<<' '<<fastest<<'\n';
      llvm::outs()<<"REGISTERS "<<name;
      for (auto attr : producer->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters")) {
        auto r=cast<DictionaryAttr>(attr);
        llvm::outs()<<" ("<<r.getAs<StringAttr>("name").getValue()<<','<<r.getAs<IntegerAttr>("offset").getInt()<<')';
      }
      llvm::outs()<<'\n';
    }
    uint64_t host=0,target=0,savedHost=0,savedTarget=0;
    std::vector<uint64_t> nextEdges(clocks.size(),0);
    unsigned cycles=StringRef(name)=="limit"?200000:4096;
    for (unsigned cycle=0;cycle<cycles;++cycle) {
      bool reset=cycle<2 || cycle==cycles/2;
      bool ready=cycle%97>=13 && cycle%7!=0;
      bool latchHost=cycle%19==0, latchTarget=cycle%23==0;
      unsigned dataHost=cycle%3==0?2:1, dataTarget=cycle%5==0?2:1;
      sim.memo.clear();sim.memo[sim.key(sim.arg(0))]=0;sim.memo[sim.key(sim.arg(1))]=reset;
      auto port=sim.key(sim.arg(2)),mcr=sim.key(sim.arg(5));sim.memo[port+".ready"]=ready;
      for (unsigned i=0;i<6;++i) {
        sim.memo[mcr+".read["+std::to_string(i)+"].ready"]=0;
        sim.memo[mcr+".write["+std::to_string(i)+"].valid"]=(i==2&&latchHost)||(i==5&&latchTarget);
        sim.memo[mcr+".write["+std::to_string(i)+"].bits"]=i==2?dataHost:dataTarget;
      }
      uint64_t mask=0,expected=0;
      auto edge=*std::min_element(nextEdges.begin(),nextEdges.end());
      for (unsigned i=0;i<clocks.size();++i) {
        mask|=sim.eval(sim.drivers.at(port+".bits["+std::to_string(i)+"]"))<<i;
        if (nextEdges[i]==edge) expected|=uint64_t(1)<<i;
      }
      require(mask==expected && mask && sim.eval(sim.drivers.at(port+".valid"))==1,"rational token mismatch");
      require(sim.eval(sim.arg(3))==host && sim.eval(sim.arg(4))==target,"fastest/host counter mismatch");
      auto read=[&](unsigned i){return sim.eval(sim.drivers.at(mcr+".read["+std::to_string(i)+"].bits"));};
      require((read(0)|(read(1)<<32))==savedHost && (read(3)|(read(4)<<32))==savedTarget,
              "rational snapshot mismatch");
      if (!outputDirectory.empty()) llvm::outs()<<"BRIDGE "<<name<<' '<<cycle<<' '<<mask<<' '
          <<host<<' '<<target<<' '<<savedHost<<' '<<savedTarget<<'\n';
      sim.edge();
      if (latchHost&&(dataHost&1)) savedHost=host;
      if (latchTarget&&(dataTarget&1)) savedTarget=target;
      host=reset?0:host+1;
      target=reset?0:target+unsigned(ready&&bool(mask&(uint64_t(1)<<fastest)));
      if (reset) std::fill(nextEdges.begin(),nextEdges.end(),0);
      else if (ready) for (unsigned i=0;i<clocks.size();++i)
        if (mask&(uint64_t(1)<<i)) nextEdges[i]+=schedule->periods[i];
    }
    // Wrong MFMR, duplicate/reordered lanes, and invalid schedules must fail
    // without leaving partly constructed modules or changing annotations.
    if (clocks.size()>1) for (unsigned bad=0;bad<4;++bad) {
      auto malformed=fixture(context,0,false,clocks);auto cc=*malformed->getOps<CircuitOp>().begin();
      OpBuilder b(&context);
      auto raw=cc->getAttrOfType<ArrayAttr>("rawAnnotations");
      SmallVector<Attribute> annos(raw.begin(),raw.end());
      NamedAttrList channel(cast<DictionaryAttr>(annos[0]));
      if (bad<3) {
        SmallVector<Attribute> endpoints(cast<ArrayAttr>(channel.get("sinks")).begin(),cast<ArrayAttr>(channel.get("sinks")).end());
        if (bad==0) endpoints[1]=endpoints[0];
        if (bad==1) std::swap(endpoints[0],endpoints[1]);
        if (bad==2) endpoints.pop_back();
        channel.set("sinks",b.getArrayAttr(endpoints));
      } else {
        NamedAttrList ci(cast<DictionaryAttr>(channel.get("channelInfo")));
        SmallVector<Attribute> mfmr(clocks.size(),b.getI64IntegerAttr(0));
        ci.set("perClockMFMR",b.getArrayAttr(mfmr));channel.set("channelInfo",ci.getDictionary(&context));
      }
      annos[0]=channel.getDictionary(&context);cc->setAttr("rawAnnotations",b.getArrayAttr(annos));
      std::string before,after;llvm::raw_string_ostream a(before);malformed->print(a);
      require(failed(goldengate::addClockBridge(cc,error)),"malformed rational endpoints accepted");
      llvm::raw_string_ostream z(after);malformed->print(z);require(before==after,"rational rejection mutated IR");
    }
  }
}
} // namespace
int main(int argc, char **argv) {
  MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    if (argc==4 && StringRef(argv[1])=="--map-handoff") {
      auto root=parseSourceFile<ModuleOp>(argv[2],&context);
      require(bool(root),"active handoff parse failed");
      auto c=*root->getOps<CircuitOp>().begin(); std::string error;
      require(succeeded(goldengate::addClockBridge(c,error)),error);
      require(succeeded(verify(*root)),"active handoff mapping invalid IR");
      auto producer=named(c,"GGSingleClockBridge");
      std::error_code ec;
      llvm::raw_fd_ostream ir(std::string(argv[3])+"/rocket.producer.mlir",ec);
      require(!ec,"cannot dump active producer");
      ir<<"module { firrtl.circuit \"GGSingleClockBridge\" {\n";
      producer->print(ir);ir<<"\n}}\n";
      llvm::raw_fd_ostream wrapper(std::string(argv[3])+"/rocket.wrapper.mlir",ec);
      require(!ec,"cannot dump active wrapper");root->print(wrapper);wrapper<<'\n';
      for (auto attr:c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
        auto d=cast<DictionaryAttr>(attr);
        auto info=d.getAs<DictionaryAttr>("channelInfo");
        if (!info || info.getAs<StringAttr>("class")!=goldengate::AnnotationClasses::TargetClockChannel) continue;
        auto clocks=info.getAs<ArrayAttr>("clockInfo"),mfmrs=info.getAs<ArrayAttr>("perClockMFMR");
        auto sources=d.getAs<ArrayAttr>("sources"),sinks=d.getAs<ArrayAttr>("sinks");
        for (auto [i,clock]:llvm::enumerate(clocks)) {
          auto cc=cast<DictionaryAttr>(clock);
          llvm::outs()<<"HANDOFF_CLOCK\t"<<i<<'\t'<<cc.getAs<StringAttr>("name").getValue()<<'\t'
              <<cc.getAs<IntegerAttr>("multiplier").getInt()<<'\t'<<cc.getAs<IntegerAttr>("divisor").getInt()<<'\t'
              <<cast<IntegerAttr>(mfmrs[i]).getInt()<<'\t'<<cast<StringAttr>(sources[i]).getValue()<<'\t'
              <<cast<StringAttr>(sinks[i]).getValue()<<'\n';
        }
      }
      for (auto attr:producer->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters")) {
        auto r=cast<DictionaryAttr>(attr);
        llvm::outs()<<"MCR\t"<<r.getAs<StringAttr>("name").getValue()<<'\t'
            <<r.getAs<IntegerAttr>("offset").getInt()<<'\t'
            <<r.getAs<BoolAttr>("readable").getValue()<<'\t'<<r.getAs<BoolAttr>("writeable").getValue()<<'\n';
      }
      return 0;
    }
    behavior(context); mapping(context, false); mapping(context, true);
    rationalBehavior(context,argc>1?argv[1]:"");
    for (unsigned bad = 1; bad <= 12; ++bad) {
      auto root = fixture(context, bad); auto c = *root->getOps<CircuitOp>().begin();
      std::string before, after, error;
      llvm::raw_string_ostream a(before); root->print(a);
      require(failed(goldengate::addClockBridge(c, error)) && !error.empty(), "malformed bridge accepted");
      llvm::raw_string_ostream b(after); root->print(b);
      require(before == after, "failed bridge mapping mutated IR");
    }
  } catch (const std::exception &e) { llvm::errs() << "SingleClockBridge: " << e.what() << '\n'; return 1; }
  llvm::outs() << "SingleClockBridge: 20000 stalled/reset/latch/read cycles, split snapshots, 32/64-bit overflow, permissions, connected MCR wrapper, 232768 rational bridge cycles and 44 atomic rejections passed\n";
}
