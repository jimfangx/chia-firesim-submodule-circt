// See LICENSE for license details.
// Oracle: midas/models/dram/LastLevelCache.scala, selected WithLLCModel geometry.
#include "goldengate/FASEDLLCMetadata.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDLLCMetadata(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name="GGFASEDLLCMetadata";
  for(auto module:circuit.getOps<FModuleLike>())if(module.getModuleName()==name){
    error="FASED LLC metadata helper already exists";return failure();
  }
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto u=[&](unsigned w){return UIntType::get(ctx,w,false);};
  SmallVector<BundleType::BundleElement> fields{{b.getStringAttr("tag"),false,u(32)},
      {b.getStringAttr("valid"),false,u(1)},{b.getStringAttr("dirty"),false,u(1)}};
  auto entry=BundleType::get(ctx,fields);auto metadata=FVectorType::get(entry,8);
  SmallVector<PortInfo> ports;
  auto port=[&](StringRef n,unsigned w,Direction d){ports.push_back({b.getStringAttr(n),u(w),d});};
  ports.push_back({b.getStringAttr("clock"),ClockType::get(ctx),Direction::In});
  for(auto n:{"reset","targetFire","lookupStart"})port(n,1,Direction::In);
  port("lookupSet",12,Direction::In);port("lookupTag",35,Direction::In);port("wayBits",2,Direction::In);
  for(auto n:{"stage1Valid","stage1IsWrite"})port(n,1,Direction::In);
  port("stage1Set",12,Direction::Out);port("stage1Tag",35,Direction::Out);
  for(auto n:{"hitValid","emptyValid","evictDirty"})port(n,1,Direction::Out);
  port("evictTag",32,Direction::Out);port("selectedWayOH",8,Direction::Out);
  ports.push_back({b.getStringAttr("metadata"),metadata,Direction::Out});port("lfsr",16,Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module=b.create<FModuleOp>(loc,b.getStringAttr(name),ConventionAttr::get(ctx,Convention::Internal),ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg=[&](unsigned i){return module.getBodyBlock()->getArgument(i);};
  auto c=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,u(w),APInt(w,n));};
  auto and_=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto or_=[&](Value a,Value z)->Value{return b.create<OrPrimOp>(loc,a,z);};
  auto not_=[&](Value a)->Value{return b.create<NotPrimOp>(loc,a);};
  auto eq=[&](Value a,Value z)->Value{return b.create<EQPrimOp>(loc,a,z);};
  auto mux=[&](Value a,Value z,Value q)->Value{return b.create<MuxPrimOp>(loc,a,z,q);};
  auto bits=[&](Value a,unsigned hi,unsigned lo)->Value{return b.create<BitsPrimOp>(loc,a,hi,lo);};
  auto field=[&](Value a,StringRef n)->Value{return b.create<SubfieldOp>(loc,a,n);};
  auto index=[&](Value a,unsigned i)->Value{return b.create<SubindexOp>(loc,a,i);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto drive=[&](Value r,Value n){connect(r,mux(arg(2),n,r));};
  auto stageSet=b.create<RegOp>(loc,u(12),arg(0),"s1_set_addr").getResult();
  auto stageTag=b.create<RegOp>(loc,u(35),arg(0),"s1_tag_addr").getResult();
  drive(stageSet,arg(4));drive(stageTag,arg(5));connect(arg(9),stageSet);connect(arg(10),stageTag);
  // Chisel compatibility LFSR16 reverses PRNG state bit order. In that exposed
  // order its seed is one, feedback taps are 0/2/3/5 and it shifts right.
  auto lfsr=b.create<RegResetOp>(loc,u(16),arg(0),and_(arg(1),arg(2)),c(16,1),"replacementLFSR").getResult();
  Value feedback=b.create<XorPrimOp>(loc,bits(lfsr,0,0),bits(lfsr,2,2));
  feedback=b.create<XorPrimOp>(loc,feedback,bits(lfsr,3,3));
  feedback=b.create<XorPrimOp>(loc,feedback,bits(lfsr,5,5));
  drive(lfsr,b.create<CatPrimOp>(loc,feedback,bits(lfsr,15,1)));connect(arg(17),lfsr);
  SmallVector<Type> memTypes{MemOp::getTypeForPort(4096,metadata,MemOp::PortKind::Read),
      MemOp::getTypeForPort(4096,metadata,MemOp::PortKind::Write)};
  SmallVector<Attribute> memNames{b.getStringAttr("read"),b.getStringAttr("write")};
  // A disabled FIRRTL latency-one memory read is unspecified, whereas Chisel's
  // emitted SFC memory explicitly retains each output register. Keep that hold
  // behavior using an asynchronous read and unreset, enabled output registers.
  // Together these implement one target-cycle read latency and old-data RUW.
  auto memory=b.create<MemOp>(loc,memTypes,0,1,4096,RUWAttr::Undefined,memNames,"md_array");
  Value reader=memory.getResult(0),writer=memory.getResult(1),rawData=field(reader,"data");
  connect(field(reader,"clk"),arg(0));connect(field(reader,"en"),c(1,1));
  connect(field(reader,"addr"),arg(4));
  // MaskGen + Reverse reduces to low 1/2/4/8 enabled ways for legal wayBits.
  Value wayMask=mux(eq(arg(6),c(2,0)),c(8,1),mux(eq(arg(6),c(2,1)),c(8,3),mux(eq(arg(6),c(2,2)),c(8,15),c(8,255))));
  std::array<Value,8> entries,hit,empty;
  Value hitValid=c(1,0),emptyValid=c(1,0),hitSel=c(8,0),emptySel=c(8,0);
  for(unsigned i=0;i<8;++i){
    entries[i]=index(arg(16),i);Value source=index(rawData,i);
    for(auto [name,width]:{std::pair<const char*,unsigned>{"tag",32},{"valid",1},{"dirty",1}}){
      auto reg=b.create<RegOp>(loc,u(width),arg(0),("s1_metadata_"+Twine(i)+"_"+name).str()).getResult();
      drive(reg,mux(arg(3),field(source,name),reg));connect(field(entries[i],name),reg);
    }
    Value enabled=bits(wayMask,i,i);
    Value extended=b.create<PadPrimOp>(loc,field(entries[i],"tag"),35);
    hit[i]=and_(enabled,and_(field(entries[i],"valid"),eq(extended,stageTag)));
    empty[i]=and_(enabled,not_(field(entries[i],"valid")));
    hitValid=or_(hitValid,hit[i]);emptyValid=or_(emptyValid,empty[i]);
  }
  for(int i=7;i>=0;--i){hitSel=mux(hit[i],c(8,1u<<i),hitSel);emptySel=mux(empty[i],c(8,1u<<i),emptySel);}
  Value indexMask=mux(eq(arg(6),c(2,0)),c(3,0),mux(eq(arg(6),c(2,1)),c(3,1),mux(eq(arg(6),c(2,2)),c(3,3),c(3,7))));
  Value victimIndex=and_(bits(lfsr,2,0),indexMask);
  Value victimOH=b.create<DShlPrimOp>(loc,c(1,1),victimIndex);
  Value victimDirty=c(1,0),victimTag=c(32,0);
  for(unsigned i=0;i<8;++i){Value sel=bits(victimOH,i,i);victimDirty=or_(victimDirty,and_(sel,field(entries[i],"dirty")));victimTag=or_(victimTag,mux(sel,field(entries[i],"tag"),c(32,0)));}
  Value doEvict=and_(not_(hitValid),not_(emptyValid)),fillEmpty=and_(not_(hitValid),emptyValid);
  Value selected=mux(hitValid,hitSel,mux(emptyValid,emptySel,victimOH));
  connect(arg(11),hitValid);connect(arg(12),emptyValid);connect(arg(13),and_(doEvict,victimDirty));
  connect(arg(14),victimTag);connect(arg(15),selected);
  connect(field(writer,"clk"),arg(0));connect(field(writer,"en"),and_(arg(2),arg(7)));
  connect(field(writer,"addr"),stageSet);
  Value writeData=field(writer,"data"),writeMask=field(writer,"mask");
  for(unsigned i=0;i<8;++i){
    Value sel=bits(selected,i,i),old=entries[i],next=index(writeData,i),mask=index(writeMask,i);
    connect(field(next,"valid"),mux(and_(sel,fillEmpty),c(1,1),field(old,"valid")));
    Value dirty=mux(arg(8),c(1,1),mux(doEvict,c(1,0),field(old,"dirty")));
    connect(field(next,"dirty"),mux(sel,dirty,field(old,"dirty")));
    connect(field(next,"tag"),mux(and_(sel,or_(doEvict,fillEmpty)),bits(stageTag,31,0),field(old,"tag")));
    for(auto n:{"tag","valid","dirty"})connect(field(mask,n),c(1,1));
  }
  result=module;return success();
}
