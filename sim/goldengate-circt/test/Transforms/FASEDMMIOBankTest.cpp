// See LICENSE for license details.
#include "goldengate/FASEDMMIOBank.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <cstdint>
#include <limits>
#include <functional>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,llvm::StringRef message){if(!ok)throw std::runtime_error(message.str());}
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
unsigned globalWord(unsigned j,unsigned k,unsigned order) {
  unsigned local=order==1?lengths[j]-1-k:order==2?(3*k+1)%lengths[j]:k;
  return starts[j]+local;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned order=0,bool selected=false,int64_t flight=10) {
  auto root=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGFASEDHistogramsWrapper" {
    firrtl.module @GGFASEDTokenEngine() {} firrtl.module @GGFASEDHistogramsWrapper() {} } })",&ctx);
  require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();
  named(c,"GGFASEDHistogramsWrapper").erase();OpBuilder b(&ctx);auto loc=c.getLoc();
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
      unsigned word=globalWord(j,k,order);
      rows.push_back(b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(names[word])),
          b.getNamedAttr("offset",b.getI32IntegerAttr(4*word)),b.getNamedAttr("readable",b.getBoolAttr(true)),
          b.getNamedAttr("writeable",b.getBoolAttr(word<4 || word==18))}));
    }bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
  }
  b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDHistogramsWrapper"),ConventionAttr::get(&ctx,Convention::Internal),ports);
  named(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
      b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI64IntegerAttr(flight))}))}));
  SmallVector<Attribute> annos;
  auto add=[&](std::string old,std::string expected){annos.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr(old)),
      b.getNamedAttr("expected",b.getStringAttr(expected))}));};
  const std::string old="~GGFASEDHistogramsWrapper",next="~GGFASEDMMIOWrapper";
  add(old,next);
  for(auto n:{"hostClock","hostReset","other"})add(old+"|GGFASEDHistogramsWrapper>"+n,next+"|GGFASEDMMIOWrapper>"+n);
  for(unsigned j=0;j<6;++j) {
    auto source=old+"|GGFASEDHistogramsWrapper>"+fragments[j].str();
    auto destination=next+"|GGFASEDMMIOWrapper>fasedBridge_mcr";
    add(source,next+"|GGFASEDHistogramsWrapper>"+fragments[j].str());
    add(source+".wstrb",destination+".wstrb");
    for(unsigned k=0;k<lengths[j];++k)for(auto g:{"read","write"})
      add(source+"."+g+"["+std::to_string(k)+"].bits",destination+"."+g+"["+std::to_string(globalWord(j,k,order))+"].bits");
  }
  add(old+"|GGFASEDLatencyRegisters>mcr.read[1].bits",next+"|GGFASEDLatencyRegisters>mcr.read[1].bits");
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if(selected) {
    std::function<Attribute(Attribute)> rename=[&](Attribute a)->Attribute {
      if(auto v=dyn_cast<StringAttr>(a)) {
        std::string text=v.getValue().str();
        for(auto name:{"GGFASEDHistogramsWrapper","GGFASEDMMIOWrapper"}) {
          size_t pos=0;while((pos=text.find(name,pos))!=std::string::npos) {
            auto replacement=std::string("GGSelectedPrint")+std::string(name).substr(2);
            text.replace(pos,std::string(name).size(),replacement);pos+=replacement.size();
          }
        }return b.getStringAttr(text);
      }
      if(auto v=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> vs;for(auto x:v)vs.push_back(rename(x));return b.getArrayAttr(vs);}
      if(auto v=dyn_cast<DictionaryAttr>(a)){NamedAttrList vs;for(auto x:v)vs.set(x.getName(),rename(x.getValue()));return vs.getDictionary(&ctx);}
      return a;
    };
    named(c,"GGFASEDHistogramsWrapper").setName("GGSelectedPrintFASEDHistogramsWrapper");
    c->setAttr("rawAnnotations",rename(c->getAttr("rawAnnotations")));
    c.setName("GGSelectedPrintFASEDHistogramsWrapper");
  }
  return root;
}
std::string valueName(Value v,FModuleOp top) {
  if(auto f=v.getDefiningOp<SubfieldOp>())return valueName(f.getInput(),top)+"."+f.getFieldName().str();
  if(auto f=v.getDefiningOp<SubindexOp>())return valueName(f.getInput(),top)+"["+std::to_string(f.getIndex())+"]";
  if(auto arg=dyn_cast<BlockArgument>(v))return top.getPortName(arg.getArgNumber()).str();
  auto inst=v.getDefiningOp<InstanceOp>();require(bool(inst),"unexpected wiring value");
  return inst.getName().str()+"."+cast<StringAttr>(inst.getPortNames()[cast<OpResult>(v).getResultNumber()]).getValue().str();
}
void mapping(MLIRContext &ctx,unsigned order,bool selected,int64_t flight) {
  auto root=fixture(ctx,order,selected,flight);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDMMIOBank(c,error)),error);
  require(succeeded(verify(*root)),"assembled bank failed verifier");
  auto top=named(c,selected?"GGSelectedPrintFASEDMMIOWrapper":"GGFASEDMMIOWrapper");require(top.getNumPorts()==4,"fragments were not consumed");
  require(top.getPortName(3)=="fasedBridge_mcr" && top.getPortDirection(3)==Direction::Out,"wrong aggregate MCR port");
  auto type=cast<BundleType>(top.getPortType(3));
  require(cast<FVectorType>(type.getElement("read")->type).getNumElements()==21,"wrong aggregate lane count");
  std::map<std::string,std::string> actual,expected;
  for(auto &op:top.getBodyBlock()->getOperations()) {
    if(auto connect=dyn_cast<ConnectOp>(&op))require(actual.emplace(valueName(connect.getDest(),top),valueName(connect.getSrc(),top)).second,"duplicate connect");
    if(auto connect=dyn_cast<StrictConnectOp>(&op))require(actual.emplace(valueName(connect.getDest(),top),valueName(connect.getSrc(),top)).second,"duplicate strict connect");
    require(!isa<RegOp,RegResetOp>(&op),"bank duplicated register state");
  }
  expected["sim.hostClock"]="hostClock";expected["sim.hostReset"]="hostReset";expected["other"]="sim.other";
  for(unsigned j=0;j<6;++j) {
    std::string frag="sim."+fragments[j].str();expected[frag+".wstrb"]="fasedBridge_mcr.wstrb";
    for(unsigned k=0;k<lengths[j];++k) {
      expected["fasedBridge_mcr.read["+std::to_string(globalWord(j,k,order))+"]"]=frag+".read["+std::to_string(k)+"]";
      expected[frag+".write["+std::to_string(k)+"]"]="fasedBridge_mcr.write["+std::to_string(globalWord(j,k,order))+"]";
    }
  }require(actual==expected,"lane or strobe routing differs");
  auto rows=top->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");require(rows.size()==21,"wrong register map length");
  for(unsigned i=0;i<21;++i){auto row=cast<DictionaryAttr>(rows[i]);require(row.getAs<StringAttr>("name").getValue()==names[i] &&
      row.getAs<IntegerAttr>("offset").getInt()==4*i && row.getAs<BoolAttr>("readable").getValue() &&
      row.getAs<BoolAttr>("writeable").getValue()==(i<4 || i==18),"register map mismatch");}
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(annos.size()==59,"annotation count changed");
  for(auto a:annos){auto row=cast<DictionaryAttr>(a);require(row.getAs<StringAttr>("target")==row.getAs<StringAttr>("expected"),"lane annotation target transfer failed");}
}
void rejection(MLIRContext &ctx,bool selected) {
  for(unsigned mode=0;mode<99;++mode) {
    auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
    auto top=named(c,selected?"GGSelectedPrintFASEDHistogramsWrapper":"GGFASEDHistogramsWrapper"),engine=named(c,"GGFASEDTokenEngine");
    if(mode==0)c.setName("WrongTop");if(mode==1)c->removeAttr("rawAnnotations");
    if(mode==2)engine->removeAttr("goldengate.bridgeConstructor");
    if(mode==3)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI32IntegerAttr(0))}))}));
    if(mode>=4 && mode<10){auto ports=llvm::to_vector(top.getPortNames());ports[3+mode-4]=b.getStringAttr("missing");top.setPortNames(ports);}
    if(mode>=10 && mode<16){SmallVector<bool> dirs;for(auto p:top.getPorts())dirs.push_back(p.direction==Direction::Out);dirs[3+mode-10]=false;top.setPortDirections(dirs);}
    if(mode>=16 && mode<22){unsigned i=3+mode-16;auto type=UIntType::get(&ctx,32,false);top.getArgument(i).setType(type);
      auto types=llvm::to_vector(top.getPortTypes());types[i]=TypeAttr::get(type);top.setPortTypes(types);}
    if(mode>=22 && mode<28)named(c,modules[mode-22]).erase();
    if(mode>=28 && mode<34)named(c,modules[mode-28])->removeAttr("goldengate.mmioRegisters");
    if(mode>=34 && mode<64) {
      unsigned group=(mode-34)/6,j=(mode-34)%6;auto bank=named(c,modules[j]);
      auto rows=llvm::to_vector(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
      if(group==0)rows.pop_back();
      else {NamedAttrList row(cast<DictionaryAttr>(rows[0]));
        if(group==1)row.set("name",b.getStringAttr("wrong"));
        if(group==2)row.set("offset",b.getI32IntegerAttr(4*starts[j]+4));
        if(group==3)row.set("readable",b.getBoolAttr(false));
        if(group==4)row.set("writeable",b.getBoolAttr(!(starts[j]<4 || starts[j]==18)));
        rows[0]=row.getDictionary(&ctx);
      }bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
    }
    if(mode==64){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(selected?"GGSelectedPrintFASEDMMIOWrapper":"GGFASEDMMIOWrapper"),
        ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
    if(mode==65){b.setInsertionPointToEnd(engine.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"usedTop");}
    if(mode==66){auto ports=llvm::to_vector(top.getPortNames());ports[2]=b.getStringAttr("fasedBridge_mcr");top.setPortNames(ports);}
    if(mode>=67 && mode<85) {
      unsigned group=(mode-67)/6,j=(mode-67)%6;auto bank=named(c,modules[j]);
      if(group==0)bank.setPortNames(ArrayRef<Attribute>{b.getStringAttr("wrong")});
      if(group==1)bank.setPortDirections(SmallVector<bool>{false});
      if(group==2){auto type=UIntType::get(&ctx,32,false);bank.getArgument(0).setType(type);
        bank.setPortTypes(ArrayRef<Attribute>{TypeAttr::get(type)});}
    }
    if(mode>=85 && mode<93) {
      auto bank=named(c,modules[0]);auto rows=llvm::to_vector(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
      NamedAttrList row(cast<DictionaryAttr>(rows[0]));
      if(mode==85)row.set("offset",b.getI32IntegerAttr(-4));
      if(mode==86)row.set("offset",b.getI32IntegerAttr(1));
      if(mode==87)row.set("offset",b.getI32IntegerAttr(84));
      if(mode==88)row.set("offset",b.getIntegerAttr(b.getIntegerType(128),0));
      if(mode==89)row.set("name",b.getStringAttr(names[1]));
      if(mode==90)row.erase("writeable");
      if(mode==91)row.set("readable",b.getStringAttr("true"));
      rows[0]=mode==92?Attribute(b.getStringAttr("invalid row")):Attribute(row.getDictionary(&ctx));
      bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
    }
    if(mode==93)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getIntegerAttr(b.getIntegerType(128),APInt(128,10)))}))}));
    if(mode==94)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI64IntegerAttr(-1))}))}));
    if(mode==95)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getStringAttr("20"))}))}));
    if(mode==96)engine.erase();
    if(mode==97)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({}));
    if(mode==98)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",b.getStringAttr("invalid"))}));
    std::string before,after;llvm::raw_string_ostream original(before);root->print(original);original.flush();std::string error;
    require(failed(goldengate::addFASEDMMIOBank(c,error)) && !error.empty(),"invalid fragment accepted");
    llvm::raw_string_ostream updated(after);root->print(updated);updated.flush();require(before==after,"rejected bank mutated IR");
  }
}
}
// Isolate the actual production routing operations, preserving bundle flips.
// Replace only the inner simulation's six fragment ports with probe inputs;
// no register-bank state or MMIO transport is reconstructed by this exporter.
void exportProduction(MLIRContext &ctx,const char *input,const char *output) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production MMIO boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  require(c.getName()=="GGFASEDMMIOWrapper","wrong production MMIO root");
  auto top=named(c,c.getName()),inner=named(c,"GGFASEDHistogramsWrapper");
  InstanceOp sim;
  for(auto i:top.getOps<InstanceOp>()) {require(!sim,"unexpected second MMIO instance");sim=i;}
  require(sim&&sim.getModuleName()==inner.getName(),"wrong production MMIO instance");
  SmallVector<PortInfo> ports;SmallVector<Value> sources;OpBuilder b(&ctx);
  for(auto name:fragments) {
    bool found=false;
    for(auto [i,p]:llvm::enumerate(inner.getPorts()))if(p.name==name) {
      require(p.direction==Direction::Out,"wrong fragment direction");
      ports.push_back({p.name,p.type,Direction::In});sources.push_back(sim.getResult(i));found=true;
    }
    require(found,"missing fragment");
  }
  for(auto [i,p]:llvm::enumerate(top.getPorts()))if(p.name=="fasedBridge_mcr") {
    require(p.direction==Direction::Out,"wrong aggregate direction");
    ports.push_back(p);sources.push_back(top.getArgument(i));
  }
  require(ports.size()==7,"missing aggregate");
  auto isolated=ModuleOp::create(c.getLoc());b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDMMIOProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDMMIOProbe"),top.getConventionAttr(),ports);
  probe.setPublic();b.setInsertionPointToEnd(probe.getBodyBlock());IRMapping map;
  for(unsigned i=0;i<7;++i)map.map(sources[i],probe.getArgument(i));
  unsigned connections=0;
  for(auto &op:*top.getBodyBlock()) {
    if(isa<InstanceOp>(op))continue;
    require(isa<SubfieldOp,SubindexOp,ConnectOp,StrictConnectOp>(op),"unexpected MMIO operation");
    if(!llvm::all_of(op.getOperands(),[&](Value v){return map.contains(v);}))continue;
    b.clone(op,map);connections+=isa<ConnectOp,StrictConnectOp>(op);
  }
  require(connections==48&&succeeded(verify(isolated)),"invalid production MMIO probe routing");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export MMIO probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned 48 production fragment/aggregate routes with original bundle flips\n";
}
int main(int argc,char **argv){try{MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  require(argc==1||argc==3,"usage: goldengate-fased-mmio-bank-test [boundary.mlir probe.mlir]");
  if(argc==3){exportProduction(ctx,argv[1],argv[2]);return 0;}
  for(bool selected:{false,true}) {
    for(int64_t flight:{int64_t(1),int64_t(9),int64_t(10),int64_t(16),int64_t(20),std::numeric_limits<int64_t>::max()})
      for(unsigned order=0;order<3;++order)mapping(ctx,order,selected,flight);
    rejection(ctx,selected);
  }
  llvm::outs()<<"FASED MMIO bank: legacy/selected profiles, six capacities and three lane orders each, 21 lanes, six strobe routes, 59 annotation targets and 198 atomic rejections passed\n";return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}}
