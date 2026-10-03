// See LICENSE for license details.
// Input: uninstantiated GGLoadMemWriteDataWrapper, rawAnnotations, hostClock
// and hostReset; fresh helper/wrapper/MMIO/read-address boundary names.
// Recorded U250 addr34, Ctrl32 only. No annotations consumed or produced;
// circuit and all copied top port identities transfer to the new wrapper.
// Mutation: two-entry asynchronous-read UInt34 RAM queue, three reset control
// bits, unreset R_ADDRESS_H register and decoded MCR words 6/7. Hierarchy
// analyses must be rebuilt. Reset flushes pointers only; writes remain active.
// Preserve the oracle's Cat(high2, queued34) truncated to addr34: the high
// register remains readable but does not affect AR address. No bypass or
// full-with-pop enqueue. AR sidebands, R data and MCR transport are later ports.
#include "goldengate/LoadMemWriter.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addLoadMemReadRequests(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGLoadMemReadRequestWrapper";
  constexpr llvm::StringLiteral helperName = "GGLoadMemReadRequests";
  constexpr llvm::StringLiteral controlName = "loadmemRead_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGLoadMemWriteDataWrapper")
    return reject("LoadMem read requests require the write data wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("LoadMem read request helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("LoadMem read requests need a top and annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  std::optional<unsigned> clock, reset;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) {
    auto n=p.name.getValue();
    if (n == controlName || n.starts_with("loadmem_mem_ar_"))
      return reject("LoadMem read boundary already exists");
    if (n == "hostClock" && p.direction == Direction::In && p.type == ClockType::get(context)) clock=i;
    if (n == "hostReset" && p.direction == Direction::In && p.type == bit) reset=i;
  }
  if (!clock || !reset) return reject("LoadMem read requests need exact host clock/reset");
  bool used=false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("LoadMem read requests need an uninstantiated top");
  const llvm::StringRef names[]{"clock","reset","write_6_ready","write_6_valid","write_6_bits",
      "read_6_valid","read_6_bits","write_7_ready","write_7_valid","write_7_bits",
      "read_7_valid","read_7_bits","read_7_ready","mem_ar_ready","mem_ar_valid","mem_ar_bits_addr"};
  const unsigned widths[]{0,1,1,1,32,1,32,1,1,32,1,32,1,1,1,34};
  SmallVector<PortInfo> hp;
  for (unsigned i=0;i<16;++i) hp.push_back({b.getStringAttr(names[i]),
      i==0 ? Type(ClockType::get(context)) : Type(uint(widths[i])),
      i==2 || i==5 || i==6 || i==7 || i==10 || i==11 || i>=14 ? Direction::Out : Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper=b.create<FModuleOp>(loc,b.getStringAttr(helperName),ConventionAttr::get(context,Convention::Internal),hp);
  helper->setAttr("goldengate.queueDepth",b.getI32IntegerAttr(2));
  helper->setAttr("goldengate.queueFlow",b.getBoolAttr(false));
  helper->setAttr("goldengate.queuePipe",b.getBoolAttr(false));
  helper->setAttr("goldengate.addressBits",b.getI32IntegerAttr(34));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg=[&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant=[&](unsigned w,uint64_t n) -> Value { return b.create<ConstantOp>(loc,uint(w),APInt(w,n)); };
  auto connect=[&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  auto field=[&](Value v,llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc,v,n); };
  auto invert=[&](Value v) -> Value { return b.create<NotPrimOp>(loc,v); };
  auto both=[&](Value x,Value y) -> Value { return b.create<AndPrimOp>(loc,x,y); };
  auto mux=[&](Value c,Value y,Value n) -> Value { return b.create<MuxPrimOp>(loc,c,y,n); };
  auto reg=[&](llvm::StringRef n) -> Value { return b.create<RegResetOp>(loc,bit,arg(0),arg(1),constant(1,0),n).getResult(); };
  Value enq=reg("enq_ptr_value"),deq=reg("deq_ptr_value"),full=reg("maybe_full");
  Value equal=b.create<EQPrimOp>(loc,enq,deq);
  Value ready=invert(both(equal,full)),valid=invert(both(equal,invert(full)));
  Value push=both(ready,arg(8)),pop=both(valid,arg(13));
  connect(arg(7),ready); connect(arg(14),valid);
  connect(enq,mux(push,invert(enq),enq)); connect(deq,mux(pop,invert(deq),deq));
  connect(full,mux(b.create<XorPrimOp>(loc,push,pop),push,full));
  SmallVector<Type> types{MemOp::getTypeForPort(2,uint(34),MemOp::PortKind::Read),
      MemOp::getTypeForPort(2,uint(34),MemOp::PortKind::Write)};
  SmallVector<Attribute> portNames{b.getStringAttr("read"),b.getStringAttr("write")};
  auto ram=b.create<MemOp>(loc,types,0,1,2,RUWAttr::Undefined,portNames,"ram");
  Value rd=ram.getResult(0),wr=ram.getResult(1);
  connect(field(rd,"clk"),arg(0)); connect(field(rd,"en"),constant(1,1)); connect(field(rd,"addr"),deq);
  connect(field(wr,"clk"),arg(0)); connect(field(wr,"en"),push); connect(field(wr,"addr"),enq);
  connect(field(wr,"mask"),constant(1,1)); connect(field(wr,"data"),b.create<PadPrimOp>(loc,arg(9),34));
  Value high=b.create<RegOp>(loc,uint(2),arg(0),"R_ADDRESS_H").getResult();
  connect(high,mux(arg(3),b.create<BitsPrimOp>(loc,arg(4),1,0),high));
  connect(arg(2),constant(1,1)); connect(arg(5),constant(1,1));
  connect(arg(6),b.create<PadPrimOp>(loc,high,32));
  connect(arg(10),constant(1,0)); connect(arg(11),constant(32,0));
  connect(arg(15),b.create<BitsPrimOp>(loc,b.create<CatPrimOp>(loc,high,field(rd,"data")),33,0));
  b.create<AssertOp>(loc,arg(0),invert(arg(12)),invert(arg(1)),
      "Can only write to this decoupled sink",ValueRange{},"");
  auto token=BundleType::get(context,{{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,uint(32)}});
  auto mcr=BundleType::get(context,{{b.getStringAttr("read_6"),false,token},{b.getStringAttr("read_7"),false,token},
      {b.getStringAttr("write_6"),true,token},{b.getStringAttr("write_7"),true,token},{b.getStringAttr("wstrb"),true,uint(4)}});
  SmallVector<PortInfo> ports(inner.getPorts()); SmallVector<unsigned> copied;
  for (unsigned i=0;i<ports.size();++i) copied.push_back(i);
  unsigned first=ports.size();
  ports.push_back({b.getStringAttr(controlName),mcr,Direction::Out});
  for (unsigned i=13;i<16;++i) ports.push_back({b.getStringAttr("loadmem_"+names[i].str()),uint(widths[i]),hp[i].direction});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  SmallVector<Attribute> metadata;
  for (unsigned word=6;word<=7;++word) metadata.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr(word==6 ? "R_ADDRESS_H" : "R_ADDRESS_L")),
      b.getNamedAttr("offset",b.getI32IntegerAttr(word*4)),b.getNamedAttr("readable",b.getBoolAttr(word==6)),
      b.getNamedAttr("writeable",b.getBoolAttr(true))}));
  wrapper->setAttr("goldengate.mmioRegisters",b.getArrayAttr(metadata));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"),requests=b.create<InstanceOp>(loc,helper,"readRequests");
  auto outer=[&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) b.create<ConnectOp>(loc,
      p.direction==Direction::In ? sim.getResult(i) : outer(i),p.direction==Direction::In ? outer(i) : sim.getResult(i));
  connect(requests.getResult(0),outer(*clock)); connect(requests.getResult(1),outer(*reset));
  for (unsigned word=6;word<=7;++word) {
    auto rdWord=field(outer(first),"read_"+std::to_string(word)),wrWord=field(outer(first),"write_"+std::to_string(word));
    unsigned start=word==6 ? 2 : 7;
    connect(field(wrWord,"ready"),requests.getResult(start));
    connect(requests.getResult(start+1),field(wrWord,"valid"));
    connect(requests.getResult(start+2),field(wrWord,"bits"));
    connect(field(rdWord,"valid"),requests.getResult(start+3));
    connect(field(rdWord,"bits"),requests.getResult(start+4));
    if (word==7) connect(requests.getResult(12),field(rdWord,"ready"));
  }
  connect(requests.getResult(13),outer(first+1)); connect(outer(first+2),requests.getResult(14)); connect(outer(first+3),requests.getResult(15));
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
