// See LICENSE for license details.
// Oracle: FASEDMemoryTimingModel.attachIO/attach and Widget.genCRFile.
// Requires: uninstantiated histogram wrapper and the six exact recorded MCR
// fragment types/maps. All checks precede mutation; no state is duplicated.
// Mutations: join Decoupled reads/writes and fan out wstrb in a new wrapper.
// Output: 21 ordered 32-bit MCR words, with ready flowing opposite valid/bits.
// Annotations: copied ports transfer; indexed fragment lanes transfer to their
// global lane. Whole-fragment references stay on the retained inner module.
// Annotations consumed/produced: none; retained constructor is inspected.
// Analyses required: fragment port/map identity. No cached analysis is used;
// existing model/channel/clock identities and module bodies are preserved.
// Scope: recorded ten-flight/default histogram profile; transport is separate.
#include "goldengate/FASEDMMIOBank.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDMMIOBank(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName="GGFASEDMMIOWrapper";
  constexpr llvm::StringLiteral controlName="fasedBridge_mcr";
  auto reject=[&](llvm::StringRef why){error=why.str();return failure();};
  if(circuit.getName()!="GGFASEDHistogramsWrapper")
    return reject("FASED MMIO bank requires the active histogram wrapper");
  const llvm::StringRef modules[]{"GGFASEDLatencyRegisters","GGFASEDRequestLimits",
      "GGFASEDHistograms","GGFASEDStatistics","GGFASEDFunctionalModelRegister","GGFASEDResponseErrors"};
  const llvm::StringRef fragments[]{"fased_latency_mcr","fased_request_limits_mcr",
      "fased_histograms_mcr","fased_statistics_mcr","fased_functional_model_mcr","fased_response_errors_mcr"};
  const unsigned starts[]{0,2,4,14,18,19}, lengths[]{2,2,10,4,1,2};
  const llvm::StringRef names[]{"writeLatency","readLatency","writeMaxReqs","readMaxReqs",
      "writeOutstandingHistogram_0","writeOutstandingHistogram_1","writeOutstandingHistogram_2",
      "writeOutstandingHistogram_3","writeOutstandingHistogram_4","readOutstandingHistogram_0",
      "readOutstandingHistogram_1","readOutstandingHistogram_2","readOutstandingHistogram_3",
      "readOutstandingHistogram_4","totalWriteBeats","totalReadBeats","totalWrites","totalReads",
      "relaxFunctionalModel","rrespError","brespError"};
  FModuleOp inner,engine; FModuleOp banks[6];
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getName()==wrapperName)return reject("FASED MMIO wrapper already exists");
    if(m.getName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getName()=="GGFASEDTokenEngine")engine=dyn_cast<FModuleOp>(m.getOperation());
    for(unsigned j=0;j<6;++j)if(m.getName()==modules[j])banks[j]=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!inner || !raw)return reject("FASED MMIO bank needs a top and retained annotations");
  auto key=engine?engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"):DictionaryAttr();
  auto edge=key?key.getAs<DictionaryAttr>("axi4Edge"):DictionaryAttr();
  auto flight=edge?edge.getAs<IntegerAttr>("maxFlight"):IntegerAttr();
  if(!flight || flight.getInt()!=10)return reject("FASED MMIO bank requires the recorded ten-flight constructor");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};auto bit=uint(1);
  auto token=BundleType::get(ctx,{{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(32)}});
  auto mcrType=[&](unsigned size){auto words=FVectorType::get(token,size);
    return BundleType::get(ctx,{{b.getStringAttr("read"),false,words},
      {b.getStringAttr("write"),true,words},{b.getStringAttr("wstrb"),true,uint(4)}});};
  unsigned indices[6];SmallVector<Attribute> registers;
  for(unsigned j=0;j<6;++j) {
    std::optional<unsigned> index;
    for(auto [i,p]:llvm::enumerate(inner.getPorts())) {
      if(p.name==controlName)return reject("FASED aggregate MCR port already exists");
      if(p.name==fragments[j] && p.direction==Direction::Out && p.type==mcrType(lengths[j]))index=i;
    }
    if(!index || !banks[j])return reject("FASED MMIO bank needs six exact register fragments");
    indices[j]=*index;
    auto rows=banks[j]->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    if(!rows || rows.size()!=lengths[j])return reject("FASED fragment MMIO map is missing or has the wrong size");
    for(unsigned k=0;k<lengths[j];++k) {
      auto row=dyn_cast<DictionaryAttr>(rows[k]);unsigned word=starts[j]+k;
      auto name=row?row.getAs<StringAttr>("name"):StringAttr();
      auto offset=row?row.getAs<IntegerAttr>("offset"):IntegerAttr();
      auto read=row?row.getAs<BoolAttr>("readable"):BoolAttr();
      auto write=row?row.getAs<BoolAttr>("writeable"):BoolAttr();
      if(!name || name.getValue()!=names[word] || !offset || offset.getInt()!=4*word ||
          !read || !read.getValue() || !write || write.getValue()!=(word<4 || word==18))
        return reject("FASED fragment MMIO map differs from the recorded 21-word layout");
      registers.push_back(row);
    }
  }
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used)return reject("FASED MMIO bank needs an uninstantiated top");

  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))
    if(!llvm::is_contained(indices,i)){copied.push_back(i);ports.push_back(p);}
  ports.push_back({b.getStringAttr(controlName),mcrType(21),Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  wrapper->setAttr("goldengate.mmioRegisters",b.getArrayAttr(registers));
  b.setInsertionPointToStart(wrapper.getBodyBlock());auto sim=b.create<InstanceOp>(loc,inner,"sim");
  for(auto [j,i]:llvm::enumerate(copied)) {
    auto p=inner.getPorts()[i];Value external=wrapper.getArgument(j);
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):external,
        p.direction==Direction::In?external:sim.getResult(i));
  }
  auto field=[&](Value v,llvm::StringRef name)->Value{return b.create<SubfieldOp>(loc,v,name);};
  auto lane=[&](Value v,llvm::StringRef group,unsigned i)->Value{
    return b.create<SubindexOp>(loc,field(v,group),i);};
  Value aggregate=wrapper.getArguments().back();
  for(unsigned j=0;j<6;++j) {
    Value fragment=sim.getResult(indices[j]);
    b.create<StrictConnectOp>(loc,field(fragment,"wstrb"),field(aggregate,"wstrb"));
    for(unsigned k=0;k<lengths[j];++k) {
      b.create<ConnectOp>(loc,lane(aggregate,"read",starts[j]+k),lane(fragment,"read",k));
      b.create<ConnectOp>(loc,lane(fragment,"write",k),lane(aggregate,"write",starts[j]+k));
    }
  }
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str();
  std::string modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);
      if(!v.consume_front(oldPrefix+"|"))return a;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto local=ref.take_front(ref.find_first_of(".["));
        for(auto i:copied)if(local==inner.getPortName(i))
          return b.getStringAttr(newPrefix+"|"+wrapperName.str()+">"+ref.str());
        for(unsigned j=0;j<6;++j)if(local==fragments[j]) {
          auto tail=ref.drop_front(local.size());
          if(tail==".wstrb")return b.getStringAttr(newPrefix+"|"+wrapperName.str()+">"+controlName.str()+tail.str());
          for(auto group:{llvm::StringRef("read"),llvm::StringRef("write")}) {
            auto remaining=tail;if(!remaining.consume_front("."+group.str()+"["))continue;
            unsigned n;auto end=remaining.find(']');
            if(end==llvm::StringRef::npos || remaining.take_front(end).getAsInteger(10,n) || n>=lengths[j])continue;
            return b.getStringAttr(newPrefix+"|"+wrapperName.str()+">"+controlName.str()+"."+group.str()+"["+
                std::to_string(starts[j]+n)+"]"+remaining.drop_front(end+1).str());
          }
        }
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto arr=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> vs;for(auto v:arr)vs.push_back(retarget(v));return b.getArrayAttr(vs);}
    if(auto d=dyn_cast<DictionaryAttr>(a)){NamedAttrList vs;for(auto v:d)vs.set(v.getName(),retarget(v.getValue()));return vs.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setName(wrapperName);return success();
}
