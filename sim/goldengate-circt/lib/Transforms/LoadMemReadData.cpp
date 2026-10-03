// See LICENSE for license details.
// Input: uninstantiated GGLoadMemReadRequestWrapper with retained annotations,
// host Clock/UInt1 reset, and fresh read-data helper/wrapper/boundary names.
// Recorded U250 in64/out32/outputDepth2 only. Annotations consumed/produced:
// none; copied top targets transfer to the wrapper. Hierarchy must be rebuilt.
// Mutation: unreset UInt64 payload, reset tail/size, decoded R_DATA word 8
// and scalar host-memory R data boundary. Low word first; no empty bypass,
// no input acceptance with a remaining word, including concurrent final pop.
// Reset flushes control only and does not suppress an accepted payload write.
// R sidebands and LoadMem MCRFile AXI transport are subsequent porting steps.
#include "goldengate/LoadMemWriter.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addLoadMemReadData(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGLoadMemReadDataWrapper";
  constexpr llvm::StringLiteral helperName = "GGLoadMemReadDataFIFO";
  constexpr llvm::StringLiteral controlName = "loadmemReadData_mcr";
  auto reject = [&](llvm::StringRef s) { error=s.str(); return failure(); };
  if (circuit.getName() != "GGLoadMemReadRequestWrapper")
    return reject("LoadMem read data requires the read request wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName()==wrapperName || m.getModuleName()==helperName)
      return reject("LoadMem read data helper or wrapper already exists");
    if (m.getModuleName()==circuit.getName()) inner=dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("LoadMem read data needs a top and annotations");
  auto *context=circuit.getContext(); OpBuilder b(context); auto loc=circuit.getLoc();
  auto uint=[&](unsigned w) { return UIntType::get(context,w,false); }; auto bit=uint(1);
  std::optional<unsigned> clock,reset;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) {
    auto n=p.name.getValue();
    if (n==controlName || n.starts_with("loadmem_mem_r_"))
      return reject("LoadMem read data boundary already exists");
    if (n=="hostClock" && p.direction==Direction::In && p.type==ClockType::get(context)) clock=i;
    if (n=="hostReset" && p.direction==Direction::In && p.type==bit) reset=i;
  }
  if (!clock || !reset) return reject("LoadMem read data needs exact host clock/reset");
  bool used=false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName()==inner.getName(); });
  if (used) return reject("LoadMem read data needs an uninstantiated top");
  const llvm::StringRef names[]{"clock","reset","in_ready","in_valid","in_bits","out_ready","out_valid","out_bits","count"};
  const unsigned widths[]{0,1,1,1,64,1,1,32,2};
  SmallVector<PortInfo> hp;
  for (unsigned i=0;i<9;++i) hp.push_back({b.getStringAttr(names[i]),
      i==0 ? Type(ClockType::get(context)) : Type(uint(widths[i])),
      i==2 || i>=6 ? Direction::Out : Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto fifo=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(context,Convention::Internal),hp);
  fifo->setAttr("goldengate.inputWidth",b.getI32IntegerAttr(64));
  fifo->setAttr("goldengate.outputWidth",b.getI32IntegerAttr(32));
  fifo->setAttr("goldengate.outputDepth",b.getI32IntegerAttr(2));
  b.setInsertionPointToStart(fifo.getBodyBlock());
  auto arg=[&](unsigned i) { return fifo.getBodyBlock()->getArgument(i); };
  auto constant=[&](unsigned w,uint64_t n) -> Value { return b.create<ConstantOp>(loc,uint(w),APInt(w,n)); };
  auto connect=[&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  auto mux=[&](Value c,Value y,Value n) -> Value { return b.create<MuxPrimOp>(loc,c,y,n); };
  auto trunc=[&](Value v) -> Value { return b.create<BitsPrimOp>(loc,v,1,0); };
  auto reg=[&](unsigned w,llvm::StringRef n) -> Value {
    return b.create<RegResetOp>(loc,uint(w),arg(0),arg(1),constant(w,0),n).getResult();
  };
  // The Scala head has zero width for this one-beat geometry and is removed.
  Value tail=reg(1,"tail"),size=reg(2,"size");
  Value payload=b.create<RegOp>(loc,uint(64),arg(0),"wdata_0").getResult();
  Value ready=b.create<LTPrimOp>(loc,size,constant(2,1));
  Value valid=b.create<GTPrimOp>(loc,size,constant(2,0));
  Value push=b.create<AndPrimOp>(loc,ready,arg(3)),pop=b.create<AndPrimOp>(loc,valid,arg(5));
  connect(arg(2),ready); connect(arg(6),valid); connect(arg(8),size);
  connect(payload,mux(push,arg(4),payload));
  connect(tail,mux(pop,b.create<NotPrimOp>(loc,tail),tail));
  Value plusOne=trunc(b.create<AddPrimOp>(loc,size,constant(2,1)));
  Value plusTwo=trunc(b.create<AddPrimOp>(loc,size,constant(2,2)));
  Value minusOne=trunc(b.create<SubPrimOp>(loc,size,constant(2,1)));
  connect(size,mux(push,mux(pop,plusOne,plusTwo),mux(pop,minusOne,size)));
  connect(arg(7),mux(tail,b.create<BitsPrimOp>(loc,payload,63,32),b.create<BitsPrimOp>(loc,payload,31,0)));
  auto field=[&](Value v,llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc,v,n); };
  auto token=BundleType::get(context,{{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(32)}});
  auto mcr=BundleType::get(context,{{b.getStringAttr("read_8"),false,token},
      {b.getStringAttr("write_8"),true,token},{b.getStringAttr("wstrb"),true,uint(4)}});
  SmallVector<PortInfo> ports(inner.getPorts()); SmallVector<unsigned> copied;
  for (unsigned i=0;i<ports.size();++i) copied.push_back(i);
  unsigned first=ports.size();
  ports.push_back({b.getStringAttr(controlName),mcr,Direction::Out});
  ports.push_back({b.getStringAttr("loadmem_mem_r_ready"),bit,Direction::Out});
  ports.push_back({b.getStringAttr("loadmem_mem_r_valid"),bit,Direction::In});
  ports.push_back({b.getStringAttr("loadmem_mem_r_bits_data"),uint(64),Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  wrapper->setAttr("goldengate.mmioRegisters",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr("R_DATA")),b.getNamedAttr("offset",b.getI32IntegerAttr(32)),
      b.getNamedAttr("readable",b.getBoolAttr(true)),b.getNamedAttr("writeable",b.getBoolAttr(false))})}));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),data=b.create<InstanceOp>(loc,fifo,"readData");
  auto outer=[&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) b.create<ConnectOp>(loc,
      p.direction==Direction::In ? sim.getResult(i) : outer(i),p.direction==Direction::In ? outer(i) : sim.getResult(i));
  connect(data.getResult(0),outer(*clock)); connect(data.getResult(1),outer(*reset));
  auto rd=field(outer(first),"read_8"),wr=field(outer(first),"write_8");
  connect(data.getResult(5),field(rd,"ready"));
  connect(field(rd,"valid"),data.getResult(6)); connect(field(rd,"bits"),data.getResult(7));
  connect(field(wr,"ready"),constant(1,0));
  b.create<AssertOp>(loc,outer(*clock),b.create<NotPrimOp>(loc,field(wr,"valid")),
      b.create<NotPrimOp>(loc,outer(*reset)),"Can only read from this decoupled source",ValueRange{},"");
  connect(outer(first+1),data.getResult(2)); connect(data.getResult(3),outer(first+2)); connect(data.getResult(4),outer(first+3));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
