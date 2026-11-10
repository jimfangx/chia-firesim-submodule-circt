// See LICENSE for license details.
// CPUManagedStreamEngine.elaborateToHostCPUStream and implementStreams.
// Requires: uninstantiated active top, host clock/reset, ordered buffered
// Decoupled512 outputs and retained annotations. Rejects unsupported boundaries before
// mutation. The 16-bit ID, 64-bit address and equal-width adapter match U250.
// Consumes no annotations; copied targets move to the new wrapper, buffered
// stream targets remain on the inner module. Adds native read transport and
// its wrapper, replacing the external stream with CPU AXI AR/R scalar ports.
// No cached analyses. Count MMIO, writes and driver/header emission are separate
// stages. AR is held until the last R handshake, as in the Scala oracle.
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/CPUStreamRead.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/MathExtras.h"
#include <limits>
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::validateF2CPUStreamAckRoute(CircuitOp circuit,
                                                     std::string &error) {
  auto reject=[&](StringRef why){error="F2 explicit source ACK route differs: "+why.str();return failure();};
  FModuleOp read,bank,rw,cw;
  for(auto m:circuit.getOps<FModuleOp>()) {
    if(m.getName()=="GGCPUStreamRead")read=m;
    if(m.getName()=="GGCPUStreamCountBank")bank=m;
    if(m.getName()=="GGCPUStreamReadWrapper")rw=m;
    if(m.getName()=="GGCPUStreamCountWrapper")cw=m;
  }
  auto sources=read?read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams"):ArrayAttr{};
  if(!read||!bank||!rw||!cw||!sources||sources.empty())return reject("module inventory");
  if(bank.getNumPorts()<4)return reject("count port inventory");
  unsigned n=sources.size(),total=bank.getNumPorts()-4;
  auto type=FVectorType::get(UIntType::get(circuit.getContext(),1),n);
  for(auto m:{read,bank,rw})
    if(m.getPorts().back().name!="sdkReadAck"||m.getPorts().back().type!=type||
       m.getPorts().back().direction!=(m==bank?Direction::Out:Direction::In))return reject("port inventory");
  SmallVector<InstanceOp> ri,bi,wi;
  circuit.walk([&](InstanceOp i){
    if(i.getModuleName()==read.getName())ri.push_back(i);
    if(i.getModuleName()==bank.getName())bi.push_back(i);
    if(i.getModuleName()==rw.getName())wi.push_back(i);
  });
  if(ri.size()!=1||bi.size()!=1||wi.size()!=1||ri[0]->getParentOfType<FModuleOp>()!=rw||
     bi[0]->getParentOfType<FModuleOp>()!=cw||wi[0]->getParentOfType<FModuleOp>()!=cw)
    return reject("instance inventory");
  auto driver=[&](FModuleOp m,Value dest)->Value{
    Value value;unsigned count=0;
    for(auto c:m.getOps<StrictConnectOp>())if(c.getDest()==dest){value=c.getSrc();++count;}
    for(auto c:m.getOps<ConnectOp>())if(c.getDest()==dest){value=c.getSrc();++count;}
    return count==1?value:Value{};
  };
  if(driver(rw,ri[0].getResults().back())!=rw.getBodyBlock()->getArguments().back()||
     driver(cw,wi[0].getResults().back())!=bi[0].getResults().back())return reject("live hierarchy wiring");
  auto operand=[](Value v,StringRef name,unsigned i)->Value{
    auto *op=v?v.getDefiningOp():nullptr;
    return op&&op->getName().getStringRef()==name&&i<op->getNumOperands()?op->getOperand(i):Value{};
  };
  auto eq=[](Value v,Value lhs,uint64_t rhs){
    auto e=v?v.getDefiningOp<EQPrimOp>():EQPrimOp{};
    auto k=e?e.getRhs().getDefiningOp<ConstantOp>():ConstantOp{};
    return e&&e.getLhs()==lhs&&k&&k.getValue()==rhs;
  };
  Value mcr=bank.getArgument(total+2),reset=bank.getArgument(1);
  auto field=[](Value v,StringRef name,Value input){auto f=v?v.getDefiningOp<SubfieldOp>():SubfieldOp{};return f&&f.getInput()==input&&f.getFieldName()==name;};
  auto slot=[&](Value v,StringRef name,unsigned i){
    auto f=v?v.getDefiningOp<SubfieldOp>():SubfieldOp{};
    auto ix=f?f.getInput().getDefiningOp<SubindexOp>():SubindexOp{};
    return f&&f.getFieldName()==name&&ix&&ix.getIndex()==i&&field(ix.getInput(),"write",mcr);
  };
  auto registers=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if(!registers||registers.size()!=total||total<n)return reject("count registry");
  unsigned ackDrivers=0; SmallVector<bool> ackSeen(n,false);
  for(auto c:bank.getOps<StrictConnectOp>()) {
    auto ix=c.getDest().getDefiningOp<SubindexOp>();
    if(!ix||ix.getInput()!=bank.getBodyBlock()->getArguments().back())continue;
    ++ackDrivers;unsigned i=ix.getIndex();if(i>=n||ackSeen[i])return reject("ACK index");ackSeen[i]=true;
    Value outer=c.getSrc(),inner=operand(outer,"firrtl.and",0),notReset=operand(outer,"firrtl.and",1);
    Value valid=operand(inner,"firrtl.and",0),accepted=operand(inner,"firrtl.and",1);
    Value checks=operand(accepted,"firrtl.and",0),nonempty=operand(accepted,"firrtl.and",1);
    Value dataEQ=operand(checks,"firrtl.and",0),strobeEQ=operand(checks,"firrtl.and",1);
    auto de=dataEQ?dataEQ.getDefiningOp<EQPrimOp>():EQPrimOp{};
    auto se=strobeEQ?strobeEQ.getDefiningOp<EQPrimOp>():EQPrimOp{};
    if(!slot(valid,"valid",i)||!de||!slot(de.getLhs(),"bits",i)||!eq(dataEQ,de.getLhs(),1)||
       !se||!field(se.getLhs(),"wstrb",mcr)||!eq(strobeEQ,se.getLhs(),15)||
       !eq(operand(nonempty,"firrtl.not",0),bank.getArgument(i+2),0)||
       operand(notReset,"firrtl.not",0)!=reset)return reject("ACK write predicate");
    unsigned assertions=0;
    for(auto a:bank.getOps<AssertOp>()) {
      Value pred=a.getPredicate();
      if(operand(operand(pred,"firrtl.or",0),"firrtl.not",0)!=valid)continue;
      if(operand(pred,"firrtl.or",1)!=accepted||a.getClock()!=bank.getArgument(0)||
         operand(a.getEnable(),"firrtl.not",0)!=reset||
         a.getMessage()!="F2 CPU stream ACK requires data1, full strobes, and a nonempty source")return reject("ACK assertion");
      ++assertions;
    }
    if(assertions!=1)return reject("ACK assertion inventory");
  }
  if(ackDrivers!=n)return reject("ACK driver inventory");
  for(unsigned i=0;i<total;++i){
    auto d=dyn_cast<DictionaryAttr>(registers[i]);auto wr=d?d.getAs<BoolAttr>("writeable"):BoolAttr{};
    if(!wr||wr.getValue()!=(i<n))return reject("source-only ACK write permissions");
  }
  // Check the actual AXI register file and every aggregate connection to the
  // count bank. A valid bank-local predicate cannot authorize a tied-off bus.
  FModuleOp file; SmallVector<InstanceOp> fi,ci;
  for(auto m:circuit.getOps<FModuleOp>())if(m.getName()=="GGCPUStreamMCRFile")file=m;
  if(!file||file.getNumPorts()!=4)return reject("control register file");
  circuit.walk([&](InstanceOp i){if(i.getModuleName()==file.getName())fi.push_back(i);if(i.getModuleName()==cw.getName())ci.push_back(i);});
  if(fi.size()!=1||ci.size()!=1)return reject("control/count instance inventory");
  auto root=[](Value v) {
    while (true) {
      if(auto f=v.getDefiningOp<SubfieldOp>()){v=f.getInput();continue;}
      if(auto i=v.getDefiningOp<SubindexOp>()){v=i.getInput();continue;}
      if(auto i=v.getDefiningOp<SubaccessOp>()){v=i.getInput();continue;}
      return v;
    }
  };
  // NIC read/write/MMIO wrappers may separate the MCRFile from CountWrapper.
  // Follow the actual aggregate connection at each boundary, checking both
  // ends: flipped write fields can be overridden at the source end as well.
  auto mcrType=bank.getPortType(total+2);
  auto owner=fi[0]->getParentOfType<FModuleOp>();
  Value dest=fi[0].getResult(3);
  llvm::SmallPtrSet<Operation *,8> visited;
  bool sawCount=false;
  while(true) {
    if(!visited.insert(owner.getOperation()).second || dest.getType()!=mcrType)
      return reject("cyclic or mistyped MCR aggregate route");
    Value source=driver(owner,dest);
    if(!source||source.getType()!=mcrType||root(source)!=source)
      return reject("missing or mistyped MCR aggregate driver");
    bool override=false;
    unsigned drivers=0;
    owner.walk([&](Operation *op) {
      Value target;
      if(auto c=dyn_cast<StrictConnectOp>(op))target=c.getDest();
      if(auto c=dyn_cast<ConnectOp>(op))target=c.getDest();
      if(!target)return;
      Value base=root(target);
      if(base!=dest&&base!=source)return;
      if(target!=base || target==source || op->getBlock()!=owner.getBodyBlock())override=true;
      if(target==dest)++drivers;
    });
    if(override||drivers!=1)return reject("partial or ambiguous MCR aggregate route");
    if(owner==cw)sawCount=true;
    if(source==bi[0].getResult(total+2)) {
      if(!sawCount||owner!=cw)return reject("MCR aggregate bypassed count wrapper");
      break;
    }
    auto result=dyn_cast<OpResult>(source);
    auto child=result?dyn_cast<InstanceOp>(result.getOwner()):InstanceOp{};
    FModuleOp childModule;
    if(child)for(auto m:circuit.getOps<FModuleOp>())
      if(m.getName()==child.getModuleName())childModule=m;
    if(!childModule||result.getResultNumber()>=childModule.getNumPorts())
      return reject("MCR aggregate is not a module output");
    unsigned port=result.getResultNumber();
    auto info=childModule.getPorts()[port];
    if(info.name!="cpuStream_mcr"||info.direction!=Direction::Out||info.type!=mcrType)
      return reject("MCR wrapper port inventory");
    unsigned instances=0;
    circuit.walk([&](InstanceOp i){if(i.getModuleName()==childModule.getName())++instances;});
    if(instances!=1)return reject("ambiguous MCR wrapper instance");
    owner=childModule;
    dest=owner.getArgument(port);
  }
  Value held,data; StrictConnectOp dataDriver;
  for(auto c:file.getOps<StrictConnectOp>()) {
    if(field(c.getDest(),"wstrb",file.getArgument(3))) {if(held)return reject("multiple strobe drivers");held=c.getSrc();}
    auto r=c.getDest().getDefiningOp<RegOp>();
    if(r&&r->getAttrOfType<StringAttr>("name")=="wData"){if(dataDriver)return reject("multiple W-data drivers");data=c.getDest();dataDriver=c;}
  }
  auto reg=held?held.getDefiningOp<RegOp>():RegOp{};
  Value heldNext=held?driver(file,held):Value{};
  auto heldMux=heldNext?heldNext.getDefiningOp<MuxPrimOp>():MuxPrimOp{};
  auto dataMux=dataDriver?dataDriver.getSrc().getDefiningOp<MuxPrimOp>():MuxPrimOp{};
  if(!reg||reg.getClockVal()!=file.getArgument(0)||reg.getResult().getType()!=UIntType::get(circuit.getContext(),4)||
     !heldMux||!dataMux||heldMux.getSel()!=dataMux.getSel()||heldMux.getLow()!=held||dataMux.getLow()!=data)
    return reject("held W-strobe/data capture");
  auto strb=heldMux.getHigh().getDefiningOp<SubfieldOp>();
  auto bitsField=strb?strb.getInput().getDefiningOp<SubfieldOp>():SubfieldOp{};
  auto w=bitsField?bitsField.getInput().getDefiningOp<SubfieldOp>():SubfieldOp{};
  Value gate=heldMux.getSel();
  auto ready=operand(gate,"firrtl.and",0).getDefiningOp<SubfieldOp>();
  auto valid=operand(gate,"firrtl.and",1).getDefiningOp<SubfieldOp>();
  auto dataField=dataMux.getHigh().getDefiningOp<SubfieldOp>();
  auto dataBits=dataField?dataField.getInput().getDefiningOp<SubfieldOp>():SubfieldOp{};
  auto dataW=dataBits?dataBits.getInput().getDefiningOp<SubfieldOp>():SubfieldOp{};
  if(!strb||strb.getFieldName()!="strb"||!bitsField||bitsField.getFieldName()!="bits"||
     !w||w.getFieldName()!="w"||w.getInput()!=file.getArgument(2)||
     !ready||ready.getFieldName()!="ready"||!field(ready.getInput(),"w",file.getArgument(2))||
     !valid||valid.getFieldName()!="valid"||!field(valid.getInput(),"w",file.getArgument(2))||
     !dataField||dataField.getFieldName()!="data"||!dataBits||dataBits.getFieldName()!="bits"||
     !dataW||dataW.getFieldName()!="w"||dataW.getInput()!=file.getArgument(2))
    return reject("AXI W handshake strobe origin");
  return success();
}

