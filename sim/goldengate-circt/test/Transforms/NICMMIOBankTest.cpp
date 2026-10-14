// See LICENSE for license details.
#include "goldengate/NICMMIOBank.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef msg) { if (!ok) throw std::runtime_error(msg.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGNICCPUStreamResponseBufferWrapper" {
      firrtl.module @GGNICCPUStreamResponseBufferWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %nic_macAddr: !firrtl.uint<48>, in %nic_rlimit_inc: !firrtl.uint<8>,
        in %nic_rlimit_period: !firrtl.uint<8>, in %nic_rlimit_size: !firrtl.uint<8>,
        in %nic_pauser_threshold: !firrtl.uint<16>, in %nic_pauser_quanta: !firrtl.uint<16>,
        in %nic_pauser_refresh: !firrtl.uint<16>, out %nic_done: !firrtl.uint<1>,
        in %other: !firrtl.uint<8>) {}
    } })", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  SmallVector<Attribute> targets;
  targets.push_back(b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper"));
  for (auto p : named(c, c.getName()).getPorts())
    targets.push_back(b.getStringAttr("~GGNICCPUStreamResponseBufferWrapper|GGNICCPUStreamResponseBufferWrapper>" + p.name.str()));
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Nested")), b.getNamedAttr("targets", b.getArrayAttr(targets))})}));
  return root;
}
void check(CircuitOp c) {
  auto bank = named(c, "GGNICMMIOBank"), top = named(c, "GGNICMMIOWrapper");
  require(std::distance(bank.getOps<RegOp>().begin(), bank.getOps<RegOp>().end()) == 6 &&
      bank.getOps<RegResetOp>().empty(), "NIC registers must all remain unreset");
  require(std::distance(bank.getOps<AssertOp>().begin(), bank.getOps<AssertOp>().end()) == 5,
      "write-only read assertions missing");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  const llvm::StringRef names[]{"macaddr_upper", "macaddr_lower", "rlimit_settings", "pause_threshold", "pause_times", "done"};
  require(regs && regs.size() == 6, "register count differs");
  for (unsigned i = 0; i < 6; ++i) {
    auto d = cast<DictionaryAttr>(regs[i]);
    require(d.getAs<StringAttr>("name") == names[i] && d.getAs<IntegerAttr>("offset").getInt() == 4*i &&
        d.getAs<BoolAttr>("readable").getValue() == (i == 5) && d.getAs<BoolAttr>("writeable").getValue(),
        "register permissions/order differs");
  }
  auto old = named(c, "GGNICCPUStreamResponseBufferWrapper");
  InstanceOp sim, mmio;
  for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getName() == "sim") sim = inst;
    if (inst.getName() == "nicRegisters") mmio = inst;
  }
  require(sim && mmio, "active bank is not instantiated");
  for (unsigned j = 2; j < 10; ++j) {
    std::optional<unsigned> index;
    for (auto [i,p] : llvm::enumerate(old.getPorts())) if (p.name == bank.getPortName(j)) index = i;
    require(bool(index), "consumed signal missing");
    bool connected = false;
    for (auto op : top.getOps<StrictConnectOp>())
      connected |= op.getDest() == (j == 9 ? mmio.getResult(j) : sim.getResult(*index)) &&
          op.getSrc() == (j == 9 ? sim.getResult(*index) : mmio.getResult(j));
    require(connected, "control/status route differs");
  }
}
void rejection(MLIRContext &ctx) {
  for (unsigned bad = 0; bad < 29; ++bad) {
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c,c.getName()); OpBuilder b(&ctx);
    if (bad < 24) {
      unsigned index = 2 + bad/3;
      if (bad%3 == 0) { SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[index]=b.getStringAttr("missing"); top.setPortNames(names); }
      if (bad%3 == 1) { auto types=top.getPortTypes(); SmallVector<Attribute> vs(types.begin(),types.end()); vs[index]=TypeAttr::get(UIntType::get(&ctx,7,false)); top->setAttr("portTypes",b.getArrayAttr(vs)); }
      if (bad%3 == 2) { SmallVector<bool> ds(top.getPortDirections().begin(),top.getPortDirections().end()); ds[index]=!ds[index]; top.setPortDirections(ds); }
    } else if (bad == 24) c.setName("WrongTop");
    else if (bad == 25) c->removeAttr("rawAnnotations");
    else if (bad == 26) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),top,"used"); }
    else if (bad == 27) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGNICMMIOBank"),top.getConventionAttr(),ArrayRef<PortInfo>{}); }
    else { SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[10]=b.getStringAttr("nicBridge_mcr"); top.setPortNames(names); }
    std::string before, after, error;
    { llvm::raw_string_ostream os(before); root->print(os); }
    require(failed(goldengate::addNICMMIOBank(c,error)), "invalid boundary accepted");
    { llvm::raw_string_ostream os(after); root->print(os); }
    require(before == after,"rejected NIC mapping mutated IR");
  }
}
void write(ModuleOp root, llvm::StringRef path) {
  std::error_code ec; llvm::raw_fd_ostream os(path,ec); require(!ec,"cannot open fixture output"); root.print(os); os << '\n';
}
}
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root = argc == 4 ? parseSourceFile<ModuleOp>(argv[1],&ctx) : fixture(ctx);
    require(bool(root),"input parse failed"); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
    require(succeeded(goldengate::addNICMMIOBank(c,error)),error);
    require(succeeded(verify(*root)),"NIC MMIO verification failed"); check(c);
    if (argc == 4) write(*root,argv[2]);
    auto bank = named(c,"GGNICMMIOBank");
    auto standalone = ModuleOp::create(c.getLoc()); OpBuilder b(&ctx); b.setInsertionPointToEnd(standalone.getBody());
    auto sc = b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGNICMMIOBank"));
    b.setInsertionPointToEnd(sc.getBodyBlock()); b.insert(bank->clone());
    require(succeeded(verify(standalone)),"standalone bank verification failed");
    if (argc == 4) write(standalone,argv[3]);
    else if (argc == 2) write(standalone,argv[1]);
    standalone.erase();
    if (argc != 4) {
      auto targets=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
      require(cast<StringAttr>(targets[0]).getValue()=="~GGNICMMIOWrapper","circuit target not transferred");
      for(unsigned i=0;i<11;++i) {
        auto port=named(c,"GGNICCPUStreamResponseBufferWrapper").getPortName(i);
        std::string expected="~GGNICMMIOWrapper|"+std::string(i<2||i==10?"GGNICMMIOWrapper":"GGNICCPUStreamResponseBufferWrapper")+">"+port.str();
        require(cast<StringAttr>(targets[i+1])==expected,"nested copied/consumed target differs");
      }
      rejection(ctx);
    }
    llvm::outs()<<"NIC MMIO: six unreset registers, permissions and production routes passed\n";
    if (argc != 4) llvm::outs()<<"Nested identities and 29 atomic rejections passed\n";
    return 0;
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
