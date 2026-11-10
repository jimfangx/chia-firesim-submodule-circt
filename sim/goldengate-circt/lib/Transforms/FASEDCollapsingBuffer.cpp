// See LICENSE for license details.
// Oracle: midas/models/dram/Util.scala217-274. The unusual reversed one-hot
// depth boundary is intentional: do not replace it with occupancy counting.
#include "goldengate/FASEDCollapsingBuffer.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDCollapsingBuffer(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDCollapsingBuffer";
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getModuleName() == name) {
      error = "FASED collapsing buffer helper already exists";
      return failure();
    }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx,w,false); };
  constexpr std::array<const char *,10> names{
      "id","len","isWrite","rowAddr","bankAddrOH","bankAddr",
      "rankAddrOH","rankAddr","isReady","mayPRE"};
  constexpr std::array<unsigned,10> widths{4,8,1,26,8,3,4,2,1,1};
  SmallVector<BundleType::BundleElement> fields;
  for(unsigned j=0;j<names.size();++j)
    fields.push_back({b.getStringAttr(names[j]),false,u(widths[j])});
  auto entryType=BundleType::get(ctx,fields);
  auto validType=BundleType::get(ctx,{{b.getStringAttr("valid"),false,u(1)},
                                     {b.getStringAttr("bits"),false,entryType}});
  auto vectorType=FVectorType::get(validType,8);
  SmallVector<PortInfo> ports;
  auto port=[&](StringRef n,Type t,Direction d){ports.push_back({b.getStringAttr(n),t,d});};
  port("clock",ClockType::get(ctx),Direction::In);
  for(auto n:{"reset","targetFire","enqValid"})port(n,u(1),Direction::In);
  port("enqReady",u(1),Direction::Out);
  port("enqBits",entryType,Direction::In);
  port("entries",vectorType,Direction::Out);
  port("updates",vectorType,Direction::In);
  port("programmableDepth",u(4),Direction::In);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module=b.create<FModuleOp>(loc,b.getStringAttr(name),
      ConventionAttr::get(ctx,Convention::Internal),ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg=[&](unsigned i){return module.getBodyBlock()->getArgument(i);};
  auto constant=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,u(w),APInt(w,n));};
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto and_=[&](Value a,Value z)->Value{return b.create<AndPrimOp>(loc,a,z);};
  auto not_=[&](Value a)->Value{return b.create<NotPrimOp>(loc,a);};
  auto or_=[&](Value a,Value z)->Value{return b.create<OrPrimOp>(loc,a,z);};
  auto mux=[&](Value c,Value a,Value z)->Value{return b.create<MuxPrimOp>(loc,c,a,z);};
  std::array<Value,8> outputs,updates,valid,shift;
  std::array<std::array<Value,10>,8> bits;
  Value enabledReset=and_(arg(1),arg(2));
  for(unsigned i=0;i<8;++i) {
    outputs[i]=b.create<SubindexOp>(loc,arg(6),i);
    updates[i]=b.create<SubindexOp>(loc,arg(7),i);
    std::string stem="entry"+std::to_string(i);
    valid[i]=b.create<RegResetOp>(loc,u(1),arg(0),enabledReset,constant(1,0),stem+"_valid").getResult();
    connect(field(outputs[i],"valid"),valid[i]);
    Value outBits=field(outputs[i],"bits");
    for(unsigned j=0;j<names.size();++j) {
      bits[i][j]=b.create<RegOp>(loc,u(widths[j]),arg(0),stem+"_"+names[j]).getResult();
      connect(field(outBits,names[j]),bits[i][j]);
    }
  }
  Value shifting=constant(1,0);
  for(unsigned i=0;i<8;++i) {
    Value vacant=or_(shifting,not_(field(updates[i],"valid")));
    // UIntToOH(depth).asBools.take(8).reverse gives depth == 7-i.
    // The final entry's linkEntries case deliberately ignores lastEntry.
    Value last=b.create<EQPrimOp>(loc,arg(8),constant(4,7-i));
    shift[i]=i==7?vacant:and_(not_(last),vacant);
    shifting=shift[i];
  }
  connect(arg(4),shift[7]);
  for(unsigned i=0;i<8;++i) {
    Value sourceValid=i==7?arg(3):field(updates[i+1],"valid");
    Value sourceBits=i==7?arg(5):field(updates[i+1],"bits");
    Value ownBits=field(updates[i],"bits");
    Value nextValid=mux(shift[i],sourceValid,field(updates[i],"valid"));
    connect(valid[i],mux(arg(2),nextValid,valid[i]));
    for(unsigned j=0;j<names.size();++j) {
      Value nextBits=mux(shift[i],field(sourceBits,names[j]),field(ownBits,names[j]));
      connect(bits[i][j],mux(arg(2),nextBits,bits[i][j]));
    }
  }
  result=module;
  return success();
}
