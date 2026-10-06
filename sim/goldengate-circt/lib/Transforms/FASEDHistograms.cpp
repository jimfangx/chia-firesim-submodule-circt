// See LICENSE for license details.
// Oracle: TimingModel.scala:169-192; FASED MCR words 4-13 in the recorded SFC RTL.
// Materialization requires a free bank symbol and preserves top/annotations.
// Attachment requires an uninstantiated statistics wrapper, ten-flight model,
// exact target reset/fire and pendingReads/pendingAW observation bundles.
// Consumes/produces annotations: none; copied top targets explicitly transfer.
// Mutations: eight UInt<32> histogram registers, ten read-only MCR lanes,
// wrapper observing pre-edge pending counts; existing drivers are preserved.
// Analyses required/preserved: retained constructor and port identity; no cache.
// Output: first matching upper bound 0,2,4,8 increments on targetFire; wraps
// at 32 bits. Model reset is synchronous and qualified by targetFire. The
// allocated fifth bin remains zero, preserving the executable Scala zip rule
// and optimized SFC RTL. Read-only assertions are suppressed by host reset.
// Scope: recorded BaseParams histogram profile, not configurable histograms.
#include "goldengate/FASEDHistograms.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

namespace {
constexpr llvm::StringLiteral bankName="GGFASEDHistograms";
constexpr llvm::StringLiteral wrapperName="GGFASEDHistogramsWrapper";
constexpr llvm::StringLiteral controlName="fased_histograms_mcr";
SmallVector<PortInfo> histogramPorts(MLIRContext *ctx) {
  OpBuilder b(ctx);
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};auto bit=uint(1);
  auto token=BundleType::get(ctx,{{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(32)}});
  auto words=FVectorType::get(token,10);
  auto mcr=BundleType::get(ctx,{{b.getStringAttr("read"),false,words},
      {b.getStringAttr("write"),true,words},{b.getStringAttr("wstrb"),true,uint(4)}});
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In}};
  for(auto name:{"hostReset","modelReset","targetFire"})
    bankPorts.push_back({b.getStringAttr(name),bit,Direction::In});
  for(auto name:{"pendingReads","pendingAW"})
    bankPorts.push_back({b.getStringAttr(name),uint(4),Direction::In});
  bankPorts.push_back({b.getStringAttr("mcr"),mcr,Direction::Out});
  return bankPorts;
}
LogicalResult attachmentBoundary(CircuitOp circuit, FModuleOp &inner,
    unsigned (&indices)[6], std::string &error) {
  auto reject=[&](llvm::StringRef why){error=why.str();return failure();};
  if(circuit.getName()!="GGFASEDStatisticsWrapper")
    return reject("FASED histograms require the active statistics wrapper");
  FModuleOp engine;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getName()==wrapperName)
      return reject("FASED histogram wrapper already exists");
    if(m.getName()==circuit.getName())inner=dyn_cast<FModuleOp>(m.getOperation());
    if(m.getName()=="GGFASEDTokenEngine")engine=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!inner || !raw)return reject("FASED histograms need a top and retained annotations");
  auto key=engine?engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"):DictionaryAttr();
  auto edge=key?key.getAs<DictionaryAttr>("axi4Edge"):DictionaryAttr();
  auto flight=edge?edge.getAs<IntegerAttr>("maxFlight"):IntegerAttr();
  if(!flight || flight.getInt()!=10)
    return reject("FASED histograms currently require the recorded ten-flight constructor");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};auto bit=uint(1);
  auto reads=BundleType::get(ctx,{{b.getStringAttr("value"),false,uint(4)},
      {b.getStringAttr("full"),false,bit}});
  auto writes=BundleType::get(ctx,{{b.getStringAttr("awValue"),false,uint(4)},
      {b.getStringAttr("wValue"),false,uint(4)},{b.getStringAttr("awFull"),false,bit},
      {b.getStringAttr("wFull"),false,bit}});
  const llvm::StringRef names[]{"hostClock","hostReset","fased_model_reset","fased_tfire",
      "fased_pending_reads","fased_pending_writes"};
  const Type types[]{ClockType::get(ctx),bit,bit,bit,reads,writes};
  const Direction directions[]{Direction::In,Direction::In,Direction::Out,
      Direction::Out,Direction::Out,Direction::Out};
  for(unsigned j=0;j<6;++j) {
    std::optional<unsigned> index;
    for(auto [i,p]:llvm::enumerate(inner.getPorts()))
      if(p.name==names[j] && p.type==types[j] && p.direction==directions[j])index=i;
    if(!index)return reject("FASED histograms need exact clock/reset/fire and pending read/write ports");
    indices[j]=*index;
  }
  for(auto p:inner.getPorts())if(p.name==controlName)
    return reject("FASED histogram MCR boundary already exists");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used)return reject("FASED histograms need an uninstantiated top");

  return success();
}
} // namespace

