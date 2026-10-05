// See LICENSE for license details.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
struct Field { std::string name, type; unsigned width; };
struct Record { std::string name, format; std::vector<Field> fields; };
struct Bridge { std::string target, reset; std::vector<Record> records; };
DictionaryAttr annotation(OpBuilder &b, const Bridge &bridge) {
  SmallVector<Attribute> records;
  for (auto &record : bridge.records) {
    SmallVector<Attribute> fields;
    for (auto &field : record.fields)
      fields.push_back(b.getDictionaryAttr({b.getNamedAttr(field.name,b.getStringAttr(field.type))}));
    records.push_back(b.getDictionaryAttr({
        b.getNamedAttr("name",b.getStringAttr(record.name)),
        b.getNamedAttr("ports",b.getArrayAttr(fields)),
        b.getNamedAttr("format",b.getStringAttr(record.format))}));
  }
  return b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
      b.getNamedAttr("target",b.getStringAttr(bridge.target)),
      b.getNamedAttr("widgetClass",b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeModule)),
      b.getNamedAttr("widgetConstructorKey",b.getDictionaryAttr({
          b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::PrintBridgeParameters)),
          b.getNamedAttr("resetPortName",b.getStringAttr(bridge.reset)),
          b.getNamedAttr("printPorts",b.getArrayAttr(records))}))});
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, ArrayRef<Bridge> bridges) {
  auto root=parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top() {}
      firrtl.module @GGPrintBridgePayload() {}
    }
  })mlir",&context);
  require(bool(root),"payload fixture parse failed");
  auto circuit=*root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> raw{b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("test.Opaque")),
      b.getNamedAttr("value",b.getStringAttr("preserve me"))})};
  for(auto &bridge:bridges)raw.push_back(annotation(b,bridge));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(raw));
  return root;
}
unsigned width(Type type) {
  auto integer=dyn_cast<IntType>(type);
  require(integer && integer.getWidthOrSentinel()>=0,"interpreter needs a sized integer");
  return integer.getWidthOrSentinel();
}
// Evaluate the emitted FIRRTL graph, including repeated aggregate projections.
// Inputs are independent leaf bit patterns; signed arguments retain two's-complement bits.
struct Interpreter {
  FModuleOp module;
  std::map<std::string,Value> drivers;
  std::map<std::string,llvm::APInt> inputs,memo;
  std::string key(Value value) {
    if(auto field=value.getDefiningOp<SubfieldOp>())
      return key(field.getInput())+"."+field.getFieldName().str();
    if(auto arg=dyn_cast<BlockArgument>(value))
      return module.getPortName(arg.getArgNumber()).str();
    return std::to_string(reinterpret_cast<uintptr_t>(value.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp module):module(module) {
    module.walk([&](Operation *op) {
      if(auto connect=dyn_cast<FConnectLike>(op))
        require(drivers.emplace(key(connect.getDest()),connect.getSrc()).second,
                "payload has multiple output drivers");
    });
  }
  llvm::APInt eval(Value value) {
    auto name=key(value);
    if(auto found=memo.find(name);found!=memo.end())return found->second;
    if(auto found=inputs.find(name);found!=inputs.end())return found->second;
    if(auto found=drivers.find(name);found!=drivers.end())return eval(found->second);
    auto *op=value.getDefiningOp();
    require(op!=nullptr,"payload leaf has no input or driver: "+name);
    auto arg=[&](unsigned i){return eval(op->getOperand(i));};
    unsigned resultWidth=width(value.getType());
    llvm::APInt result(resultWidth,0);
    if(auto constant=dyn_cast<ConstantOp>(op))result=constant.getValue();
    else if(isa<AndPrimOp>(op))result=arg(0)&arg(1);
    else if(isa<OrPrimOp>(op)) {
      auto a=arg(0).zextOrTrunc(resultWidth),b=arg(1).zextOrTrunc(resultWidth);result=a|b;
    } else if(isa<NotPrimOp>(op))result=~arg(0);
    else if(isa<CatPrimOp>(op)) {
      auto a=arg(0),b=arg(1);
      result=a.zext(resultWidth).shl(b.getBitWidth())|b.zext(resultWidth);
    } else if(isa<AsUIntPrimOp,NodeOp>(op))result=arg(0);
    else if(isa<PadPrimOp>(op)) {
      auto a=arg(0);
      result=isa<SIntType>(op->getOperand(0).getType())?a.sextOrTrunc(resultWidth):a.zextOrTrunc(resultWidth);
    } else if(auto bits=dyn_cast<BitsPrimOp>(op))
      result=arg(0).lshr(bits.getLo()).trunc(resultWidth);
    else throw std::runtime_error("unsupported payload operation: "+op->getName().getStringRef().str());
    result=result.zextOrTrunc(resultWidth);memo.emplace(name,result);return result;
  }
};
unsigned tokenWidth(const Bridge &bridge) {
  unsigned used=2;for(auto &record:bridge.records)for(auto &field:record.fields)used+=field.width;
  unsigned padded=8;while(padded<used)padded*=2;return padded;
}
void checkMetadata(FModuleOp module,const Bridge &bridge) {
  auto metadata=module->getAttrOfType<DictionaryAttr>("goldengate.printPayload");
  require(bool(metadata),"payload module lacks collateral metadata");
  auto integer=[&](StringRef name){auto value=metadata.getAs<IntegerAttr>(name);
    require(bool(value),"payload metadata integer missing: "+name.str());return value.getInt();};
  unsigned bits=tokenWidth(bridge),idle=std::min(16u,bits)-1;
  require(metadata.getAs<StringAttr>("bridgeTarget").getValue()==bridge.target &&
      metadata.getAs<StringAttr>("resetPortName").getValue()==bridge.reset &&
      integer("tokenBits")==bits && integer("tokenBytes")==bits/8 &&
      integer("idleCycleBits")==idle && integer("idleCycleMask")==((uint64_t(1)<<idle)-1)*2,
      "payload token geometry disagrees with Scala PrintBridgeModule");
  auto records=metadata.getAs<ArrayAttr>("records");
  require(records && records.size()==bridge.records.size(),"payload metadata lost print records");
  unsigned offset=1;
  for(auto [i,attr]:llvm::enumerate(records)) {
    auto record=cast<DictionaryAttr>(attr);auto &expected=bridge.records[i];
    unsigned recordWidth=0;for(auto &field:expected.fields)recordWidth+=field.width;
    require(record.getAs<StringAttr>("name").getValue()==expected.name &&
        record.getAs<StringAttr>("format").getValue()==expected.format &&
        record.getAs<IntegerAttr>("offset").getInt()==offset &&
        record.getAs<IntegerAttr>("width").getInt()==recordWidth,
        "print record order/offset/format mismatch");
    auto args=record.getAs<ArrayAttr>("argumentWidths");
    require(args && args.size()+1==expected.fields.size(),"argument width metadata lost zero-width/no-arg fields");
    for(auto [j,value]:llvm::enumerate(args))
      require(cast<IntegerAttr>(value).getInt()==expected.fields[j+1].width,"argument width differs from constructor");
    offset+=recordWidth;
  }
  require(module.getNumPorts()==3 && module.getPortName(0)=="hBits" &&
      module.getPortDirection(0)==Direction::In && module.getPortName(1)=="valid" &&
      module.getPortDirection(1)==Direction::Out && module.getPortName(2)=="data" &&
      module.getPortDirection(2)==Direction::Out && width(module.getPortType(1))==1 &&
      width(module.getPortType(2))==bits,"payload module interface mismatch");
  auto bag=cast<BundleType>(module.getPortType(0));auto fields=bag.getElements();
  require(fields.size()==bridge.records.size()+1 && fields[0].name.getValue()==bridge.reset &&
      !fields[0].isFlip && width(fields[0].type)==1,"payload hBits reset field mismatch");
  for(auto [i,record]:llvm::enumerate(bridge.records)) {
    require(fields[i+1].name.getValue()==record.name && !fields[i+1].isFlip,"payload hBits record ordering mismatch");
    auto elements=cast<BundleType>(fields[i+1].type).getElements();
    require(elements.size()==record.fields.size(),"payload record field count mismatch");
    for(auto [j,field]:llvm::enumerate(record.fields))
      require(elements[j].name.getValue()==field.name && !elements[j].isFlip &&
          width(elements[j].type)==field.width &&
          isa<SIntType>(elements[j].type)==StringRef(field.type).starts_with("SInt"),
          "payload hBits signed field identity mismatch");
  }
}
void checkBehavior(FModuleOp module,const Bridge &bridge,std::mt19937_64 &random) {
  Interpreter interpreter(module);unsigned bits=tokenWidth(bridge);
  for(unsigned sample=0;sample<256;++sample) {
    interpreter.inputs.clear();interpreter.memo.clear();bool reset=sample%3==0,enabled=false;
    interpreter.inputs.emplace("hBits."+bridge.reset,llvm::APInt(1,reset));
    llvm::APInt expected(bits,0);unsigned offset=1;
    for(auto &record:bridge.records)for(auto [i,field]:llvm::enumerate(record.fields)) {
      if(field.width==0)continue;
      llvm::APInt value(field.width,random());
      for(unsigned shift=64;shift<field.width;shift+=64)
        value|=llvm::APInt(field.width,random()).shl(shift);
      // Explicit all-disabled and all-enabled cases supplement random enables.
      if(i==0) {value=llvm::APInt(1,sample%4==0?0:sample%4==1?1:random()&1);enabled|=value.getBoolValue();}
      interpreter.inputs.emplace("hBits."+record.name+"."+field.name,value);
      expected|=value.zextOrTrunc(bits).shl(offset);offset+=field.width;
    }
    bool valid=enabled&&!reset;if(valid)expected.setBit(0);
    auto actualValid=interpreter.eval(module.getBodyBlock()->getArgument(1));
    auto actualData=interpreter.eval(module.getBodyBlock()->getArgument(2));
    require(actualValid==llvm::APInt(1,valid),"payload validity failed enable/reset truth table");
    require(actualData==expected,"payload packing lost argument bits, record order, reset suppression, or padding");
  }
}
void validCases(MLIRContext &context) {
  Record mixed{"first","signed %d unsigned %x\n",{{"enable","UInt<1>",1},{"u","UInt<3>",3},{"s","SInt<5>",5},{"zero","UInt<0>",0}}};
  Record other{"second","negative %d",{{"enable","UInt<1>",1},{"s","SInt<17>",17}}};
  std::vector<Bridge> cases{
      {"~Top|Top>print","reset",{mixed,other}},
      {"~Top|Top>print","reset",{other,mixed}},
      {"~Top|Top>print","reset",{{"boundary","%d",{{"enable","UInt<1>",1},{"arg","UInt<6>",6}}}}},
      {"~Top|Top>print","reset",{{"empty","constant",{{"enable","UInt<1>",1}}}}},
      {"~Top|Top>print","reset",{{"zero","%d",{{"enable","UInt<1>",1},{"arg","SInt<0>",0}}}}},
      {"~Top|Top>print","reset",{{"wide","%d",{{"enable","UInt<1>",1},{"arg","SInt<129>",129},{"tail","UInt<2>",2}}}}}};
  std::mt19937_64 random(0x51a7c1);
  for(auto &bridge:cases) {
    auto root=fixture(context,{bridge});auto circuit=*root->getOps<CircuitOp>().begin();
    auto raw=circuit->getAttr("rawAnnotations");SmallVector<FModuleOp> modules;std::string error;
    require(succeeded(goldengate::materializePrintBridgePayloads(circuit,modules,error)),error);
    require(modules.size()==1 && modules[0].getName()!="GGPrintBridgePayload" &&
        modules[0].isPublic() && circuit->getAttr("rawAnnotations")==raw,
        "payload module naming, visibility, or input annotation preservation failed");
    checkMetadata(modules[0],bridge);checkBehavior(modules[0],bridge,random);
    require(succeeded(verify(*root)),"payload materialization produced invalid FIRRTL");
    auto before=dump(*root);auto count=modules.size();
    require(failed(goldengate::materializePrintBridgePayloads(circuit,modules,error)) &&
        dump(*root)==before && modules.size()==count,"repeated payload materialization mutated module graph");
  }
  // Scala emits one constructor per clock, sharing the synthesizedPrintf target.
  Bridge a{"~Top|Top>synthesizedPrintf","reset0",{mixed}};
  Bridge b{"~Top|Top>synthesizedPrintf","reset1",{other}};
  auto root=fixture(context,{a,b});auto circuit=*root->getOps<CircuitOp>().begin();
  SmallVector<FModuleOp> modules;std::string error;
  require(succeeded(goldengate::materializePrintBridgePayloads(circuit,modules,error)),error);
  require(modules.size()==2 && modules[0].getName()!=modules[1].getName(),"independent clock payload modules were merged");
  checkMetadata(modules[0],a);checkMetadata(modules[1],b);
  checkBehavior(modules[0],a,random);checkBehavior(modules[1],b,random);
  require(succeeded(verify(*root)),"two-clock payload modules contain invalid FIRRTL");
}
void invalidCases(MLIRContext &context) {
  Bridge good{"~Top|Top>print","reset",{{"record","%d",{{"enable","UInt<1>",1},{"arg","UInt<8>",8}}}}};
  std::vector<Bridge> invalid;
  for(auto type:{"UInt<2>","SInt<1>","Clock","UInt","UInt<-1>"}) {
    auto bad=good;bad.records[0].fields[0].type=type;invalid.push_back(bad);
  }
  {auto bad=good;bad.records[0].fields.erase(bad.records[0].fields.begin());invalid.push_back(bad);}
  {auto bad=good;bad.records[0].fields.push_back(bad.records[0].fields.back());invalid.push_back(bad);}
  {auto bad=good;bad.records.push_back(bad.records[0]);invalid.push_back(bad);}
  {auto bad=good;bad.records[0].name=bad.reset;invalid.push_back(bad);}
  {auto bad=good;bad.records[0].fields[1].type="AsyncReset";invalid.push_back(bad);}
  {auto bad=good;bad.records[0].fields[1].name="clock";invalid.push_back(bad);}
  for(auto &bad:invalid) {
    bad.target="~Top|Top>invalid";
    auto root=fixture(context,{good,bad});auto circuit=*root->getOps<CircuitOp>().begin();
    SmallVector<FModuleOp> modules{*circuit.getOps<FModuleOp>().begin()};std::string error;
    auto before=dump(*root);
    require(failed(goldengate::materializePrintBridgePayloads(circuit,modules,error)) &&
        !error.empty() && dump(*root)==before && modules.size()==1,
        "late invalid constructor committed partial modules/results");
  }
  for(unsigned mode=0;mode<6;++mode) {
    auto root=fixture(context,{good,good});auto circuit=*root->getOps<CircuitOp>().begin();OpBuilder builder(&context);
    auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> edited(raw.begin(),raw.end());
    NamedAttrList bridge(cast<DictionaryAttr>(edited.back()));NamedAttrList key(cast<DictionaryAttr>(bridge.get("widgetConstructorKey")));
    if(mode==0)bridge.erase("target");
    if(mode==1)key.set("class",builder.getStringAttr("test.WrongConstructor"));
    if(mode==2)key.set("printPorts",builder.getStringAttr("invalid"));
    if(mode==3)key.erase("resetPortName");
    if(mode==4)key.erase("class");
    if(mode==5)key.set("class",builder.getI64IntegerAttr(7));
    bridge.set("widgetConstructorKey",key.getDictionary(&context));edited.back()=bridge.getDictionary(&context);
    circuit->setAttr("rawAnnotations",builder.getArrayAttr(edited));SmallVector<FModuleOp> modules;std::string error;auto before=dump(*root);
    require(failed(goldengate::materializePrintBridgePayloads(circuit,modules,error)) &&
        dump(*root)==before && modules.empty(),"malformed constructor partially materialized a valid earlier bridge");
  }
  auto root=fixture(context,{});auto circuit=*root->getOps<CircuitOp>().begin();SmallVector<FModuleOp> modules;std::string error;auto before=dump(*root);
  require(succeeded(goldengate::materializePrintBridgePayloads(circuit,modules,error)) &&
      modules.empty() && dump(*root)==before,"no-print bridge materialization changed IR");
  // Other BridgeIO records may lack a widget class; skip them safely without
  // dereferencing a null StringAttr or changing unrelated retained records.
  for(bool nonString:{false,true}) {
    auto root=fixture(context,{good});auto circuit=*root->getOps<CircuitOp>().begin();OpBuilder builder(&context);
    auto raw=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> edited(raw.begin(),raw.end());
    NamedAttrList bridge(cast<DictionaryAttr>(edited.back()));
    if(nonString)bridge.set("widgetClass",builder.getI64IntegerAttr(5));
    else bridge.erase("widgetClass");
    edited.back()=bridge.getDictionary(&context);circuit->setAttr("rawAnnotations",builder.getArrayAttr(edited));
    SmallVector<FModuleOp> modules;std::string error;auto before=dump(*root);
    require(succeeded(goldengate::materializePrintBridgePayloads(circuit,modules,error)) &&
        modules.empty() && dump(*root)==before,"non-PrintBridge widget metadata was not skipped safely");
  }
}
} // namespace
int main() {
  MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {validCases(context);invalidCases(context);llvm::outs()<<"PrintBridge payload PASS\n";return 0;}
  catch(const std::exception &error){llvm::errs()<<"PrintBridge payload FAIL: "<<error.what()<<'\n';return 1;}
}
