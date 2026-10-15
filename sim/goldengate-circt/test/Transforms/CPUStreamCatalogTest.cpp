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
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned banks = 1, bool nic = true) {
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
  auto token = top.getPortType(4);
  for (unsigned i = 1; i < banks; ++i) {
    auto suffix = "_" + std::to_string(i);
    unsigned n = top.getNumPorts();
    top.insertPorts({{n, PortInfo(b.getStringAttr("tracerv_stream" + suffix), token, Direction::Out)},
        {n, PortInfo(b.getStringAttr("tracerv_stream_count" + suffix), UIntType::get(&ctx,13), Direction::Out)}});
  }
  b.setInsertionPointToEnd(top.getBodyBlock());
  b.create<ConnectOp>(c.getLoc(),top.getArgument(3),top.getArgument(2));
  for (unsigned i = 0; i < banks; ++i) {
    auto suffix = i ? "_" + std::to_string(i) : "";
    auto queue = goldengate::createCPUStreamQueue6144(c,"GGTracerVStreamQueue6144" + suffix);
    queue->setAttr("goldengate.streamParameters", b.getDictionaryAttr({
        b.getNamedAttr("name",b.getStringAttr("TRACERVBRIDGEMODULE_" + std::to_string(i) + "_to_cpu_stream")),
        b.getNamedAttr("depth",b.getI64IntegerAttr(6144)), b.getNamedAttr("widthBytes",b.getI64IntegerAttr(64)),
        b.getNamedAttr("index",b.getI64IntegerAttr(i))}));
    b.setInsertionPointToEnd(top.getBodyBlock());
    auto q = b.create<InstanceOp>(c.getLoc(),queue,"tracer_queue" + suffix);
    for (unsigned j=0;j<2;++j) b.create<StrictConnectOp>(c.getLoc(),q.getResult(j),top.getArgument(j));
    b.create<ConnectOp>(c.getLoc(),q.getResult(2),top.getArgument(2));
    unsigned port = 4 + 2*i;
    b.create<ConnectOp>(c.getLoc(),top.getArgument(port),q.getResult(3));
    b.create<StrictConnectOp>(c.getLoc(),top.getArgument(port+1),q.getResult(4));
  }
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
      b.getNamedAttr("widgetClass",b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule"))})}));
  if (!nic) c->setAttr("rawAnnotations",b.getArrayAttr({}));
  std::string error; if (nic) require(succeeded(goldengate::addNICCPUStreamQueues(c,error)),error);
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
    // Four independent queues, both alone and following a NIC constructor.
    for (bool nic : {false, true}) {
      auto quad = fixture(ctx,4,nic); auto qc = *quad->getOps<CircuitOp>().begin();
      SmallVector<CPUStreamSourcePort> qs, qk; SmallVector<CPUStreamCountPort> qr;
      before = dump(quad->getOperation());
      require(succeeded(goldengate::deriveCPUStreamPorts(qc,qs,qk,qr,error)),error);
      require(dump(quad->getOperation())==before && qs.size()==4+nic &&
          qk.size()==unsigned(nic) && qr.size()==4+2*nic,"quad catalog sizes/read-only");
      for(unsigned i=0;i<4;++i) require(qs[i+nic].streamName=="TRACERVBRIDGEMODULE_"+
          std::to_string(i)+"_to_cpu_stream" && qr[i+nic].streamName==qs[i+nic].streamName,
          "quad constructor/count order");
      require(succeeded(goldengate::addCPUStreamRead(qc,qs,error)),error);
      require(succeeded(goldengate::addCPUStreamCountBank(qc,qr,error)),error);
      require(succeeded(verify(*quad)),"quad read/count composition");
      auto allocations=named(qc,"GGCPUStreamRead")->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
      for(unsigned i=0;i<qs.size();++i) require(cast<DictionaryAttr>(allocations[i]).getAs<IntegerAttr>(
          "bufferBaseAddress").getInt()==int64_t(i)*524288,"quad read windows");
      auto words=named(qc,"GGCPUStreamCountBank")->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
      for(unsigned i=0;i<qr.size();++i) require(cast<DictionaryAttr>(words[i]).getAs<IntegerAttr>(
          "offset").getInt()==4*i,"quad count offsets");
    }
    // A bad final bank must not publish the first three descriptors.
    for(unsigned mode=0;mode<13;++mode) {
      auto bad=fixture(ctx,4,false); auto bc=*bad->getOps<CircuitOp>().begin();
      auto inner=named(bc,"GGNICPacketAdaptersWrapper");
      auto queue=named(bc,"GGTracerVStreamQueue6144_3"); OpBuilder b(&ctx);
      if(mode<4) { NamedAttrList attrs(queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters"));
        if(mode==0) attrs.set("index",b.getI64IntegerAttr(2));
        if(mode==1) attrs.set("name",b.getStringAttr("TRACERVBRIDGEMODULE_2_to_cpu_stream"));
        if(mode==2) attrs.set("depth",b.getI64IntegerAttr(3072));
        if(mode==3) attrs.set("widthBytes",b.getI64IntegerAttr(32));
        queue->setAttr("goldengate.streamParameters",attrs.getDictionary(&ctx));
      }
      if(mode==4) { for(auto m:queue.getOps<MemOp>()) m->setAttr("depth",b.getI64IntegerAttr(3072)); }
      InstanceOp previous;
      for(auto inst:inner.getOps<InstanceOp>()) if(inst.getName()=="tracer_queue_2") previous=inst;
      for(auto op:llvm::make_early_inc_range(inner.getOps<StrictConnectOp>()))
        if(op.getDest()==inner.getArgument(11)) {
          if(mode==5) op->setOperand(1,previous.getResult(4));
          if(mode==6) op.erase();
          if(mode==7) { b.setInsertionPointAfter(op); b.create<StrictConnectOp>(bc.getLoc(),op.getDest(),op.getSrc()); }
        }
      if(mode==8) for(auto op:inner.getOps<ConnectOp>())
        if(op.getDest()==inner.getArgument(10)) op->setOperand(1,previous.getResult(3));
      if(mode>=9) {
        auto outer=named(bc,bc.getName()); unsigned n=outer.getNumPorts();
        auto suffix=mode==10 ? "_5" : mode==11 ? "_03" : "_4";
        auto type=mode==12 ? Type(UIntType::get(&ctx,1)) : outer.getPortType(4);
        outer.insertPorts({{n,PortInfo(b.getStringAttr(std::string("tracerv_stream")+suffix),type,Direction::Out)}});
        if(mode!=9) outer.insertPorts({{n+1,PortInfo(b.getStringAttr(std::string("tracerv_stream_count")+suffix),
            UIntType::get(&ctx,13),Direction::Out)}});
      }
      before=dump(bad->getOperation());
      require(failed(goldengate::deriveCPUStreamPorts(bc,sources,sinks,counts,error)) &&
          dump(bad->getOperation())==before && sources.size()==2 && sinks.size()==1 && counts.size()==3,
          "atomic malformed fourth bank rejection");
    }
    if(argc==2) { c->removeAttr("rawAnnotations"); std::error_code ec; llvm::raw_fd_ostream out(argv[1],ec); require(!ec,"dump"); root->print(out);out<<'\n'; }
    llvm::outs()<<"PASS live NIC/TracerV catalogs, four independent queues, 17 atomic rejects, read windows and count order\n";
    return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n';return 1; }
}
