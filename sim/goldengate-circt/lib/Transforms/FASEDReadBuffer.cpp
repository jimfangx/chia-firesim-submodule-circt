// See LICENSE for license details.
// Oracle: Lib.scala MultiQueue and EgressUnit.scala ReadEgress without ROB.
// Recorded Rocket profile: 16 IDs, eight beats/ID, data+last only. A synchronous
// read prefetches after an accepted dequeue and remembers the previous ID.
// Host R is always accepted by ReadEgress; internal MultiQueue can be full.
// Qualified egress reset clears pointer/full/valid state, never RAM or read ID.
// Consumes flat R handshakes; exposes R payload and raw read-buffer dequeue.
// Request scheduling, target response/token wiring remain subsequent passes.
// All supported-profile, hierarchy and port checks precede mutation.
#include "goldengate/FASEDReadBuffer.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDReadBuffer(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGFASEDReadBuffer16x8";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDReadBufferWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDIngressIssueWrapper")
    return reject("FASED read buffer requires the active ingress issue wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == queueName || m.getModuleName() == wrapperName)
      return reject("FASED read buffer helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !engine || !raw) return reject("FASED read buffer needs top, engine and annotations");
  auto key = engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto widths = key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto has = [&](DictionaryAttr d, llvm::StringRef n, int v) {
    auto a = d ? d.getAs<IntegerAttr>(n) : IntegerAttr(); return a && a.getInt() == v;
  };
  if (!has(edge,"maxReadTransfer",8) || !has(edge,"idReuse",1) || !has(edge,"maxFlight",10) ||
      !has(widths,"dataBits",64) || !has(widths,"idBits",4) || !has(widths,"addrBits",35))
    return reject("FASED read buffer supports the recorded 35/64/4-bit, 8-beat, one-request-per-ID profile");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx,w,false); }; auto bit = uint(1);
  auto bundle = [&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> es) {
    SmallVector<BundleType::BundleElement> fields;
    for (auto [n,w] : es) fields.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(ctx,fields);
  };
  auto token = [&](FIRRTLBaseType t) { return BundleType::get(ctx,{
      {b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},
      {b.getStringAttr("bits"),false,t}}); };
  auto responses = bundle({{"rReady",1},{"rValid",1},{"rLast",1},{"bReady",1},{"bValid",1}});
  auto stored = bundle({{"data",64},{"last",1}});
  auto read = bundle({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}});
  auto port = [&](llvm::StringRef n, Type t, Direction d) -> std::optional<unsigned> {
    for (auto [i,p] : llvm::enumerate(inner.getPorts()))
      if (p.name == n && p.type == t && p.direction == d) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock",ClockType::get(ctx),Direction::In);
  auto reset = port("fased_egress_reset",bit,Direction::Out);
  auto host = port("fased_host_responses",responses,Direction::In);
  if (!clock || !reset || !host) return reject("FASED read buffer needs exact clock, egress reset and host response boundary");
  for (auto p : inner.getPorts())
    if (p.name == "fased_host_read_response" || p.name == "fased_host_write_responses" ||
        p.name == "fased_read_buffer_address" || p.name == "fased_read_buffer_deq")
      return reject("FASED read buffer boundary already exists");
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED read buffer needs an uninstantiated top");

  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("enq"),token(stored),Direction::In},
      {b.getStringAttr("deq"),token(stored),Direction::Out},
      {b.getStringAttr("enqAddr"),uint(4),Direction::In},{b.getStringAttr("deqAddr"),uint(4),Direction::In},
      {b.getStringAttr("empty"),bit,Direction::Out}};
  auto queue = b.create<FModuleOp>(loc,b.getStringAttr(queueName),ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto arg = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v,llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc,v,n); };
  auto connect = [&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  auto constant = [&](unsigned w,uint64_t v)->Value { return b.create<ConstantOp>(loc,uint(w),APInt(w,v)); };
  auto reg = [&](unsigned w,llvm::StringRef n)->Value {
    return b.create<RegResetOp>(loc,uint(w),arg(0),arg(1),constant(w,0),n).getResult();
  };
  auto both = [&](Value a,Value z)->Value { return b.create<AndPrimOp>(loc,a,z); };
  auto either = [&](Value a,Value z)->Value { return b.create<OrPrimOp>(loc,a,z); };
  auto neg = [&](Value a)->Value { return b.create<NotPrimOp>(loc,a); };
  auto eq = [&](Value a,Value z)->Value { return b.create<EQPrimOp>(loc,a,z); };
  auto mux = [&](Value c,Value a,Value z)->Value { return b.create<MuxPrimOp>(loc,c,a,z); };
  auto inc = [&](Value v)->Value { return b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,v,constant(3,1)),2,0); };
  SmallVector<Value> enqPtrs,deqPtrs,fullFlags;
  for (unsigned i=0;i<16;++i) {
    enqPtrs.push_back(reg(3,"enqPtrs_"+std::to_string(i)));
    deqPtrs.push_back(reg(3,"deqPtrs_"+std::to_string(i)));
    fullFlags.push_back(reg(1,"maybe_full_"+std::to_string(i)));
  }
  Value previousId = b.create<RegOp>(loc,uint(4),arg(0),"deqAddrReg").getResult();
  Value deqValid = reg(1,"deqValid"); connect(previousId,arg(5));
  auto select = [&](ArrayRef<Value> values,Value id)->Value {
    Value v=values.front(); for (unsigned i=1;i<16;++i) v=mux(eq(id,constant(4,i)),values[i],v); return v;
  };
  Value wp=select(enqPtrs,arg(4)),rp=select(deqPtrs,arg(5)),ep=select(enqPtrs,arg(5));
  Value full=both(eq(wp,select(deqPtrs,arg(4))),select(fullFlags,arg(4)));
  Value ready=neg(full),push=both(ready,field(arg(2),"valid"));
  Value pop=both(deqValid,field(arg(3),"ready")),sameId=eq(arg(4),previousId);
  Value advance=both(pop,eq(previousId,arg(5))),nextRP=inc(rp);
  Value empty=mux(advance,eq(nextRP,ep),both(eq(rp,ep),neg(select(fullFlags,arg(5)))));
  connect(field(arg(2),"ready"),ready);connect(field(arg(3),"valid"),deqValid);
  connect(arg(6),empty);connect(deqValid,neg(empty));
  for (unsigned i=0;i<16;++i) {
    Value enqId=eq(arg(4),constant(4,i)),deqId=eq(previousId,constant(4,i));
    connect(enqPtrs[i],mux(both(push,enqId),inc(enqPtrs[i]),enqPtrs[i]));
    connect(deqPtrs[i],mux(both(pop,deqId),inc(deqPtrs[i]),deqPtrs[i]));
    Value different=mux(both(pop,deqId),constant(1,0),mux(both(push,enqId),constant(1,1),fullFlags[i]));
    connect(fullFlags[i],mux(sameId,mux(both(enqId,b.create<XorPrimOp>(loc,push,pop)),push,fullFlags[i]),different));
  }
  SmallVector<Type> memTypes{MemOp::getTypeForPort(128,uint(65),MemOp::PortKind::Read),
      MemOp::getTypeForPort(128,uint(65),MemOp::PortKind::Write)};
  SmallVector<Attribute> memNames{b.getStringAttr("read"),b.getStringAttr("write")};
  auto ram=b.create<MemOp>(loc,memTypes,1,1,128,RUWAttr::Undefined,memNames,"ram");
  auto reader=ram.getResult(0),writer=ram.getResult(1);
  connect(field(reader,"clk"),arg(0));connect(field(reader,"en"),constant(1,1));
  connect(field(reader,"addr"),b.create<CatPrimOp>(loc,arg(5),mux(advance,nextRP,rp)));
  Value readData=field(reader,"data"),out=field(arg(3),"bits"),in=field(arg(2),"bits");
  connect(field(out,"data"),b.create<BitsPrimOp>(loc,readData,64,1));
  connect(field(out,"last"),b.create<BitsPrimOp>(loc,readData,0,0));
  connect(field(writer,"clk"),arg(0));connect(field(writer,"en"),push);
  connect(field(writer,"mask"),constant(1,1));connect(field(writer,"addr"),b.create<CatPrimOp>(loc,arg(4),wp));
  connect(field(writer,"data"),b.create<CatPrimOp>(loc,field(in,"data"),field(in,"last")));

  SmallVector<PortInfo> ports;SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) if (i!=*host) { copied.push_back(i);ports.push_back(p); }
  unsigned added=ports.size();
  ports.push_back({b.getStringAttr("fased_host_write_responses"),bundle({{"bReady",1},{"bValid",1}}),Direction::In});
  ports.push_back({b.getStringAttr("fased_host_read_response"),token(read),Direction::In});
  ports.push_back({b.getStringAttr("fased_read_buffer_address"),uint(4),Direction::In});
  ports.push_back({b.getStringAttr("fased_read_buffer_deq"),token(stored),Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),buffer=b.create<InstanceOp>(loc,queue,"readBuffer");
  auto outer=[&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied,i)-copied.begin()); };
  auto extra=[&](unsigned i) { return wrapper.getBodyBlock()->getArgument(added+i); };
  for (auto [j,i] : llvm::enumerate(copied)) {
    Value v=wrapper.getBodyBlock()->getArgument(j);auto p=inner.getPorts()[i];
    b.create<ConnectOp>(loc,p.direction==Direction::In?sim.getResult(i):v,p.direction==Direction::In?v:sim.getResult(i));
  }
  connect(buffer.getResult(0),outer(*clock));connect(buffer.getResult(1),sim.getResult(*reset));
  Value hostRead=extra(1),bits=field(hostRead,"bits"),queueEnq=buffer.getResult(2),flat=sim.getResult(*host);
  // ReadEgress does not backpressure host R in the non-translation branch.
  connect(field(hostRead,"ready"),constant(1,1));connect(field(flat,"rReady"),field(hostRead,"ready"));
  connect(field(flat,"rValid"),field(hostRead,"valid"));connect(field(flat,"rLast"),field(bits,"last"));
  for (auto n : {"bReady","bValid"}) connect(field(flat,n),field(extra(0),n));
  connect(field(queueEnq,"valid"),field(hostRead,"valid"));
  for (auto n : {"data","last"}) connect(field(field(queueEnq,"bits"),n),field(bits,n));
  connect(buffer.getResult(4),field(bits,"id"));connect(buffer.getResult(5),extra(2));
  b.create<ConnectOp>(loc,extra(3),buffer.getResult(3));

  std::string oldPrefix="~"+circuit.getName().str(),newPrefix="~"+wrapperName.str();
  std::string modulePrefix="|"+inner.getName().str()+">";
  std::function<Attribute(Attribute)> retarget=[&](Attribute attr)->Attribute {
    if (auto s=dyn_cast<StringAttr>(attr)) {
      auto v=s.getValue();if (v==oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix+"|")) return attr;
      std::string suffix="|"+v.str();llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto n=ref.take_front(ref.find_first_of(".["));
        for (auto i:copied) if (n==inner.getPortName(i)) { suffix="|"+wrapperName.str()+">"+ref.str();break; }
        llvm::StringRef leaf=ref;
        if (leaf.consume_front("fased_host_responses.")) {
          if (leaf=="bReady"||leaf=="bValid") suffix="|"+wrapperName.str()+">fased_host_write_responses."+leaf.str();
          for (auto [oldName,newName]: {std::pair<llvm::StringRef,llvm::StringRef>{"rReady","ready"},{"rValid","valid"},{"rLast","bits.last"}})
            if (leaf==oldName) suffix="|"+wrapperName.str()+">fased_host_read_response."+newName.str();
        }
      }
      return b.getStringAttr(newPrefix+suffix);
    }
    if (auto a=dyn_cast<ArrayAttr>(attr)) { SmallVector<Attribute> values;for (auto v:a) values.push_back(retarget(v));return b.getArrayAttr(values); }
    if (auto d=dyn_cast<DictionaryAttr>(attr)) { NamedAttrList values;for (auto v:d) values.set(v.getName(),retarget(v.getValue()));return values.getDictionary(ctx); }
    return attr;
  };
  SmallVector<Attribute> annotations;for (auto a:raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations));circuit.setName(wrapperName);
  return success();
}
