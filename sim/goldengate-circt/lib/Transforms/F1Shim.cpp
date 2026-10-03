// See LICENSE for license details.
// Oracle: F1Shim.scala and AXI4NastiAssigner.toAXI4Slave.
// Invariant: exact U250 FPGATop five-port boundary, one memory channel,
// CPU-managed AXI4 enabled, no QSFP target endpoints. Unsupported schemas
// fail before mutation. All existing module bodies and local targets remain.
// Produces one F1Shim and FPGATop instance, leaf PCIS/control bindings,
// two host-clock synchronous-reset modulo-4096 request ID counters, and
// inactive QSFP outputs. No target-cycle state or buffering is introduced.
// Produces DDR GoldenGateOutputFileAnnotation and U250 XDC circuit paths.
// Circuit identity is retargeted; HostClockSource stays on FPGATop.clock.
#include "goldengate/F1Shim.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
using namespace mlir;
using namespace circt::firrtl;
namespace {
using Contract = std::map<std::string, std::pair<unsigned, bool>>;
// bool is the leaf's input direction after resolving all bundle flips.
bool flatten(Type t, std::string path, bool input, Contract &out) {
  if (auto bundle = dyn_cast<BundleType>(t)) {
    for (auto f : bundle.getElements())
      if (!flatten(f.type, path + "." + f.name.getValue().str(),
                   input != f.isFlip, out)) return false;
    return true;
  }
  auto u = dyn_cast<UIntType>(t);
  return u && u.getWidth() && out.emplace(path, std::make_pair(*u.getWidth(), input)).second;
}
Contract expected(StringRef root) {
  Contract out;
  using Fields = ArrayRef<std::pair<StringRef, unsigned>>;
  bool master = root == "mem_0";
  auto channel = [&](StringRef n, Fields fields) {
    bool request = n == "aw" || n == "w" || n == "ar";
    bool input = request != master;
    auto prefix = root.str() + "." + n.str();
    out[prefix + ".ready"] = {1, !input};
    out[prefix + ".valid"] = {1, input};
    for (auto [f,w] : fields) out[prefix + ".bits." + f.str()] = {w,input};
  };
  if (root == "ctrl") {
    const std::pair<StringRef,unsigned> address[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},{"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
    channel("aw",address); channel("ar",address);
    channel("w",{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}});
    channel("b",{{"resp",2},{"id",12},{"user",1}});
    channel("r",{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}});
  } else {
    const std::pair<StringRef,unsigned> address[]{{"id",16},{"addr",master?34u:64u},{"len",8},{"size",3},{"burst",2},{"lock",1},{"cache",4},{"prot",3},{"qos",4}};
    channel("aw",address); channel("ar",address);
    channel("w",{{"data",master?64u:512u},{"strb",master?8u:64u},{"last",1}});
    channel("b",{{"id",16},{"resp",2}});
    channel("r",{{"id",16},{"data",master?64u:512u},{"resp",2},{"last",1}});
  }
  return out;
}
} // namespace
LogicalResult goldengate::assembleF1Shim(CircuitOp circuit, std::string &error) {
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner;
  for(auto m:circuit.getOps<FModuleLike>()) {
    if(m.getModuleName()=="F1Shim") return reject("F1Shim already exists");
    if(m.getModuleName()=="FPGATop") inner=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(circuit.getName()!="FPGATop"||!inner||!raw||inner.getNumPorts()!=5)
    return reject("F1Shim requires the five-port U250 FPGATop and annotations");
  std::map<std::string,unsigned> indices;
  for(auto [i,p]:llvm::enumerate(inner.getPorts())) {
    auto n=p.name.getValue();
    if(!indices.emplace(n.str(),i).second) return reject("duplicate FPGATop port");
    if(n=="clock") {
      if(!isa<ClockType>(p.type)||p.direction!=Direction::In) return reject("invalid FPGATop clock");
    } else if(n=="reset") {
      auto u=dyn_cast<UIntType>(p.type);
      if(!u||u.getWidth()!=1||p.direction!=Direction::In) return reject("invalid FPGATop reset");
    } else if(n=="ctrl"||n=="mem_0"||n=="cpu_managed_axi4") {
      Contract actual;
      if(!flatten(p.type,n.str(),p.direction==Direction::In,actual)||actual!=expected(n))
        return reject("unsupported U250 FPGATop AXI4 contract");
    } else return reject("unsupported FPGATop port (QSFP/PCIM/additional memory)");
  }
  for(auto n:{"clock","reset","ctrl","mem_0","cpu_managed_axi4"})
    if(!indices.count(n)) return reject("missing FPGATop port");
  bool used=false; circuit.walk([&](InstanceOp i){used|=i.getModuleName()=="FPGATop";});
  if(used) return reject("FPGATop must be the unique uninstantiated circuit top");
  unsigned sourceCount=0;
  for(auto a:raw) {
    auto d=dyn_cast<DictionaryAttr>(a); auto cls=d?d.getAs<StringAttr>("class"):StringAttr();
    if(!cls) return reject("malformed FPGATop annotation");
    if(cls.getValue()==AnnotationClasses::XDCPaths)
      return reject("U250 XDC circuit paths already exist");
    if(cls.getValue()==AnnotationClasses::OutputFile) {
      auto suffix=d.getAs<StringAttr>("fileSuffix");
      if(suffix&&suffix.getValue()==".defines.vh")
        return reject("DDR defines output already exists");
    }
    if(cls.getValue()==AnnotationClasses::HostClockSource) {
      auto target=d.getAs<StringAttr>("target");
      if(!target||target.getValue()!="~FPGATop|FPGATop>clock") return reject("invalid retained host clock source");
      ++sourceCount;
    }
  }
  if(sourceCount!=1) return reject("F1Shim requires one retained FPGATop host clock source");
  // Every rejection above precedes mutation.
  auto *ctx=circuit.getContext(); OpBuilder b(ctx); auto loc=circuit.getLoc();
  auto u=[&](unsigned w){return UIntType::get(ctx,w);};
  auto type=[&](StringRef n){return inner.getPorts()[indices.at(n.str())].type;};
  // Nasti has region/user and W.id absent from the CPU AXI4 bundle.
  // Build its schema from the validated control Nasti contract, changing only
  // the PCIS widths from CreateNastiParameters(CPUManagedAXI4Key).
  std::function<Type(Type,StringRef)> pcisType=[&](Type t,StringRef n)->Type {
    if(auto xs=dyn_cast<BundleType>(t)) {
      SmallVector<BundleType::BundleElement> es;
      for(auto x:xs.getElements()) es.push_back({x.name,x.isFlip,cast<FIRRTLBaseType>(pcisType(x.type,x.name.getValue()))});
      return BundleType::get(ctx,es);
    }
    return n=="addr"?u(64):n=="id"?u(16):n=="data"?u(512):n=="strb"?u(64):t;
  };
  auto decoupled=BundleType::get(ctx,{{b.getStringAttr("ready"),true,u(1)},
    {b.getStringAttr("valid"),false,u(1)},{b.getStringAttr("bits"),false,u(256)}});
  SmallVector<PortInfo> ports{{b.getStringAttr("clock"),type("clock"),Direction::In},
    {b.getStringAttr("reset"),type("reset"),Direction::In},
    {b.getStringAttr("io_qsfp_channel_up"),FVectorType::get(u(1),2),Direction::In},
    {b.getStringAttr("io_qsfp_tx"),FVectorType::get(decoupled,2),Direction::Out},
    {b.getStringAttr("io_qsfp_rx"),FVectorType::get(decoupled,2),Direction::In},
    {b.getStringAttr("io_master"),type("ctrl"),Direction::In},
    {b.getStringAttr("io_pcis"),pcisType(type("ctrl"),""),Direction::In},
    {b.getStringAttr("io_slave"),FVectorType::get(cast<FIRRTLBaseType>(type("mem_0")),1),Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto shim=b.create<FModuleOp>(loc,b.getStringAttr("F1Shim"),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(shim.getBodyBlock());
  auto top=b.create<InstanceOp>(loc,inner,"top");
  auto arg=[&](unsigned i){return shim.getBodyBlock()->getArgument(i);};
  auto child=[&](StringRef n){return top.getResult(indices.at(n.str()));};
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto k=[&](unsigned w,unsigned v)->Value{return b.create<ConstantOp>(loc,u(w),APInt(w,v));};
  connect(child("clock"),arg(0)); connect(child("reset"),arg(1));
  auto counter=[&](StringRef channel,StringRef name)->Value {
    auto request=f(arg(5),channel);
    Value fire=b.create<AndPrimOp>(loc,f(request,"valid"),f(request,"ready"));
    Value r=b.create<RegResetOp>(loc,u(12),arg(0),arg(1),k(12,0),name).getResult();
    Value sum=b.create<AddPrimOp>(loc,r,k(12,1));
    Value next=b.create<BitsPrimOp>(loc,sum,11,0);
    connect(r,b.create<MuxPrimOp>(loc,fire,next,r));
    return r;
  };
  Value writeID=counter("aw","wCounterValue"),readID=counter("ar","rCounterValue");
  // Forward common leaves with resolved direction; override control IDs at
  // the destination, with no overlapping aggregate/leaf drivers.
  std::function<void(Value,Value,bool,std::string,bool)> bind=
    [&](Value dst,Value src,bool input,std::string path,bool control) {
      if(auto xs=dyn_cast<BundleType>(dst.getType())) {
        for(auto x:xs.getElements()) bind(f(dst,x.name.getValue()),f(src,x.name.getValue()),input!=x.isFlip,path+"."+x.name.getValue().str(),control);
      } else if(control&&(path==".aw.bits.id"||path==".ar.bits.id"))
        connect(dst,path==".aw.bits.id"?writeID:readID);
      else connect(input?dst:src,input?src:dst);
    };
  bind(child("ctrl"),arg(5),true,"",true);
  bind(child("cpu_managed_axi4"),arg(6),true,"",false);
  // SFC DontCare response user bits lower to zero in the recorded RTL.
  for(auto ch:{"b","r"}) connect(f(f(f(arg(6),ch),"bits"),"user"),k(1,0));
  Value memory=b.create<SubindexOp>(loc,arg(7),0);
  b.create<ConnectOp>(loc,memory,child("mem_0"));
  for(unsigned i=0;i<2;++i) {
    Value tx=b.create<SubindexOp>(loc,arg(3),i),rx=b.create<SubindexOp>(loc,arg(4),i);
    connect(f(tx,"valid"),k(1,0)); connect(f(tx,"bits"),k(256,0)); connect(f(rx,"ready"),k(1,0));
  }
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue(); if(v=="~FPGATop") return b.getStringAttr("~F1Shim");
      if(v.consume_front("~FPGATop|")) return b.getStringAttr("~F1Shim|"+v.str());
      return a;
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)) {SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)) {NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  // Scala F1Shim.channelInUse uses FPGATop.dramChannelsRequired. The
  // corresponding CIRCT boundary is the validated io_slave vector length.
  unsigned channels=cast<FVectorType>(ports[7].type).getNumElements();
  std::string defines="// Optionally instantiate additional memory channels if required.\n"
                      "// The first channel (C) is provided by the shell and is not optional.\n";
  for(auto [index,name]:{std::pair<unsigned,StringRef>{1,"A"},{2,"B"},{3,"D"}})
    defines+="`define USE_DDR_CHANNEL_"+name.str()+" "+(index<channels?"1":"0")+"\n";
  auto annotations=cast<ArrayAttr>(retarget(raw));
  SmallVector<Attribute> updated(annotations.begin(),annotations.end());
  updated.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::OutputFile)),
    b.getNamedAttr("body",b.getStringAttr(defines)),
    b.getNamedAttr("fileSuffix",b.getStringAttr(".defines.vh"))}));
  // PlatformShim supplies these from XilinxAlveoU250Config. This pass already
  // validates only that platform's exact interface; other profiles fail above.
  updated.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr(AnnotationClasses::XDCPaths)),
    b.getNamedAttr("preLinkPath",b.getStringAttr("firesim_top")),
    b.getNamedAttr("postLinkPath",b.getStringAttr("firesim_top"))}));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(updated)); circuit.setNameAttr(b.getStringAttr("F1Shim"));
  return success();
}
