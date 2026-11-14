// See LICENSE for license details.
// Oracle: midas/models/dram/LastLevelCache.scala LLCModel.
#include "goldengate/FASEDLLCModel.h"
#include "goldengate/FASEDLLCMetadata.h"
#include "goldengate/FASEDLLCMSHRTable.h"
#include "mlir/IR/Builders.h"
#include "goldengate/FASEDGeometry.h"
#include <array>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDLLCModel(
    CircuitOp circuit,FModuleOp &result,std::string &error) {
  for(auto module:circuit.getOps<FModuleLike>())
    for(auto name:{"GGFASEDLLCModel","GGFASEDLLCMetadata","GGFASEDLLCMSHRTable"})
      if(module.getModuleName()==name) {error="LLC model requires free model, metadata and MSHR symbols";return failure();}
  auto geometry = getFASEDGeometry(circuit);
  FModuleOp metadata,table;
  if(failed(materializeFASEDLLCMetadata(circuit,metadata,error)) ||
     failed(materializeFASEDLLCMSHRTable(circuit,table,error)))return failure();
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto u=[&](unsigned w){return UIntType::get(ctx,w,false);};
  SmallVector<PortInfo> ports;
  auto port=[&](StringRef n,unsigned w,Direction d){ports.push_back({b.getStringAttr(n),u(w),d});};
  auto in=[&](StringRef n,unsigned w=1){port(n,w,Direction::In);};
  auto out=[&](StringRef n,unsigned w=1){port(n,w,Direction::Out);};
  ports.push_back({b.getStringAttr("clock"),ClockType::get(ctx),Direction::In});
  in("reset");in("targetFire");
  auto req=[&](StringRef n,bool input){
    port((n+"Valid").str(),1,input?Direction::In:Direction::Out);
    port((n+"Ready").str(),1,input?Direction::Out:Direction::In);
    for(auto [suffix,width]:{std::pair<const char*,unsigned>{"Id",4},{"Len",8},{"Addr",35}})
      port((n+suffix).str(),width,input?Direction::In:Direction::Out);
  };
  req("ar",true);req("aw",true);in("wValid");out("wReady");in("wLast");
  out("rRespValid");in("rRespReady");out("rRespId",4);out("rRespLen",8);
  out("wRespValid");in("wRespReady");out("wRespId",4);
  req("memAr",false);req("memAw",false);out("memWValid");in("memWReady");out("memWLast");
  in("memRValid");out("memRReady");in("memRId",4);in("memBValid");out("memBReady");in("memBId",4);
  in("wayBits",2);in("setBits",4);in("blockBits",3);in("activeMSHRs",4);
  for(auto n:{"misses","writebacks","refills"})out(n,32);
  out("peakMSHRs",4);out("state",3);out("busy",8);
  for(auto n:{"hitValid","emptyValid","evictDirty","readStart","writeStart","refillStart"})out(n);
  out("allocated",4);out("nextMshr",3);
  for(auto [m,n,renamed]:{std::tuple<FModuleOp,const char*,const char*>{table,"entries","mshrEntries"},
      {metadata,"metadata","metadata"}})
    for(auto p:m.getPorts())if(p.getName()==n)ports.push_back({b.getStringAttr(renamed),p.type,Direction::Out});
  out("stage1Set",12);out("stage1Tag",35);out("lfsr",16);out("selectedWayOH",8);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDLLCModel"),ConventionAttr::get(ctx,Convention::Internal),ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg=[&](StringRef n)->Value {for(unsigned i=0;i<ports.size();++i)if(ports[i].getName()==n)return module.getBodyBlock()->getArgument(i);llvm_unreachable("LLC port");};
  auto md=b.create<InstanceOp>(loc,metadata,"metadata");auto mt=b.create<InstanceOp>(loc,table,"mshrs");
  auto pin=[&](InstanceOp instance,FModuleOp helper,StringRef n)->Value {for(unsigned i=0;i<helper.getNumPorts();++i)if(helper.getPortName(i)==n)return instance.getResult(i);llvm_unreachable("helper port");};
  auto mp=[&](StringRef n){return pin(md,metadata,n);};auto tp=[&](StringRef n){return pin(mt,table,n);};
  auto c=[&](unsigned w,uint64_t v)->Value{return b.create<ConstantOp>(loc,u(w),APInt(w,v));};
  auto con=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto a=[&](Value x,Value y)->Value{return b.create<AndPrimOp>(loc,x,y);};
  auto o=[&](Value x,Value y)->Value{return b.create<OrPrimOp>(loc,x,y);};
  auto n=[&](Value x)->Value{return b.create<NotPrimOp>(loc,x);};
  auto eq=[&](Value x,Value y)->Value{return b.create<EQPrimOp>(loc,x,y);};
  auto mux=[&](Value z,Value x,Value y)->Value{return b.create<MuxPrimOp>(loc,z,x,y);};
  auto trim=[&](Value x,unsigned w)->Value{return b.create<BitsPrimOp>(loc,x,w-1,0);};
  auto resized=[&](Value x,unsigned w)->Value{return trim(b.create<PadPrimOp>(loc,x,w),w);};
  Value enabledReset=a(arg("reset"),arg("targetFire"));
  auto reg=[&](StringRef name,unsigned width)->Value{return b.create<RegResetOp>(loc,u(width),arg("clock"),enabledReset,c(width,0),name).getResult();};
  auto update=[&](Value r,Value value){con(r,mux(arg("targetFire"),value,r));};
  auto wire=[&](StringRef name,unsigned width)->Value{return b.create<WireOp>(loc,u(width),name).getResult();};
  Value zero=c(1,0),one=c(1,1);
  for(auto port:{"clock","reset","targetFire"}){con(mp(port),arg(port));con(tp(port),arg(port));}
  for(auto name:{"wayBits"})con(mp(name),arg(name));
  con(tp("activeMSHRs"),arg("activeMSHRs"));
  struct Queue {Value ready,valid,id,len,addr;};
  auto queue=[&](StringRef stem,Value valid,Value id,Value len,Value addr,Value ready)->Queue {
    Value enq=reg((stem+"Enq").str(),1),deq=reg((stem+"Deq").str(),1),fullFlag=reg((stem+"Full").str(),1);
    Value equal=eq(enq,deq),canEnq=n(a(equal,fullFlag)),canDeq=n(a(equal,n(fullFlag)));
    Value push=a(valid,canEnq),pop=a(canDeq,ready);
    update(enq,mux(push,n(enq),enq));update(deq,mux(pop,n(deq),deq));
    update(fullFlag,mux(b.create<XorPrimOp>(loc,push,pop),push,fullFlag));
    std::array<Value,3> data{id,len,addr},head;std::array<unsigned,3> widths{4,8,35};
    for(unsigned f=0;f<3;++f) {
      std::array<Value,2> slots;
      for(unsigned i=0;i<2;++i) {
        slots[i]=b.create<RegOp>(loc,u(widths[f]),arg("clock"),(stem+"Data"+Twine(f)+"_"+Twine(i)).str()).getResult();
        update(slots[i],mux(a(push,eq(enq,c(1,i))),data[f],slots[i]));
      }
      head[f]=mux(deq,slots[1],slots[0]);
    }
    return {canEnq,canDeq,head[0],head[1],head[2]};
  };
  Value readReady=wire("readDequeue",1),writeReady=wire("writeDequeue",1);
  Queue reads=queue("reads",arg("arValid"),arg("arId"),arg("arLen"),arg("arAddr"),readReady);
  Queue writes=queue("writes",arg("awValid"),arg("awId"),arg("awLen"),arg("awAddr"),writeReady);
  con(arg("arReady"),reads.ready);con(arg("awReady"),writes.ready);
  Value needRefill=wire("needRefill",1),needWriteback=wire("needWriteback",1);
  Value refillAddr=wire("refillAddress",35),writebackAddr=wire("writebackAddress",35),blockLen=wire("blockLength",8);
  Value mshrId=b.create<PadPrimOp>(loc,tp("nextIndex"),4);
  Queue memReads=queue("memReads",needRefill,mshrId,blockLen,refillAddr,arg("memArReady"));
  Queue memWrites=queue("memWrites",needWriteback,mshrId,blockLen,writebackAddr,arg("memAwReady"));
  for(auto [prefix,q]:{std::pair<const char*,Queue>{"memAr",memReads},{"memAw",memWrites}}) {
    con(arg(std::string(prefix)+"Valid"),q.valid);con(arg(std::string(prefix)+"Id"),q.id);
    con(arg(std::string(prefix)+"Len"),q.len);con(arg(std::string(prefix)+"Addr"),q.addr);
  }
  Value state=reg("state",3),busy=reg("dataArrayBusy",8);
  auto is=[&](unsigned s){return eq(state,c(3,s));};
  Value idle=is(0),readMD=is(1),readWB=is(2),readData=is(3),writeMD=is(4),writeWB=is(5),writeData=is(6),refill=is(7);
  Value stage1=o(readMD,writeMD),wbState=o(readWB,writeWB);
  con(mp("stage1Valid"),stage1);con(mp("stage1IsWrite"),writeMD);
  Value hit=mp("hitValid"),empty=mp("emptyValid"),evict=mp("evictDirty");
  Value resourceHazard=o(n(tp("available")),o(n(memReads.ready),n(memWrites.ready)));
  Value canRead=a(a(a(reads.valid,n(tp("readCollision"))),n(resourceHazard)),arg("rRespReady"));
  Value canWrite=a(a(a(writes.valid,n(tp("writeCollision"))),n(resourceHazard)),arg("wRespReady"));
  Value canRefill=a(arg("memRValid"),o(tp("responseIsWrite"),arg("rRespReady")));
  Value refillStart=a(idle,canRefill),readStart=a(a(idle,n(canRefill)),canRead);
  Value writeStart=a(a(a(idle,n(canRefill)),n(canRead)),canWrite);
  Value start=o(readStart,writeStart);
  auto maskSet=[&](Value addr)->Value {
    Value shifted=b.create<DShrPrimOp>(loc,addr,arg("blockBits"));
    Value mask=trim(b.create<SubPrimOp>(loc,b.create<DShlPrimOp>(loc,c(1,1),arg("setBits")),c(16,1)),12);
    return a(trim(shifted,12),mask);
  };
  Value readSet=maskSet(reads.addr),writeSet=maskSet(writes.addr);
  Value tagShift=b.create<AddPrimOp>(loc,arg("blockBits"),arg("setBits"));
  Value tag=b.create<DShrPrimOp>(loc,mux(writeStart,writes.addr,reads.addr),tagShift);
  con(mp("lookupStart"),start);con(mp("lookupSet"),mux(writeStart,writeSet,readSet));con(mp("lookupTag"),tag);
  con(tp("readSet"),readSet);con(tp("writeSet"),writeSet);
  Value shift = trim(
      b.create<SubPrimOp>(loc, arg("blockBits"), c(3, geometry.byteBits())), 3);
  con(blockLen,trim(b.create<SubPrimOp>(loc,b.create<DShlPrimOp>(loc,c(1,1),shift),c(8,1)),8));
  auto physical=[&](Value set,Value tag)->Value {
    Value setPart=resized(b.create<DShlPrimOp>(loc,set,arg("blockBits")),35);
    Value tagPart=resized(b.create<DShlPrimOp>(loc,tag,tagShift),35);return o(setPart,tagPart);
  };
  con(refillAddr,physical(mp("stage1Set"),mp("stage1Tag")));
  con(writebackAddr,physical(mp("stage1Set"),mp("evictTag")));
  Value shortWrite=b.create<LTPrimOp>(loc,writes.len,blockLen);
  Value refillNeeded=o(a(readMD,n(hit)),a(a(writeMD,shortWrite),n(hit)));
  Value wbNeeded=a(stage1,evict);
  con(needRefill,refillNeeded);con(needWriteback,wbNeeded);
  con(tp("allocate"),o(refillNeeded,wbNeeded));con(tp("allocateSet"),mp("stage1Set"));
  con(tp("allocateId"),mux(readMD,reads.id,writes.id));
  con(tp("allocateLen"),mux(readMD,reads.len,writes.len));con(tp("allocateAddr"),mux(readMD,reads.addr,writes.addr));
  con(tp("allocateIsWrite"),n(readMD));con(tp("allocateRead"),refillNeeded);con(tp("allocateWrite"),wbNeeded);
  con(tp("completedRead"),refillStart);con(tp("completedReadId"),arg("memRId"));
  con(tp("completedWrite"),arg("memBValid"));con(tp("completedWriteId"),arg("memBId"));
  con(arg("memBReady"),one);con(arg("memRReady"),refillStart);
  Value wReady=o(writeData,a(writeMD,n(evict))),wFire=a(wReady,arg("wValid"));
  Value memWValid=wbState,memWLast=eq(busy,c(8,0)),memWFire=a(memWValid,arg("memWReady"));
  con(arg("memWValid"),memWValid);con(arg("memWLast"),memWLast);con(arg("wReady"),wReady);
  con(readReady,readMD);con(writeReady,a(wFire,arg("wLast")));
  con(arg("rRespValid"),o(a(refillStart,n(tp("responseIsWrite"))),a(readMD,hit)));
  con(arg("rRespId"),mux(refillStart,tp("responseId"),reads.id));
  con(arg("rRespLen"),mux(refillStart,tp("responseLen"),reads.len));
  con(arg("wRespValid"),a(o(writeMD,writeData),a(wFire,arg("wLast"))));con(arg("wRespId"),writes.id);
  Value busySet=o(a(stage1,evict),refillStart);
  Value readHit=a(readMD,hit);
  Value writeSetBusy=o(a(writeMD,o(hit,empty)),a(writeWB,a(memWFire,memWLast)));
  Value busySetValid=o(busySet,o(readHit,writeSetBusy));
  Value busySetBits=mux(busySet,blockLen,mux(readHit,reads.len,writes.len));
  Value decr=mux(wbState,memWFire,mux(writeData,arg("wValid"),one));
  Value setLarger=a(busySetValid,b.create<GEQPrimOp>(loc,busySetBits,busy));
  Value busyDec=a(decr,n(eq(busy,c(8,0))));
  update(busy,mux(setLarger,busySetBits,mux(busyDec,trim(b.create<SubPrimOp>(loc,busy,c(8,1)),8),busy)));
  Value next=state;
  // Explicit priority mirrors the eight-state switch; all assignments are
  // gated by targetFire and synchronous reset through the register helper.
  next=mux(a(o(readData,refill),eq(busy,c(8,1))),c(3,0),next);
  next=mux(a(writeData,a(arg("wValid"),arg("wLast"))),c(3,0),next);
  next=mux(a(writeWB,a(memWFire,memWLast)),c(3,6),next);
  next=mux(a(readWB,a(memWFire,memWLast)),c(3,0),next);
  next=mux(writeMD,mux(evict,c(3,5),mux(a(arg("wValid"),arg("wLast")),c(3,0),c(3,6))),next);
  next=mux(readMD,mux(hit,mux(eq(reads.len,c(8,0)),c(3,0),c(3,3)),mux(evict,c(3,2),c(3,0))),next);
  next=mux(idle,mux(canRefill,c(3,7),mux(canRead,c(3,1),mux(canWrite,c(3,4),c(3,0)))),next);
  update(state,next);
  auto count=[&](StringRef name,Value enable) {
    Value value=reg(name,32);update(value,mux(enable,trim(b.create<AddPrimOp>(loc,value,c(32,1)),32),value));con(arg(name),value);
  };
  count("misses",a(stage1,n(hit)));count("writebacks",a(stage1,evict));count("refills",a(readMD,n(hit)));
  Value peak=reg("peakMSHRs",4);update(peak,mux(b.create<LTPrimOp>(loc,peak,tp("allocated")),tp("allocated"),peak));con(arg("peakMSHRs"),peak);
  for(auto [name,value]:{std::pair<const char*,Value>{"state",state},{"busy",busy},{"hitValid",hit},{"emptyValid",empty},{"evictDirty",evict},
      {"readStart",readStart},{"writeStart",writeStart},{"refillStart",refillStart},{"allocated",tp("allocated")},{"nextMshr",tp("nextIndex")}})con(arg(name),value);
  con(arg("mshrEntries"),tp("entries"));
  for(auto name:{"metadata","stage1Set","stage1Tag","lfsr","selectedWayOH"})con(arg(name),mp(name));
  result=module;return success();
}
