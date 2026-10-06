// See LICENSE for license details.
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/SimulationMasterControl.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef s) { if (!ok) throw std::runtime_error(s.str()); }
std::string dump(ModuleOp m) { std::string s; llvm::raw_string_ostream out(s); m.print(out); return s; }
FModuleOp named(CircuitOp c, llvm::StringRef n) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == n) return m;
  throw std::runtime_error("module missing");
}
struct BindingSpec {
  std::string stem, widget, input, catalogName, slaveAttr;
  unsigned slave, words;
  LogicalResult (*mapControl)(CircuitOp, unsigned, unsigned, std::string &);
  LogicalResult (*bindControl)(CircuitOp, std::string &);
};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, const BindingSpec &spec,
                            unsigned bad = 0, unsigned count = 11) {
  const auto &stem = spec.stem;
  const auto &widget = spec.widget;
  const std::string slot = std::to_string(spec.slave);
  const unsigned words = spec.words;
  const auto &input = spec.input;
  std::string token = "bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>";
  std::string text = "module { firrtl.circuit \""+input+"\" { firrtl.module @"+input+"("
    "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, in %other: !firrtl.uint<8>, "
    "out %"+widget+"_mcr: !firrtl.bundle<read: vector<"+token+", "+std::to_string(words)+">, write flip: vector<"+token+", "+std::to_string(words)+">, wstrb flip: uint<4>>";
  auto scalar = [&](std::string n, unsigned w, bool input) {
    if (bad == 1 && n == "ctrl_write_dispatch_slave_"+slot+"_w_ready") return;
    if (bad == 2 && n == "ctrl_read_arb_in_"+slot+"_bits_id") w = 11;
    if (bad == 3 && n == "ctrl_write_arb_in_"+slot+"_ready") input = true;
    if (bad == 8 && n == "ctrl_write_dispatch_master_w_bits_strb") return;
    if (bad == 22 && n == "ctrl_write_dispatch_master_w_bits_user") w = 2;
    if (bad == 23 && n == "ctrl_write_dispatch_master_aw_bits_prot") input = false;
    text += ", "+std::string(input ? "in" : "out")+" %"+n+": !firrtl.uint<"+std::to_string(w)+">";
  };
  const std::pair<const char*,unsigned> address[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
    {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
  const std::pair<const char*,unsigned> data[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
  for (auto ch : {"aw","w"}) {
    scalar("ctrl_write_dispatch_slave_"+slot+"_"+std::string(ch)+"_ready",1,true);
    scalar("ctrl_write_dispatch_slave_"+slot+"_"+std::string(ch)+"_valid",1,false);
    for (auto [n,w] : ch == StringRef("aw") ? ArrayRef(address) : ArrayRef(data)) {
      bool local = ch == StringRef("aw") ? StringRef(n)=="addr"||StringRef(n)=="len"||StringRef(n)=="id" : StringRef(n)=="data"||StringRef(n)=="last";
      scalar(std::string(local ? "ctrl_write_dispatch_slave_"+slot+"_" : "ctrl_write_dispatch_master_")+ch+"_bits_"+n,w,!local);
    }
  }
  std::string ar = "!firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<";
  for (auto [n,w] : address) ar += std::string(n)+": uint<"+std::to_string(w)+">, ";
  ar.resize(ar.size()-2); ar += ">>";
  text += ", out %ctrl_read_dispatch_slave_"+slot+"_ar: "+(bad == 9 ? "!firrtl.uint<1>" : ar);
  for (auto ch : {"r","b"}) {
    std::string prefix = ch == StringRef("r") ? "ctrl_read_arb_in_"+slot : "ctrl_write_arb_in_"+slot;
    scalar(prefix+"_ready",1,false); scalar(prefix+"_valid",1,true);
    for (auto [n,w] : ArrayRef<std::pair<const char*,unsigned>>{{"resp",2},{"id",12},{"user",1}})
      scalar(prefix+"_bits_"+n,w,true);
    if (ch == StringRef("r")) { scalar(prefix+"_bits_data",32,true); scalar(prefix+"_bits_last",1,true); }
  }
  // Retirement diagnostics remain visible; arbiters drive their tracker inputs.
  for (auto ch : {"read","write"}) for (auto n : {"data","matches"})
    scalar("ctrl_"+std::string(ch)+"_tracker_deq_"+slot+"_"+n,StringRef(n)=="data" ? 4 : 1,false);
  auto root = parseSourceString<ModuleOp>(text+") {} firrtl.module @GGControlAddressDecode() {} } }",&ctx);
  require(bool(root),"fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  c->setAttr("rawAnnotations",b.getArrayAttr({})); std::string error;
  if (spec.mapControl == goldengate::mapFASEDBridgeControl ||
      spec.mapControl == goldengate::mapBlockDevBridgeControl) {
    SmallVector<Attribute> registers;
    for (unsigned i = 0; i < words; ++i) registers.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr("register_"+std::to_string(i))),
      b.getNamedAttr("offset",b.getI64IntegerAttr(4*i)),
      b.getNamedAttr("readable",b.getBoolAttr(true)),
      b.getNamedAttr("writeable",b.getBoolAttr(true))}));
    auto owner = named(c,input);
    if (spec.mapControl == goldengate::mapBlockDevBridgeControl) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      const PortInfo ports[]{{b.getStringAttr("mcr"),owner.getPortType(3),Direction::Out}};
      owner = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGBlockDevMMIOBank"),
          owner.getConventionAttr(),ports);
    }
    owner->setAttr("goldengate.mmioRegisters",b.getArrayAttr(registers));
  }
  require(succeeded(spec.mapControl(c,25,12,error)),error);
  SmallVector<Attribute> rows;
  // Moving a widget and changing its base/region size must still select the
  // matching decoder slot. Distinct rows also exercise whole-catalog checks.
  // Large catalogs end exactly at 2^25, a legal half-open decoder endpoint.
  const uint64_t base = count >= 31 ? (uint64_t(1)<<25)-count*0x80 : 0x1000;
  for (unsigned i = 0; i < (bad == 32 ? 64 : count); ++i) rows.push_back(b.getDictionaryAttr({
    b.getNamedAttr("name",b.getStringAttr(i == spec.slave ? spec.catalogName : "Other_"+std::to_string(i))),
    b.getNamedAttr("slave",b.getI32IntegerAttr(i)),
    b.getNamedAttr("start",b.getI64IntegerAttr(base+i*0x80)),
    b.getNamedAttr("size",b.getI64IntegerAttr(0x80))}));
  auto decoder = named(c,"GGControlAddressDecode");
  if (bad == 12) rows.clear();
  if (bad == 14) rows[spec.slave] = b.getStringAttr("malformed row");
  if (bad == 4 || (bad >= 24 && bad <= 36 && bad != 32)) {
    unsigned other = (spec.slave+1)%count;
    unsigned selected = bad == 24 || bad == 25 || bad == 26 || bad == 28 || bad == 31 ? other : spec.slave;
    NamedAttrList row(cast<DictionaryAttr>(rows[selected]));
    if (bad == 4) row.set("start",b.getI64IntegerAttr((1<<25)-0x40));
    if (bad == 24) row.set("name",b.getStringAttr(spec.catalogName));
    if (bad == 25) row.set("slave",b.getI32IntegerAttr(spec.slave));
    if (bad == 26) row.set("start",b.getI64IntegerAttr(0x1000+spec.slave*0x80));
    if (bad == 27) row.set("name",b.getStringAttr("MissingWidget"));
    if (bad == 28) row.set("name",b.getStringAttr(""));
    if (bad == 29) row.set("start",b.getI64IntegerAttr(-1));
    if (bad == 30) row.set("size",b.getI64IntegerAttr(0));
    if (bad == 31) row.erase("size");
    if (bad == 33) row.set("start",b.getIntegerAttr(b.getIntegerType(128),APInt(128,1).shl(100)));
    if (bad == 34) row.set("start",b.getI64IntegerAttr(1<<25));
    if (bad == 35) row.set("slave",b.getI64IntegerAttr(-1));
    if (bad == 36) row.set("size",b.getI64IntegerAttr((1<<25)+1));
    rows[selected] = row.getDictionary(&ctx);
  }
  if (bad >= 15 && bad <= 21) {
    NamedAttrList row(cast<DictionaryAttr>(rows[spec.slave]));
    if (bad == 15) row.erase("name");
    if (bad == 16) row.set("name",b.getI32IntegerAttr(0));
    // Slave zero is valid for BlockDev; every fixture must use a different slot.
    if (bad == 17) row.set("slave",b.getI32IntegerAttr((spec.slave+1)%11));
    if (bad == 18) row.erase("slave");
    if (bad == 19) row.erase("start");
    if (bad == 20) row.erase("size");
    if (bad == 21) row.set("size",b.getI64IntegerAttr(-1));
    rows[spec.slave] = row.getDictionary(&ctx);
  }
  decoder->setAttr("goldengate.controlRegions",bad == 13 ? Attribute(b.getStringAttr("malformed catalog")) : Attribute(b.getArrayAttr(rows)));
  if (bad == 11) decoder->removeAttr("goldengate.controlRegions");
  SmallVector<Attribute> annos;
  for (const auto &n : {std::string("other"),widget+"_ctrl.ar.bits.id","ctrl_read_arb_in_"+slot+"_valid",std::string("ctrl_write_dispatch_master_w_bits_strb")})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("target",b.getStringAttr("~"+stem+"ControlWrapper|"+stem+"ControlWrapper>"+n))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if (bad == 5) c->removeAttr("rawAnnotations");
  auto top = named(c,stem+"ControlWrapper");
  if (bad == 6) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),top,"used"); }
  if (bad == 7) c.setName("WrongTop");
  if (bad == 10) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),b.getStringAttr(stem+"BoundWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{}); }
  return root;
}
void test(MLIRContext &ctx, const BindingSpec &spec, unsigned count = 11,
          bool rejections = true) {
  const auto &stem = spec.stem;
  const std::string slot = std::to_string(spec.slave);
  const auto &widget = spec.widget;
  auto bind = spec.bindControl;
  auto root = fixture(ctx,spec,0,count); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto catalog = named(c,"GGControlAddressDecode")->getAttr("goldengate.controlRegions");
  auto before = named(c,stem+"ControlWrapper"); auto portCount = before.getNumPorts();
  require(succeeded(bind(c,error)),error);
  require(succeeded(verify(*root)),"bound IR invalid");
  auto top = named(c,stem+"BoundWrapper");
  require(top.getNumPorts()+23 == portCount,"not all slave boundaries consumed");
  require(top->getAttrOfType<IntegerAttr>(spec.slaveAttr).getInt()==spec.slave,"wrong slave identity");
  require(c.getName()==stem+"BoundWrapper","top identity not updated");
  require(named(c,"GGControlAddressDecode")->getAttr("goldengate.controlRegions")==catalog,"decoder catalog mutated");
  InstanceOp sim = *top.getOps<InstanceOp>().begin();
  require(sim.getModuleName()==before.getName() && sim.getName()=="sim","inner instance identity lost");
  std::function<std::string(Value)> key = [&](Value v) -> std::string {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput())+"."+f.getFieldName().str();
    if (auto a = dyn_cast<BlockArgument>(v)) return top.getPortName(a.getArgNumber()).str();
    for (unsigned i = 0; i < sim.getNumResults(); ++i) if (v == sim.getResult(i)) return "sim_"+before.getPortName(i).str();
    throw std::runtime_error("unknown wire value");
  };
  std::map<std::string,std::string> wires, expected;
  for (auto x : top.getOps<StrictConnectOp>()) require(wires.emplace(key(x.getDest()),key(x.getSrc())).second,"duplicate scalar driver");
  const std::string ctrl = "sim_"+widget+"_ctrl";
  const std::pair<const char*,unsigned> address[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
    {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
  const std::pair<const char*,unsigned> data[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
  for (auto ch : {"aw","w"}) {
    std::string p = "sim_ctrl_write_dispatch_slave_"+slot+"_"+ch;
    expected[p+"_ready"]=ctrl+"."+ch+".ready";
    expected[ctrl+"."+ch+".valid"]=p+"_valid";
    for (auto [n,w] : ch == StringRef("aw") ? ArrayRef(address) : ArrayRef(data)) {
      bool local = ch == StringRef("aw") ? StringRef(n)=="addr"||StringRef(n)=="len"||StringRef(n)=="id" : StringRef(n)=="data"||StringRef(n)=="last";
      expected[ctrl+"."+ch+".bits."+n]=(local ? p : "sim_ctrl_write_dispatch_master_"+std::string(ch))+"_bits_"+n;
    }
  }
  for (auto ch : {"r","b"}) {
    std::string p = "sim_ctrl_"+std::string(ch == StringRef("r") ? "read" : "write")+"_arb_in_"+slot;
    expected[ctrl+"."+ch+".ready"]=p+"_ready";
    expected[p+"_valid"]=ctrl+"."+ch+".valid";
    for (auto n : {"resp","id","user"}) expected[p+"_bits_"+n]=ctrl+"."+ch+".bits."+n;
    if (ch == StringRef("r")) for (auto n : {"data","last"}) expected[p+"_bits_"+n]=ctrl+"."+ch+".bits."+n;
  }
  require(expected.size()==32 && wires==expected,"request/response scalar binding differs");
  bool ar = false; unsigned copied = 0;
  for (auto x : top.getOps<ConnectOp>()) {
    if (key(x.getDest())==ctrl+".ar") {
      require(!ar && key(x.getSrc())=="sim_ctrl_read_dispatch_slave_"+slot+"_ar","aggregate AR/ready binding reversed"); ar=true;
      continue;
    }
    ++copied;
    auto a = dyn_cast<BlockArgument>(x.getDest());
    if (!a) a = dyn_cast<BlockArgument>(x.getSrc());
    require(bool(a),"copied port missing boundary argument");
    auto p = top.getPorts()[a.getArgNumber()];
    unsigned i = 0;
    while (i < before.getNumPorts() && before.getPortName(i) != p.name.getValue()) ++i;
    require(i < before.getNumPorts(),"copied port renamed"); auto old = before.getPorts()[i];
    require(p.type==old.type && p.direction==old.direction,"copied port contract changed");
    require(key(p.direction==Direction::In ? x.getDest() : x.getSrc())=="sim_"+p.name.getValue().str(),"copied port connection reversed");
  }
  require(ar && copied==top.getNumPorts(),"copied ports or aggregate AR missing");
  for (auto p : top.getPorts()) {
    auto n=p.name.getValue();
    require(n!=widget+"_ctrl" && !n.starts_with("ctrl_write_dispatch_slave_"+slot+"_") &&
      n!="ctrl_read_dispatch_slave_"+slot+"_ar" && !n.starts_with("ctrl_read_arb_in_"+slot+"_") &&
      !n.starts_with("ctrl_write_arb_in_"+slot+"_"),"consumed boundary remains visible");
  }
  auto annotations = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(annotations.size()==4,"annotation count changed");
  for (auto [i,a] : llvm::enumerate(annotations)) {
    auto t = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();
    require(t.starts_with("~"+stem+"BoundWrapper|"),"circuit target lost");
    require(t.contains(i == 0 || i == 3 ? "|"+stem+"BoundWrapper>" : "|"+stem+"ControlWrapper>"),"consumed target transferred incorrectly");
  }
  auto good = dump(*root);
  require(failed(bind(c,error)) && good == dump(*root),"repeat mutated IR");
  for (unsigned bad = 1; rejections && bad <= 36; ++bad) {
    auto root = fixture(ctx,spec,bad); auto c = *root->getOps<CircuitOp>().begin(); auto s = dump(*root);
    require(failed(bind(c,error)),"invalid contract accepted: "+std::to_string(bad));
    require(s == dump(*root),"rejection mutated IR");
  }
  llvm::outs()<<widget<<" control: "<<count<<" regions, slave "<<slot
    <<", all 32 scalar and aggregate AR bindings, copied metadata, target transfer"
    <<(rejections ? ", 37 atomic rejections" : "")<<" passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  const BindingSpec specs[]{
    {"GGSimulationMaster", "simulationMaster", "GGSimulationMasterWrapper",
     "SimulationMaster_0", "goldengate.simulationMasterSlave", 8, 3,
     goldengate::mapSimulationMasterControl, goldengate::bindSimulationMasterControl},
    {"GGTSIBridge", "tsiBridge", "GGTSIMMIOWrapper",
     "TSIBridgeModule_0", "goldengate.tsiSlave", 3, 9,
     goldengate::mapTSIBridgeControl, goldengate::bindTSIBridgeControl},
    {"GGBlockDevBridge", "blockdevBridge", "GGBlockDevMMIOWrapper",
     "BlockDevBridgeModule_0", "goldengate.blockdevSlave", 0, 26,
     goldengate::mapBlockDevBridgeControl, goldengate::bindBlockDevBridgeControl},
    {"GGFASEDBridge", "fasedBridge", "GGFASEDMMIOWrapper",
     "FASEDMemoryTimingModel_0", "goldengate.fasedSlave", 1, 21,
     goldengate::mapFASEDBridgeControl, goldengate::bindFASEDBridgeControl}
  };
  try {
    unsigned cases = 0;
    for (const auto &spec : specs) {
      test(ctx,spec); ++cases;
      for (unsigned count : {1u,2u,3u,11u,13u,31u,63u})
        for (unsigned slot : std::set<unsigned>{0,count/2,count-1}) {
          auto shifted = spec; shifted.slave = slot;
          test(ctx,shifted,count,false); ++cases;
        }
    }
    llvm::outs()<<cases<<" widget catalog bindings and 148 atomic rejections passed\n";
    return 0;
  }
  catch (const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; }
}