LogicalResult goldengate::validateF2CPUStreamWordReads(FModuleOp read,
                                                      std::string &error) {
  auto reject = [&](StringRef why = "metadata") {
    error = "F2 CPU DWORD profile differs: " + why.str(); return failure();
  };
  auto profile = read->getAttrOfType<DictionaryAttr>("goldengate.f2CPUWordReads");
  auto sources = read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams");
  auto space = read->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits");
  if (!profile || profile.size() != 9 || !sources || sources.empty() || !space) return reject();
  for (auto field : {std::pair<StringRef, int64_t>{"wordBytes", 4}, {"streamBeatBytes", 64},
                     {"ackWriteValue", 1}, {"pciRequestID", 0x20}, {"maxNativeNarrowReadBytes", 32}}) {
    auto value = profile.getAs<IntegerAttr>(field.first);
    if (!value || value.getInt() != field.second) return reject();
  }
  for (StringRef field : {"shellNormalizedSize6", "pciReadsNonDestructive", "fullWidthDMABurstsUnchanged", "explicitCountWriteAck"}) {
    auto value = profile.getAs<BoolAttr>(field);
    if (!value || !value.getValue()) return reject();
  }
  auto port = [&](StringRef name, unsigned width) -> Value {
    for (auto [i, info] : llvm::enumerate(read.getPorts()))
      if (info.name == name && info.direction == Direction::In &&
          info.type == UIntType::get(read.getContext(), width)) return read.getArgument(i);
    return {};
  };
  Value address = port("ar_bits_addr", 64), size = port("ar_bits_size", 3), length = port("ar_bits_len", 8);
  Value id = port("ar_bits_id", 16), valid = port("ar_valid", 1), rready = port("r_ready", 1), reset = port("reset", 1);
  if (!address || !size || !length || !id || !valid || !rready || !reset ||
      read.getPortName(0) != "clock" || read.getPortType(0) != ClockType::get(read.getContext())) return reject("port interface");
  auto operand = [&](Value value, StringRef name, unsigned index) -> Value {
    auto *op = value ? value.getDefiningOp() : nullptr;
    return op && op->getName().getStringRef() == name && index < op->getNumOperands() ? op->getOperand(index) : Value{};
  };
  auto eq = [&](Value value, Value lhs, uint64_t rhs) {
    auto operation = value ? value.getDefiningOp<EQPrimOp>() : EQPrimOp{};
    auto constant = operation ? operation.getRhs().getDefiningOp<ConstantOp>() : ConstantOp{};
    return operation && operation.getLhs() == lhs && constant && constant.getValue() == rhs;
  };
  auto bits = [&](Value value, unsigned hi, unsigned lo) {
    auto operation = value ? value.getDefiningOp<BitsPrimOp>() : BitsPrimOp{};
    return operation && operation.getInput() == address && operation.getHi() == hi && operation.getLo() == lo;
  };
  SmallVector<StrictConnectOp> ready(sources.size());
  for (auto connect : read.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    auto argument = field ? dyn_cast<BlockArgument>(field.getInput()) : BlockArgument{};
    if (!argument || field.getFieldName() != "ready") continue;
    unsigned index = argument.getArgNumber();
    if (index < 2 || index >= 2 + sources.size() || ready[index - 2]) return reject();
    ready[index - 2] = connect;
  }
  if (llvm::any_of(ready, [](auto connect) { return !connect; })) return reject("missing source handshake");
  Value firstDMA = operand(ready.front().getSrc(), "firrtl.or", 0);
  Value full = operand(firstDMA, "firrtl.and", 1);
  Value fullSize = operand(full, "firrtl.and", 0), notPeek = operand(full, "firrtl.and", 1);
  Value pci = operand(notPeek, "firrtl.not", 0);
  auto ackType = FVectorType::get(UIntType::get(read.getContext(), 1), sources.size());
  if (read.getNumPorts() != sources.size() + 15 || read.getPorts().back().name != "sdkReadAck" ||
      read.getPorts().back().direction != Direction::In || read.getPorts().back().type != ackType)
    return reject("explicit ACK input");
  Value ack = read.getBodyBlock()->getArguments().back();
  if (!eq(fullSize, size, 6) || !eq(pci, id, 0x20)) return reject("SDK consume predicate");
  for (auto [i, connect] : llvm::enumerate(ready)) {
    Value dma = operand(connect.getSrc(), "firrtl.or", 0);
    Value legacy = operand(dma, "firrtl.and", 0);
    Value grant = operand(legacy, "firrtl.and", 0), handshake = operand(legacy, "firrtl.and", 1);
    auto grantEQ = grant ? grant.getDefiningOp<EQPrimOp>() : EQPrimOp{};
    auto ackSlot = operand(connect.getSrc(), "firrtl.or", 1).getDefiningOp<SubindexOp>();
    if (operand(dma, "firrtl.and", 1) != full || !grantEQ ||
        !bits(grantEQ.getLhs(), 63, space.getInt()) || !eq(grant, grantEQ.getLhs(), i) ||
        operand(handshake, "firrtl.and", 0) != valid || operand(handshake, "firrtl.and", 1) != rready ||
        !ackSlot || ackSlot.getInput() != ack || ackSlot.getIndex() != i)
      return reject("source grant/ACK dequeue predicate");
  }
  Value narrow;
  for (auto assertion : read.getOps<AssertOp>())
    narrow = operand(operand(assertion.getPredicate(), "firrtl.or", 1), "firrtl.or", 1);
  Value rest = operand(narrow, "firrtl.and", 1);
  Value single = operand(rest, "firrtl.and", 0), sizes = operand(rest, "firrtl.and", 1);
  if (operand(narrow, "firrtl.and", 0) != pci || !eq(single, length, 0))
    return reject("SDK single read predicate");
  SmallVector<Value> alignmentChecks;
  // Inspect every size/alignment branch independently. Metadata alone cannot
  // authorize wider PCI loads or a weakened alignment/cross-line predicate.
  for (unsigned nativeSize = 5; nativeSize >= 2; --nativeSize) {
    Value branch = operand(sizes, "firrtl.or", 1);
    Value sizeEQ = operand(branch, "firrtl.and", 0);
    Value aligned = operand(branch, "firrtl.and", 1);
    auto alignedEQ = aligned ? aligned.getDefiningOp<EQPrimOp>() : EQPrimOp{};
    if (!eq(sizeEQ, size, nativeSize) || !alignedEQ ||
        !bits(alignedEQ.getLhs(), nativeSize - 1, 0) ||
        !eq(aligned, alignedEQ.getLhs(), 0))
      return reject("SDK native narrow read size/alignment");
    alignmentChecks.push_back(aligned);
    sizes = operand(sizes, "firrtl.or", 0);
  }
  Value normalizedAlignment = operand(sizes, "firrtl.and", 1);
  auto normalizedEQ = normalizedAlignment ? normalizedAlignment.getDefiningOp<EQPrimOp>() : EQPrimOp{};
  if (operand(sizes, "firrtl.and", 0) != fullSize || !normalizedEQ ||
      !bits(normalizedEQ.getLhs(), 1, 0) ||
      !eq(normalizedAlignment, normalizedEQ.getLhs(), 0))
    return reject("SDK shell-normalized read alignment");
  alignmentChecks.push_back(normalizedAlignment);
  unsigned assertions = 0;
  for (auto assertion : read.getOps<AssertOp>()) {
    ++assertions;
    Value predicate = assertion.getPredicate(), alternatives = operand(predicate, "firrtl.or", 1);
    if (assertion.getMessage() != "F2 CPU stream reads require full DMA beats or aligned PCI-ID0x20 single 4-to-64-byte peeks" ||
        assertion.getClock() != read.getArgument(0) || operand(assertion.getEnable(), "firrtl.not", 0) != reset ||
        operand(operand(predicate, "firrtl.or", 0), "firrtl.not", 0) != valid ||
        operand(alternatives, "firrtl.or", 0) != full || operand(alternatives, "firrtl.or", 1) != narrow)
      return reject("transaction assertion");
  }
  if (assertions != 1) return reject("assertion inventory");
  // Keep low-address alignment tests distinct from high-address grants.
  // Reject extra ambiguous address comparisons beyond the validated branches.
  for (auto comparison : read.getOps<EQPrimOp>()) {
    auto slice = comparison.getLhs().getDefiningOp<BitsPrimOp>();
    if (slice && slice.getInput() == address && slice.getHi() != 63 && !llvm::is_contained(alignmentChecks, comparison.getResult()))
      return reject("unexpected address comparison");
  }
  return validateF2CPUStreamAckRoute(read->getParentOfType<CircuitOp>(), error);
}

