// See LICENSE for license details.
#include "goldengate/CPUStreamRead.h"
#include "goldengate/CPUStreamQueue.h"
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
using goldengate::CPUStreamSourcePort;
using goldengate::CPUStreamCountPort;
static void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
static FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
static std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGNICPacketAdaptersWrapper" {
    firrtl.module @GGNICPacketAdaptersWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      in %nic_from_host_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      out %nic_to_host_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      out %tracerv_stream_count: !firrtl.uint<13>) {
    }
  } })", &ctx);
  require(bool(root), "parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto top = named(c,c.getName());
  auto queue = goldengate::createCPUStreamQueue6144(c,"GGTracerVStreamQueue6144");
  queue->setAttr("goldengate.streamParameters", b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr("TRACERVBRIDGEMODULE_0_to_cpu_stream")),
      b.getNamedAttr("depth",b.getI64IntegerAttr(6144)), b.getNamedAttr("widthBytes",b.getI64IntegerAttr(64)),
      b.getNamedAttr("index",b.getI64IntegerAttr(0))}));
  b.setInsertionPointToEnd(top.getBodyBlock());
  b.create<ConnectOp>(c.getLoc(),top.getArgument(3),top.getArgument(2));
  auto q = b.create<InstanceOp>(c.getLoc(),queue,"tracer_queue");
  for (unsigned i=0;i<2;++i) b.create<StrictConnectOp>(c.getLoc(),q.getResult(i),top.getArgument(i));
  b.create<ConnectOp>(c.getLoc(),q.getResult(2),top.getArgument(2));
  b.create<ConnectOp>(c.getLoc(),top.getArgument(4),q.getResult(3));
  b.create<StrictConnectOp>(c.getLoc(),top.getArgument(5),q.getResult(4));
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
      b.getNamedAttr("widgetClass",b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule"))})}));
  std::string error; require(succeeded(goldengate::addNICCPUStreamQueues(c,error)),error);
  // Exercise incoming and outgoing discovery across an additional wrapper.
  top = named(c,c.getName()); b.setInsertionPointToEnd(c.getBodyBlock());
  auto outer = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("CatalogTop"),top.getConventionAttr(),top.getPorts());
  b.setInsertionPointToStart(outer.getBodyBlock()); auto sim = b.create<InstanceOp>(c.getLoc(),top,"sim");
  for (auto [i,p] : llvm::enumerate(top.getPorts()))
    b.create<ConnectOp>(c.getLoc(),p.direction==Direction::In ? sim.getResult(i) : outer.getArgument(i),
        p.direction==Direction::In ? outer.getArgument(i) : sim.getResult(i));
  c.setName("CatalogTop"); return root;
}
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>(); std::string error;
    auto root=fixture(ctx); auto c=*root->getOps<CircuitOp>().begin();
    SmallVector<CPUStreamSourcePort> sources, sinks; SmallVector<CPUStreamCountPort> counts;
    auto before=dump(root->getOperation());
    require(succeeded(goldengate::deriveCPUStreamPorts(c,sources,sinks,counts,error)),error);
    require(dump(root->getOperation())==before,"catalog mutated IR");
    require(sources.size()==2 && sources[0].streamName=="SIMPLENICBRIDGEMODULE_0_to_cpu_stream" &&
      sources[0].portName=="nic_to_host_stream" && sources[0].depth==3072 &&
      sources[1].streamName=="TRACERVBRIDGEMODULE_0_to_cpu_stream" && sources[1].depth==6144 &&
      sinks.size()==1 && sinks[0].streamName=="SIMPLENICBRIDGEMODULE_0_from_cpu_stream" && sinks[0].depth==3072,
      "directional constructor order");
    require(counts.size()==3 && counts[0].streamName==sources[0].streamName && counts[0].countBits==12 &&
      counts[1].streamName==sources[1].streamName && counts[1].countBits==13 &&
      counts[2].streamName==sinks[0].streamName && counts[2].countBits==12,"source then sink counts");
    for (unsigned mode=0;mode<4;++mode) {
      auto bad=fixture(ctx); auto bc=*bad->getOps<CircuitOp>().begin(); auto top=named(bc,"GGNICCPUStreamQueuesWrapper");
      InstanceOp incoming,outgoing;
      for (auto i:top.getOps<InstanceOp>()) { if(i.getName().contains("incomingQueue"))incoming=i; if(i.getName().contains("outgoingQueue"))outgoing=i; }
      OpBuilder b(&ctx);
      if(mode==0) for(auto op:top.getOps<StrictConnectOp>()) {
        if(op.getSrc()==outgoing.getResult(4)) op->setOperand(1,incoming.getResult(4));
      }
      if(mode==1) outgoing->removeAttr("goldengate.streamParameters");
      if(mode==2) { NamedAttrList attrs(outgoing->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"));
        attrs.set("depth",b.getI64IntegerAttr(6144)); outgoing->setAttr("goldengate.streamParameters",attrs.getDictionary(&ctx)); }
      if(mode==3) { NamedAttrList attrs(incoming->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"));
        attrs.set("direction",b.getStringAttr("toHostCPU")); incoming->setAttr("goldengate.streamParameters",attrs.getDictionary(&ctx)); }
      before=dump(bad->getOperation());
      require(failed(goldengate::deriveCPUStreamPorts(bc,sources,sinks,counts,error)) && dump(bad->getOperation())==before &&
        sources.size()==2 && sinks.size()==1 && counts.size()==3 && sources[0].depth==3072,"atomic malformed catalog rejection");
    }
    require(succeeded(goldengate::addCPUStreamRead(c,sources,error)),error);
    require(succeeded(goldengate::addCPUStreamCountBank(c,counts,error)),error);
    require(succeeded(verify(*root)),"invalid combined read/count IR");
    auto read=named(c,"GGCPUStreamRead"); auto streams=read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
    require(read->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt()==19 && streams.size()==2,"read window bits");
    for (unsigned i=0;i<2;++i) { auto d=cast<DictionaryAttr>(streams[i]);
      require(d.getAs<IntegerAttr>("index").getInt()==i && d.getAs<IntegerAttr>("bufferBaseAddress").getInt()==int64_t(i)*524288 &&
        d.getAs<StringAttr>("name").getValue()==sources[i].streamName,"read allocation"); }
    auto regs=named(c,"GGCPUStreamCountBank")->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    require(regs.size()==3,"count words");
    for(unsigned i=0;i<3;++i) require(cast<DictionaryAttr>(regs[i]).getAs<IntegerAttr>("offset").getInt()==4*i,"count offset");
    if(argc==2) { c->removeAttr("rawAnnotations"); std::error_code ec; llvm::raw_fd_ostream out(argv[1],ec); require(!ec,"dump"); root->print(out);out<<'\n'; }
    llvm::outs()<<"PASS live NIC/TracerV stream catalog, independent directions, atomic rejects, read windows and count order\n";
    return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n';return 1; }
}
