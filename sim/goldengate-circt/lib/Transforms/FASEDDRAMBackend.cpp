// See LICENSE for license details.
// Oracle: Util.scala DynamicLatencyPipe / HasFIFOPointers; DramCommon DRAMBackend.
#include "goldengate/FASEDDRAMBackend.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDDRAMBackend(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name="GGFASEDDRAMBackend";
  for(auto module:circuit.getOps<FModuleLike>())if(module.getModuleName()==name) {
    error="FASED DRAM backend helper already exists";return failure();
  }
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto u=[&](unsigned w){return UIntType::get(ctx,w,false);};
  SmallVector<PortInfo> ports;
  auto port=[&](StringRef n,unsigned w,Direction d){ports.push_back({b.getStringAttr(n),u(w),d});};
  ports.push_back({b.getStringAttr("clock"),ClockType::get(ctx),Direction::In});
  for(auto n:{"reset","targetFire","newReadValid"})port(n,1,Direction::In);
  port("newReadReady",1,Direction::Out);port("newReadID",4,Direction::In);port("newReadLen",8,Direction::In);
  port("newWriteValid",1,Direction::In);port("newWriteReady",1,Direction::Out);port("newWriteID",4,Direction::In);
  port("completedReadValid",1,Direction::Out);port("completedReadReady",1,Direction::In);
  port("completedReadID",4,Direction::Out);port("completedReadLen",8,Direction::Out);
  port("completedWriteValid",1,Direction::Out);port("completedWriteReady",1,Direction::In);port("completedWriteID",4,Direction::Out);
  for(auto n:{"readLatency","writeLatency","tCycle"})port(n,12,Direction::In);
  for(auto n:{"readCount","writeCount"})port(n,3,Direction::Out);
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
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto trim=[&](Value a,unsigned w)->Value{return b.create<BitsPrimOp>(loc,a,w-1,0);};
  auto drive=[&](Value reg,Value next){connect(reg,mux(arg(2),next,reg));};
  auto reg=[&](StringRef n,unsigned w,bool reset)->Value {
    if(reset)return b.create<RegResetOp>(loc,u(w),arg(0),and_(arg(1),arg(2)),c(w,0),n).getResult();
    return b.create<RegOp>(loc,u(w),arg(0),n).getResult();
  };
  auto queue=[&](StringRef stem,unsigned validIn,unsigned readyOut,unsigned idIn,
                 unsigned validOut,unsigned readyIn,unsigned idOut,unsigned latency,
                 unsigned countOut,bool read) {
    auto named=[&](StringRef n){return (stem+"_"+n).str();};
    Value enq=reg(named("enqPtr"),2,true),deq=reg(named("deqPtr"),2,true),fullFlag=reg(named("maybeFull"),1,true);
    Value matched=eq(enq,deq),empty=and_(matched,not_(fullFlag)),full=and_(matched,fullFlag);
    Value ready=not_(full);connect(arg(readyOut),ready);
    std::array<Value,4> pending,deadline,id,len,done;
    for(unsigned i=0;i<4;++i) {
      auto n=[&](StringRef field){return named((field+Twine(i)).str());};
      pending[i]=reg(n("pending"),1,true);deadline[i]=reg(n("deadline"),12,false);id[i]=reg(n("id"),4,false);
      if(read)len[i]=reg(n("len"),8,false);
      done[i]=or_(eq(deadline[i],arg(19)),not_(pending[i]));
    }
    auto select=[&](const std::array<Value,4>& values)->Value {
      Value v=values[3];for(int i=2;i>=0;--i)v=mux(eq(deq,c(2,i)),values[i],v);return v;
    };
    Value valid=and_(not_(empty),select(done));connect(arg(validOut),valid);connect(arg(idOut),select(id));
    if(read)connect(arg(13),select(len));
    Value doEnq=and_(arg(validIn),ready),doDeq=and_(valid,arg(readyIn));
    drive(enq,mux(doEnq,trim(b.create<AddPrimOp>(loc,enq,c(2,1)),2),enq));
    drive(deq,mux(doDeq,trim(b.create<AddPrimOp>(loc,deq,c(2,1)),2),deq));
    drive(fullFlag,mux(not_(eq(doEnq,doDeq)),doEnq,fullFlag));
    Value diff=trim(b.create<SubPrimOp>(loc,enq,deq),2);
    connect(arg(countOut),b.create<CatPrimOp>(loc,full,diff));
    Value expires=trim(b.create<AddPrimOp>(loc,arg(19),arg(latency)),12);
    for(unsigned i=0;i<4;++i) {
      Value insert=and_(doEnq,eq(enq,c(2,i)));
      drive(pending[i],mux(insert,not_(eq(arg(latency),c(12,1))),mux(eq(deadline[i],arg(19)),c(1,0),pending[i])));
      drive(deadline[i],mux(insert,expires,deadline[i]));drive(id[i],mux(insert,arg(idIn),id[i]));
      if(read)drive(len[i],mux(insert,arg(6),len[i]));
    }
    Value legal=or_(not_(doEnq),not_(eq(arg(latency),c(12,0))));
    b.create<AssertOp>(loc,arg(0),legal,and_(arg(2),not_(arg(1))),
        "DynamicLatencyPipe only supports latencies > 0",ValueRange{},named("positiveLatency"));
  };
  queue("read",3,4,5,10,11,12,17,20,true);
  queue("write",7,8,9,14,15,16,18,21,false);
  result=module;return success();
}
