// See LICENSE for license details.
#include "NICTokenQueuesFixture.h"
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); std::string error;
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICTokenQueues(c, error)), error.c_str());
    require(succeeded(verify(*root)), "invalid NIC IR");
    FModuleOp queue, wrapper;
    for (auto m : c.getOps<FModuleOp>()) { if (m.getName() == "GGNICTokenQueue10") queue=m; if (m.getName()==c.getName()) wrapper=m; }
    require(queue && wrapper && wrapper.getNumPorts() == 12, "wrong NIC boundary");
    auto ram = *queue.getOps<MemOp>().begin();
    require(ram.getDepth()==10 && ram.getReadLatency()==0 && ram.getWriteLatency()==1 &&
        ram.getDataType()==UIntType::get(&ctx,66,false), "wrong queue geometry");
    unsigned count=0; for (auto i:wrapper.getOps<InstanceOp>()) { ++count; if (i.getName()!="sim") require(i.getModuleName()==queue.getName(), "wrong queue instance"); }
    require(count==3, "queues not instantiated");
    // Evaluate the emitted combinational gate operations, with independent
    // queue capacity and each individual pipe valid/ready as external inputs.
    std::function<std::string(Value)> key = [&](Value v) -> std::string {
      if (auto f=v.getDefiningOp<SubfieldOp>()) return key(f.getInput())+"."+f.getFieldName().str();
      return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
    };
    std::map<std::string,Value> drivers;
    for (auto connect:wrapper.getOps<StrictConnectOp>()) require(drivers.emplace(key(connect.getDest()),connect.getSrc()).second,"multiple drivers");
    InstanceOp sim,htnt,ntht;
    for (auto i:wrapper.getOps<InstanceOp>()) { if(i.getName()=="sim")sim=i; if(i.getName()=="htnt_queue")htnt=i; if(i.getName()=="ntht_queue")ntht=i; }
    std::mt19937 rng(0x169); unsigned singleSource=0,singleSink=0;
    for(unsigned cycle=0;cycle<12000;++cycle) {
      std::map<std::string,bool> inputs;
      bool src[4],sink[11],allValid=true,allReady=true;
      for(unsigned j=0;j<4;++j) { src[j]=cycle<4?cycle!=j:bool(rng()&1); allValid &= src[j]; inputs[key(sim.getResult(j+2))+".valid"]=src[j]; }
      for(unsigned j=0;j<11;++j) { sink[j]=cycle>=4&&cycle<15?cycle-4!=j:bool(rng()&1); allReady &= sink[j]; inputs[key(sim.getResult(j+6))+".ready"]=sink[j]; }
      bool inputValid=rng()&1,outputReady=rng()&1;
      inputs[key(htnt.getResult(3))+".valid"]=inputValid; inputs[key(ntht.getResult(2))+".ready"]=outputReady;
      std::function<bool(Value)> eval = [&](Value v)->bool {
        auto k=key(v); if(inputs.count(k))return inputs.at(k);
        if(drivers.count(k))return eval(drivers.at(k));
        auto op=v.getDefiningOp();
        if(auto c=dyn_cast_or_null<ConstantOp>(op)) return !c.getValue().isZero();
        if(isa_and_nonnull<AndPrimOp>(op)) return eval(op->getOperand(0))&&eval(op->getOperand(1));
        if(isa_and_nonnull<NotPrimOp>(op)) return !eval(op->getOperand(0));
        throw std::runtime_error("unsupported gate operation");
      };
      require(eval(drivers.at(key(ntht.getResult(2))+".valid"))==allValid,"NIC enq incorrectly coupled to sink ready");
      require(eval(drivers.at(key(htnt.getResult(3))+".ready"))==allReady,"NIC deq incorrectly coupled to source valid");
      for(unsigned j=0;j<4;++j) {
        bool expected=outputReady; for(unsigned k=0;k<4;++k) if(k!=j)expected&=src[k];
        require(eval(drivers.at(key(sim.getResult(j+2))+".ready"))==expected,"source exclusion gate mismatch");
        if(cycle<4&&!src[j])++singleSource;
      }
      for(unsigned j=0;j<11;++j) {
        bool expected=inputValid; for(unsigned k=0;k<11;++k) if(k!=j)expected&=sink[k];
        require(eval(drivers.at(key(sim.getResult(j+6))+".valid"))==expected,"sink exclusion gate mismatch");
        if(cycle>=4&&cycle<15&&!sink[j])++singleSink;
      }
      require(eval(drivers.at(key(wrapper.getBodyBlock()->getArgument(11))))==!(allValid&&allReady),"NIC done predicate mismatch");
    }
    require(singleSource==4&&singleSink==11,"missing isolated channel stalls");

    // Each NIC must be able to progress without consulting another widget's
    // ready/valid or queue capacity. Missing foreign inputs are deliberate:
    // the evaluator rejects any cross-widget dependence.
    auto multi = fixture(ctx, true, 1, 2);
    auto mc = *multi->getOps<CircuitOp>().begin();
    OpBuilder multiBuilder(&ctx);
    SmallVector<Attribute> activeAnnotations;
    for (auto attr:mc->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      NamedAttrList d(cast<DictionaryAttr>(attr));
      if (auto target=d.get("target")) {
        auto global=cast<DictionaryAttr>(d.get("channelMapping")).getAs<StringAttr>("nic_out_bits_data");
        d.set("target",multiBuilder.getStringAttr("~GGCPUStreamPlatformWrapper|GGCPUStreamPlatformWrapper>FireSim_"+
            global.getValue().str()+"_source.bits"));
      }
      activeAnnotations.push_back(d.getDictionary(&ctx));
    }
    mc->setAttr("rawAnnotations",multiBuilder.getArrayAttr(activeAnnotations));
    require(succeeded(goldengate::addNICTokenQueues(mc,error)), error.c_str());
    require(succeeded(verify(*multi)), "invalid multiple-NIC IR");
    auto singleWrapper = wrapper;
    for (auto m:mc.getOps<FModuleOp>()) if(m.getName()==mc.getName()) wrapper=m;
    require(wrapper.getNumPorts()==22, "multiple-NIC external ABI");
    auto bindings=wrapper->getAttrOfType<ArrayAttr>("goldengate.nicTokenBindings");
    require(bindings && bindings.size()==2, "missing independent NIC bindings");
    unsigned bridgeIndex=0;
    for (auto attr:mc->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      auto d=cast<DictionaryAttr>(attr);
      if (d.get("widgetClass")) {
        auto target=d.getAs<StringAttr>("target");
        require(target && target.getValue().starts_with("~GGNICTokenQueuesWrapper|"),
            "active NIC target did not transfer");
        require(cast<DictionaryAttr>(bindings[bridgeIndex++]).get("bridgeTarget")==target,
            "NIC binding and BridgeIO identities differ");
      }
    }
    require(bridgeIndex==2,"lost multiple-NIC bridge identities");
    for (unsigned widget=0; widget<2; ++widget) {
    std::function<std::string(Value)> key = [&](Value v) -> std::string {
      if (auto f=v.getDefiningOp<SubfieldOp>()) return key(f.getInput())+"."+f.getFieldName().str();
      return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
    };
    std::map<std::string,Value> drivers;
    for (auto connect:wrapper.getOps<StrictConnectOp>()) require(drivers.emplace(key(connect.getDest()),connect.getSrc()).second,"multiple drivers");
    InstanceOp sim,htnt,ntht;
    for (auto i:wrapper.getOps<InstanceOp>()) {
      if(i.getName()=="sim")sim=i;
      if(i.getName()=="nic" + std::to_string(widget) + "_htnt_queue")htnt=i;
      if(i.getName()=="nic" + std::to_string(widget) + "_ntht_queue")ntht=i;
    }
    require(sim && htnt && ntht, "missing independent widget queues");
    std::mt19937 rng(0x169); unsigned singleSource=0,singleSink=0;
    for(unsigned cycle=0;cycle<12000;++cycle) {
      std::map<std::string,bool> inputs;
      bool src[4],sink[11],allValid=true,allReady=true;
      for(unsigned j=0;j<4;++j) { src[j]=cycle<4?cycle!=j:bool(rng()&1); allValid &= src[j]; inputs[key(sim.getResult(widget*15+j+2))+".valid"]=src[j]; }
      for(unsigned j=0;j<11;++j) { sink[j]=cycle>=4&&cycle<15?cycle-4!=j:bool(rng()&1); allReady &= sink[j]; inputs[key(sim.getResult(widget*15+j+6))+".ready"]=sink[j]; }
      bool inputValid=rng()&1,outputReady=rng()&1;
      inputs[key(htnt.getResult(3))+".valid"]=inputValid; inputs[key(ntht.getResult(2))+".ready"]=outputReady;
      std::function<bool(Value)> eval = [&](Value v)->bool {
        auto k=key(v); if(inputs.count(k))return inputs.at(k);
        if(drivers.count(k))return eval(drivers.at(k));
        auto op=v.getDefiningOp();
        if(auto c=dyn_cast_or_null<ConstantOp>(op)) return !c.getValue().isZero();
        if(isa_and_nonnull<AndPrimOp>(op)) return eval(op->getOperand(0))&&eval(op->getOperand(1));
        if(isa_and_nonnull<NotPrimOp>(op)) return !eval(op->getOperand(0));
        throw std::runtime_error("unsupported gate operation");
      };
      require(eval(drivers.at(key(ntht.getResult(2))+".valid"))==allValid,"NIC enq incorrectly coupled to sink ready");
      require(eval(drivers.at(key(htnt.getResult(3))+".ready"))==allReady,"NIC deq incorrectly coupled to source valid");
      for(unsigned j=0;j<4;++j) {
        bool expected=outputReady; for(unsigned k=0;k<4;++k) if(k!=j)expected&=src[k];
        require(eval(drivers.at(key(sim.getResult(widget*15+j+2))+".ready"))==expected,"source exclusion gate mismatch");
        if(cycle<4&&!src[j])++singleSource;
      }
      for(unsigned j=0;j<11;++j) {
        bool expected=inputValid; for(unsigned k=0;k<11;++k) if(k!=j)expected&=sink[k];
        require(eval(drivers.at(key(sim.getResult(widget*15+j+6))+".valid"))==expected,"sink exclusion gate mismatch");
        if(cycle>=4&&cycle<15&&!sink[j])++singleSink;
      }
      require(eval(drivers.at(key(wrapper.getBodyBlock()->getArgument(11 + widget*10))))==!(allValid&&allReady),"NIC done predicate mismatch");
    }
    require(singleSource==4&&singleSink==11,"missing isolated channel stalls");

    }
    wrapper = singleWrapper;
    // Alias a channel only in the second widget. The complete preflight must
    // reject before introducing any queue/wrapper or rewriting annotations.
    auto aliased = fixture(ctx, true, 1, 2);
    auto acircuit = *aliased->getOps<CircuitOp>().begin();
    OpBuilder ab(&ctx);
    auto annotations=acircuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> changed(annotations.begin(),annotations.end());
    NamedAttrList last(cast<DictionaryAttr>(changed.back()));
    NamedAttrList mapping(cast<DictionaryAttr>(last.get("channelMapping")));
    mapping.set("nic_out_bits_data", ab.getStringAttr("ep_3_nic_out_bits_data"));
    last.set("channelMapping", mapping.getDictionary(&ctx));
    changed.back()=last.getDictionary(&ctx);
    acircuit->setAttr("rawAnnotations", ab.getArrayAttr(changed));
    std::string before; llvm::raw_string_ostream beforeOS(before); aliased->print(beforeOS); beforeOS.flush();
    require(failed(goldengate::addNICTokenQueues(acircuit,error)), "cross-widget alias accepted");
    std::string after; llvm::raw_string_ostream afterOS(after); aliased->print(afterOS); afterOS.flush();
    require(before==after, "late NIC rejection mutated source");

    unsigned channels=0;
    for (auto attr:c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      auto d=cast<DictionaryAttr>(attr);
      for (auto key:{"sources","sinks"}) if (auto ends=d.getAs<ArrayAttr>(key)) for (auto end:ends) {
        auto t=goldengate::resolveAnnotationTarget(c,cast<StringAttr>(end).getValue(),error);
        require(t && t->port && t->module.getModuleName()=="GGCPUStreamPlatformWrapper", "lost consumed channel identity"); ++channels;
      }
      if (auto clock=d.getAs<StringAttr>("clock")) {
        auto t=goldengate::resolveAnnotationTarget(c,clock.getValue(),error);
        require(t && t->module.getModuleName()==wrapper.getName(), "lost copied clock identity");
      }
    }
    require(channels==15, "missing channel transfer");
    if (argc==2) {
      auto dump=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGNICTokenQueue10" { firrtl.module @GGNICTokenQueue10() {} } })",&ctx);
      auto dc=*dump->getOps<CircuitOp>().begin(); (*dc.getOps<FModuleOp>().begin()).erase(); dc.getBodyBlock()->push_back(queue->clone());
      std::error_code ec; llvm::raw_fd_ostream out(argv[1],ec); require(!ec,"dump file"); dump->print(out); out << '\n';
    }
    auto bad=fixture(ctx,true,2); auto bc=*bad->getOps<CircuitOp>().begin();
    require(failed(goldengate::addNICTokenQueues(bc,error)) && bc.getName()=="GGCPUStreamPlatformWrapper" &&
        std::distance(bc.getOps<FModuleOp>().begin(),bc.getOps<FModuleOp>().end())==1,"latency rejection mutated circuit");
    auto duplicate=fixture(ctx); auto dupc=*duplicate->getOps<CircuitOp>().begin();
    OpBuilder builder(&ctx); auto original=dupc->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> duplicated(original.begin(),original.end()); duplicated.push_back(original[0]);
    dupc->setAttr("rawAnnotations",builder.getArrayAttr(duplicated));
    require(failed(goldengate::addNICTokenQueues(dupc,error)) && dupc.getName()=="GGCPUStreamPlatformWrapper" &&
        std::distance(dupc.getOps<FModuleOp>().begin(),dupc.getOps<FModuleOp>().end())==1,"duplicate channel rejection mutated circuit");
    auto badclock=fixture(ctx); auto cc=*badclock->getOps<CircuitOp>().begin();
    SmallVector<Attribute> clockAnnotations;
    for(auto a:cc->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      NamedAttrList d(cast<DictionaryAttr>(a));
      if(clockAnnotations.empty())d.set("clock",builder.getStringAttr("~GGCPUStreamPlatformWrapper|GGCPUStreamPlatformWrapper>otherClock"));
      clockAnnotations.push_back(d.getDictionary(&ctx));
    }
    cc->setAttr("rawAnnotations",builder.getArrayAttr(clockAnnotations));
    require(failed(goldengate::addNICTokenQueues(cc,error)) && cc.getName()=="GGCPUStreamPlatformWrapper" &&
        std::distance(cc.getOps<FModuleOp>().begin(),cc.getOps<FModuleOp>().end())==1,"inconsistent clock rejection mutated circuit");
    // Actual post-FAME channels retain this identity after the physical
    // FAMETop clock port has been removed. Do not re-resolve it as a host port.
    auto historical=fixture(ctx); auto hc=*historical->getOps<CircuitOp>().begin();
    SmallVector<Attribute> historicalAnnotations;
    for(auto a:hc->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      NamedAttrList d(cast<DictionaryAttr>(a));
      if(d.get("clock"))d.set("clock",builder.getStringAttr("~GGCPUStreamPlatformWrapper|FAMETop>ep_3_clock"));
      historicalAnnotations.push_back(d.getDictionary(&ctx));
    }
    hc->setAttr("rawAnnotations",builder.getArrayAttr(historicalAnnotations));
    require(succeeded(goldengate::addNICTokenQueues(hc,error)) && succeeded(verify(*historical)),"historical FAME clock rejected");
    for(auto a:hc->getAttrOfType<ArrayAttr>("rawAnnotations")) if(auto clock=cast<DictionaryAttr>(a).getAs<StringAttr>("clock"))
      require(clock.getValue()=="~GGNICTokenQueuesWrapper|FAMETop>ep_3_clock","historical clock identity lost");
    auto absent=fixture(ctx,false); auto ac=*absent->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICTokenQueues(ac,error)) && ac.getName()=="GGCPUStreamPlatformWrapper","absent NIC mutated circuit");
    llvm::outs()<<"PASS NIC queue geometry, instantiation, channel/clock transfer and pre-mutation rejection\n";
    return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; }
}