LogicalResult goldengate::enableF2CPUStreamWordReads(CircuitOp circuit,
                                                    std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  FModuleOp read;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == "GGCPUStreamRead") read = module;
  auto sources = read ? read->getAttrOfType<ArrayAttr>("goldengate.sourceStreams") : ArrayAttr{};
  if (!read || !sources || sources.empty() || read->hasAttr("goldengate.f2CPUWordReads"))
    return reject("F2 DWORD reads require one unmodified native CPU source engine");
  auto *ctx = circuit.getContext();
  auto port = [&](StringRef name, unsigned width) -> Value {
    for (auto [index, info] : llvm::enumerate(read.getPorts()))
      if (info.name == name && info.direction == Direction::In &&
          info.type == UIntType::get(ctx, width)) return read.getArgument(index);
    return {};
  };
  Value address = port("ar_bits_addr", 64), size = port("ar_bits_size", 3);
  Value length = port("ar_bits_len", 8), valid = port("ar_valid", 1), id = port("ar_bits_id", 16);
  if (!address || !size || !length || !valid || !id)
    return reject("F2 DWORD reads require the native address/size/length interface");
  SmallVector<StrictConnectOp> ready;
  for (auto connect : read.getOps<StrictConnectOp>()) {
    auto field = connect.getDest().getDefiningOp<SubfieldOp>();
    auto argument = field ? dyn_cast<BlockArgument>(field.getInput()) : BlockArgument{};
    if (!argument || field.getFieldName() != "ready") continue;
    auto name = read.getPortName(argument.getArgNumber());
    if (name == "stream" || name.starts_with("stream_")) ready.push_back(connect);
  }
  AssertOp assertion;
  for (auto candidate : read.getOps<AssertOp>()) {
    if (assertion || candidate.getMessage() != "CPUManagedStreamEngine requires 64-byte read beats")
      return reject("F2 DWORD reads require the unchanged full-width transaction assertion");
    assertion = candidate;
  }
  if (ready.size() != sources.size() || !assertion)
    return reject("F2 DWORD reads require every live source dequeue and the size assertion");
  FModuleOp bank, readWrapper, countWrapper, controlFile;
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module.getName() == "GGCPUStreamCountBank") bank = module;
    if (module.getName() == "GGCPUStreamMCRFile") controlFile = module;
    if (module.getName() == "GGCPUStreamReadWrapper") readWrapper = module;
    if (module.getName() == "GGCPUStreamCountWrapper") countWrapper = module;
  }
  if (!bank || !readWrapper || !countWrapper || !controlFile || controlFile.getNumPorts()!=4 || bank.getNumPorts() < sources.size() + 3)
    return reject("F2 explicit ACK requires the canonical count bank and wrappers");
  SmallVector<InstanceOp> readUses, bankUses, wrapperUses;
  circuit.walk([&](InstanceOp instance) {
    if (instance.getModuleName() == read.getName()) readUses.push_back(instance);
    if (instance.getModuleName() == bank.getName()) bankUses.push_back(instance);
    if (instance.getModuleName() == readWrapper.getName()) wrapperUses.push_back(instance);
  });
  if (readUses.size()!=1 || bankUses.size()!=1 || wrapperUses.size()!=1 ||
      readUses[0]->getParentOfType<FModuleOp>() != readWrapper ||
      bankUses[0]->getParentOfType<FModuleOp>() != countWrapper ||
      wrapperUses[0]->getParentOfType<FModuleOp>() != countWrapper)
    return reject("F2 explicit ACK requires unique nested read/count instances");
  StrictConnectOp strobeConnect, dataConnect;
  for (auto c : controlFile.getOps<StrictConnectOp>()) {
    auto f=c.getDest().getDefiningOp<SubfieldOp>();
    if (f && f.getInput()==controlFile.getArgument(3) && f.getFieldName()=="wstrb") {
      if(strobeConnect) return reject("ambiguous CPU count MCR strobes"); strobeConnect=c;
    }
    auto reg=c.getDest().getDefiningOp<RegOp>();
    if(reg && reg->getAttrOfType<StringAttr>("name")=="wData") dataConnect=c;
  }
  auto dataMux=dataConnect?dataConnect.getSrc().getDefiningOp<MuxPrimOp>():MuxPrimOp{};
  auto oldStrobe=strobeConnect?strobeConnect.getSrc().getDefiningOp<ConstantOp>():ConstantOp{};
  if(!dataMux||!oldStrobe||!oldStrobe.getValue().isZero())
    return reject("F2 ACK strobes require the unchanged CPU count MCR W-data capture");
  // Validate completely before modifying the engine. These conditions use
  // only stable, held AR fields and add no state or payload transformation.
  OpBuilder b(ctx); auto loc = read.getLoc(); b.setInsertionPointToStart(read.getBodyBlock());
  auto k = [&](unsigned width, uint64_t value) -> Value {
    return b.create<ConstantOp>(loc, UIntType::get(ctx, width), APInt(width, value));
  };
  // Capture WSTRB on precisely the same handshake as WDATA, including W-before-AW.
  // The generic legacy MCRFile intentionally drives zero because old bindReg
  // clients ignore strobes. Only this F2 explicit-ACK endpoint extends it.
  b.setInsertionPoint(dataConnect);
  Value heldStrobes=b.create<RegOp>(loc,UIntType::get(ctx,4),controlFile.getArgument(0),"f2AckWriteStrobe").getResult();
  Value w=b.create<SubfieldOp>(loc,controlFile.getArgument(2),"w");
  Value wbits=b.create<SubfieldOp>(loc,w,"bits");
  Value strobes=b.create<SubfieldOp>(loc,wbits,"strb");
  Value nextStrobes=b.create<MuxPrimOp>(loc,dataMux.getSel(),strobes,heldStrobes);
  b.create<StrictConnectOp>(loc,heldStrobes,nextStrobes);
  strobeConnect->setOperand(1,heldStrobes);
  b.setInsertionPointToStart(read.getBodyBlock());
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  Value fullSize = b.create<EQPrimOp>(loc, size, k(3, 6));
  Value single = b.create<EQPrimOp>(loc, length, k(8, 0));
  auto aligned = [&](unsigned logBytes) -> Value {
    return b.create<EQPrimOp>(loc,
        b.create<BitsPrimOp>(loc, address, logBytes - 1, 0), k(logBytes, 0));
  };
  // The shell BFM normalizes all PCI peek widths to ARSIZE6 while retaining
  // their low address lanes. Keep that profile and also admit naturally
  // aligned native 4/8/16/32-byte requests. Natural alignment prevents a
  // request crossing the 64-byte head; PCI peeks must always be single-beat.
  // Every PCI width is non-destructive, including speculative full-line
  // fetches generated by WC streaming loads. Only software ACK owns dequeue.
  Value pci = b.create<EQPrimOp>(loc, id, k(16, 0x20));
  Value sizes = both(fullSize, aligned(2));
  for (unsigned nativeSize = 2; nativeSize <= 5; ++nativeSize)
    sizes = either(sizes, both(b.create<EQPrimOp>(loc, size, k(3, nativeSize)), aligned(nativeSize)));
  Value narrow = both(pci, both(single, sizes));
  Value fullBeat = both(fullSize, b.create<NotPrimOp>(loc, pci));
  auto ackType = FVectorType::get(UIntType::get(ctx, 1), sources.size());
  auto append = [&](FModuleOp module, Direction direction, InstanceOp instance) {
    unsigned old = module.getNumPorts();
    SmallVector<std::pair<unsigned, PortInfo>> ports{{old,
        PortInfo(b.getStringAttr("sdkReadAck"), ackType, direction)}};
    module.insertPorts(ports);
    auto replacement = instance.cloneAndInsertPorts(ports);
    for (auto attr : instance->getAttrs())
      if (!replacement->hasAttr(attr.getName())) replacement->setAttr(attr.getName(), attr.getValue());
    for (unsigned i=0; i<old; ++i) instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
    instance.erase(); return replacement;
  };
  auto readInstance = append(read, Direction::In, readUses[0]);
  auto bankInstance = append(bank, Direction::Out, bankUses[0]);
  auto wrapperInstance = append(readWrapper, Direction::In, wrapperUses[0]);
  b.setInsertionPointToEnd(readWrapper.getBodyBlock());
  b.create<StrictConnectOp>(loc, readInstance.getResults().back(), readWrapper.getBodyBlock()->getArguments().back());
  b.setInsertionPointToEnd(countWrapper.getBodyBlock());
  b.create<StrictConnectOp>(loc, wrapperInstance.getResults().back(), bankInstance.getResults().back());
  b.setInsertionPointToStart(read.getBodyBlock());
  SmallVector<Value> ackSlots;
  for (unsigned i=0; i<sources.size(); ++i)
    ackSlots.push_back(b.create<SubindexOp>(loc, read.getBodyBlock()->getArguments().back(), i));
  for (auto [i, connect] : llvm::enumerate(ready)) {
    b.setInsertionPoint(connect);
    connect->setOperand(1, either(both(connect.getSrc(), fullBeat), ackSlots[i]));
  }
  // Reuse the source-count words as explicit, ordered software consume ACKs.
  // PCI reads may speculate or coalesce: they only observe the current head.
  b.setInsertionPointToStart(bank.getBodyBlock());
  unsigned countSlots = bank.getNumPorts() - 4;
  Value mcr = bank.getArgument(countSlots + 2);
  auto field = [&](Value value, StringRef name)->Value {return b.create<SubfieldOp>(loc,value,name);};
  auto registry = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  SmallVector<Attribute> updated(registry.begin(), registry.end());
  SmallVector<AssertOp> oldAssertions;
  for (auto a : bank.getOps<AssertOp>()) oldAssertions.push_back(a);
  for (unsigned i=0; i<sources.size(); ++i) {
    Value slot=b.create<SubindexOp>(loc,field(mcr,"write"),i);
    Value wrValid=field(slot,"valid"), wrBits=field(slot,"bits");
    Value dataOK=b.create<EQPrimOp>(loc,wrBits,k(32,1));
    Value strobeOK=b.create<EQPrimOp>(loc,field(mcr,"wstrb"),k(4,15));
    Value nonempty=b.create<NotPrimOp>(loc,b.create<EQPrimOp>(loc,bank.getArgument(i+2),
        k(*cast<UIntType>(bank.getPortType(i+2)).getWidth(),0)));
    Value accepted=both(both(dataOK,strobeOK),nonempty);
    Value pulse=both(both(wrValid,accepted),b.create<NotPrimOp>(loc,bank.getArgument(1)));
    b.create<StrictConnectOp>(loc,b.create<SubindexOp>(loc,bank.getBodyBlock()->getArguments().back(),i),pulse);
    b.create<AssertOp>(loc,bank.getArgument(0),either(b.create<NotPrimOp>(loc,wrValid),accepted),
        b.create<NotPrimOp>(loc,bank.getArgument(1)),"F2 CPU stream ACK requires data1, full strobes, and a nonempty source",ValueRange{},"");
    oldAssertions[i].erase();
    NamedAttrList word(cast<DictionaryAttr>(updated[i])); word.set("writeable",b.getBoolAttr(true));
    updated[i]=word.getDictionary(ctx);
  }
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(updated));
  b.setInsertionPoint(assertion);
  Value allowed = either(b.create<NotPrimOp>(loc, valid), either(fullBeat, narrow));
  assertion.getPredicateMutable().set(allowed);
  assertion.setMessage("F2 CPU stream reads require full DMA beats or aligned PCI-ID0x20 single 4-to-64-byte peeks");
  read->setAttr("goldengate.f2CPUWordReads", b.getDictionaryAttr({
      b.getNamedAttr("wordBytes", b.getI64IntegerAttr(4)),
      b.getNamedAttr("maxNativeNarrowReadBytes", b.getI64IntegerAttr(32)),
      b.getNamedAttr("streamBeatBytes", b.getI64IntegerAttr(64)),
      b.getNamedAttr("ackWriteValue", b.getI64IntegerAttr(1)),
      b.getNamedAttr("pciRequestID", b.getI64IntegerAttr(0x20)),
      b.getNamedAttr("shellNormalizedSize6", b.getBoolAttr(true)),
      b.getNamedAttr("pciReadsNonDestructive", b.getBoolAttr(true)),
      b.getNamedAttr("fullWidthDMABurstsUnchanged", b.getBoolAttr(true)),
      b.getNamedAttr("explicitCountWriteAck", b.getBoolAttr(true))}));
  return success();
}

