// See LICENSE for license details.
// Oracle: FPGATop.cpuManagedAXI4NodeTuple and cpu_managed_axi4 IO <> node.out.
// Required boundary: uninstantiated GGHostMemoryPlatformWrapper with all 25
// recorded U250 CPU stream scalars forwarded uniquely to its retained child.
// Required engines: one outgoing TracerV CPU stream, zero incoming streams.
// Mutation: aggregate 64/512/16 AXI4 slave plus combinational leaf adapter.
// Unused AW/AR metadata and W data/last inputs have no users in this profile.
// No state, buffering, ID adaptation, or protocol gating is added here.
// Annotations: none consumed/produced. Copied targets transfer; CPU scalar
// identities stay on the retained inner module. Existing bodies are preserved.
#include "goldengate/CPUStreamPort.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <map>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::assembleCPUStreamPort(CircuitOp circuit,
                                               std::string &error) {
  constexpr StringLiteral oldName="GGHostMemoryPlatformWrapper";
  constexpr StringLiteral newName="GGCPUStreamPlatformWrapper";
  constexpr StringLiteral helperName="GGCPUStreamPortAdapter";
  auto reject=[&](StringRef s){error=s.str();return failure();};
  FModuleOp inner,read,write;
  for(auto m:circuit.getOps<FModuleLike>()) {
    auto n=m.getModuleName();if(n==newName||n==helperName)return reject("CPU stream platform adapter already exists");
    auto f=dyn_cast<FModuleOp>(m.getOperation());if(!f)continue;
    if(n==oldName)inner=f;if(n=="GGCPUStreamRead")read=f;if(n=="GGEmptyCPUStreamWrite")write=f;
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto addr=read?read->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits"):IntegerAttr();
  auto sinks=write?write->getAttrOfType<IntegerAttr>("goldengate.fromHostCPUStreamCount"):IntegerAttr();
  if(circuit.getName()!=oldName||!inner||!raw||!addr||addr.getInt()!=19||!sinks||sinks.getInt()!=0)
    return reject("CPU stream platform adapter requires the recorded buffered outgoing-only Rocket boundary and annotations");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w){return UIntType::get(ctx,w,false);};
  struct Leaf {StringRef name;unsigned width;Direction direction;};
  const Leaf leaves[]{
    {"ar_ready",1,Direction::Out},{"ar_valid",1,Direction::In},
    {"ar_bits_id",16,Direction::In},{"ar_bits_addr",64,Direction::In},
    {"ar_bits_len",8,Direction::In},{"ar_bits_size",3,Direction::In},
    {"r_ready",1,Direction::In},{"r_valid",1,Direction::Out},
    {"r_bits_id",16,Direction::Out},{"r_bits_data",512,Direction::Out},
    {"r_bits_last",1,Direction::Out},{"r_bits_resp",2,Direction::Out},
    {"aw_ready",1,Direction::Out},{"aw_valid",1,Direction::In},
    {"aw_bits_id",16,Direction::In},{"aw_bits_size",3,Direction::In},
    {"w_ready",1,Direction::Out},{"w_valid",1,Direction::In},
    {"w_bits_strb",64,Direction::In},{"b_valid",1,Direction::Out},
    {"b_bits_id",16,Direction::Out},{"b_bits_resp",2,Direction::Out},
    {"b_ready",1,Direction::In},{"aw_bits_addr",64,Direction::In},{"aw_bits_len",8,Direction::In}};
  std::map<std::string,unsigned> old,consumed,copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts())) {
    auto n=p.name.getValue();old[n.str()]=i;
    if(n=="cpu_managed_axi4"||n.contains("from_cpu_stream"))return reject("CPU stream platform boundary already exists or has incoming streams");
    if(n.starts_with("cpu_stream_")) {
      bool found=false;
      for(auto leaf:leaves)if(n=="cpu_stream_"+leaf.name.str()&&p.type==uint(leaf.width)&&p.direction==leaf.direction){found=true;consumed[n.str()]=i;break;}
      if(!found)return reject("CPU stream platform adapter requires exact U250 scalar widths/directions and no extra stream fields");
    }
  }
  if(consumed.size()!=std::size(leaves)||!old.count("hostClock")||!old.count("hostReset")||!old.count("mem_0"))
    return reject("CPU stream platform adapter requires all 25 CPU scalar fields and host memory/clock/reset");
  auto cp=inner.getPorts()[old.at("hostClock")],rp=inner.getPorts()[old.at("hostReset")];
  if(cp.type!=ClockType::get(ctx)||cp.direction!=Direction::In||rp.type!=uint(1)||rp.direction!=Direction::In)
    return reject("CPU stream platform adapter requires host clock/reset inputs");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==oldName;});
  InstanceOp sim,memory;unsigned instances=0;
  for(auto i:inner.getOps<InstanceOp>()) {
    ++instances;if(i.getName()=="sim"&&i.getModuleName()=="GGHostMemoryReadResponseWrapper")sim=i;
    if(i.getName()=="memory_port"&&i.getModuleName()=="GGHostMemoryPortAdapter")memory=i;
  }
  if(used||instances!=2||!sim||!memory)return reject("CPU stream platform adapter requires the unique memory platform wrapper");
  auto childPort=[&](StringRef n)->Value {
    for(auto [i,p]:llvm::enumerate(sim.getPortNames()))if(cast<StringAttr>(p).getValue()==n)return sim.getResult(i);
    return {};
  };
  auto arg=[&](FModuleOp m,unsigned i){return m.getBodyBlock()->getArgument(i);};
  for(auto leaf:leaves) {
    auto n="cpu_stream_"+leaf.name.str();Value a=arg(inner,consumed.at(n)),v=childPort(n);
    if(!v||v.getType()!=a.getType()||!a.hasOneUse())return reject("CPU stream scalar requires one exact child forwarding connection");
    auto d=leaf.direction==Direction::In?v:a,s=leaf.direction==Direction::In?a:v;
    unsigned count=0;for(auto c:inner.getOps<ConnectOp>())count+=c.getDest()==d&&c.getSrc()==s;
    if(count!=1)return reject("CPU stream scalar requires a unique direct child binding");
  }
  // All rejection paths precede mutation. Generate ordinary FIRRTL operations.
  auto token=[&](ArrayRef<std::pair<StringRef,unsigned>> fs){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fs)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},
      {b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,BundleType::get(ctx,es)}});
  };
  auto address=token({{"id",16},{"addr",64},{"len",8},{"size",3},{"burst",2},{"lock",1},{"cache",4},{"prot",3},{"qos",4}});
  auto master=BundleType::get(ctx,{{b.getStringAttr("aw"),false,address},
    {b.getStringAttr("w"),false,token({{"data",512},{"strb",64},{"last",1}})},
    {b.getStringAttr("b"),true,token({{"id",16},{"resp",2}})},
    {b.getStringAttr("ar"),false,address},
    {b.getStringAttr("r"),true,token({{"id",16},{"data",512},{"resp",2},{"last",1}})}});
  SmallVector<PortInfo> hp{{b.getStringAttr("cpu_managed_axi4"),master,Direction::In}};
  for(auto leaf:leaves)hp.push_back({b.getStringAttr("stream_"+leaf.name.str()),uint(leaf.width),leaf.direction==Direction::In?Direction::Out:Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  for(auto [i,leaf]:llvm::enumerate(leaves)) {
    auto path=leaf.name.split('_');Value v=field(arg(helper,0),path.first);
    if(path.second.starts_with("bits_")){v=field(v,"bits");v=field(v,path.second.drop_front(5));}else v=field(v,path.second);
    Value scalar=arg(helper,i+1);
    b.create<StrictConnectOp>(loc,leaf.direction==Direction::In?scalar:v,leaf.direction==Direction::In?v:scalar);
  }
  SmallVector<PortInfo> ports;
  for(auto p:inner.getPorts())if(!consumed.count(p.name.getValue().str())){copied[p.name.getValue().str()]=ports.size();ports.push_back(p);}
  unsigned pi=ports.size();ports.push_back({b.getStringAttr("cpu_managed_axi4"),master,Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(newName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child=b.create<InstanceOp>(loc,inner,"sim"),adapter=b.create<InstanceOp>(loc,helper,"cpu_port");
  for(auto [n,i]:copied) {
    auto p=inner.getPorts()[old.at(n)];Value v=child.getResult(old.at(n)),a=arg(wrapper,i);
    b.create<ConnectOp>(loc,p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  b.create<ConnectOp>(loc,adapter.getResult(0),arg(wrapper,pi));
  for(auto [i,leaf]:llvm::enumerate(leaves)) {
    Value v=child.getResult(consumed.at("cpu_stream_"+leaf.name.str())),a=adapter.getResult(i+1);
    b.create<StrictConnectOp>(loc,leaf.direction==Direction::In?v:a,leaf.direction==Direction::In?a:v);
  }
  std::string op="~"+oldName.str(),np="~"+newName.str(),mp="|"+oldName.str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute a)->Attribute {
    if(auto s=dyn_cast<StringAttr>(a)) {
      auto v=s.getValue();if(v==op)return b.getStringAttr(np);if(!v.consume_front(op+"|"))return a;
      std::string suffix="|"+v.str();StringRef ref(suffix);
      if(ref.consume_front(mp)) {
        auto root=ref.take_front(ref.find_first_of(".[")).str();
        if(copied.count(root))suffix.replace(0,mp.size(),"|"+newName.str()+">");
      }
      return b.getStringAttr(np+suffix);
    }
    if(auto xs=dyn_cast<ArrayAttr>(a)){SmallVector<Attribute> out;for(auto x:xs)out.push_back(retarget(x));return b.getArrayAttr(out);}
    if(auto xs=dyn_cast<DictionaryAttr>(a)){NamedAttrList out;for(auto x:xs)out.set(x.getName(),retarget(x.getValue()));return out.getDictionary(ctx);}
    return a;
  };
  circuit->setAttr("rawAnnotations",retarget(raw));circuit.setNameAttr(b.getStringAttr(newName));return success();
}
