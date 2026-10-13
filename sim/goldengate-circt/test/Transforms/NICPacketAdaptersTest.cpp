// See LICENSE for license details.
#include "goldengate/NICPacketAdapters.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, bool nic = true, unsigned width = 66) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGNICTokenQueuesWrapper" { firrtl.module @GGNICTokenQueuesWrapper() {} } })", &ctx);
  require(bool(root), "parse"); auto c = *root->getOps<CircuitOp>().begin(); (*c.getOps<FModuleOp>().begin()).erase(); OpBuilder b(&ctx);
  auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  auto tok = BundleType::get(&ctx,{{b.getStringAttr("ready"),true,uint(1)}, {b.getStringAttr("valid"),false,uint(1)}, {b.getStringAttr("bits"),false,uint(width)}});
  SmallVector<PortInfo> ps{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("hostReset"),uint(1),Direction::In}, {b.getStringAttr("nic_from_host_enq"),tok,Direction::In},
    {b.getStringAttr("nic_to_host_deq"),tok,Direction::Out}, {b.getStringAttr("nic_macAddr"),uint(48),Direction::In}};
  b.setInsertionPointToEnd(c.getBodyBlock());
  b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  SmallVector<Attribute> annos;
  if(nic) annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
    b.getNamedAttr("widgetClass",b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule"))}));
  for(auto name : {"hostClock","nic_from_host_enq","nic_macAddr"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.TargetIdentity")),
      b.getNamedAttr("target",b.getStringAttr("~GGNICTokenQueuesWrapper|GGNICTokenQueuesWrapper>"+std::string(name)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos)); return root;
}
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>(); std::string error;
    auto root=fixture(ctx); auto c=*root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICPacketAdapters(c,error)),error.c_str());
    require(succeeded(verify(*root)),"invalid adapter IR");
    FModuleOp decoder,encoder,wrapper;
    for(auto m:c.getOps<FModuleOp>()) { if(m.getName()=="GGNICBigTokenDecoder")decoder=m; if(m.getName()=="GGNICBigTokenEncoder")encoder=m; if(m.getName()==c.getName())wrapper=m; }
    require(decoder&&encoder&&wrapper&&wrapper.getNumPorts()==5,"adapter boundary");
    unsigned buffers=0,resetRegs=0,instances=0;
    for(auto r:encoder.getOps<RegOp>()) { ++buffers; require(r.getType(0)==UIntType::get(&ctx,66,false),"buffer width"); }
    for(auto r:encoder.getOps<RegResetOp>()) { ++resetRegs; require(r.getType(0)==UIntType::get(&ctx,32,false)||r.getType(0)==UIntType::get(&ctx,43,false),"counter width"); }
    for(auto i:wrapper.getOps<InstanceOp>()) ++instances;
    require(buffers==6&&resetRegs==2&&instances==3,"buffer/reset/bypass geometry");
    auto a=c->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(cast<DictionaryAttr>(a[1]).getAs<StringAttr>("target").getValue()=="~GGNICPacketAdaptersWrapper|GGNICPacketAdaptersWrapper>hostClock","host identity transfer");
    require(cast<DictionaryAttr>(a[2]).getAs<StringAttr>("target").getValue()=="~GGNICPacketAdaptersWrapper|GGNICTokenQueuesWrapper>nic_from_host_enq","consumed identity transfer");
    require(cast<DictionaryAttr>(a[3]).getAs<StringAttr>("target").getValue()=="~GGNICPacketAdaptersWrapper|GGNICPacketAdaptersWrapper>nic_macAddr","copied identity transfer");
    auto bad=fixture(ctx,true,65); auto bc=*bad->getOps<CircuitOp>().begin();
    require(failed(goldengate::addNICPacketAdapters(bc,error))&&bc.getName()=="GGNICTokenQueuesWrapper"&&std::distance(bc.getOps<FModuleOp>().begin(),bc.getOps<FModuleOp>().end())==1,"typed rejection mutated IR");
    auto none=fixture(ctx,false); auto nc=*none->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICPacketAdapters(nc,error))&&nc.getName()=="GGNICTokenQueuesWrapper","no-NIC mutation");
    require(failed(goldengate::addNICPacketAdapters(c,error)),"duplicate adapter accepted");
    if(argc==2) {
      // Standalone RTL fixture retains both adapters through a fully wired top.
      wrapper.erase(); (*c.getOps<FModuleOp>().begin()).erase();
      c->removeAttr("rawAnnotations"); c.setName("PacketTest"); OpBuilder b(&ctx); auto loc=c.getLoc();
      SmallVector<PortInfo> ps{{b.getStringAttr("clock"),ClockType::get(&ctx),Direction::In}, {b.getStringAttr("reset"),UIntType::get(&ctx,1,false),Direction::In}};
      for(auto pair : {std::make_pair(decoder,"decode"),std::make_pair(encoder,"encode")}) {
        ps.push_back({b.getStringAttr(std::string(pair.second)+"_in"),pair.first.getPortType(2),Direction::In});
        ps.push_back({b.getStringAttr(std::string(pair.second)+"_out"),pair.first.getPortType(3),Direction::Out});
      }
      b.setInsertionPointToEnd(c.getBodyBlock()); auto top=b.create<FModuleOp>(loc,b.getStringAttr("PacketTest"),ConventionAttr::get(&ctx,Convention::Internal),ps);
      b.setInsertionPointToStart(top.getBodyBlock()); unsigned offset=2;
      for(auto m:{decoder,encoder}) {
        auto i=b.create<InstanceOp>(loc,m,m==decoder?"decode":"encode");
        for(unsigned j=0;j<2;++j)b.create<StrictConnectOp>(loc,i.getResult(j),top.getBodyBlock()->getArgument(j));
        b.create<ConnectOp>(loc,i.getResult(2),top.getBodyBlock()->getArgument(offset++));
        b.create<ConnectOp>(loc,top.getBodyBlock()->getArgument(offset++),i.getResult(3));
      }
      require(succeeded(verify(*root)),"invalid standalone fixture"); std::error_code ec; llvm::raw_fd_ostream os(argv[1],ec); require(!ec,"dump"); root->print(os); os<<'\n';
    }
    llvm::outs()<<"PASS NIC packet adapters: bypass geometry, typed boundary and target transfer\n"; return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; }
}
