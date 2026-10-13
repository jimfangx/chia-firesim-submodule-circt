// See LICENSE for license details.
#include "goldengate/NICCPUStreamQueues.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
static FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned width = 512, unsigned nics = 1) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGNICPacketAdaptersWrapper" { firrtl.module @GGNICPacketAdaptersWrapper() {} } })", &ctx);
  require(bool(root), "parse"); auto c = *root->getOps<CircuitOp>().begin(); named(c,c.getName()).erase(); OpBuilder b(&ctx);
  auto uint = [&](unsigned w) { return UIntType::get(&ctx,w,false); };
  auto tok = BundleType::get(&ctx, {{b.getStringAttr("ready"),true,uint(1)},
    {b.getStringAttr("valid"),false,uint(1)}, {b.getStringAttr("bits"),false,uint(width)}});
  SmallVector<PortInfo> ps{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("hostReset"),uint(1),Direction::In}, {b.getStringAttr("nic_from_host_stream"),tok,Direction::In},
    {b.getStringAttr("nic_to_host_stream"),tok,Direction::Out}, {b.getStringAttr("nic_done"),uint(1),Direction::Out}};
  b.setInsertionPointToEnd(c.getBodyBlock()); auto m = b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  // A complete loopback makes both independent buffers observable in RTL.
  b.setInsertionPointToStart(m.getBodyBlock());
  b.create<ConnectOp>(c.getLoc(),m.getBodyBlock()->getArgument(3),m.getBodyBlock()->getArgument(2));
  b.create<StrictConnectOp>(c.getLoc(),m.getBodyBlock()->getArgument(4),b.create<ConstantOp>(c.getLoc(),uint(1),APInt(1,0)));
  SmallVector<Attribute> annos;
  for (unsigned i=0;i<nics;++i) annos.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
    b.getNamedAttr("widgetClass",b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule"))}));
  for (auto name : {"hostClock", "nic_from_host_stream.bits", "nic_to_host_stream", "nic_done"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.TargetIdentity")),
      b.getNamedAttr("target",b.getStringAttr("~GGNICPacketAdaptersWrapper|GGNICPacketAdaptersWrapper>"+std::string(name)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos)); return root;
}
static std::string print(ModuleOp m) { std::string s; llvm::raw_string_ostream out(s); m.print(out); return s; }
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>(); std::string error;
    auto root=fixture(ctx); auto c=*root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICCPUStreamQueues(c,error)),error);
    require(succeeded(verify(*root)),"invalid stream queue IR");
    auto queue=named(c,"GGNICCPUStreamQueue3072"), top=named(c,c.getName());
    require(top.getNumPorts()==7 && top.getPortType(5)==UIntType::get(&ctx,12,false) &&
      top.getPortType(6)==UIntType::get(&ctx,12,false),"count geometry");
    require(queue.getPortType(4)==UIntType::get(&ctx,12,false),"queue count width");
    unsigned memories=0,resets=0,unreset=0;
    for(auto r:queue.getOps<MemOp>()) { ++memories; require(r.getDepth()==3072 && r.getDataType()==UIntType::get(&ctx,512,false) &&
      r.getReadLatency()==0 && r.getWriteLatency()==1 && r.getRuw()==RUWAttr::Undefined &&
      r->getAttrOfType<StringAttr>("goldengate.ramStyle")=="ULTRA","RAM geometry/style"); }
    for(auto r:queue.getOps<RegResetOp>()) { ++resets; require(r.getType(0)==UIntType::get(&ctx,12,false)||r.getType(0)==UIntType::get(&ctx,1,false),"reset pointer width"); }
    for(auto r:queue.getOps<RegOp>()) { ++unreset; require(r.getName()=="ram_read_addr"&&r.getType(0)==UIntType::get(&ctx,12,false),"read address policy"); }
    require(memories==1&&resets==3&&unreset==1,"state geometry");
    InstanceOp from,to,sim;
    for(auto i:top.getOps<InstanceOp>()) {
      if(i.getName().contains("incomingQueue")) from=i;
      else if(i.getName().contains("outgoingQueue")) to=i; else sim=i;
    }
    require(from&&to&&sim&&from!=to,"independent queues");
    for(auto fifo:{from,to}) {
      auto p=fifo->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
      require(p && p.getAs<IntegerAttr>("depth").getInt()==3072 && p.getAs<IntegerAttr>("widthBytes").getInt()==64 && !p.get("index"),"premature stream allocation");
      auto direction=p.getAs<StringAttr>("direction");
      require(direction==(fifo==from?"fromHostCPU":"toHostCPU"),"stream direction");
      require(p.getAs<StringAttr>("name")== (fifo==from?
        "SIMPLENICBRIDGEMODULE_0_from_cpu_stream":"SIMPLENICBRIDGEMODULE_0_to_cpu_stream"),"stream name");
    }
    auto bulk=[&](Value d,Value s) { for(auto op:top.getOps<ConnectOp>())if(op.getDest()==d&&op.getSrc()==s)return true;return false; };
    auto strict=[&](Value d,Value s) { for(auto op:top.getOps<StrictConnectOp>())if(op.getDest()==d&&op.getSrc()==s)return true;return false; };
    auto arg=[&](unsigned i){return top.getBodyBlock()->getArgument(i);};
    require(bulk(from.getResult(2),arg(2))&&bulk(sim.getResult(2),from.getResult(3))&&
      bulk(to.getResult(2),sim.getResult(3))&&bulk(arg(3),to.getResult(3)),"stream wiring");
    require(strict(arg(5),from.getResult(4))&&strict(arg(6),to.getResult(4)),"count wiring");
    for(auto fifo:{from,to}) require(strict(fifo.getResult(0),arg(0))&&strict(fifo.getResult(1),arg(1)),"host state domain");
    auto a=c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(a.size()==5,"annotation loss");
    const char* expected[]={"~GGNICCPUStreamQueuesWrapper|GGNICCPUStreamQueuesWrapper>hostClock",
      "~GGNICCPUStreamQueuesWrapper|GGNICPacketAdaptersWrapper>nic_from_host_stream.bits",
      "~GGNICCPUStreamQueuesWrapper|GGNICPacketAdaptersWrapper>nic_to_host_stream",
      "~GGNICCPUStreamQueuesWrapper|GGNICCPUStreamQueuesWrapper>nic_done"};
    for(unsigned i=0;i<4;++i)require(cast<DictionaryAttr>(a[i+1]).getAs<StringAttr>("target")==expected[i],"target transfer");
    auto before=print(*root); require(failed(goldengate::addNICCPUStreamQueues(c,error))&&print(*root)==before,"repeat mutation");
    for(auto pair:{std::make_pair(511u,1u),std::make_pair(512u,2u)}) {
      auto bad=fixture(ctx,pair.first,pair.second);auto bc=*bad->getOps<CircuitOp>().begin();auto old=print(*bad);
      require(failed(goldengate::addNICCPUStreamQueues(bc,error))&&print(*bad)==old,"invalid boundary mutation");
    }
    auto none=fixture(ctx,512,0);auto nc=*none->getOps<CircuitOp>().begin();auto old=print(*none);
    require(succeeded(goldengate::addNICCPUStreamQueues(nc,error))&&print(*none)==old,"no-NIC mutation");
    auto constructor=fixture(ctx);auto cc=*constructor->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
    auto ca=cc->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> attrs(ca.begin(),ca.end());
    NamedAttrList record(cast<DictionaryAttr>(attrs[0]));record.set("widgetConstructorKey",b.getStringAttr("unsupported"));
    attrs[0]=record.getDictionary(&ctx);cc->setAttr("rawAnnotations",b.getArrayAttr(attrs));old=print(*constructor);
    require(failed(goldengate::addNICCPUStreamQueues(cc,error))&&print(*constructor)==old,"constructor rejection mutation");
    auto collision=fixture(ctx);auto dc=*collision->getOps<CircuitOp>().begin();b.setInsertionPointToEnd(dc.getBodyBlock());
    b.create<FModuleOp>(dc.getLoc(),b.getStringAttr("GGNICCPUStreamQueue3072"),ConventionAttr::get(&ctx,Convention::Internal),SmallVector<PortInfo>{});
    old=print(*collision);require(failed(goldengate::addNICCPUStreamQueues(dc,error))&&print(*collision)==old,"collision rejection mutation");
    if(argc==2) {
      c->removeAttr("rawAnnotations");std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"dump");root->print(out);out<<'\n';
    }
    llvm::outs()<<"PASS NIC CPU stream queues: geometry, independent directions, host reset, count wiring and target transfer\n";
    return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n';return 1; }
}
