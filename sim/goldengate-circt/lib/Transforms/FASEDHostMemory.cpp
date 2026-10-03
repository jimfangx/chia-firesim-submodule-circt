// See LICENSE for license details.
// Oracle: FASEDMemoryTimingModel.scala nastiToHostDRAM and
// junctions/nasti.scala AXI4NastiAssigner.toAXI4Slave.
// Requires: active control master, retained constructor/target metadata and
// exact recorded Rocket 35/64/4-bit NASTI request and response boundaries.
// Consumes: three split host bundles; creates one stateless AXI master wrapper.
// Preserves: all inner state, clocks, response errors, accepted transactions,
// and annotation classes. Mapped leaf targets follow the assembled boundary;
// discarded NASTI metadata and whole split-bundle targets remain on inner sim.
// Output: pre-translation AXI memory master. DRAM translation, deinterleaving,
// width adaptation and LoadMem arbitration are separate subsequent transforms.
#include "goldengate/FASEDHostMemory.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::bindFASEDHostMemory(CircuitOp circuit,
                                             std::string &error) {
  constexpr llvm::StringLiteral inputName = "GGControlMasterWrapper";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDHostMemoryWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != inputName)
    return reject("FASED host memory requires the active control master");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName)
      return reject("FASED host memory wrapper already exists");
    if (m.getModuleName() == inputName) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw)
    return reject("FASED host memory needs top, token engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto has = [&](llvm::StringRef n, int v) {
    auto a = widths ? widths.getAs<IntegerAttr>(n) : IntegerAttr();
    return a && a.getInt() == v;
  };
  if (!has("addrBits",35) || !has("dataBits",64) || !has("idBits",4))
    return reject("FASED host memory supports the recorded 35/64/4-bit profile");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx,w,false); };
  struct Field { const char *name; unsigned width; };
  const Field address[]{{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},
    {"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}};
  const Field data[]{{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}};
  const Field read[]{{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}};
  const Field write[]{{"user",1},{"id",4},{"resp",2}};
  auto payload = [&](ArrayRef<Field> fs, bool axi) {
    SmallVector<BundleType::BundleElement> fields;
    for (auto f : fs) {
      if (axi && (StringRef(f.name)=="user" || StringRef(f.name)=="region" ||
                  (fs.data()==data && StringRef(f.name)=="id"))) continue;
      fields.push_back({b.getStringAttr(f.name),false,uint(f.width)});
    }
    return BundleType::get(ctx,fields);
  };
  auto token = [&](ArrayRef<Field> fs, bool axi) {
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},
      {b.getStringAttr("bits"),false,payload(fs,axi)}});
  };
  auto requests = BundleType::get(ctx,{{b.getStringAttr("aw"),false,token(address,false)},
    {b.getStringAttr("w"),false,token(data,false)},
    {b.getStringAttr("ar"),false,token(address,false)}});
  auto master = BundleType::get(ctx,{{b.getStringAttr("aw"),false,token(address,true)},
    {b.getStringAttr("w"),false,token(data,true)},
    {b.getStringAttr("b"),true,token(write,true)},
    {b.getStringAttr("ar"),false,token(address,true)},
    {b.getStringAttr("r"),true,token(read,true)}});
  const StringRef names[]{"fased_host_requests","fased_host_read_response","fased_host_write_response"};
  const Type types[]{requests,token(read,false),token(write,false)};
  std::map<std::string,unsigned> old, copied; std::set<std::string> consumed;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) old[p.name.getValue().str()] = i;
  if (old.count("fased_host_mem")) return reject("FASED host memory boundary exists");
  for (unsigned j=0;j<3;++j) {
    auto i=old.find(names[j].str());
    if (i==old.end() || inner.getPorts()[i->second].type!=types[j] ||
        inner.getPorts()[i->second].direction!=(j==0?Direction::Out:Direction::In))
      return reject("FASED host memory needs exact split request/response bundles");
    consumed.insert(names[j].str());
  }
  bool used=false; circuit.walk([&](InstanceOp i){used |= i.getModuleName()==inputName;});
  if (used) return reject("FASED host memory requires an uninstantiated top");
  // Every failure above leaves the circuit untouched.
  SmallVector<PortInfo> ports;
  for (auto p : inner.getPorts()) if (!consumed.count(p.name.getValue().str())) {
    copied[p.name.getValue().str()] = ports.size(); ports.push_back(p);
  }
  unsigned memoryIndex=ports.size();
  ports.push_back({b.getStringAttr("fased_host_mem"),master,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim");
  for (auto [n,i] : copied) {
    Value v=sim.getResult(old.at(n)), a=wrapper.getBodyBlock()->getArgument(i);
    auto d=inner.getPorts()[old.at(n)].direction;
    b.create<ConnectOp>(loc,d==Direction::In?v:a,d==Direction::In?a:v);
  }
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  Value mem=wrapper.getBodyBlock()->getArgument(memoryIndex);
  std::map<std::string,std::string> mapped;
  for (auto ch : {"aw","w","ar","r","b"}) {
    bool request=StringRef(ch)=="aw" || StringRef(ch)=="w" || StringRef(ch)=="ar";
    std::string boundary=request?names[0].str():StringRef(ch)=="r"?names[1].str():names[2].str();
    Value v=sim.getResult(old.at(boundary)); if(request) v=field(v,ch);
    Value a=field(mem,ch);
    auto connect=[&](StringRef leaf,Value inside,Value outside,bool outward) {
      b.create<StrictConnectOp>(loc,outward?outside:inside,outward?inside:outside);
      mapped[boundary+(request?"."+std::string(ch):"")+"."+leaf.str()]=
          "fased_host_mem."+std::string(ch)+"."+leaf.str();
    };
    connect("valid",field(v,"valid"),field(a,"valid"),request);
    connect("ready",field(v,"ready"),field(a,"ready"),!request);
    auto fs=StringRef(ch)=="w"?ArrayRef<Field>(data):StringRef(ch)=="r"?ArrayRef<Field>(read):
      StringRef(ch)=="b"?ArrayRef<Field>(write):ArrayRef<Field>(address);
    for (auto f : fs) {
      if (StringRef(f.name)=="user" || StringRef(f.name)=="region" ||
          (StringRef(ch)=="w" && StringRef(f.name)=="id")) continue;
      connect("bits."+std::string(f.name),field(field(v,"bits"),f.name),
              field(field(a,"bits"),f.name),request);
    }
    if (!request) b.create<StrictConnectOp>(loc,field(field(v,"bits"),"user"),
        b.create<InvalidValueOp>(loc,uint(1)).getResult());
  }
  std::string op="~"+inputName.str(), np="~"+wrapperName.str(), mp="|"+inputName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if (auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue(); if(v==op)return b.getStringAttr(np);
      if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str(); StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto mapping=mapped.find(ref.str());
        if(mapping!=mapped.end()) suffix="|"+wrapperName.str()+">"+mapping->second;
        else if(copied.count(ref.take_front(ref.find_first_of(".[")).str()))
          suffix.replace(0,mp.size(),"|"+wrapperName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw)); circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}
