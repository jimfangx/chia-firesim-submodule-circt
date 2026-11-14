// See LICENSE for license details.
// Oracle: TransactionSchedulers.scala UnifiedFIFOXactionScheduler, Chisel
// RRArbiter and Queue, widgets/Lib.scala SatUpDownCounter.
#include "goldengate/FASEDUnifiedTransactionScheduler.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDUnifiedTransactionScheduler(
    CircuitOp circuit, unsigned maxWrites, FModuleOp &result,
    std::string &error) {
  if (maxWrites == 0 || maxWrites > 32) {
    error = "selected FASED unified scheduler supports maxWrites one through "
            "thirty-two";
    return failure();
  }
  std::string name = "GGFASEDUnifiedTransactionScheduler" + std::to_string(maxWrites);
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name) {
      error = "FASED unified transaction scheduler helper already exists";
      return failure();
    }
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto u=[&](unsigned w){return UIntType::get(ctx,w,false);};
  SmallVector<PortInfo> ports;
  auto port=[&](StringRef n,unsigned w,Direction d){ports.push_back({b.getStringAttr(n),u(w),d});};
  ports.push_back({b.getStringAttr("clock"),ClockType::get(ctx),Direction::In});
  for(auto n:{"reset","targetFire","arValid"})port(n,1,Direction::In);
  port("arReady",1,Direction::Out);port("arId",4,Direction::In);
  port("arLen",8,Direction::In);port("arAddr",35,Direction::In);
  port("awValid",1,Direction::In);port("awReady",1,Direction::Out);
  port("awId",4,Direction::In);port("awLen",8,Direction::In);port("awAddr",35,Direction::In);
  port("wValid",1,Direction::In);port("wLast",1,Direction::In);port("wReady",1,Direction::Out);
  port("pendingWReq",maxWrites+1,Direction::In);port("pendingAWReq",maxWrites+1,Direction::In);
  port("nextReady",1,Direction::In);port("nextValid",1,Direction::Out);
  port("nextId",4,Direction::Out);port("nextLen",8,Direction::Out);
  port("nextIsWrite",1,Direction::Out);port("nextAddr",35,Direction::Out);
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
  auto trim=[&](Value x,unsigned w)->Value{return b.create<BitsPrimOp>(loc,x,w-1,0);};
  auto inc=[&](Value x,unsigned w)->Value{return trim(b.create<AddPrimOp>(loc,x,c(w,1)),w);};
  auto enabledReset=a(arg(1),arg(2));
  auto reg=[&](StringRef name,unsigned w,bool reset)->Value{
    if(reset)return b.create<RegResetOp>(loc,u(w),arg(0),enabledReset,c(w,0),name).getResult();
    return b.create<RegOp>(loc,u(w),arg(0),name).getResult();
  };
  auto update=[&](Value r,Value x){con(r,mux(arg(2),x,r));};
  auto enqPtr=reg("enqPtr",3,true),deqPtr=reg("deqPtr",3,true);
  auto maybeFull=reg("maybeFull",1,true),lastGrant=reg("lastGrant",1,false);
  unsigned cw = llvm::Log2_64_Ceil(maxWrites + 1);
  auto completed=reg("completedWrites",cw,true);
  auto ptrMatch=eq(enqPtr,deqPtr),empty=a(ptrMatch,n(maybeFull));
  auto ready=n(a(ptrMatch,maybeFull));
  // Literal two-way RR control: ready can assert even for an invalid input.
  auto maskedAW=a(arg(8),n(lastGrant));
  auto chosen=o(maskedAW,n(arg(3)));
  auto arReady=a(n(maskedAW),ready);
  auto awReady=a(o(n(lastGrant),n(o(maskedAW,arg(3)))),ready);
  con(arg(4),arReady);con(arg(9),awReady);
  auto enq=a(ready,mux(chosen,arg(8),arg(3)));
  update(lastGrant,mux(enq,chosen,lastGrant));
  std::array<unsigned,4> widths{4,8,1,35};
  std::array<std::array<Value,8>,4> memory;
  std::array<Value,4> head;
  std::array<Value,4> data{mux(chosen,arg(10),arg(5)),mux(chosen,arg(11),arg(6)),chosen,mux(chosen,arg(12),arg(7))};
  for(unsigned j=0;j<4;++j){
    for(unsigned i=0;i<8;++i){
      memory[j][i]=reg("entry"+std::to_string(i)+"_field"+std::to_string(j),widths[j],false);
      update(memory[j][i],mux(a(enq,eq(enqPtr,c(3,i))),data[j],memory[j][i]));
    }
    head[j]=memory[j][7];
    for(int i=6;i>=0;--i)head[j]=mux(eq(deqPtr,c(3,i)),memory[j][i],head[j]);
  }
  auto gate=o(n(head[2]),n(eq(completed,c(cw,0))));
  auto nextValid=a(n(empty),gate),deq=a(nextValid,arg(18));
  con(arg(19),nextValid);for(unsigned j=0;j<4;++j)con(arg(20+j),head[j]);
  update(enqPtr,mux(enq,inc(enqPtr,3),enqPtr));
  update(deqPtr,mux(deq,inc(deqPtr,3),deqPtr));
  auto different=b.create<NEQPrimOp>(loc,enq,deq).getResult();
  update(maybeFull,mux(different,enq,maybeFull));
  auto wReady=b.create<LEQPrimOp>(loc,arg(16),arg(17)).getResult();con(arg(15),wReady);
  auto add=a(a(arg(13),wReady),arg(14)),sub=a(deq,head[2]);
  auto full=b.create<GEQPrimOp>(loc,completed,c(cw,maxWrites)).getResult();
  auto up=a(a(add,n(sub)),n(full)),down=a(a(n(add),sub),n(eq(completed,c(cw,0))));
  Value decremented=trim(b.create<SubPrimOp>(loc,completed,c(cw,1)),cw);
  update(completed,mux(up,inc(completed,cw),mux(down,decremented,completed)));
  result=module;return success();
}
