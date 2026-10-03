// See LICENSE for license details.
// Oracle: EgressUnit.scala ReadEgress, recorded Rocket no-ROB profile.
// Requests address the synchronous buffer immediately on targetFire. The
// current ID is not reset; only its valid bit is. A new request wins over an
// accepted final beat. Neither retirement nor dequeue-ready includes the
// buffer's valid bit: token readiness guarantees that on legal target cycles.
// Timing-model requests/response payload remain explicit until model lowering.
#include "goldengate/FASEDReadScheduler.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDReadScheduler(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGFASEDReadScheduler";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDReadSchedulerWrapper";
  auto reject = [&](llvm::StringRef s) { error=s.str();return failure(); };
  if (circuit.getName() != "GGFASEDReadBufferWrapper")
    return reject("FASED read scheduler requires the active read buffer wrapper");
  FModuleOp inner,engine;
  for (auto m:circuit.getOps<FModuleLike>()) {
    if (m.getModuleName()==helperName || m.getModuleName()==wrapperName)
      return reject("FASED read scheduler helper or wrapper already exists");
    if (m.getModuleName()==circuit.getName()) inner=dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName()=="GGFASEDTokenEngine") engine=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED read scheduler needs top, engine and annotations");
  auto key=engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge=key?key.getAs<DictionaryAttr>("axi4Edge"):DictionaryAttr();
  auto widths=key?key.getAs<DictionaryAttr>("axi4Widths"):DictionaryAttr();
  auto has=[&](DictionaryAttr d,llvm::StringRef n,int v) { auto a=d?d.getAs<IntegerAttr>(n):IntegerAttr();return a&&a.getInt()==v; };
  if (!has(edge,"maxReadTransfer",8)||!has(edge,"idReuse",1)||!has(edge,"maxFlight",10)||
      !has(widths,"addrBits",35)||!has(widths,"dataBits",64)||!has(widths,"idBits",4))
    return reject("FASED read scheduler supports the recorded 35/64/4-bit no-ROB profile");
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned w) {return UIntType::get(ctx,w,false);};auto bit=uint(1);
  auto bundle=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> es) {
    SmallVector<BundleType::BundleElement> fields;for(auto [n,w]:es) fields.push_back({b.getStringAttr(n),false,uint(w)});return BundleType::get(ctx,fields);
  };
  auto stored=bundle({{"data",64},{"last",1}});
  auto token=BundleType::get(ctx,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,stored}});
  auto readiness=bundle({{"readValid",1},{"writeValid",1}});
  const llvm::StringRef names[]{"hostClock","fased_egress_reset","fased_tfire","fased_egress_readiness","fased_read_buffer_address","fased_read_buffer_deq"};
  const Type types[]{ClockType::get(ctx),bit,bit,readiness,uint(4),token};
  const Direction dirs[]{Direction::In,Direction::Out,Direction::Out,Direction::In,Direction::In,Direction::Out};
  unsigned indices[6];
  for(unsigned j=0;j<6;++j) {
    std::optional<unsigned> i;
    for(auto [n,p]:llvm::enumerate(inner.getPorts())) if(p.name==names[j]&&p.type==types[j]&&p.direction==dirs[j]) i=n;
    if(!i) return reject("FASED read scheduler needs exact clock/reset/fire/readiness/buffer boundaries");indices[j]=*i;
  }
  for(auto p:inner.getPorts()) if(p.name=="fased_write_egress_valid"||p.name=="fased_read_egress_req"||p.name=="fased_read_egress_resp")
    return reject("FASED read scheduler boundary already exists");
  bool used=false;circuit.walk([&](InstanceOp i){used|=i.getModuleName()==inner.getName();});
  if(used) return reject("FASED read scheduler needs an uninstantiated top");
  auto req=BundleType::get(ctx,{{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(4)}});
  auto read=bundle({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}});
  auto resp=BundleType::get(ctx,{{b.getStringAttr("tReady"),true,bit},{b.getStringAttr("hValid"),false,bit},{b.getStringAttr("tBits"),false,read}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),types[0],Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("targetFire"),bit,Direction::In},
      {b.getStringAttr("req"),req,Direction::In},{b.getStringAttr("bufferDeq"),token,Direction::In},
      {b.getStringAttr("bufferAddress"),uint(4),Direction::Out},{b.getStringAttr("resp"),resp,Direction::Out}};
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i){return helper.getBodyBlock()->getArgument(i);};
  auto field=[&](Value v,llvm::StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto constant=[&](unsigned w,uint64_t v)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,v));};
  auto both=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  Value valid=b.create<RegResetOp>(loc,bit,arg(0),arg(1),constant(1,0),"currReqReg_valid").getResult();
  Value id=b.create<RegOp>(loc,uint(4),arg(0),"currReqReg_bits").getResult();
  Value start=both(arg(2),field(arg(3),"valid"));
  Value ready=both(both(arg(2),valid),field(arg(6),"tReady"));
  Value data=field(arg(4),"bits"),done=both(ready,field(data,"last"));
  connect(valid,mux(start,constant(1,1),mux(done,constant(1,0),valid)));
  connect(id,mux(start,field(arg(3),"bits"),id));
  connect(arg(5),mux(start,field(arg(3),"bits"),id));
  connect(field(arg(4),"ready"),ready);
  connect(field(arg(6),"hValid"),b.create<OrPrimOp>(loc,b.create<NotPrimOp>(loc,valid),both(valid,field(arg(4),"valid"))));
  Value out=field(arg(6),"tBits");connect(field(out,"id"),id);
  for(auto n:{"data","last"}) connect(field(out,n),field(data,n));
  connect(field(out,"resp"),constant(2,0));connect(field(out,"user"),constant(1,0));

  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for(auto [i,p]:llvm::enumerate(inner.getPorts())) if(i!=indices[3]&&i!=indices[4]&&i!=indices[5]) {copied.push_back(i);ports.push_back(p);}
  unsigned added=ports.size();
  ports.push_back({b.getStringAttr("fased_write_egress_valid"),bit,Direction::In});
  ports.push_back({b.getStringAttr("fased_read_egress_req"),req,Direction::In});
  ports.push_back({b.getStringAttr("fased_read_egress_resp"),resp,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),scheduler=b.create<InstanceOp>(loc,helper,"readScheduler");
  for(auto [j,i]:llvm::enumerate(copied)) {
    auto p=inner.getPorts()[i];Value v=wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,p.direction==Direction::In?v:sim.getResult(i));
  }
  auto extra=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(added+i);};
  connect(scheduler.getResult(0),wrapper.getBodyBlock()->getArgument(llvm::find(copied,indices[0])-copied.begin()));
  connect(scheduler.getResult(1),sim.getResult(indices[1]));connect(scheduler.getResult(2),sim.getResult(indices[2]));
  connect(scheduler.getResult(3),extra(1));b.create<ConnectOp>(loc,scheduler.getResult(4),sim.getResult(indices[5]));
  connect(sim.getResult(indices[4]),scheduler.getResult(5));b.create<ConnectOp>(loc,extra(2),scheduler.getResult(6));
  connect(field(sim.getResult(indices[3]),"readValid"),field(scheduler.getResult(6),"hValid"));
  connect(field(sim.getResult(indices[3]),"writeValid"),extra(0));
  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str(),modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute attr)->Attribute {
    if(auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue();if(v==oldPrefix)return b.getStringAttr(newPrefix);if(!v.consume_front(oldPrefix+"|"))return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if(ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));for(auto i:copied)if(n==inner.getPortName(i)){suffix="|"+wrapperName.str()+">"+ref.str();break;}
        if(ref=="fased_egress_readiness.writeValid")suffix="|"+wrapperName.str()+">fased_write_egress_valid";
        if(ref=="fased_egress_readiness.readValid")suffix="|"+wrapperName.str()+">fased_read_egress_resp.hValid";
        if(ref=="fased_read_buffer_deq.bits.data")suffix="|"+wrapperName.str()+">fased_read_egress_resp.tBits.data";
        if(ref=="fased_read_buffer_deq.bits.last")suffix="|"+wrapperName.str()+">fased_read_egress_resp.tBits.last";
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if(auto a=dyn_cast<ArrayAttr>(attr)){SmallVector<Attribute> values;for(auto v:a)values.push_back(retarget(v));return b.getArrayAttr(values);}
    if(auto d=dyn_cast<DictionaryAttr>(attr)){NamedAttrList values;for(auto v:d)values.set(v.getName(),retarget(v.getValue()));return values.getDictionary(ctx);}
    return attr;
  };
  SmallVector<Attribute> annotations;for(auto a:raw)annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));circuit.setName(wrapperName);return success();
}
