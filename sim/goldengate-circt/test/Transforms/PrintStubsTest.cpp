// See LICENSE for license details.
#include "goldengate/PrintStubs.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void check(CircuitOp circuit, ArrayRef<goldengate::PrintStub> stubs, ArrayAttr before) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == before.size() + stubs.size(), "wiring annotation count mismatch");
  for (unsigned i=0; i<before.size(); ++i)
    require(raw[i] == before[i], "input annotations changed before bridge construction");
  for (auto [i, stub] : llvm::enumerate(stubs)) {
    auto print = stub.print;
    auto bundleOp = stub.bundle;
    auto bundle = cast<BundleType>(bundleOp.getResult().getType());
    auto fields = bundle.getElements();
    require(fields.size() == print.getSubstitutions().size()+1 &&
        fields[0].name.getValue() == "enable" &&
        fields[0].type == UIntType::get(circuit.getContext(), 1), "print enable bundle mismatch");
    llvm::DenseMap<unsigned, Value> drivers;
    bundleOp->getParentOfType<FModuleOp>().walk([&](StrictConnectOp connect) {
      auto field = connect.getDest().getDefiningOp<SubfieldOp>();
      if (field && field.getInput() == bundleOp.getResult())
        require(drivers.try_emplace(field.getFieldIndex(), connect.getSrc()).second,
                "duplicate bundle field driver");
    });
    require(drivers.size() == fields.size() && drivers.lookup(0) == print.getCond(),
            "print enable does not drive bundle");
    for (auto [index,arg] : llvm::enumerate(print.getSubstitutions()))
      require(fields[index+1].name.getValue() == "args_"+std::to_string(index) &&
          !fields[index+1].isFlip && fields[index+1].type == arg.getType() &&
          drivers.lookup(index+1) == arg, "argument order/type/SSA identity mismatch");
    require(!fields[0].isFlip && stub.formatString == print.getFormatString() &&
        stub.clock == print.getClock(), "print clock/format changed");
    std::string error;
    require(goldengate::resolveInternalAnnotationTarget(circuit, stub.target, error) ==
        bundleOp.getOperation(), "bundle annotation identity mismatch: "+error);
    auto clock = goldengate::resolveAnnotationTarget(circuit, stub.clockTarget, error);
    if (clock && clock->port) {
      auto module = cast<FModuleOp>(clock->module.getOperation());
      require(module.getBodyBlock()->getArgument(*clock->port) == stub.clock, "clock port identity mismatch");
    } else {
      auto *op = goldengate::resolveInternalAnnotationTarget(circuit, stub.clockTarget, error);
      require(op && op->getNumResults()==1 && op->getResult(0)==stub.clock, "clock declaration identity mismatch");
    }
    auto annotation = cast<DictionaryAttr>(raw[before.size()+i]);
    require(annotation.getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::BridgeTopWiring &&
        annotation.getAs<StringAttr>("target").getValue() == stub.target &&
        annotation.getAs<StringAttr>("clock").getValue() == stub.clockTarget,
        "BridgeTopWiring annotation mismatch");
  }
}
void run(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, in %clock2: !firrtl.clock,
          in %enable: !firrtl.uint<1>, in %a: !firrtl.uint<8>,
          in %b: !firrtl.sint<9>, in %wide: !firrtl.uint<129>) {
        %message_wire = firrtl.wire : !firrtl.uint<1>
        firrtl.strictconnect %message_wire, %enable : !firrtl.uint<1>
      }
    }
  })mlir", &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto module = *circuit.getOps<FModuleOp>().begin();
  auto arg = [&](unsigned index) {return module.getBodyBlock()->getArgument(index);};
  OpBuilder b(&context);
  b.setInsertionPointToEnd(module.getBodyBlock());
  auto loc=module.getLoc();
  auto alias=b.create<NodeOp>(loc,arg(1),b.getStringAttr("clockAlias"));
  auto first=b.create<PrintFOp>(loc,arg(0),arg(2),"quoted \"%d\" \\ %d %x\n",
      ValueRange{arg(3),arg(4),arg(5)},"message");
  auto second=b.create<PrintFOp>(loc,alias.getResult(),arg(2),"constant text\n",ValueRange{},"empty");
  auto computed=b.create<AsClockPrimOp>(loc,arg(2));
  auto unselected=b.create<PrintFOp>(loc,computed.getResult(),arg(2),"unselected\n",ValueRange{},"other");
  auto anno = [&](StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::SynthPrintf)),
        b.getNamedAttr("target",b.getStringAttr(target))});
  };
  // Reversed records and duplicates must still create one bundle per statement.
  auto raw=b.getArrayAttr({anno("~Top|Top>empty"),anno("~Top|Top>message"),anno("~Top|Top>message"),
      b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Opaque"))})});
  circuit->setAttr("rawAnnotations",raw);
  SmallVector<goldengate::PrintStub> stubs;
  std::string error;
  // Disabled synthesis consumes selections without resolving targets, including
  // duplicates and selections with unsupported clocks. All operations survive.
  auto opaque = raw[3];
  auto disabled = b.getArrayAttr({opaque, anno("~Top|Top>message"),
      anno("~Top|Top>message"), anno("~Top|Top>missing"),
      anno("~Top|Top>other"), opaque});
  circuit->setAttr("rawAnnotations", disabled);
  auto moduleBefore = dump(module);
  unsigned removed = 0;
  require(succeeded(goldengate::dropDisabledPrintAnnotations(circuit, removed, error)), error);
  require(removed == 4 && dump(module) == moduleBefore &&
      circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == b.getArrayAttr({opaque, opaque}),
      "disabled PrintSynthesis changed operations or retained annotation order");
  auto cleaned = dump(*root);
  require(succeeded(goldengate::dropDisabledPrintAnnotations(circuit, removed, error)) &&
      removed == 0 && dump(*root) == cleaned, "disabled PrintSynthesis is not idempotent");
  circuit->removeAttr("rawAnnotations");
  auto missing = dump(*root);
  removed = 19;
  require(failed(goldengate::dropDisabledPrintAnnotations(circuit, removed, error)) &&
      removed == 19 && dump(*root) == missing, "missing annotations mutated disabled state");
  circuit->setAttr("rawAnnotations", raw);
  for (StringRef invalid : {"~Top|Top>message_wire", "~Top|Top>missing", "~Top|Top>other"}) {
    SmallVector<Attribute> bad(raw.begin(),raw.end());bad.push_back(anno(invalid));
    circuit->setAttr("rawAnnotations",b.getArrayAttr(bad));
    auto before=dump(*root);
    require(failed(goldengate::synthesizePrintStubs(circuit,stubs,error)) &&
        stubs.empty() && dump(*root)==before,"invalid late selection mutated IR/results");
  }
  circuit->setAttr("rawAnnotations",raw);
  require(succeeded(goldengate::synthesizePrintStubs(circuit,stubs,error)),error);
  require(stubs.size()==2 && stubs[0].print==first && stubs[1].print==second,
          "statement selection/order mismatch");
  require(stubs[0].bundle.getName()!="message_wire" &&
      unselected->getBlock()==module.getBodyBlock(),"name collision or unselected print changed");
  check(circuit,stubs,raw);
  require(succeeded(verify(*root)),"invalid print stub FIRRTL");
}
void runGolden(MLIRContext &context, StringRef path) {
  auto root=parseSourceFile<ModuleOp>(path,&context);
  require(bool(root),"golden candidate parse failed");
  auto circuit=*root->getOps<CircuitOp>().begin();
  auto before=circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<goldengate::PrintStub> stubs;
  std::string error;
  require(succeeded(goldengate::synthesizePrintStubs(circuit,stubs,error)),error);
  require(stubs.size()==372,"golden selected printf count mismatch");
  check(circuit,stubs,before);
  require(succeeded(verify(*root)),"invalid golden print stub FIRRTL");
  llvm::outs()<<"Checked 372 Rocket printf bundle/clock/annotation graphs\n";
}
}
int main(int argc,char **argv) {
  MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {run(context);if(argc==2)runGolden(context,argv[1]);
    llvm::outs()<<"Print stubs PASS\n";return 0;
  } catch(const std::exception &error) {llvm::errs()<<"Print stubs FAIL: "<<error.what()<<'\n';return 1;}
}
