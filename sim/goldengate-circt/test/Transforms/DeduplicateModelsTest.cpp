// See LICENSE for license details.
#include "goldengate/DeduplicateModels.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s);op->print(out);return s; }
const char *fixture = R"mlir(module { firrtl.circuit "Top" {
 firrtl.module @Top() {
  %a_x = firrtl.instance a @A(out x: !firrtl.uint<1>)
  %b_x = firrtl.instance b @B(out x: !firrtl.uint<1>)
 }
 firrtl.module private @A(out %x: !firrtl.uint<1>) {
  %value = firrtl.constant 1 : !firrtl.uint<1>
  firrtl.strictconnect %x, %value : !firrtl.uint<1>
 }
 firrtl.module private @B(out %x: !firrtl.uint<1>) {
  %value = firrtl.constant 1 : !firrtl.uint<1>
  firrtl.strictconnect %x, %value : !firrtl.uint<1>
 }
} })mlir";
void run(MLIRContext &context, unsigned mode) {
 std::string text(fixture);
 if (mode == 5) {
  const std::string oldBody = "%value = firrtl.constant 1 : !firrtl.uint<1>\n  firrtl.strictconnect %x, %value : !firrtl.uint<1>";
  auto body = [](StringRef prefix) { return "%one = firrtl.constant 1 : !firrtl.uint<1>\n%" + prefix.str() + "_part = firrtl.node %one : !firrtl.uint<1>\nfirrtl.strictconnect %x, %" + prefix.str() + "_part : !firrtl.uint<1>"; };
  auto offset=text.find(oldBody);text.replace(offset,oldBody.size(),body("aggregateA"));
  offset=text.find(oldBody);text.replace(offset,oldBody.size(),body("aggregateB"));
 }
 if (mode == 4) {
  auto start = text.find("firrtl.module private @B");
  auto header = text.find("out %x", start); text.replace(header, 6, "out %different");
  auto connection = text.find("%x, %value", start); text.replace(connection, 2, "%different");
  auto instance = text.find("@B(out x:"); text.replace(instance, 9, "@B(out different:");
 }
 if (mode == 3) {
  auto body = [](StringRef firstName, StringRef secondName) {
   return "%one = firrtl.constant 1 : !firrtl.uint<1>\n"
          "%zero = firrtl.constant 0 : !firrtl.uint<1>\n"
          "%" + firstName.str() + " = firrtl.node %one : !firrtl.uint<1>\n"
          "%" + secondName.str() + " = firrtl.node %zero : !firrtl.uint<1>\n"
          "firrtl.strictconnect %x, %" + firstName.str() + " : !firrtl.uint<1>";
  };
  const std::string oldBody = "%value = firrtl.constant 1 : !firrtl.uint<1>\n  firrtl.strictconnect %x, %value : !firrtl.uint<1>";
  auto offset=text.find(oldBody); text.replace(offset,oldBody.size(),body("value","other"));
  offset=text.find(oldBody); text.replace(offset,oldBody.size(),body("other","value"));
 }
 auto root = parseSourceString<ModuleOp>(text, &context);
 require(bool(root), "dedup parse failed");
 auto circuit = *root->getOps<CircuitOp>().begin();
 OpBuilder b(&context);
 SmallVector<Attribute> raw;
 auto target = [&](StringRef kind, StringRef spelling, StringRef group = "") {
  SmallVector<NamedAttribute> fields{b.getNamedAttr("class",b.getStringAttr(kind)),b.getNamedAttr("target",b.getStringAttr(spelling))};
  if (!group.empty()) fields.push_back(b.getNamedAttr("group",b.getStringAttr(group)));
  raw.push_back(b.getDictionaryAttr(fields));
 };
 target(goldengate::AnnotationClasses::EnableModelMultiThreading,"~Top|Top/a:A");
 target(goldengate::AnnotationClasses::EnableModelMultiThreading,"~Top|Top/b:B");
 target("firrtl.transforms.DontTouchAnnotation",mode == 4 ? "~Top|B>different" : "~Top|B>x");
 if (mode == 5) {target("chisel3.experimental.EnumAnnotations$EnumVecAnnotation", "Top.A.aggregateA.part");target("chisel3.experimental.EnumAnnotations$EnumVecAnnotation", "Top.B.aggregateB.part");}
 if (mode == 3) {target("firrtl.transforms.DontTouchAnnotation", "~Top|A>value"); target("firrtl.transforms.DontTouchAnnotation", "~Top|B>value");}
 if (mode == 1) target("test.Unknown", "~Top|B>x");
 if (mode == 2) {target("firrtl.transforms.DedupGroupAnnotation","~Top|A","one");target("firrtl.transforms.DedupGroupAnnotation","~Top|B","two");}
 circuit->setAttr("rawAnnotations",b.getArrayAttr(raw));
 auto before=dump(*root); unsigned removed=77; std::string error;
 auto result=goldengate::deduplicateModels(circuit,removed,error);
 if (mode == 1) { require(failed(result)&&removed==77&&dump(*root)==before,"unknown dedup target was accepted or mutated IR"); return; }
 require(succeeded(result),error);
 require(succeeded(verify(*root)),"dedup invalid IR");
 require(removed == (mode==2?0:1),"dedup did not respect grouping policy");
 if (mode == 5) {
  auto annotations=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(cast<DictionaryAttr>(annotations[3]).getAs<StringAttr>("target") ==
              cast<DictionaryAttr>(annotations[4]).getAs<StringAttr>("target"),
          "legacy aggregate identity did not follow proved corresponding scalar descendants");
 }
 if (mode == 3) {
  auto annotations=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto first=cast<DictionaryAttr>(annotations[3]).getAs<StringAttr>("target").getValue();
  auto second=cast<DictionaryAttr>(annotations[4]).getAs<StringAttr>("target").getValue();
  require(first.split('|').second.split('>').first == second.split('|').second.split('>').first &&
          first.split('>').second != second.split('>').second,
          "same-named different expressions were incorrectly mapped to one canonical identity");
 }
 if (!mode || mode == 4) {
  auto annotations=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto first=cast<DictionaryAttr>(annotations[0]).getAs<StringAttr>("target").getValue();
  auto second=cast<DictionaryAttr>(annotations[1]).getAs<StringAttr>("target").getValue();
  require(first.split(':').second==second.split(':').second,"selectors do not share the actual canonical definition");
  require(cast<DictionaryAttr>(annotations[2]).getAs<StringAttr>("target").getValue()==("~Top|"+first.split(':').second+">"+(first.split(':').second == "A" ? "x" : "different")).str(),"DontTouch did not follow canonical module");
 }
}
}
int main(int argc, char **argv) {
 MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
 try {
  if (argc == 4 && StringRef(argv[1]) == "--input") {
   auto root=parseSourceFile<ModuleOp>(argv[2], &context);
   require(bool(root), "dedup input parse failed");
   auto circuit=*root->getOps<CircuitOp>().begin();unsigned removed=0;std::string error;
   require(succeeded(goldengate::deduplicateModels(circuit,removed,error)),error);
   require(succeeded(verify(*root)),"dedup input verification failed");
   std::error_code ec;llvm::raw_fd_ostream output(argv[3],ec);
   require(!ec,"dedup output open failed");root->print(output);
   llvm::outs()<<"Removed "<<removed<<" duplicate definitions\n";
   return 0;
  }
  require(argc==1,"usage: test [--input snapshot.mlir output.mlir]");
  for(unsigned i=0;i<6;++i)run(context,i);
 } catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}
 llvm::outs()<<"PASS native model dedup, selector/reference identities, policy separation and atomic unknown-schema rejection\n";
}
