// See LICENSE for license details.
// Oracle: AXI4AddressTranslation.scala and FPGATop.scala region allocation.
// Requires the recorded single MainMemory_0 region and 35/64/4 AXI master.
// Maps two addresses into the 34-bit host region; checks valid requests on the
// host clock independently of ready. All other AXI leaves pass through.
// Preserves inner state and pre-translation annotation identities. This is a
// stateless boundary; deinterleaving, buffering and LoadMem arbitration remain
// separate transforms. Reject unsupported allocation profiles before mutation.
#include "goldengate/FASEDAddressTranslation.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDAddressTranslation(CircuitOp circuit,
                                                   std::string &error) {
  constexpr StringLiteral inputName="GGFASEDHostMemoryWrapper";
  constexpr StringLiteral wrapperName="GGFASEDAddressTranslationWrapper";
  constexpr StringLiteral helperName="GGFASEDAddressTranslation";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!=inputName || !raw)
    return reject("FASED translation requires active host memory and annotations");
  FModuleOp inner, engine; unsigned engines=0;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()==wrapperName || m.getModuleName()==helperName)
      return reject("FASED translation module already exists");
    if(m.getModuleName()==inputName) inner=dyn_cast<FModuleOp>(m.getOperation());
    if(auto f=dyn_cast<FModuleOp>(m.getOperation()))
      if(auto k=f->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"))
        if(k.getAs<DictionaryAttr>("axi4Edge")){++engines;engine=f;}
  }
  if(!inner || !engine || engines!=1 || engine.getName()!="GGFASEDTokenEngine")
    return reject("FASED translation requires the single recorded memory region");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto region=key.getAs<StringAttr>("memoryRegionName");
  auto widths=key.getAs<DictionaryAttr>("axi4Widths");
  auto edge=key.getAs<DictionaryAttr>("axi4Edge");
  auto addresses=edge.getAs<ArrayAttr>("address");
  auto has=[&](StringRef n,int v){auto a=widths?widths.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v;};
  if(!region || region.getValue()!="MainMemory_0" || !has("addrBits",35) ||
      !has("dataBits",64) || !has("idBits",4) || !addresses || addresses.size()!=4)
    return reject("FASED translation supports the recorded MainMemory_0 profile");
  const uint64_t bases[]{0x80000000ULL,0x100000000ULL,0x200000000ULL,0x400000000ULL};
  const uint64_t masks[]{0x7fffffffULL,0xffffffffULL,0x1ffffffffULL,0x7fffffffULL};
  uint64_t base=UINT64_MAX,bound=0;
  for(auto [j,a]:llvm::enumerate(addresses)) {
    auto d=dyn_cast<DictionaryAttr>(a);
    auto x=d?d.getAs<IntegerAttr>("base"):IntegerAttr();
    auto y=d?d.getAs<IntegerAttr>("mask"):IntegerAttr();
    if(!x || !y || x.getInt()!=int64_t(bases[j]) || y.getInt()!=int64_t(masks[j]))
      return reject("FASED translation needs the recorded four address sets");
    base=std::min(base,uint64_t(x.getInt()));
    bound=std::max(bound,uint64_t(x.getInt())|uint64_t(y.getInt()));
  }
  // FPGATop places the only region at host base zero. BytesOfDRAMRequired
  // spans its virtual bounds; this recorded allocation occupies exactly 16 GiB.
  constexpr uint64_t hostCapacity=uint64_t(1)<<34;
  if(bound-base+1!=hostCapacity)
    return reject("FASED translation allocation is not the recorded 16 GiB");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  auto master=[&](unsigned addrBits){
    auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
      SmallVector<BundleType::BundleElement> payload;
      for(auto [n,w]:fs)payload.push_back({b.getStringAttr(n),false,uint(w)});
      return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
        {b.getStringAttr("valid"),false,uint(1)},
        {b.getStringAttr("bits"),false,BundleType::get(ctx,payload)}});
    };
    auto address=token({{"id",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},
      {"burst",2},{"size",3},{"len",8},{"addr",addrBits}});
    return BundleType::get(ctx,{{b.getStringAttr("aw"),false,address},
      {b.getStringAttr("w"),false,token({{"strb",8},{"last",1},{"data",64}})},
      {b.getStringAttr("b"),true,token({{"id",4},{"resp",2}})},
      {b.getStringAttr("ar"),false,address},
      {b.getStringAttr("r"),true,token({{"id",4},{"last",1},{"data",64},{"resp",2}})}});
  };
  auto inType=master(35),outType=master(34);
  std::map<std::string,unsigned> old,copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts()))old[p.name.getValue().str()]=i;
  auto port=[&](StringRef n,Type t,Direction d){auto i=old.find(n.str());return i!=old.end()&&inner.getPorts()[i->second].type==t&&inner.getPorts()[i->second].direction==d;};
  if(!port("fased_host_mem",inType,Direction::Out) ||
      !port("hostClock",ClockType::get(ctx),Direction::In) ||
      !port("hostReset",uint(1),Direction::In))
    return reject("FASED translation needs exact memory, host clock and reset ports");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inputName;});
  if(used)return reject("FASED translation requires an uninstantiated top");

  // All failure paths precede creating or changing operations.
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("reset"),uint(1),Direction::In},
    {b.getStringAttr("in"),inType,Direction::In},
    {b.getStringAttr("out"),outType,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  helper->setAttr("goldengate.memoryRegion",b.getDictionaryAttr({
    b.getNamedAttr("name",region),b.getNamedAttr("virtualBase",b.getI64IntegerAttr(base)),
    b.getNamedAttr("virtualBound",b.getI64IntegerAttr(bound)),
    b.getNamedAttr("hostBase",b.getI64IntegerAttr(0)),
    b.getNamedAttr("offset",b.getI64IntegerAttr(-int64_t(base)))}));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto constant=[&](uint64_t v)->Value{return b.create<ConstantOp>(loc,uint(35),APInt(35,v));};
  auto shift=constant(hostCapacity-base),lower=constant(base),upper=constant(bound);
  Value enabled=b.create<NotPrimOp>(loc,arg(1));
  SmallVector<std::pair<Value,Value>> bounds;
  for(auto ch:{"aw","w","b","ar","r"}) {
    bool request=StringRef(ch)=="aw"||StringRef(ch)=="w"||StringRef(ch)=="ar";
    Value in=field(arg(2),ch),out=field(arg(3),ch);
    connect(field(request?out:in,"valid"),field(request?in:out,"valid"));
    connect(field(request?in:out,"ready"),field(request?out:in,"ready"));
    auto bits=cast<BundleType>(cast<BundleType>(in.getType()).getElements()[2].type);
    for(auto f:bits.getElements()) {
      Value x=field(field(in,"bits"),f.name.getValue());
      Value y=field(field(out,"bits"),f.name.getValue());
      if(f.name=="addr") {
        Value sum=b.create<AddPrimOp>(loc,x,shift);
        connect(y,b.create<BitsPrimOp>(loc,sum,33,0));
        Value inactive=b.create<NotPrimOp>(loc,field(in,"valid"));
        bounds.push_back({b.create<OrPrimOp>(loc,inactive,b.create<LEQPrimOp>(loc,x,upper)),
                          b.create<OrPrimOp>(loc,inactive,b.create<GEQPrimOp>(loc,x,lower))});
      } else connect(request?y:x,request?x:y);
    }
  }
  // Preserve the oracle's AW bound, AR bound, AW base, AR base ordering.
  for(unsigned j=0;j<4;++j) {
    std::string msg=std::string(j%2?"AR":"AW")+" request address in memory region "+region.getValue().str()+
      (j<2?" exceeds region bound.":" is less than region base.");
    b.create<AssertOp>(loc,arg(0),j<2?bounds[j].first:bounds[j-2].second,enabled,msg,ValueRange{},"");
  }
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(p.name!="fased_host_mem") {
    copied[p.name.getValue().str()]=ports.size();ports.push_back(p);
  }
  unsigned memoryIndex=ports.size();ports.push_back({b.getStringAttr("fased_host_mem"),outType,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim");
  auto translation=b.create<InstanceOp>(loc,helper,"translation");
  for(auto [n,i]:copied) {
    Value v=sim.getResult(old.at(n)),a=wrapper.getBodyBlock()->getArgument(i);
    bool input=inner.getPorts()[old.at(n)].direction==Direction::In;
    b.create<ConnectOp>(loc,input?v:a,input?a:v);
  }
  connect(translation.getResult(0),wrapper.getBodyBlock()->getArgument(copied.at("hostClock")));
  connect(translation.getResult(1),wrapper.getBodyBlock()->getArgument(copied.at("hostReset")));
  b.create<ConnectOp>(loc,translation.getResult(2),sim.getResult(old.at("fased_host_mem")));
  b.create<ConnectOp>(loc,wrapper.getBodyBlock()->getArgument(memoryIndex),translation.getResult(3));
  std::string op="~"+inputName.str(),np="~"+wrapperName.str(),mp="|"+inputName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);
      if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)&&copied.count(ref.take_front(ref.find_first_of(".[")).str()))
        suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}
