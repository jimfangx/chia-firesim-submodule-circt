// See LICENSE for license details.
#include "goldengate/FASEDHeader.h"
#include "goldengate/FASEDMMIOBank.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
std::string dump(Operation *op) {
  std::string s; llvm::raw_string_ostream out(s);op->print(out, OpPrintingFlags().printGenericOpForm());return s;
}
const llvm::StringRef modules[]{"GGFASEDLatencyRegisters","GGFASEDRequestLimits","GGFASEDHistograms",
    "GGFASEDStatistics","GGFASEDFunctionalModelRegister","GGFASEDResponseErrors"};
const llvm::StringRef fragments[]{"fased_latency_mcr","fased_request_limits_mcr","fased_histograms_mcr",
    "fased_statistics_mcr","fased_functional_model_mcr","fased_response_errors_mcr"};
const unsigned starts[]{0,2,4,14,18,19},lengths[]{2,2,10,4,1,2};
const llvm::StringRef names[]{"writeLatency","readLatency","writeMaxReqs","readMaxReqs",
    "writeOutstandingHistogram_0","writeOutstandingHistogram_1","writeOutstandingHistogram_2",
    "writeOutstandingHistogram_3","writeOutstandingHistogram_4","readOutstandingHistogram_0",
    "readOutstandingHistogram_1","readOutstandingHistogram_2","readOutstandingHistogram_3",
    "readOutstandingHistogram_4","totalWriteBeats","totalReadBeats","totalWrites","totalReads",
    "relaxFunctionalModel","rrespError","brespError"};