LogicalResult goldengate::deriveRocketCPUStreamPorts(CircuitOp circuit,
    SmallVectorImpl<CPUStreamSourcePort> &sources,
    SmallVectorImpl<CPUStreamCountPort> &counts, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto find = [&](StringRef name) -> FModuleOp {
    for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
    return {};
  };
  auto top = find(circuit.getName());
  if (!top) return reject("Rocket CPU stream catalog requires an active top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto bit = UIntType::get(ctx, 1), word = UIntType::get(ctx, 512);
  auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, word}});
  // Preserve constructor order, regardless of the order of top-level ports.
  // Every canonical producer must have a count; never silently drain only zero.
  SmallVector<Value> data(top.getNumPorts()), count(top.getNumPorts());
  unsigned banks = 0, countBanks = 0;
  for (auto [i, p] : llvm::enumerate(top.getPorts())) {
    StringRef portName = p.name.getValue();
    bool isCount = portName.starts_with("tracerv_stream_count");
    StringRef base = isCount ? "tracerv_stream_count" : "tracerv_stream";
    if (!portName.starts_with(base)) continue;
    StringRef suffix = portName.drop_front(base.size());
    unsigned index = 0;
    if (!suffix.empty() && (!suffix.consume_front("_") || suffix.empty() ||
        suffix.getAsInteger(10, index) || index == 0 || suffix != std::to_string(index)))
      return reject("Rocket CPU catalog has a noncanonical TracerV port");
    if (index >= data.size()) return reject("Rocket CPU catalog has a sparse TracerV index");
    auto &ports = isCount ? count : data;
    if (ports[index] || p.direction != Direction::Out ||
        p.type != (isCount ? Type(UIntType::get(ctx, 13)) : Type(token)))
      return reject("Rocket CPU catalog has duplicate or mistyped TracerV ports");
    ports[index] = top.getArgument(i);
    (isCount ? countBanks : banks)++;
  }
  if (!banks || banks != countBanks)
    return reject("Rocket CPU stream catalog needs paired buffered TracerV payload/count ports");
  for (unsigned i = 0; i < banks; ++i)
    if (!data[i] || !count[i]) return reject("Rocket CPU catalog has a missing TracerV payload/count pair");
  using Path = SmallVector<InstanceOp>;
  struct Endpoint { FModuleOp queue; Path path; unsigned port = 0; };
  auto trace = [&](Value value) -> Endpoint {
    Path path; SmallVector<Value> seen;
    while (value && seen.size() < 4096) {
      if (llvm::is_contained(seen, value)) return {};
      seen.push_back(value);
      Value driver; unsigned drivers = 0;
      for (auto &op : *value.getParentBlock()) {
        if (auto c = dyn_cast<ConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
        if (auto c = dyn_cast<StrictConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
      }
      if (drivers) { if (drivers != 1) return {}; value = driver; continue; }
      auto inst = value.getDefiningOp<InstanceOp>();
      auto module = inst ? find(inst.getModuleName()) : FModuleOp{};
      if (!module) return {};
      unsigned port = cast<OpResult>(value).getResultNumber();
      if (module.getPortDirection(port) != Direction::Out) return {};
      path.push_back(inst);
      if (module.getName() == "GGTracerVStreamQueue6144" ||
          module.getName().starts_with("GGTracerVStreamQueue6144_")) return {module, path, port};
      value = module.getArgument(port);
    }
    return {};
  };
  SmallVector<CPUStreamSourcePort> outgoing;
  SmallVector<CPUStreamCountPort> registers;
  SmallVector<Path> queuePaths;
  for (unsigned i = 0; i < banks; ++i) {
    auto payload = trace(data[i]), occupancy = trace(count[i]);
    if (!payload.queue || !occupancy.queue || payload.path != occupancy.path ||
        payload.port != 3 || occupancy.port != 4)
      return reject("Rocket CPU payload/count must forward the same live TracerV queue instance");
    if (llvm::is_contained(queuePaths, payload.path))
      return reject("Rocket CPU catalog producers alias one queue instance");
    queuePaths.push_back(payload.path);
    auto queue = payload.queue;
    std::string suffix = i ? "_" + std::to_string(i) : "";
    std::string streamName = "TRACERVBRIDGEMODULE_" + std::to_string(i) + "_to_cpu_stream";
    auto info = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
    auto name = info ? info.getAs<StringAttr>("name") : StringAttr{};
    auto depth = info ? info.getAs<IntegerAttr>("depth") : IntegerAttr{};
    auto width = info ? info.getAs<IntegerAttr>("widthBytes") : IntegerAttr{};
    auto index = info ? info.getAs<IntegerAttr>("index") : IntegerAttr{};
    if (!name || name.getValue() != streamName || queue.getName() != "GGTracerVStreamQueue6144" + suffix ||
        !depth || depth.getInt() != 6144 || !width || width.getInt() != 64 || !index || index.getInt() != i ||
        queue.getNumPorts() != 5 || queue.getPortType(3) != token || queue.getPortType(4) != UIntType::get(ctx, 13))
      return reject("Rocket CPU catalog queue identity or geometry differs");
    unsigned memories = 0;
    for (auto mem : queue.getOps<MemOp>()) {
      ++memories;
      if (mem.getDepth() != 6144 || mem.getDataType() != word)
        return reject("Rocket CPU catalog queue storage differs from allocation");
    }
    if (memories != 1) return reject("Rocket CPU catalog requires one native queue memory");
    outgoing.push_back({streamName, "tracerv_stream" + suffix, 6144});
    registers.push_back({streamName, "tracerv_stream_count" + suffix, 13});
  }
  sources.assign(outgoing.begin(), outgoing.end());
  counts.assign(registers.begin(), registers.end());
  return success();
}

LogicalResult goldengate::deriveCPUStreamPorts(CircuitOp circuit,
    SmallVectorImpl<CPUStreamSourcePort> &sources,
    SmallVectorImpl<CPUStreamSourcePort> &sinks,
    SmallVectorImpl<CPUStreamCountPort> &counts, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  SmallVector<CPUStreamSourcePort> outgoing, incoming;
  SmallVector<CPUStreamCountPort> registers;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("CPU stream catalog needs retained bridge annotations");
  unsigned nics = 0, tracers = 0;
  SmallVector<std::pair<bool, unsigned>> producerOrder;
  for (auto a : raw) {
    auto d = dyn_cast<DictionaryAttr>(a);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr{};
    auto widget = d ? d.getAs<StringAttr>("widgetClass") : StringAttr{};
    if (cls == "firesim.lib.bridgeutils.BridgeIOAnnotation" &&
        widget == "firechip.goldengateimplementations.SimpleNICBridgeModule") {
      if (d.get("widgetConstructorKey")) return reject("CPU catalog needs constructor-free NIC");
      producerOrder.emplace_back(true, nics++);
    } else if (cls == "firesim.lib.bridgeutils.BridgeIOAnnotation" &&
        widget == "firechip.goldengateimplementations.TracerVBridgeModule") {
      producerOrder.emplace_back(false, tracers++);
    }
  }
  if (tracers) {
    if (failed(deriveRocketCPUStreamPorts(circuit, outgoing, registers, error))) return failure();
  } else {
    FModuleOp active;
    for (auto module : circuit.getOps<FModuleOp>())
      if (module.getName() == circuit.getName()) active = module;
    if (!active) return reject("CPU stream catalog requires an active top");
    for (auto port : active.getPorts())
      if (port.name.getValue().starts_with("tracerv_stream"))
        return reject("CPU stream catalog has TracerV ports without a constructor");
  }
  if (tracers != outgoing.size())
    return reject("CPU catalog TracerV constructor count differs from live queues");
  if (nics) {
    auto find = [&](StringRef name) -> FModuleOp {
      for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
      return {};
    };
    auto top = find(circuit.getName()); auto *ctx = circuit.getContext(); OpBuilder b(ctx);
    auto bit = UIntType::get(ctx, 1), word = UIntType::get(ctx, 512), countType = UIntType::get(ctx, 12);
    auto token = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, word}});
    auto port = [&](StringRef name, Type type, Direction direction) -> Value {
      for (auto [i,p] : llvm::enumerate(top.getPorts()))
        if (p.name == name && p.type == type && p.direction == direction) return top.getArgument(i);
      return {};
    };
    using Path = SmallVector<InstanceOp>;
    struct Endpoint { FModuleOp queue; Path path; unsigned port = 0; };
    // Output tracing follows unique drivers. Input tracing follows unique
    // instance consumers, preserving the full instance path for identity.
    auto trace = [&](Value value, bool input) -> Endpoint {
      Path path; SmallVector<Value> seen;
      while (value && seen.size() < 4096) {
        if (llvm::is_contained(seen, value)) return {};
        seen.push_back(value); Value next; unsigned connections = 0;
        for (auto &op : *value.getParentBlock()) {
          auto visit = [&](Value dest, Value src) {
            if ((!input && dest == value) || (input && src == value)) {
              next = input ? dest : src; ++connections;
            }
          };
          if (auto c = dyn_cast<ConnectOp>(op)) visit(c.getDest(), c.getSrc());
          if (auto c = dyn_cast<StrictConnectOp>(op)) visit(c.getDest(), c.getSrc());
        }
        if (connections) { if (connections != 1) return {}; value = next; }
        auto inst = value.getDefiningOp<InstanceOp>();
        if (!inst) { if (connections) continue; return {}; }
        auto module = find(inst.getModuleName());
        unsigned i = cast<OpResult>(value).getResultNumber();
        if (!module || module.getPortDirection(i) != (input ? Direction::In : Direction::Out)) return {};
        path.push_back(inst);
        if (module.getName() == "GGNICCPUStreamQueue3072") return {module, path, i};
        value = module.getArgument(i);
      }
      return {};
    };
    SmallVector<Path> queuePaths;
    SmallVector<CPUStreamSourcePort> nicOutgoing;
    SmallVector<CPUStreamCountPort> nicOutgoingCounts;
    for (unsigned widget = 0; widget < nics; ++widget) {
    for (bool input : {false, true}) {
      std::string prefix = nics == 1 ? "nic_" : "nic" + std::to_string(widget) + "_";
      std::string payloadName = prefix + (input ? "from_host_stream" : "to_host_stream");
      std::string countName = payloadName + "_count";
      std::string streamName = "SIMPLENICBRIDGEMODULE_" + std::to_string(widget) +
          (input ? "_from_cpu_stream" : "_to_cpu_stream");
      auto payload = trace(port(payloadName, token, input ? Direction::In : Direction::Out), input);
      auto occupancy = trace(port(countName, countType, Direction::Out), false);
      if (!payload.queue || payload.path != occupancy.path ||
          payload.port != (input ? 2u : 3u) || occupancy.port != 4)
        return reject("NIC payload/count must reach the same live directional queue instance");
      if (llvm::is_contained(queuePaths, payload.path))
        return reject("NIC widgets and directions must use independent queue instances");
      queuePaths.push_back(payload.path);
      auto queue = payload.queue;
      auto info = payload.path.back()->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
      auto depth = info ? info.getAs<IntegerAttr>("depth") : IntegerAttr{};
      auto width = info ? info.getAs<IntegerAttr>("widthBytes") : IntegerAttr{};
      if (!info || info.getAs<StringAttr>("name") != streamName ||
          info.getAs<StringAttr>("direction") != (input ? "fromHostCPU" : "toHostCPU") ||
          !depth || depth.getInt() != 3072 || !width || width.getInt() != 64 ||
          queue.getNumPorts() != 5 || queue.getPortType(2) != token ||
          queue.getPortType(3) != token || queue.getPortType(4) != countType)
        return reject("NIC CPU catalog instance identity or geometry differs");
      unsigned memories = 0;
      for (auto mem : queue.getOps<MemOp>()) {
        ++memories;
        if (mem.getDepth() != 3072 || mem.getDataType() != word)
          return reject("NIC CPU catalog storage differs from allocation");
      }
      if (memories != 1) return reject("NIC CPU catalog requires one native queue memory");
      CPUStreamSourcePort stream{streamName, payloadName, 3072};
      CPUStreamCountPort count{streamName, countName, 12};
      if (input) { incoming.push_back(stream); registers.push_back(count); }
      else { nicOutgoing.push_back(stream); nicOutgoingCounts.push_back(count); }
    }
    }
    outgoing.insert(outgoing.begin(), nicOutgoing.begin(), nicOutgoing.end());
    registers.insert(registers.begin(), nicOutgoingCounts.begin(), nicOutgoingCounts.end());
  }
  // FPGATop constructs its ListMap in BridgeIO annotation order and filters
  // that same order separately for each stream direction. Keep NIC and TracerV
  // interleaving for read windows and source count words; sink count words then
  // follow in the independently filtered from-host order.
  SmallVector<CPUStreamSourcePort> orderedSources;
  SmallVector<CPUStreamCountPort> orderedCounts;
  for (auto [nic, index] : producerOrder) {
    unsigned position = nic ? index : nics + index;
    orderedSources.push_back(outgoing[position]);
    orderedCounts.push_back(registers[position]);
  }
  orderedCounts.append(registers.begin() + outgoing.size(), registers.end());
  sources.assign(orderedSources.begin(), orderedSources.end());
  sinks.assign(incoming.begin(), incoming.end());
  counts.assign(orderedCounts.begin(), orderedCounts.end());
  return success();
}