LogicalResult goldengate::materializeFASEDHistograms(CircuitOp circuit,
    FModuleOp &result, std::string &error) {
  for (auto m : circuit.getOps<FModuleLike>()) if (m.getName() == bankName) {
    error = "FASED histogram module already exists"; return failure();
  }
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  // Local lanes 0-9 retain global words 4-13 (16-52 bytes).
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank=b.create<FModuleOp>(loc,b.getStringAttr(bankName),
      ConventionAttr::get(ctx,Convention::Internal),histogramPorts(ctx));
  SmallVector<Attribute> registers;
  for(unsigned i=0;i<10;++i)registers.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr(std::string(i<5?"writeOutstandingHistogram_":"readOutstandingHistogram_")+std::to_string(i%5))),
      b.getNamedAttr("offset",b.getI32IntegerAttr(16+4*i)),
      b.getNamedAttr("readable",b.getBoolAttr(true)),b.getNamedAttr("writeable",b.getBoolAttr(false))}));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(registers));
  bank->setAttr("goldengate.histogramUpperBounds",b.getI32ArrayAttr({0,2,4,8}));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg=[&](unsigned i){return bank.getArgument(i);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto slot=[&](llvm::StringRef group,unsigned i)->Value{return b.create<SubindexOp>(loc,field(arg(6),group),i);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto both=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto constant=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  Value one=constant(1,1),zero=constant(32,0),increment=constant(32,1);
  Value enabled=b.create<NotPrimOp>(loc,arg(1)),reset=both(arg(2),arg(3));
  const unsigned upperBounds[]{0,2,4,8};
  for(unsigned group=0;group<2;++group) {
    Value count=arg(group?4:5),matched=constant(1,0);
    for(unsigned bin=0;bin<5;++bin) {
      unsigned index=group*5+bin;
      std::string name=std::string(group?"readOutstandingHistogram_":"writeOutstandingHistogram_")+std::to_string(bin);
      Value value=zero;
      if(bin<4) {
        value=b.create<RegResetOp>(loc,uint(32),arg(0),reset,zero,name).getResult();
        Value fits=b.create<LEQPrimOp>(loc,count,constant(4,upperBounds[bin]));
        Value selected=both(b.create<NotPrimOp>(loc,matched),fits);
        Value next=b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,value,increment),31,0);
        connect(value,b.create<MuxPrimOp>(loc,both(arg(3),selected),next,value));
        matched=b.create<OrPrimOp>(loc,matched,fits);
      }
      Value read=slot("read",index),write=slot("write",index);
      connect(field(read,"bits"),value);connect(field(read,"valid"),one);
      connect(field(write,"ready"),one);
      b.create<AssertOp>(loc,arg(0),b.create<NotPrimOp>(loc,field(write,"valid")),enabled,
          "Register "+name+" is read only",ValueRange{},"");
    }
  }

  result = bank;
  return success();
}

LogicalResult goldengate::attachFASEDHistograms(CircuitOp circuit,
    FModuleOp bank, std::string &error) {
  FModuleOp inner; unsigned indices[6];
  if (failed(attachmentBoundary(circuit, inner, indices, error))) return failure();
  auto reject=[&](llvm::StringRef why){error=why.str();return failure();};
  if (!bank || bank->getParentOp()!=circuit.getOperation() || bank.getName()!=bankName)
    return reject("FASED histogram attachment requires its materialized bank in this circuit");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto expected=histogramPorts(ctx); auto actual=bank.getPorts();
  if (actual.size()!=expected.size()) return reject("FASED histogram bank needs exactly seven ports");
  for (auto [i,p]:llvm::enumerate(actual))
    if (p.name!=expected[i].name || p.type!=expected[i].type || p.direction!=expected[i].direction)
      return reject("FASED histogram bank port identity, type or direction differs");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==bankName;});
  if (used) return reject("FASED histogram bank is already instantiated");
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto mcr=expected.back().type;
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    copied.push_back(i); ports.push_back(port);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "histograms");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto port = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
                             port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0),outer(indices[0]));
  connect(mmio.getResult(1),outer(indices[1]));
  connect(mmio.getResult(2),sim.getResult(indices[2]));
  connect(mmio.getResult(3),sim.getResult(indices[3]));
  connect(mmio.getResult(4),field(sim.getResult(indices[4]),"value"));
  connect(mmio.getResult(5),field(sim.getResult(indices[5]),"awValue"));
  b.create<ConnectOp>(loc,wrapper.getArguments().back(),mmio.getResult(6));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto local = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (local == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> vs; for (auto v : arr) vs.push_back(retarget(v)); return b.getArrayAttr(vs); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList vs; for (auto v : d) vs.set(v.getName(), retarget(v.getValue())); return vs.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(wrapperName);
  return success();
}

LogicalResult goldengate::addFASEDHistograms(CircuitOp circuit, std::string &error) {
  FModuleOp inner, bank; unsigned indices[6];
  if (failed(attachmentBoundary(circuit, inner, indices, error)) ||
      failed(materializeFASEDHistograms(circuit, bank, error))) return failure();
  return attachFASEDHistograms(circuit, bank, error);
}