FModuleOp named(CircuitOp c,llvm::StringRef name){for(auto m:c.getOps<FModuleOp>())if(m.getName()==name)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0, unsigned targetBits = 36) {
  auto root=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGFASEDHistogramsWrapper" {
    firrtl.module @GGFASEDTokenEngine() {} firrtl.module @GGFASEDHistogramsWrapper() {} } })",&ctx);
  require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();
  named(c,"GGFASEDHistogramsWrapper").erase();
  named(c,"GGFASEDTokenEngine").erase();OpBuilder b(&ctx);auto loc=c.getLoc();
  auto u=[&](unsigned w){return UIntType::get(&ctx,w,false);};
  auto token=BundleType::get(&ctx,{{b.getStringAttr("ready"),true,u(1)},
      {b.getStringAttr("valid"),false,u(1)},{b.getStringAttr("bits"),false,u(32)}});
  auto mcr=[&](unsigned n){auto vec=FVectorType::get(token,n);return BundleType::get(&ctx,
      {{b.getStringAttr("read"),false,vec},{b.getStringAttr("write"),true,vec},{b.getStringAttr("wstrb"),true,u(4)}});};
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
      {b.getStringAttr("hostReset"),u(1),Direction::In},{b.getStringAttr("other"),u(8),Direction::Out}};
  b.setInsertionPointToEnd(c.getBodyBlock());
  for(unsigned j=0;j<6;++j) {
    ports.push_back({b.getStringAttr(fragments[j]),mcr(lengths[j]),Direction::Out});
    auto bank=b.create<FModuleOp>(loc,b.getStringAttr(modules[j]),ConventionAttr::get(&ctx,Convention::Internal),
        ArrayRef<PortInfo>{{b.getStringAttr("mcr"),mcr(lengths[j]),Direction::Out}});
    SmallVector<Attribute> rows;for(unsigned k=0;k<lengths[j];++k) {
      unsigned word=starts[j]+k;
      rows.push_back(b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(names[word])),
          b.getNamedAttr("offset",b.getI32IntegerAttr(4*word)),b.getNamedAttr("readable",b.getBoolAttr(true)),
          b.getNamedAttr("writeable",b.getBoolAttr(word<4 || word==18))}));
    }bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
  }
  b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDHistogramsWrapper"),ConventionAttr::get(&ctx,Convention::Internal),ports);
  auto payload = [&](StringRef field, unsigned width) { return BundleType::get(&ctx,{{b.getStringAttr(field),false,u(width)}}); };
  auto address = BundleType::get(&ctx,{{b.getStringAttr("addr"),false,u(targetBits)}, {b.getStringAttr("id"),false,u(4)}});
  auto channel = [&](FIRRTLBaseType bits) { return BundleType::get(&ctx,{{b.getStringAttr("bits"),false,bits}}); };
  auto axi = BundleType::get(&ctx,{{b.getStringAttr("aw"),false,channel(address)}, {b.getStringAttr("ar"),false,channel(address)},
      {b.getStringAttr("w"),false,channel(payload("data",64))}, {b.getStringAttr("r"),true,channel(payload("data",64))}});
  auto hPort = BundleType::get(&ctx,{{b.getStringAttr("hBits"),false,BundleType::get(&ctx,{{b.getStringAttr("axi4"),false,axi}})}});
  auto engine = b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{
      {b.getStringAttr("hPort"),hPort,Direction::In}, {b.getStringAttr("timing"),axi,Direction::Out}});
  auto widths=b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI32IntegerAttr(targetBits)),
      b.getNamedAttr("dataBits",b.getI32IntegerAttr(64)),b.getNamedAttr("idBits",b.getI32IntegerAttr(4))});
  auto key=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("firesim.lib.bridges.CompleteConfig")),
      b.getNamedAttr("axi4Widths",widths),b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(10))}))});
  engine->setAttr("goldengate.bridgeConstructor",key);
  auto inner=named(c,"GGFASEDHistogramsWrapper");b.setInsertionPointToStart(inner.getBodyBlock());
  b.create<InstanceOp>(loc,engine,"targetMemory");
  for(unsigned j=0;j<6;++j) {
    auto inst=b.create<InstanceOp>(loc,named(c,modules[j]),modules[j]);
    b.create<ConnectOp>(loc,inner.getArgument(3+j),inst.getResult(0));
  }
  auto output=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("previous body\n"))});
  auto other=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("value",b.getI32IntegerAttr(9))});
  c->setAttr("rawAnnotations",b.getArrayAttr({other,output}));std::string error;
  require(succeeded(goldengate::addFASEDMMIOBank(c,error)),error);
  auto bank=named(c,"GGFASEDMMIOWrapper");
  require(succeeded(goldengate::mapFASEDBridgeControl(c,25,12,error)),error);
  auto control=named(c,"GGFASEDBridgeControlWrapper");
  // Synthetic allocation and final dispatch wrapper; bank aggregation and the
  // Nasti-to-MCRFile adapter above use the production transforms.
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto decoder=b.create<FModuleOp>(loc,b.getStringAttr("GGControlAddressDecode"),control.getConventionAttr(),ArrayRef<PortInfo>{
      {b.getStringAttr("aw_addr"),u(25),Direction::In}});
  auto row=b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr("FASEDMemoryTimingModel_2")),
      b.getNamedAttr("slave",b.getI32IntegerAttr(0)),b.getNamedAttr("start",b.getI64IntegerAttr(2048)),b.getNamedAttr("size",b.getI64IntegerAttr(128))});
  decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({row}));
  auto bound=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDBridgeBoundWrapper"),control.getConventionAttr(),ArrayRef<PortInfo>{});
  bound->setAttr("goldengate.fasedSlave",b.getI32IntegerAttr(0));
  b.setInsertionPointToStart(bound.getBodyBlock());b.create<InstanceOp>(loc,decoder,"decoder");
  auto sim=b.create<InstanceOp>(loc,control,"sim");
  std::optional<unsigned> ci;for(auto [i,p]:llvm::enumerate(control.getPorts()))if(p.name=="fasedBridge_ctrl")ci=i;
  require(bool(ci),"missing native control port");Value ctrl;
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  // Route sources are explicit output ports of the synthetic dispatcher.
  SmallVector<PortInfo> controlPorts=control.getPorts();
  auto addrToken=cast<BundleType>(control.getPortType(*ci)).getElement("ar")->type;
  for(auto p:ArrayRef<PortInfo>{{b.getStringAttr("ctrl_write_dispatch_slave_0_aw_bits_addr"),u(25),Direction::Out},
      {b.getStringAttr("ctrl_read_dispatch_slave_0_ar"),addrToken,Direction::Out},
      {b.getStringAttr("ctrl_read_arb_in_0_ready"),u(1),Direction::Out}, {b.getStringAttr("ctrl_write_arb_in_0_ready"),u(1),Direction::Out}})controlPorts.push_back(p);
  // Rebuild the control module with the extra dispatcher ports, preserving its
  // native adapter wiring and original port indices.
  b.setInsertionPoint(control);auto replacement=b.create<FModuleOp>(loc,b.getStringAttr("temporary"),control.getConventionAttr(),controlPorts);
  replacement.getBodyBlock()->getOperations().splice(replacement.getBodyBlock()->end(),control.getBodyBlock()->getOperations());
  for(unsigned i=0;i<control.getNumPorts();++i)control.getArgument(i).replaceAllUsesWith(replacement.getArgument(i));
  control.erase();replacement.setName("GGFASEDBridgeControlWrapper");control=replacement;
  sim.erase();b.setInsertionPointToEnd(bound.getBodyBlock());sim=b.create<InstanceOp>(loc,control,"sim");ctrl=sim.getResult(*ci);
  unsigned n=control.getNumPorts()-4;
  b.create<StrictConnectOp>(loc,field(field(field(ctrl,"aw"),"bits"),"addr"),sim.getResult(n));
  b.create<ConnectOp>(loc,field(ctrl,"ar"),sim.getResult(n+1));
  b.create<StrictConnectOp>(loc,field(field(ctrl,"r"),"ready"),sim.getResult(n+2));
  b.create<StrictConnectOp>(loc,field(field(ctrl,"b"),"ready"),sim.getResult(n+3));
  c.setName("GGFASEDBridgeBoundWrapper");
  auto update = [&](FModuleOp m, StringRef attribute, StringRef keyName, Attribute value) {
    NamedAttrList list(m->getAttrOfType<DictionaryAttr>(attribute));list.set(keyName,value);m->setAttr(attribute,list.getDictionary(&ctx));
  };
  if(bad>=1 && bad<=5) {
    auto rows=llvm::to_vector(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));NamedAttrList word(cast<DictionaryAttr>(rows[4]));
    if(bad==1)word.set("name",b.getStringAttr("wrong"));if(bad==2)word.set("offset",b.getI32IntegerAttr(20));
    if(bad==3)word.set("readable",b.getBoolAttr(false));if(bad==4)word.set("writeable",b.getBoolAttr(true));
    rows[4]=word.getDictionary(&ctx);if(bad==5)rows.pop_back();bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
  }
  if(bad>=6 && bad<=11) {
    NamedAttrList changed(row);
    if(bad==6)changed.set("start",b.getI64IntegerAttr(2049));if(bad==7)changed.set("size",b.getI64IntegerAttr(80));
    if(bad==8)changed.set("start",b.getI64IntegerAttr(1<<25));if(bad==9)changed.set("name",b.getStringAttr("Other_2"));
    if(bad==10)changed.set("slave",b.getI32IntegerAttr(1));
    SmallVector<Attribute> rows{changed.getDictionary(&ctx)};
    if(bad==11){NamedAttrList overlap(row);overlap.set("name",b.getStringAttr("Other_1"));rows.push_back(overlap.getDictionary(&ctx));}
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
  }
  if(bad>=12 && bad<=15) {
    NamedAttrList w(widths);if(bad==12)w.set("addrBits",b.getI32IntegerAttr(targetBits-1));
    if(bad==13)w.set("addrBits",b.getI32IntegerAttr(63));if(bad==14)w.set("dataBits",b.getI32IntegerAttr(32));
    if(bad==15)w.set("idBits",b.getI32IntegerAttr(5));update(engine,"goldengate.bridgeConstructor","axi4Widths",w.getDictionary(&ctx));
  }
  if(bad==16)update(engine,"goldengate.bridgeConstructor","axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(9))}));
  if(bad==17)engine->removeAttr("goldengate.bridgeConstructor");
  if(bad==18)bank->removeAttr("goldengate.mmioRegisters");
  if(bad==19)decoder->removeAttr("goldengate.controlRegions");
  if(bad==20)bound->removeAttr("goldengate.fasedSlave");
  if(bad==21){b.setInsertionPointToEnd(bound.getBodyBlock());b.create<InstanceOp>(loc,engine,"duplicate");}
  if(bad==22)for(auto inst:inner.getOps<InstanceOp>())if(inst.getModuleName()==engine.getName()){inst.erase();break;}
  if(bad==23)named(c,modules[0])->removeAttr("goldengate.mmioRegisters");
  if(bad==24)for(auto conn:bank.getOps<ConnectOp>())if(conn.getDest().getDefiningOp<SubindexOp>()) {conn.erase();break;}
  if(bad==25)for(auto conn:inner.getOps<ConnectOp>()) {conn.erase();break;}
  if(bad==26)for(auto conn:control.getOps<ConnectOp>()) {
    auto i=conn.getDest().getDefiningOp<InstanceOp>();if(i && i.getModuleName()=="GGFASEDMCRFile" && cast<OpResult>(conn.getDest()).getResultNumber()==3){conn.erase();break;}}
  if(bad==27)for(auto conn:bound.getOps<ConnectOp>()){conn.erase();break;}
  if(bad==28)for(auto conn:bound.getOps<StrictConnectOp>()){conn.erase();break;}
  if(bad==29) {auto annotations=llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations"));annotations.push_back(output);c->setAttr("rawAnnotations",b.getArrayAttr(annotations));}
  if(bad==30)c->removeAttr("rawAnnotations");
  if(bad==31){auto rows=llvm::to_vector(named(c,modules[1])->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
    NamedAttrList word(cast<DictionaryAttr>(rows[0]));word.set("writeable",b.getBoolAttr(false));rows[0]=word.getDictionary(&ctx);
    named(c,modules[1])->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));}
  if(bad==32) {b.setInsertionPointToEnd(bound.getBodyBlock());b.create<InstanceOp>(loc,bank,"duplicateBank");}
  if(bad==33)for(auto conn:bank.getOps<ConnectOp>())if(auto dest=conn.getDest().getDefiningOp<SubindexOp>()) {
    if(dest.getIndex()==0){auto src=conn.getSrc().getDefiningOp<SubindexOp>();src.setIndex(1);break;}}
  if(bad==34) {bound->setAttr("goldengate.fasedSlave",b.getI32IntegerAttr(1));}
  if(bad==35)update(engine,"goldengate.bridgeConstructor","class",b.getStringAttr("OtherConfig"));
  if(bad==36){for(auto conn:bank.getOps<ConnectOp>())if(conn.getDest().getDefiningOp<SubindexOp>()) {
    b.setInsertionPoint(conn);b.clone(*conn);break;}}
  if(bad==37 || bad==38) {
    NamedAttrList changed(row);
    if(bad==37)changed.set("start",b.getI64IntegerAttr(2052));
    if(bad==38)changed.set("size",b.getI64IntegerAttr(84));
    decoder->setAttr("goldengate.controlRegions",b.getArrayAttr({changed.getDictionary(&ctx)}));
  }
  if(bad==39)for(auto slice:named(c,"GGFASEDMCRFile").getOps<BitsPrimOp>()) {slice.setHi(7);break;}
  if(bad==40)for(auto conn:control.getOps<ConnectOp>()) {
    auto i=conn.getDest().getDefiningOp<InstanceOp>();
    if(i && i.getModuleName()=="GGFASEDMCRFile" && cast<OpResult>(conn.getDest()).getResultNumber()==2){conn.erase();break;}}
  return root;
}
std::string run(CircuitOp c) {
  auto before=c->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<std::string> modules;
  for(auto m:c.getOps<FModuleLike>())modules.push_back(dump(m.getOperation()));
  std::string error;require(succeeded(goldengate::prepareFASEDHeader(c,error)),error);
  auto after=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(before.size()==after.size(),"annotation count changed");
  unsigned changed=0;std::string body;
  for(unsigned i=0;i<before.size();++i) {
    if(before[i]==after[i])continue;++changed;auto old=cast<DictionaryAttr>(before[i]),updated=cast<DictionaryAttr>(after[i]);
    body=updated.getAs<StringAttr>("body").getValue().str();require(StringRef(body).starts_with(old.getAs<StringAttr>("body").getValue()),"lost existing header");
    for(auto a:old)if(a.getName()!="body")require(updated.get(a.getName())==a.getValue(),"changed output metadata");
  }
  require(changed==1,"changed unrelated annotations");unsigned i=0;
  for(auto m:c.getOps<FModuleLike>())require(modules[i++]==dump(m.getOperation()),"changed hardware");return body;
}
}
int main(int argc, char **argv) {
  try {
    require(argc==1 || argc==3,"usage: FASEDHeaderTest [boundary.mlir output-directory]");
    MLIRContext ctx;ctx.disableMultithreading();ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root=fixture(ctx);require(succeeded(verify(*root)),"fixture verifier");auto c=*root->getOps<CircuitOp>().begin();auto body=run(c);
    require(StringRef(body).contains("{ std::string(\"writeLatency\"), 2048U }") &&
        StringRef(body).contains("{ std::string(\"brespError\"), 2128U }"),"varied allocation differs");
    require(StringRef(body).contains("  2,\n  args,\n  std::string(\"memory_stats2.csv\"),\n  1L << 36U"),"widget/memory-size ABI differs");
    require(StringRef(body).contains("GET_BRIDGE_CONSTRUCTOR") && !StringRef(body).contains("_struct"),"guard/fixed substruct differs");
    unsigned pairs=0;StringRef rest(body);while(rest.contains("{ std::string(")){rest=rest.drop_front(rest.find("{ std::string(")+1);++pairs;}
    require(pairs==26,"read/write AddressMap counts differ");
    std::string before=dump(*root),error;
    require(failed(goldengate::prepareFASEDHeader(c,error)) && dump(*root)==before,"duplicate header mutated IR");
    for(unsigned bad=1;bad<=40;++bad) {
      auto negative=fixture(ctx,bad);auto nc=*negative->getOps<CircuitOp>().begin();before=dump(*negative);
      require(failed(goldengate::prepareFASEDHeader(nc,error)) && !error.empty() && dump(*negative)==before,
          "invalid boundary accepted or mutated: "+std::to_string(bad));
    }
    llvm::outs()<<"FASED header: native 21-read/5-write bank routes, varied widget/allocation/AXI size; 41 atomic rejections passed\n";
    if(argc==3) {
      auto boundary=parseSourceFile<ModuleOp>(argv[1],&ctx);require(bool(boundary),"boundary parse");auto bc=*boundary->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareMetasimInterfaceHeader(bc,"FireSim",error)),error);body=run(bc);
      require(StringRef(body).contains("{ std::string(\"writeLatency\"), 128U }") &&
          StringRef(body).contains("{ std::string(\"brespError\"), 208U }") &&
          StringRef(body).contains("std::string(\"memory_stats0.csv\"),\n  1L << 35U"),"recorded U250 FASED map differs");
      require(succeeded(goldengate::emitOutputFiles(bc,argv[2],"FireSim-generated",error)),error);
      llvm::outs()<<"Recorded U250 boundary: native FASED constructor emitted\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