LogicalResult goldengate::addCPUStreamRead(CircuitOp circuit, std::string &error) {
  if (circuit.getName() != "GGTracerVStreamQueueWrapper") {
    error = "CPU stream read requires the TracerV queue wrapper";
    return failure();
  }
  // The recorded Rocket adapter retains its count boundary contract. General
  // transport bindings leave count allocation to addCPUStreamCountBank.
  FModuleOp top;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  bool count = false;
  if (top) for (auto p : top.getPorts())
    count |= p.name == "tracerv_stream_count" && p.direction == Direction::Out &&
        p.type == UIntType::get(circuit.getContext(), 13, false);
  if (!count) {
    error = "CPU stream read needs the TracerV count13 output";
    return failure();
  }
  return addCPUStreamRead(circuit,
      {{"TRACERVBRIDGEMODULE_0_to_cpu_stream", "tracerv_stream", 6144}}, error);
}

LogicalResult goldengate::addCPUStreamRead(
    CircuitOp circuit, ArrayRef<CPUStreamSourcePort> sources, std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGCPUStreamRead";
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamReadWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (sources.empty()) return reject("CPU stream read requires outgoing streams");
  llvm::StringSet<> names, portNames;
  uint64_t maxDepth = 0;
  for (const auto &source : sources) {
    if (source.streamName.empty() || source.streamName.find('\0') != std::string::npos ||
        source.portName.empty() || source.portName.find('\0') != std::string::npos ||
        !names.insert(source.streamName).second || !portNames.insert(source.portName).second)
      return reject("CPU stream read requires unique nonempty stream and port identities");
    if (!source.depth || source.depth > uint64_t(std::numeric_limits<int32_t>::max()) / 64)
      return reject("CPU stream queue depth exceeds supported Scala address allocation");
    maxDepth = std::max(maxDepth, uint64_t(source.depth));
  }
  unsigned addressSpaceBits = llvm::Log2_64_Ceil(64 * maxDepth);
  uint64_t windowBytes = uint64_t(1) << addressSpaceBits;
  if (uint64_t(sources.size() - 1) > uint64_t(std::numeric_limits<int32_t>::max()) / windowBytes)
    return reject("CPU stream base addresses exceed supported Scala driver allocation");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op); if (!m) continue;
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("CPU stream read helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("CPU stream read needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context);
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(512)}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(context), Direction::In);
  auto reset = port("hostReset", bit, Direction::In);
  if (!clock || !reset)
    return reject("CPU stream read needs host clock/reset");
  SmallVector<unsigned> streamPorts;
  for (const auto &source : sources) {
    auto stream = port(source.portName, token, Direction::Out);
    if (!stream) return reject("CPU stream read needs every declared Decoupled512 output");
    streamPorts.push_back(*stream);
  }
  for (auto p : inner.getPorts()) if (p.name.getValue().starts_with("cpu_stream_"))
    return reject("CPU stream read boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("CPU stream read needs an uninstantiated top");

  Location loc = circuit.getLoc();
  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  for (unsigned i = 0; i < sources.size(); ++i)
    helperPorts.push_back({b.getStringAttr(sources.size() == 1 ? "stream" : "stream_" + std::to_string(i)), token, Direction::In});
  unsigned firstHelperAXI = helperPorts.size();
  auto append = [&](llvm::StringRef n, unsigned w, Direction d) {
    helperPorts.push_back({b.getStringAttr(n), uint(w), d});
  };
  append("ar_ready", 1, Direction::Out); append("ar_valid", 1, Direction::In);
  append("ar_bits_id", 16, Direction::In); append("ar_bits_addr", 64, Direction::In);
  append("ar_bits_len", 8, Direction::In); append("ar_bits_size", 3, Direction::In);
  append("r_ready", 1, Direction::In); append("r_valid", 1, Direction::Out);
  append("r_bits_id", 16, Direction::Out); append("r_bits_data", 512, Direction::Out);
  append("r_bits_last", 1, Direction::Out); append("r_bits_resp", 2, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName), ConventionAttr::get(context, Convention::Internal), helperPorts);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
  // AXI ports follow the ordered source ports. Equal-width StreamWidthAdapter
  // forwards each buffered payload directly. Every counter observes the shared
  // R.fire, including counters for unselected streams (Scala's exact behavior).
  auto axi = [&](unsigned i) { return arg(firstHelperAXI + i); };
  SmallVector<Value> counters, grants, lasts, valids;
  for (unsigned i = 0; i < sources.size(); ++i) {
    counters.push_back(b.create<RegResetOp>(loc, uint(9), arg(0), arg(1), constant(9, 0),
        sources.size() == 1 ? "readBeatCounter" : "readBeatCounter_" + std::to_string(i)).getResult());
    grants.push_back(b.create<EQPrimOp>(loc, b.create<BitsPrimOp>(loc, axi(3), 63, addressSpaceBits), constant(64 - addressSpaceBits, i)));
    lasts.push_back(b.create<EQPrimOp>(loc, counters.back(), b.create<PadPrimOp>(loc, axi(4), 9)));
    valids.push_back(both(grants.back(), both(axi(1), field(arg(2 + i), "valid"))));
  }
  Value valid = valids.front();
  for (Value v : ArrayRef<Value>(valids).drop_front()) valid = b.create<OrPrimOp>(loc, valid, v);
  Value fire = both(valid, axi(6));
  Value arReady;
  Value data = sources.size() == 1 ? field(arg(2), "bits") : constant(512, 0);
  Value last = sources.size() == 1 ? lasts.front() : constant(1, 0);
  for (unsigned i = 0; i < sources.size(); ++i) {
    Value beats = counters[i];
    Value next = mux(lasts[i], constant(9, 0), b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, beats, constant(9, 1)), 8, 0));
    connect(beats, mux(fire, next, beats));
    // DecoupledHelper excludes its own handshake predicate: AR ready is not
    // gated by AR valid, nor queue ready by queue valid.
    Value ready = both(grants[i], both(axi(6), both(field(arg(2 + i), "valid"), lasts[i])));
    arReady = arReady ? Value(b.create<OrPrimOp>(loc, arReady, ready)) : ready;
    connect(field(arg(2 + i), "ready"), both(grants[i], both(axi(1), axi(6))));
    if (sources.size() != 1) {
      data = mux(grants[i], field(arg(2 + i), "bits"), data);
      last = mux(grants[i], lasts[i], last);
    }
  }
  connect(axi(0), arReady); connect(axi(7), valid); connect(axi(8), axi(2));
  connect(axi(9), data); connect(axi(10), last); connect(axi(11), constant(2, 0));
  Value permitted = b.create<OrPrimOp>(loc, b.create<NotPrimOp>(loc, axi(1)), b.create<EQPrimOp>(loc, axi(5), constant(3, 6)));
  b.create<AssertOp>(loc, arg(0), permitted, b.create<NotPrimOp>(loc, arg(1)),
      "CPUManagedStreamEngine requires 64-byte read beats", ValueRange{}, "");
  helper->setAttr("goldengate.streamAddressSpaceBits", b.getI64IntegerAttr(addressSpaceBits));
  SmallVector<Attribute> allocations;
  for (auto [i, source] : llvm::enumerate(sources))
    allocations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name", b.getStringAttr(source.streamName)),
        b.getNamedAttr("port", b.getStringAttr(source.portName)),
        b.getNamedAttr("index", b.getI64IntegerAttr(i)),
        b.getNamedAttr("depth", b.getI64IntegerAttr(source.depth)),
        b.getNamedAttr("widthBytes", b.getI64IntegerAttr(64)),
        b.getNamedAttr("bufferBaseAddress", b.getI64IntegerAttr(i * windowBytes))}));
  helper->setAttr("goldengate.sourceStreams", b.getArrayAttr(allocations));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!llvm::is_contained(streamPorts, i)) { copied.push_back(i); ports.push_back(p); }
  unsigned firstAXI = ports.size();
  for (auto p : ArrayRef<PortInfo>(helperPorts).drop_front(firstHelperAXI)) {
    p.name = b.getStringAttr("cpu_stream_" + p.name.getValue().str()); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto transport = b.create<InstanceOp>(loc, helper, "cpuStreamRead");
  auto externalFor = [&](unsigned innerIndex) -> Value {
    return wrapper.getBodyBlock()->getArgument(llvm::find(copied, innerIndex) - copied.begin());
  };
  for (auto [i, old] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[old]; Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(old) : external,
        p.direction == Direction::In ? external : sim.getResult(old));
  }
  connect(transport.getResult(0), externalFor(*clock)); connect(transport.getResult(1), externalFor(*reset));
  for (auto [i, innerPort] : llvm::enumerate(streamPorts))
    b.create<ConnectOp>(loc, transport.getResult(2 + i), sim.getResult(innerPort));
  for (unsigned i = firstHelperAXI; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(firstAXI + i - firstHelperAXI);
    if (helperPorts[i].direction == Direction::In) connect(transport.getResult(i), external);
    else connect(external, transport.getResult(i));
  }
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
