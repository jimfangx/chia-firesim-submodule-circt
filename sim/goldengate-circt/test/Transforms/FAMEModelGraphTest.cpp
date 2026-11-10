// See LICENSE for license details.
#include "goldengate/FAMEModelGraph.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Support/Timing.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
#include <cstdlib>
using namespace mlir;
using namespace circt::firrtl;
using namespace goldengate;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
FModuleOp named(CircuitOp c, StringRef name) { for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m; return {}; }
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, StringRef fir, StringRef anno) {
  llvm::SourceMgr mgr;
  for (auto path : {fir, anno}) { auto buffer = llvm::MemoryBuffer::getFile(path); require(bool(buffer), "missing graph fixture"); mgr.AddNewSourceBuffer(std::move(*buffer),llvm::SMLoc()); }
  DefaultTimingManager timer; auto scope = timer.getRootScope(); FIRParserOptions options; options.numAnnotationFiles=1;
  auto root=importFIRFile(mgr,&ctx,scope,options); require(bool(root) && succeeded(verify(*root)), "invalid graph fixture");
  auto c=*root->getOps<CircuitOp>().begin(); Builder b(&ctx);
  c->setAttr("test.metadata",b.getStringAttr("whole graph metadata"));
  for(auto i:named(c,"Top").getOps<InstanceOp>())i->setAttr("test.preserved",b.getStringAttr(i.getName()));
  return root;
}
void run(MLIRContext &ctx, StringRef fir, StringRef anno) {
  for(unsigned bad=0;bad<10;++bad) {
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);auto top=named(c,"Top");
    auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> rows(raw.begin(),raw.end());
    if(bad==0) { // A late transport collision occurs after full model rewriting.
      b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFAMEPipeWrapper"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    }
    if(bad==1) {
      rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.UnsupportedDataOwner")),b.getNamedAttr("target",b.getStringAttr("~Top|Worker>data"))}));
    }
    if(bad==9) {
      rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.UnsupportedInstanceDataOwner")),b.getNamedAttr("nested",b.getArrayAttr({b.getStringAttr("~Top|Top/w0_:Worker>data")}))}));
    }
    if(bad==2) {
      top.insertPorts({{top.getNumPorts(),PortInfo(b.getStringAttr("unboundData"),UIntType::get(&ctx,1),Direction::In)}});
    }
    if(bad==3) {
      b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("AbstractClockGate"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    }
    if(bad==4) for(auto &row:rows) {
      auto d=cast<DictionaryAttr>(row);auto cls=d.getAs<StringAttr>("class");
      if(cls==AnnotationClasses::ChannelPorts && d.getAs<StringAttr>("localName")=="input") {
        auto ports=d.getAs<ArrayAttr>("ports");
        if(ports && cast<StringAttr>(ports[0]).getValue().contains("|Worker>")) {NamedAttrList v(d);v.set("clockPort",b.getStringAttr("~Top|Hub>alias0"));row=v.getDictionary(&ctx);}
      }
    }
    if(bad==5) rows.erase(llvm::find_if(rows,[&](Attribute a){auto d=cast<DictionaryAttr>(a);auto info=d.getAs<DictionaryAttr>("channelInfo");return info&&info.getAs<StringAttr>("class")==AnnotationClasses::TargetClockChannel;}));
    if(bad==6) {
      b.setInsertionPointToEnd(top.getBodyBlock());auto second=b.create<InstanceOp>(c.getLoc(),named(c,"Hub"),"secondHub");(void)second;
    }
    if(bad==7) for(auto &row:rows) {
      auto d=cast<DictionaryAttr>(row);
      if(d.getAs<StringAttr>("class")==AnnotationClasses::ChannelConnection&&d.getAs<StringAttr>("globalName") && d.getAs<StringAttr>("globalName").getValue()=="w1_output") {
        NamedAttrList v(d);v.set("sources",b.getArrayAttr({b.getStringAttr("~Top|Top>w0_out")}));row=v.getDictionary(&ctx);
      }
    }
    if(bad==8) {
      auto worker=named(c,"Worker");
      SmallVector<std::pair<unsigned,PortInfo>> added{{worker.getNumPorts(),PortInfo(b.getStringAttr("exportedClock"),ClockType::get(&ctx),Direction::Out)}};
      worker.insertPorts(added);
      SmallVector<InstanceOp> originals;for(auto i:top.getOps<InstanceOp>())if(i.getModuleName()=="Worker")originals.push_back(i);
      for(auto i:originals){auto replacement=i.cloneAndInsertPorts(added);for(unsigned j=0;j<i.getNumResults();++j)i.getResult(j).replaceAllUsesWith(replacement.getResult(j));i.erase();}
    }
    c->setAttr("rawAnnotations",b.getArrayAttr(rows));auto before=dump(*root);std::string error;
    FAMEModelGraphResult result{97,98,99,100};
    require(failed(rewriteFAMEModelGraph(c,result,error))&&!error.empty()&&dump(*root)==before&&
      result.definitions==97&&result.instances==98&&result.clockDomains==99&&result.dataEndpoints==100,
      "graph rejection changed IR/result or accepted bad case "+std::to_string(bad));
  }
  for(bool transport:{false,true}) {
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();std::string error;FAMEModelGraphResult result;
    require(succeeded(rewriteFAMEModelGraph(c,result,error,transport)),error);
    require(succeeded(verify(*root)),"graph output is invalid");
    require(result.definitions==2&&result.instances==3&&result.clockDomains==2&&result.dataEndpoints==8,"model graph inventory differs");
    require(c->hasAttr("test.metadata"),"graph metadata lost");
    auto top=named(c,"Top");unsigned hub=0,workers=0;
    for(auto i:top.getOps<InstanceOp>()) {
      if(i.getModuleName()=="Hub")++hub;if(i.getModuleName()=="Worker")++workers;
      if(i.getModuleName()=="Hub"||i.getModuleName()=="Worker")require(i->hasAttr("test.preserved"),"model instance metadata lost");
    }
    require(hub==1&&workers==2,"graph merged or dropped model instances");
    unsigned constraints=0;named(c,"Hub").walk([&](InstanceOp i){constraints+=i->hasAttr("goldengate.generatedClockConstraint");});
    require(constraints==2,"multidomain gate/XDC identity differs");
    if(transport) {
      auto wrapper=named(c,c.getName());require(c.getName()=="GGFAMEPipeWrapper","graph transport is not active");
      unsigned queues=0,latencyOne=0;
      for(auto i:wrapper.getOps<InstanceOp>())if(i.getModuleName().starts_with("GGFAMEPipe")){
        ++queues;auto queue=named(c,i.getModuleName());
        queue.walk([&](RegOp reg){latencyOne+=reg.getName()=="initializing";});
      }
      require(queues==7&&latencyOne==1,"internal/external fanout queue multiplicity or latency differs");
      require(wrapper.getNumPorts()==8,"internal graph data leaked into external wrapper");
    } else require(c.getName()=="Top","pre-transport graph changed top identity");
  }
}
void runThreading(MLIRContext &ctx, StringRef fir, StringRef anno) {
  for (bool transport : {false,true}) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); Builder b(&ctx);
    auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> rows(raw.begin(),raw.end());
    rows.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::EnableModelMultiThreading)),
        b.getNamedAttr("target",b.getStringAttr("~Top|Top/w0_:Worker"))}));
    c->setAttr("rawAnnotations",b.getArrayAttr(rows));
    std::string error; FAMEModelGraphResult result;
    require(succeeded(rewriteFAMEModelGraph(c,result,error,transport,true,FAME5ClockSemantics::XilinxBUFGCE)),
            "threading before graph transport: "+error);
    require(succeeded(verify(*root)) && result.definitions==2 && result.instances==3 &&
            result.threadedGroups==1 && result.sharedThreads==2,
            "threaded graph inventory or final verification differs");
    auto top=named(c,"Top"); unsigned physical=0,original=0;
    for (auto instance:top.getOps<InstanceOp>()) {
      original+=instance.getModuleName()=="Worker";
      if(instance.getModuleName()=="Worker_threaded") {
        ++physical;
        auto members=instance->getAttrOfType<ArrayAttr>("goldengate.fame5.members");
        auto metadata=instance->getAttrOfType<ArrayAttr>("goldengate.fame5.memberMetadata");
        require(members && metadata && members.size()==2 && metadata.size()==2,
                "logical thread identities lost in physical shared hardware");
        for (unsigned i=0;i<2;++i)
          require(cast<DictionaryAttr>(metadata[i]).getAs<StringAttr>("test.preserved")==members[i],
                  "independent logical member metadata lost");
      }
    }
    require(physical==1 && original==0 && named(c,"Worker") && named(c,"Worker_threaded"),
            "threading did not share only selected repeated hardware");
    auto allocation=c->getAttrOfType<ArrayAttr>("goldengate.fame5.groups");
    require(allocation && allocation.size()==1,"threaded graph allocation lost during transport");
    if(transport) {
      auto wrapper=named(c,c.getName());
      require(c.getName()=="GGFAMEPipeWrapper" && wrapper.getNumPorts()==8,
              "threaded graph external transport identity differs");
      unsigned queues=0;
      for(auto instance:wrapper.getOps<InstanceOp>())
        queues+=instance.getModuleName().starts_with("GGFAMEPipe");
      require(queues==7,"threading lost logical transport channels or fanout queues");
    }
    // Optional supporting-artifact export from the very API invocation tested
    // above. This never runs another transform or changes compiler behavior.
    if (const char *prefix=std::getenv("FAME5_GRAPH_TEST_OUTPUT")) {
      std::error_code ec;
      llvm::raw_fd_ostream out(std::string(prefix)+(transport?"-transport.mlir":"-boundary.mlir"),ec);
      require(!ec,"cannot export tested threaded graph"); root->print(out);
    }
  }
  // Abstract grouping still rejects dynamic virtual clocks, atomically, even
  // when invoked through the production graph composition API.
  auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();Builder b(&ctx);
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> rows(raw.begin(),raw.end());
  rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::EnableModelMultiThreading)),
      b.getNamedAttr("target",b.getStringAttr("~Top|Top/w0_:Worker"))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(rows));auto before=dump(*root);std::string error;
  FAMEModelGraphResult result{97,98,99,100,101,102};
  require(failed(rewriteFAMEModelGraph(c,result,error,true,true)) && dump(*root)==before &&
          result.threadedGroups==101 && result.sharedThreads==102,
          "production graph abstract dynamic-clock rejection was not atomic");
}
void runPassthrough(MLIRContext &ctx, StringRef fir, StringRef anno) {
  for (unsigned bad=0;bad<7;++bad) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); auto top=named(c,"Top"); OpBuilder b(&ctx);
    unsigned input=top.getNumPorts(),output=input+1;
    auto type=SIntType::get(&ctx,9);
    PortInfo sink(b.getStringAttr("passthroughInput"),type,Direction::In);
    PortInfo source(b.getStringAttr("passthroughOutput"),type,Direction::Out);
    // Payload metadata must follow the whole Decoupled identity.
    sink.sym=circt::hw::InnerSymAttr::get(b.getStringAttr("loopback_identity"));
    sink.annotations=AnnotationSet({Annotation(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::DontTouch))}))},&ctx);
    top.insertPorts({{input,sink},{input,source}});
    b.setInsertionPointToEnd(top.getBodyBlock());
    b.create<ConnectOp>(top.getLoc(),top.getBodyBlock()->getArgument(output),top.getBodyBlock()->getArgument(input));
    if (bad==1) { // A second user needs token fanout, not a bare whole connect.
      auto extra=b.create<WireOp>(top.getLoc(),type,"extraConsumer");
      b.create<ConnectOp>(top.getLoc(),extra.getResult(),top.getBodyBlock()->getArgument(input));
    }
    if (bad==2) top.insertPorts({{top.getNumPorts(),PortInfo(b.getStringAttr("loopInput_sink"),UIntType::get(&ctx,1),Direction::In)}});
    auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> rows(raw.begin(),raw.end());
    auto channel=[&](StringRef name,StringRef endpoint,StringRef port) {
      rows.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("globalName",b.getStringAttr(name)),
        b.getNamedAttr("channelInfo",b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::PipeChannel)),b.getNamedAttr("latency",b.getI64IntegerAttr(0))})),
        b.getNamedAttr(endpoint,b.getArrayAttr({b.getStringAttr("~Top|Top>"+port.str())}))}));
    };
    channel("loopInput","sinks","passthroughInput");
    if (bad!=3) channel("loopOutput","sources","passthroughOutput");
    if (bad==4) channel("duplicateInput","sinks","passthroughInput");
    rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(bad==5?"test.UnsupportedLoopMetadata":AnnotationClasses::DontTouch)),
      b.getNamedAttr("target",b.getStringAttr("~Top|Top>passthroughOutput"))}));
    c->setAttr("rawAnnotations",b.getArrayAttr(rows));auto before=dump(*root);std::string error;FAMEModelGraphResult result{97,98,99,100};
    if(bad && bad!=6) {
      require(failed(rewriteFAMEModelGraph(c,result,error,false)) && !error.empty() && dump(*root)==before && result.instances==98,
        "loopback rejection accepted or partially mutated case "+std::to_string(bad));
      continue;
    }
    require(succeeded(rewriteFAMEModelGraph(c,result,error,bad==6)),error);
    require(succeeded(verify(*root)),"loopback graph invalid");
    auto boundary=named(c,"Top");
    require(bool(boundary),"loopback boundary lost");
    bool foundInput=false,foundOutput=false;unsigned connects=0;
    for(auto p:boundary.getPorts()) {
      if(p.getName()=="loopInput_sink") {
        foundInput=true;auto channel=dyn_cast<BundleType>(p.type);require(channel && p.direction==Direction::In,"loopback input is not Decoupled");
        require(p.sym && p.sym.size()==1 && (*p.sym.begin()).getName()=="loopback_identity" && (*p.sym.begin()).getFieldID()==3,"loopback symbol did not move to payload");
        require(p.annotations.size()==1,"loopback protection lost");
      }
      if(p.getName()=="loopOutput_source")foundOutput=true;
      require(p.getName()!="passthroughInput" && p.getName()!="passthroughOutput","stale scalar loopback port");
    }
    boundary.walk([&](ConnectOp op) {
      auto d=dyn_cast<BlockArgument>(op.getDest()),r=dyn_cast<BlockArgument>(op.getSrc());
      connects+=d&&r&&boundary.getPortName(d.getArgNumber())=="loopOutput_source"&&boundary.getPortName(r.getArgNumber())=="loopInput_sink";
    });
    require(foundInput && foundOutput && connects==1,"whole Decoupled loopback connection missing");
    auto dumpText=dump(c);require(dumpText.find("loopOutput_source.bits")!=std::string::npos,"loopback annotation target was not transferred");
  }
}
void runGrouped(MLIRContext &ctx, StringRef fir, StringRef anno, StringRef output) {
  for (unsigned bad=0; bad<3; ++bad) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); Builder b(&ctx);
    auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> rows(raw.begin(),raw.end());
    for (auto &row:rows) {
      auto d=cast<DictionaryAttr>(row);
      if (bad==0 && d.getAs<StringAttr>("globalName") && d.getAs<StringAttr>("globalName").getValue()=="w1_output") {
        auto values=d.getAs<ArrayAttr>("sources"); NamedAttrList v(d);
        v.set("sources",b.getArrayAttr({values[1],values[0]})); row=v.getDictionary(&ctx);
      }
      if (bad==1 && d.getAs<StringAttr>("globalName") && d.getAs<StringAttr>("globalName").getValue()=="w1_input") {
        auto values=d.getAs<ArrayAttr>("sinks"); NamedAttrList v(d);
        v.set("sinks",b.getArrayAttr({values[0]})); row=v.getDictionary(&ctx);
      }
    }
    if (bad==2) rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.UnknownGroupedOwner")),
      b.getNamedAttr("target",b.getStringAttr("~Top|Worker>w1_input_b"))}));
    c->setAttr("rawAnnotations",b.getArrayAttr(rows)); auto before=dump(*root); std::string error;
    FAMEModelGraphResult result{97,98,99,100};
    require(failed(rewriteFAMEModelGraph(c,result,error,false)) && !error.empty() && dump(*root)==before && result.instances==98,
      "shared grouped negative accepted or mutated IR: "+std::to_string(bad));
  }
  for(bool transport:{false,true}) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); std::string error; FAMEModelGraphResult result;
    if(transport) {
      auto before=dump(*root);
      require(failed(rewriteFAMEModelGraph(c,result,error,true)) && error.find("aggregated PipeChannel transport")!=std::string::npos && dump(*root)==before,
        "SFC unsupported aggregate pipe transport accepted or mutated IR");
      continue;
    }
    require(succeeded(rewriteFAMEModelGraph(c,result,error,false)),error);
    require(succeeded(verify(*root)),"shared grouped graph is invalid");
    require(result.definitions==2 && result.instances==3 && result.clockDomains==2 && result.dataEndpoints==8,"shared grouped inventory");
    auto worker=named(c,"Worker"); unsigned inputs=0,outputs=0;
    for(auto p:worker.getPorts()) { inputs+=p.getName()=="w1_input_sink"; outputs+=p.getName()=="w1_output_source"; }
    require(inputs==1 && outputs==1,"shared grouped channel ABI not collapsed exactly once");
    if(!transport && !output.empty()) { std::error_code ec; llvm::raw_fd_ostream out(output,ec); require(!ec,"cannot write graph fixture output"); root->print(out); }
  }
}
// Complete pre-FAME target-to-target RV graph, including independent domain
// state and the reverse ready path. The SFC FAMETransform supports this graph;
// its SimWrapper does not expose internal RV pairs in its lookup maps.
void runReadyValid(MLIRContext &ctx, StringRef fir, StringRef anno, StringRef output) {
  for (unsigned bad=0; bad<4; ++bad) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); Builder b(&ctx);
    auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> rows(raw.begin(),raw.end());
    for (auto &row:rows) {
      auto d=cast<DictionaryAttr>(row); auto name=d.getAs<StringAttr>("globalName");
      if (!name) continue;
      NamedAttrList v(d);
      if (name=="link_fwd" && bad<3) {
        NamedAttrList info(d.getAs<DictionaryAttr>("channelInfo"));
        if (bad==0) info.erase("readySource");
        if (bad==1) info.set("validSink",b.getStringAttr("~Top|Top>link_fwbits"));
        if (bad==2) v.set("sinks",b.getArrayAttr({b.getStringAttr("~Top|Top>link_fwbits")}));
        v.set("channelInfo",info.getDictionary(&ctx));
      }
      if (name=="link_rev" && bad==3)
        v.set("sources",b.getArrayAttr({b.getStringAttr("~Top|Top>w_observed")}));
      row=v.getDictionary(&ctx);
    }
    c->setAttr("rawAnnotations",b.getArrayAttr(rows)); auto before=dump(*root); std::string error;
    FAMEModelGraphResult result{97,98,99,100};
    require(failed(rewriteFAMEModelGraph(c,result,error,true)) && !error.empty() && dump(*root)==before && result.instances==98,
      "RV graph malformed pair accepted or partially mutated: "+std::to_string(bad));
  }
  for(bool transport:{false,true}) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); std::string error; FAMEModelGraphResult result;
    require(succeeded(rewriteFAMEModelGraph(c,result,error,transport)),error);
    require(succeeded(verify(*root)),"RV model graph output is invalid");
    require(result.definitions==2 && result.instances==2 && result.clockDomains==2 && result.dataEndpoints==9,"RV graph inventory");
    if (transport) {
      auto wrapper=named(c,c.getName()); unsigned queues=0;
      for(auto i:wrapper.getOps<InstanceOp>()) queues+=i.getModuleName()=="GGFAMEReadyValid8";
      require(queues==1 && wrapper.getNumPorts()==8,"internal RV queue multiplicity or bridge port leakage");
    } else if (!output.empty()) {
      std::error_code ec; llvm::raw_fd_ostream out(output,ec); require(!ec,"cannot write RV graph boundary"); root->print(out);
    }
  }
}
void runMemory(MLIRContext &ctx, StringRef fir, StringRef anno, StringRef output) {
  for (unsigned bad=0; bad<3; ++bad) {
    auto root=fixture(ctx,fir,anno); auto c=*root->getOps<CircuitOp>().begin(); Builder b(&ctx);
    if (bad==0) {
      // Changing an alias definition shared by another instance needs a
      // multi-instance clock-output transfer policy, not a partial rewrite.
      OpBuilder add(named(c,"Top").getBodyBlock(),named(c,"Top").getBodyBlock()->end());
      add.create<InstanceOp>(c.getLoc(),named(c,"Memory"),"secondMemory");
    } else {
      auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> rows(raw.begin(),raw.end());
      if (bad==1) rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.UnsupportedMemoryPortOwner")),
        b.getNamedAttr("target",b.getStringAttr("~Top|Memory>request"))}));
      if (bad==2) for (auto &row:rows) {
        auto d=cast<DictionaryAttr>(row);
        if (auto local=d.getAs<StringAttr>("localName"); local && local=="asynchronous") {
          NamedAttrList v(d);v.set("clockPort",b.getStringAttr("~Top|Hub>alias1"));row=v.getDictionary(&ctx);
        }
      }
      c->setAttr("rawAnnotations",b.getArrayAttr(rows));
    }
    auto before=dump(*root);std::string error;FAMEModelGraphResult result{97,98,99,100};
    require(failed(rewriteFAMEModelGraph(c,result,error,true)) && !error.empty() && dump(*root)==before && result.instances==98,
      "memory/clock alias malformed graph accepted or partially mutated: "+std::to_string(bad));
  }
  for (bool transport:{false,true}) {
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();std::string error;FAMEModelGraphResult result;
    require(succeeded(rewriteFAMEModelGraph(c,result,error,transport)),error);
    require(succeeded(verify(*root)),"memory/combinational model graph invalid");
    require(result.definitions==3 && result.instances==3 && result.clockDomains==2 && result.dataEndpoints==11,"memory graph inventory");
    unsigned memories=0,combinationalState=0;
    named(c,"Memory").walk([&](MemOp){++memories;});
    named(c,"Combinational").walk([&](RegOp){++combinationalState;});
    require(memories==2 && combinationalState==0,"target memory/state shape changed");
    for (auto name:{"Memory","Combinational"}) {
      auto model=named(c,name);unsigned aliases=0;
      for (auto port:model.getPorts()) require(!isa<ClockType>(port.type) || port.getName()=="hostClock","virtual target clock escaped");
      model.walk([&](WireOp wire){aliases+=wire.getName()=="aliasClock";});
      require(aliases==1,"SFC clock-output wire identity missing");
    }
    if (!transport && !output.empty()) {std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot write memory boundary");root->print(out);}
  }
}
void runRepeatedAliases(MLIRContext &ctx, StringRef fir, StringRef anno, StringRef output) {
  for (unsigned bad=0; bad<3; ++bad) {
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"Top");auto memory=named(c,"Memory");Builder b(&ctx);
    SmallVector<StringRef> names{"m_","m1_"};
    if (bad==0) names.pop_back(); // Complete shared definition ABI is required.
    if (bad==1) for (auto instance:top.getOps<InstanceOp>()) if (instance.getName()=="m1_") {
      auto attrs=instance.getPortAnnotationsAttr();SmallVector<Attribute> rows(attrs.begin(),attrs.end());
      rows.back()=b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.ClockOwner"))})});
      instance.setPortAnnotationsAttr(b.getArrayAttr(rows));
    }
    if (bad==2) {OpBuilder add(top.getBodyBlock(),top.getBodyBlock()->end());add.create<InstanceOp>(c.getLoc(),memory,"unlistedMemory");}
    auto before=dump(*root);std::string error;
    require(failed(internalizeFAMEOutputClocks(top,memory,names,error)) && !error.empty() && dump(*root)==before,
      "all-instance clock helper accepted malformed case or mutated IR: "+std::to_string(bad));
  }
  { // The standalone transfer touches only the selected definition instances.
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"Top");auto memory=named(c,"Memory");
    auto unrelated=dump(named(c,"Combinational"));std::string error;SmallVector<StringRef> names{"m_","m1_"};
    require(succeeded(internalizeFAMEOutputClocks(top,memory,names,error)),error);
    require(succeeded(verify(*root)),"all-instance clock helper produced invalid IR");
    require(dump(named(c,"Combinational"))==unrelated,"unrelated definition changed");
    unsigned wires=0;memory.walk([&](WireOp w){wires+=w.getName()=="aliasClock";});require(wires==1,"shared definition has duplicate alias wires");
    for(auto instance:top.getOps<InstanceOp>()) require(instance->hasAttr("test.preserved"),"clock transfer lost independent instance metadata");
  }
  for(bool transport:{false,true}) {
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();std::string error;FAMEModelGraphResult result;
    require(succeeded(rewriteFAMEModelGraph(c,result,error,transport)),error);
    require(succeeded(verify(*root)),"repeated alias graph invalid");
    require(result.definitions==3 && result.instances==5 && result.clockDomains==2 && result.dataEndpoints==18,"repeated alias graph inventory");
    unsigned memories=0;named(c,"Memory").walk([&](MemOp){++memories;});require(memories==2,"shared target memory definitions duplicated or lost");
    for(auto name:{"Memory","Combinational"}) {
      auto model=named(c,name);unsigned wires=0;model.walk([&](WireOp w){wires+=w.getName()=="aliasClock";});require(wires==1,"shared clock alias wire differs");
      for(auto port:model.getPorts()) require(!isa<ClockType>(port.type)||port.getName()=="hostClock","shared target clock escaped");
    }
    unsigned memoryInstances=0,comboInstances=0;
    for(auto instance:named(c,"Top").getOps<InstanceOp>()) {
      memoryInstances+=instance.getModuleName()=="Memory";comboInstances+=instance.getModuleName()=="Combinational";
      require(instance->hasAttr("test.preserved"),"independent repeated-instance metadata lost");
    }
    require(memoryInstances==2 && comboInstances==2,"independent repeated model instances merged or dropped");
    if(!transport && !output.empty()){std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot write repeated alias graph");root->print(out);}
  }
}
void runVirtual(MLIRContext &ctx, StringRef fir, StringRef anno, StringRef output) {
  { // SFC's SimWrapper still requires exactly one explicit clock channel.
    auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();auto before=dump(*root);
    std::string error;FAMEModelGraphResult result{97,98,99,100};
    require(failed(rewriteFAMEModelGraph(c,result,error,true)) && error.find("transport requires an explicit")!=std::string::npos &&
      dump(*root)==before && result.instances==98,"hubless transport accepted or rejection mutated IR/result");
  }
  auto root=fixture(ctx,fir,anno);auto c=*root->getOps<CircuitOp>().begin();std::string error;FAMEModelGraphResult result;
  require(succeeded(rewriteFAMEModelGraph(c,result,error,false)),error);
  require(succeeded(verify(*root)),"hubless virtual graph is invalid");
  require(result.definitions==1 && result.instances==2 && result.clockDomains==0 && result.dataEndpoints==4,"hubless graph inventory");
  auto top=named(c,"Top");unsigned instances=0;
  for(auto i:top.getOps<InstanceOp>())if(i.getModuleName()=="Worker") {++instances;require(i->hasAttr("test.preserved"),"hubless instance metadata lost");}
  require(instances==2,"hubless shared instances merged or dropped");
  for(auto p:top.getPorts())require(!isa<ClockType>(p.type)||p.getName()=="hostClock","virtual target clock escaped top");
  unsigned gates=0;named(c,"Worker").walk([&](InstanceOp i){gates+=i.getModuleName()=="AbstractClockGate";});
  require(gates==1,"shared virtual definition lost its own gate");
  if(!output.empty()){std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot write hubless graph");root->print(out);}
}
} // namespace
int main(int argc,char **argv){try{
 if(argc==3 && StringRef(argv[1])=="--threaded-input") {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  auto root=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(root),"threaded graph snapshot parse failed");
  auto circuit=*root->getOps<CircuitOp>().begin();FAMEModelGraphResult graph;std::string error;
  require(succeeded(rewriteFAMEModelGraph(circuit,graph,error,true,true,FAME5ClockSemantics::XilinxBUFGCE)),error);
  require(succeeded(verify(*root)),"threaded graph snapshot verification failed");
  llvm::outs()<<"Grouped "<<graph.threadedGroups<<" definitions, "<<graph.sharedThreads<<" threads\n";return 0;
 }
 require(argc==3 || argc==4 || argc==5 || argc==6 || argc==7 || argc==8,"usage: graph-test input.fir input.json [boundary.mlir [ready-valid]]");MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();if(argc==8) runVirtual(ctx,argv[1],argv[2],argv[3]); else if(argc==7) runRepeatedAliases(ctx,argv[1],argv[2],argv[3]); else if(argc==6) runMemory(ctx,argv[1],argv[2],argv[3]); else if(argc==5) runReadyValid(ctx,argv[1],argv[2],argv[3]); else if(argc==4) runGrouped(ctx,argv[1],argv[2],argv[3]); else {run(ctx,argv[1],argv[2]);runThreading(ctx,argv[1],argv[2]);runPassthrough(ctx,argv[1],argv[2]);}llvm::outs()<<(argc==8 ? "PASS hubless repeated virtual-clock graph boundary and atomic unsupported simulator transport rejection\n" : argc==7 ? "PASS repeated memory/combinational clock aliases: complete definition transfer, independent retained instances and three atomic helper rejections\n" : argc==6 ? "PASS memory/combinational virtual models with exported clock aliases: two RAM latencies, two real hub domains, three atomic malformed cases\n" : argc==5 ? "PASS complete internal ready-valid model graph: two definitions/domains, one transport, four atomic malformed cases\n" : argc==4 ? "PASS shared grouped model graph: two independent aggregate instances, complete boundary, three atomic negative cases and guarded aggregate Pipe transport\n" : "PASS full selected model graph: shared instances, multidomain controls, internal fanout transport and ten atomic rejection cases\n");return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
