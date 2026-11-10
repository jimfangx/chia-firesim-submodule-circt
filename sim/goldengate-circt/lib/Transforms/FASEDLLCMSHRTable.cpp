// See LICENSE for license details.
// Oracle: midas/models/dram/LastLevelCache.scala MSHR and LLCModel.
#include "goldengate/FASEDLLCMSHRTable.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDLLCMSHRTable(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDLLCMSHRTable";
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name) {
      error = "FASED LLC MSHR table helper already exists";
      return failure();
    }
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto u=[&](unsigned w){return UIntType::get(ctx,w,false);};
  constexpr std::array<const char *,8> fields{"setAddr","id","len","isWrite","addr","writeInFlight","readInFlight","enabled"};
  constexpr std::array<unsigned,8> widths{12,4,8,1,35,1,1,1};
  SmallVector<BundleType::BundleElement> elements;
  for(unsigned j=0;j<8;++j)elements.push_back({b.getStringAttr(fields[j]),false,u(widths[j])});
  auto entryType=BundleType::get(ctx,elements);
  SmallVector<PortInfo> ports;
  auto port=[&](StringRef n,unsigned w,Direction d){ports.push_back({b.getStringAttr(n),u(w),d});};
  ports.push_back({b.getStringAttr("clock"),ClockType::get(ctx),Direction::In});
  port("reset",1,Direction::In);port("targetFire",1,Direction::In);
  port("activeMSHRs",4,Direction::In);port("readSet",12,Direction::In);port("writeSet",12,Direction::In);
  port("allocate",1,Direction::In);port("allocateSet",12,Direction::In);port("allocateId",4,Direction::In);
  port("allocateLen",8,Direction::In);port("allocateIsWrite",1,Direction::In);port("allocateAddr",35,Direction::In);
  port("allocateRead",1,Direction::In);port("allocateWrite",1,Direction::In);
  port("completedRead",1,Direction::In);port("completedReadId",4,Direction::In);
  port("completedWrite",1,Direction::In);port("completedWriteId",4,Direction::In);
  port("available",1,Direction::Out);port("nextIndex",3,Direction::Out);port("allocated",4,Direction::Out);
  port("readCollision",1,Direction::Out);port("writeCollision",1,Direction::Out);
  port("responseId",4,Direction::Out);port("responseLen",8,Direction::Out);port("responseIsWrite",1,Direction::Out);port("responseAddr",35,Direction::Out);
  ports.push_back({b.getStringAttr("entries"),FVectorType::get(entryType,8),Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module=b.create<FModuleOp>(loc,b.getStringAttr(name),ConventionAttr::get(ctx,Convention::Internal),ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg=[&](unsigned i){return module.getBodyBlock()->getArgument(i);};
  auto c=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,u(w),APInt(w,n));};
  auto con=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto a=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto o=[&](Value x,Value y)->Value{return b.create<OrPrimOp>(loc,x,y);};
  auto n=[&](Value x)->Value{return b.create<NotPrimOp>(loc,x);};
  auto eq=[&](Value x,Value y)->Value{return b.create<EQPrimOp>(loc,x,y);};
  auto mux=[&](Value z,Value x,Value y)->Value{return b.create<MuxPrimOp>(loc,z,x,y);};
  auto update=[&](Value r,Value next){con(r,mux(arg(2),next,r));};
  auto enabledReset=a(arg(1),arg(2));
  std::array<std::array<Value,8>,8> state;
  std::array<Value,8> valid,free;
  Value available=c(1,0),allocated=c(4,0),readCollision=c(1,0),writeCollision=c(1,0);
  for(unsigned i=0;i<8;++i){
    auto out=b.create<SubindexOp>(loc,arg(27),i).getResult();
    for(unsigned j=0;j<8;++j){
      std::string stem="entry"+std::to_string(i)+"_"+fields[j];
      state[i][j]=j<5?b.create<RegOp>(loc,u(widths[j]),arg(0),stem).getResult():
        b.create<RegResetOp>(loc,u(widths[j]),arg(0),enabledReset,c(widths[j],j==7),stem).getResult();
      con(b.create<SubfieldOp>(loc,out,fields[j]),state[i][j]);
    }
    valid[i]=a(o(state[i][5],state[i][6]),state[i][7]);free[i]=a(n(valid[i]),state[i][7]);
    available=o(available,free[i]);
    Value sum=b.create<AddPrimOp>(loc,allocated,b.create<PadPrimOp>(loc,valid[i],4));
    allocated=b.create<BitsPrimOp>(loc,sum,3,0);
    readCollision=o(readCollision,a(valid[i],eq(state[i][0],arg(4))));
    writeCollision=o(writeCollision,a(valid[i],eq(state[i][0],arg(5))));
  }
  Value nextIndex=c(3,7);
  for(int i=6;i>=0;--i)nextIndex=mux(free[i],c(3,i),nextIndex);
  con(arg(18),available);con(arg(19),nextIndex);con(arg(20),allocated);
  con(arg(21),readCollision);con(arg(22),writeCollision);
  Value readIndex=b.create<BitsPrimOp>(loc,arg(15),2,0);
  Value writeIndex=b.create<BitsPrimOp>(loc,arg(17),2,0);
  for(unsigned j=1;j<5;++j){
    Value selected=state[0][j];
    for(unsigned i=1;i<8;++i)selected=mux(eq(readIndex,c(3,i)),state[i][j],selected);
    con(arg(22+j),selected);
  }
  for(unsigned i=0;i<8;++i){
    Value allocate=a(arg(6),eq(nextIndex,c(3,i)));
    for(unsigned j=0;j<5;++j)update(state[i][j],mux(allocate,arg(7+j),state[i][j]));
    Value writeNext=mux(allocate,arg(13),state[i][5]);
    Value readNext=mux(allocate,arg(12),state[i][6]);
    update(state[i][5],mux(a(arg(16),eq(writeIndex,c(3,i))),c(1,0),writeNext));
    update(state[i][6],mux(a(arg(14),eq(readIndex,c(3,i))),c(1,0),readNext));
    update(state[i][7],b.create<GTPrimOp>(loc,arg(3),c(4,i)));
  }
  // RegNext(activeMSHRs) is unreset, and advances on the target clock.
  Value previousActive=b.create<RegOp>(loc,u(4),arg(0),"previousActiveMSHRs").getResult();
  update(previousActive,arg(3));
  auto assertionEnable=a(arg(2),n(arg(1)));
  Value upper=o(b.create<LTPrimOp>(loc,allocated,previousActive),n(available));
  Value lower=o(eq(allocated,previousActive),available);
  b.create<AssertOp>(loc,arg(0),upper,assertionEnable,"Too many runtime MSHRs exposed given runtime programmable limit",ValueRange{},"mshrUpperLimit");
  b.create<AssertOp>(loc,arg(0),lower,assertionEnable,"Too few runtime MSHRs exposed given runtime programmable limit",ValueRange{},"mshrLowerLimit");
  result=module;return success();
}
