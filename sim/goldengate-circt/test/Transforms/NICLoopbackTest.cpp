// See LICENSE for license details.
#include "NICTokenQueuesFixture.h"
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    std::string error;
    auto root=fixture(ctx,true,1,2,true);auto c=*root->getOps<CircuitOp>().begin();
    OpBuilder builder(&ctx);SmallVector<Attribute> annotations;
    unsigned widget=0;
    for(auto attr:c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      NamedAttrList d(cast<DictionaryAttr>(attr));
      if(d.get("widgetClass")) {
        d.set("target",builder.getStringAttr(
          "~GGCPUStreamPlatformWrapper|GGCPUStreamPlatformWrapper>FireSim_ep_"+
          std::to_string(widget+3)+"_nic_out_bits_data_source.bits"));
        ++widget;
      }
      annotations.push_back(d.getDictionary(&ctx));
    }
    c->setAttr("rawAnnotations",builder.getArrayAttr(annotations));
    if(argc==3) {
      std::error_code ec;llvm::raw_fd_ostream out(argv[2],ec);require(!ec,"loopback input output");root->print(out);out<<'\n';
    }
    require(succeeded(goldengate::addNICLoopbackTokenQueues(c,error)),error.c_str());
    require(succeeded(verify(*root)),"invalid loopback IR");
    require(c.getName()=="GGNICLoopbackTokenQueuesWrapper","loopback not distinct from CPU packet boundary");
    FModuleOp queue,top;for(auto m:c.getOps<FModuleOp>()) {
      if(m.getName()==c.getName())top=m;
      if(m.getName()=="GGNICLoopbackTokenQueue10")queue=m;
    }
    require(queue&&top&&top.getNumPorts()==94,"loopback external ABI");
    auto ram=*queue.getOps<MemOp>().begin();
    require(ram.getDataType()==UIntType::get(&ctx,74,false)&&ram.getDepth()==10&&
        ram.getReadLatency()==0&&ram.getWriteLatency()==1,"loopback lost keep/queue geometry");
    unsigned instances=0,regs=0,invalids=0;
    for(auto i:top.getOps<InstanceOp>())++instances;
    for(auto r:top.getOps<RegResetOp>())++regs;
    for(auto v:top.getOps<InvalidValueOp>())++invalids;
    require(instances==5&&regs==4&&invalids==6,"independent loopback scheduler/state/pauser fields");
    auto bindings=top->getAttrOfType<ArrayAttr>("goldengate.nicTokenBindings");
    require(bindings&&bindings.size()==2,"loopback bindings missing");
    for(auto a:bindings) {
      auto d=cast<DictionaryAttr>(a);
      require(d.getAs<StringAttr>("mode").getValue()=="compile_time_loopback"&&
          d.getAs<StringAttr>("fromHost").getValue().empty()&&
          d.getAs<StringAttr>("toHost").getValue().empty(),"loopback silently advertises normal CPU stream ABI");
    }
    if(argc>=2) {
      std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"loopback output");root->print(out);out<<'\n';
    }
    auto bad=fixture(ctx,true,2,2);auto bc=*bad->getOps<CircuitOp>().begin();
    std::string before,after;llvm::raw_string_ostream b(before);bad->print(b);b.flush();
    require(failed(goldengate::addNICLoopbackTokenQueues(bc,error)),"bad loopback channel accepted");
    llvm::raw_string_ostream a(after);bad->print(a);a.flush();require(before==after,"loopback failure mutated circuit");
    llvm::outs()<<"PASS independent two-widget compile-time NIC loopback channels/74-bit queues/seed schedule\n";return 0;
  }catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}
}
