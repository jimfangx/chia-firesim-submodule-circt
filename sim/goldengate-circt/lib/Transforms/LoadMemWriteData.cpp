// See LICENSE for license details.
// Required input invariants: uninstantiated GGLoadMemWriteMMIOWrapper with
// retained rawAnnotations, exact host clock/reset and UInt64 writer data ports,
// and fresh helper/wrapper/MMIO names. Recorded U250 in32/out64/depth32 only.
// Annotations consumed/produced: none; transfer circuit and copied port targets,
// retain consumed writer-data targets on the inner top. Produce W_DATA offset
// and permission metadata for decoded MCR word 4.
// IR mutations: add 64 unreset UInt32 payload registers, three reset control
// registers and an outer wrapper connecting decoded writes to the writer.
// Analyses required: none. Preserved: inner operations/channel endpoints.
// Hierarchy analyses must be rebuilt. Output invariants: verified FIRRTL;
// low-word-first packing, no partial-beat/empty bypass, no full-with-pop input
// acceptance, and reset-time payload writes preserved. No AXI MCRFile transport.
#include "goldengate/LoadMemWriter.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addLoadMemWriteData(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGLoadMemWriteDataWrapper";
  constexpr llvm::StringLiteral fifoName = "GGLoadMemWriteDataFIFO";
  constexpr llvm::StringLiteral controlName = "loadmemData_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGLoadMemWriteMMIOWrapper")
    return reject("LoadMem write data requires the active write MMIO wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == fifoName)
      return reject("LoadMem data helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("LoadMem data needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  const llvm::StringRef names[]{"hostClock", "hostReset", "loadmem_data_ready", "loadmem_data_valid", "loadmem_data_bits"};
  const unsigned widths[]{0,1,1,1,64}; unsigned indices[5];
  for (unsigned j = 0; j < 5; ++j) {
    std::optional<unsigned> found;
    for (auto [i,p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.direction == (j == 2 ? Direction::Out : Direction::In) &&
          p.type == (j == 0 ? Type(ClockType::get(context)) : Type(uint(widths[j])))) found = i;
    if (!found) return reject("LoadMem data needs exact host clock/reset and UInt64 data geometry");
    indices[j] = *found;
  }
  for (auto p : inner.getPorts()) {
    auto n = p.name.getValue();
    if (n.starts_with("loadmem_data_") && !llvm::is_contained(ArrayRef<llvm::StringRef>(names),n))
      return reject("unsupported LoadMem data field");
    if (n == controlName) return reject("LoadMem data MCR boundary already exists");
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("LoadMem data needs an uninstantiated top");
  const llvm::StringRef fnames[]{"clock","reset","in_ready","in_valid","in_bits","out_ready","out_valid","out_bits","count"};
  const unsigned fwidths[]{0,1,1,1,32,1,1,64,6};
  SmallVector<PortInfo> fports;
  for (unsigned i = 0; i < 9; ++i) fports.push_back({b.getStringAttr(fnames[i]),
      i == 0 ? Type(ClockType::get(context)) : Type(uint(fwidths[i])),
      i == 2 || i >= 6 ? Direction::Out : Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto fifo = b.create<FModuleOp>(loc,b.getStringAttr(fifoName),ConventionAttr::get(context,Convention::Internal),fports);
  fifo->setAttr("goldengate.outputDepth",b.getI32IntegerAttr(32));
  fifo->setAttr("goldengate.inputWidth",b.getI32IntegerAttr(32));
  fifo->setAttr("goldengate.outputWidth",b.getI32IntegerAttr(64));
  b.setInsertionPointToStart(fifo.getBodyBlock());
  auto arg = [&](unsigned i) { return fifo.getBodyBlock()->getArgument(i); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc,d,s); };
  auto constant = [&](unsigned w,uint64_t n) -> Value { return b.create<ConstantOp>(loc,uint(w),APInt(w,n)); };
  auto reg = [&](unsigned w,llvm::StringRef n) -> Value {
    return b.create<RegResetOp>(loc,uint(w),arg(0),arg(1),constant(w,0),n).getResult();
  };
  auto mux = [&](Value c,Value y,Value n) -> Value { return b.create<MuxPrimOp>(loc,c,y,n); };
  auto trunc = [&](Value v,unsigned w) -> Value { return b.create<BitsPrimOp>(loc,v,w-1,0); };
  Value head=reg(6,"head"), tail=reg(5,"tail"), size=reg(7,"size");
  Value count=b.create<BitsPrimOp>(loc,size,6,1);
  Value ready=b.create<LTPrimOp>(loc,size,constant(7,64));
  Value valid=b.create<GTPrimOp>(loc,count,constant(6,0));
  Value push=b.create<AndPrimOp>(loc,ready,arg(3)), pop=b.create<AndPrimOp>(loc,valid,arg(5));
  connect(arg(2),ready); connect(arg(6),valid); connect(arg(8),count);
  connect(head,mux(push,trunc(b.create<AddPrimOp>(loc,head,constant(6,1)),6),head));
  connect(tail,mux(pop,trunc(b.create<AddPrimOp>(loc,tail,constant(5,1)),5),tail));
  Value plus=trunc(b.create<AddPrimOp>(loc,size,constant(7,1)),7);
  Value minusOne=trunc(b.create<SubPrimOp>(loc,size,constant(7,1)),7);
  Value minusTwo=trunc(b.create<SubPrimOp>(loc,size,constant(7,2)),7);
  connect(size,mux(push,mux(pop,minusOne,plus),mux(pop,minusTwo,size)));
  SmallVector<Value> payload;
  for (unsigned i = 0; i < 64; ++i) {
    Value data=b.create<RegOp>(loc,uint(32),arg(0),"wdata_"+std::to_string(i)).getResult();
    payload.push_back(data);
    Value selected=b.create<AndPrimOp>(loc,push,b.create<EQPrimOp>(loc,head,constant(6,i)));
    connect(data,mux(selected,arg(4),data));
  }
  Value output=b.create<CatPrimOp>(loc,payload[63],payload[62]);
  for (unsigned i = 0; i < 31; ++i)
    output=mux(b.create<EQPrimOp>(loc,tail,constant(5,i)),b.create<CatPrimOp>(loc,payload[2*i+1],payload[2*i]),output);
  connect(arg(7),output);
  auto token = BundleType::get(context,{{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(32)}});
  auto mcr = BundleType::get(context,{{b.getStringAttr("read_4"),false,token},
      {b.getStringAttr("write_4"),true,token},{b.getStringAttr("wstrb"),true,uint(4)}});
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices).drop_front(2),i)) { copied.push_back(i); ports.push_back(p); }
  ports.push_back({b.getStringAttr(controlName),mcr,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  wrapper->setAttr("goldengate.mmioRegisters",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr("W_DATA")),b.getNamedAttr("offset",b.getI32IntegerAttr(16)),
      b.getNamedAttr("readable",b.getBoolAttr(false)),b.getNamedAttr("writeable",b.getBoolAttr(true))})}));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"), data=b.create<InstanceOp>(loc,fifo,"writeData");
  for (auto [j,i] : llvm::enumerate(copied)) {
    auto p=inner.getPorts()[i]; auto external=wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc,p.direction==Direction::In ? sim.getResult(i) : external,
        p.direction==Direction::In ? external : sim.getResult(i));
    if (i == indices[0] || i == indices[1]) connect(data.getResult(i == indices[0] ? 0 : 1),external);
  }
  connect(data.getResult(5),sim.getResult(indices[2]));
  connect(sim.getResult(indices[3]),data.getResult(6)); connect(sim.getResult(indices[4]),data.getResult(7));
  auto field = [&](Value v,llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc,v,n); };
  auto rd=field(wrapper.getBodyBlock()->getArguments().back(),"read_4"), wr=field(wrapper.getBodyBlock()->getArguments().back(),"write_4");
  connect(field(wr,"ready"),data.getResult(2)); connect(data.getResult(3),field(wr,"valid")); connect(data.getResult(4),field(wr,"bits"));
  connect(field(rd,"valid"),constant(1,0)); connect(field(rd,"bits"),constant(32,0));
  b.create<AssertOp>(loc,wrapper.getBodyBlock()->getArgument(llvm::find(copied,indices[0])-copied.begin()),
      b.create<NotPrimOp>(loc,field(rd,"ready")),
      b.create<NotPrimOp>(loc,wrapper.getBodyBlock()->getArgument(llvm::find(copied,indices[1])-copied.begin())),
      "Can only write to this decoupled sink",ValueRange{},"");
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
