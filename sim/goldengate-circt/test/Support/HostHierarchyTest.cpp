// See LICENSE for license details.
// Assembly-only inlining must preserve target hierarchy and final XDC paths.
#include "goldengate/SimulatorRTL.h"
#include "goldengate/XDCEmission.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, StringRef reason) {
  if (!ok) throw std::runtime_error(reason.str());
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned depth) {
  std::string text = "module { firrtl.circuit \"FPGATop\" {\n";
  auto name = [&](unsigned i) {
    return i == 0 ? std::string("FPGATop") :
        "GGStage" + std::to_string(i) + "Wrapper";
  };
  for (unsigned i = 0; i <= depth; ++i) {
    text += "firrtl.module " + std::string(i ? "private " : "") + "@" + name(i) +
      "(in %clock: !firrtl.clock, in %in: !firrtl.uint<8>, out %out: !firrtl.uint<8>) {\n";
    if (i == 0)
      text += "%protocol = firrtl.wire : !firrtl.uint<1>\n"
        "%disabled = firrtl.constant 0 : !firrtl.uint<1>\n"
        "firrtl.connect %protocol, %disabled : !firrtl.uint<1>, !firrtl.uint<1>\n";
    if (i == 2)
      text += "%zero = firrtl.constant 0 : !firrtl.uint<8>\n"
        "%predicate = firrtl.neq %in, %zero : (!firrtl.uint<8>, !firrtl.uint<8>) -> !firrtl.uint<1>\n"
        "%enable = firrtl.constant 1 : !firrtl.uint<1>\n"
        "firrtl.assert %clock, %predicate, %enable, \"anonymous protocol A\" : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1>\n"
        "firrtl.assert %clock, %predicate, %enable, \"anonymous protocol B\" : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1>\n"
        "firrtl.assert %clock, %predicate, %enable, \"named protocol\" : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<1> {name = \"protocol\"}\n";
    auto child = i == depth ? "Target" : name(i + 1);
    text += "%child:3 = firrtl.instance " + std::string(i == depth ? "target" : "sim") +
      " @" + child + "(in clock: !firrtl.clock, in in: !firrtl.uint<8>, out out: !firrtl.uint<8>)\n"
      "firrtl.connect %child#0, %clock : !firrtl.clock, !firrtl.clock\n"
      "firrtl.connect %child#1, %in : !firrtl.uint<8>, !firrtl.uint<8>\n"
      "firrtl.connect %out, %child#2 : !firrtl.uint<8>, !firrtl.uint<8>\n}\n";
  }
  text += "firrtl.module private @Target(in %clock: !firrtl.clock, in %in: !firrtl.uint<8>, out %out: !firrtl.uint<8>) {\n"
    "%state = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<8>\n"
    "firrtl.connect %state, %in : !firrtl.uint<8>, !firrtl.uint<8>\n"
    "firrtl.connect %out, %state : !firrtl.uint<8>, !firrtl.uint<8>\n}\n} }";
  auto result = parseSourceString<ModuleOp>(text, &ctx);
  require(bool(result), "fixture parse");
  OpBuilder b(&ctx); auto c = *result->getOps<CircuitOp>().begin();
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::XDCPaths)),
      b.getNamedAttr("postLinkPath", b.getStringAttr("firesim_top/top"))})}));
  return result;
}
}
int main(int argc, char **argv) {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::chirrtl::CHIRRTLDialect,
                    circt::hw::HWDialect>();
    std::string error; unsigned count;
    for (unsigned depth : {1u, 3u, 98u}) {
      auto root = fixture(ctx, depth);
      auto c = *root->getOps<CircuitOp>().begin();
      FModuleOp target;
      for (auto m : c.getOps<FModuleOp>()) if (m.getName() == "Target") target = m;
      auto targetBefore = dump(target);
      require(succeeded(goldengate::normalizeHostHierarchy(*root, "/tmp/host-hierarchy-test.sv", count, error)), error);
      require(count == depth - 1 && succeeded(verify(*root)), "wrong collapse count or invalid IR");
      // The source circuit operation survives; its old child handles do not.
      c = *root->getOps<CircuitOp>().begin();
      for (auto m : c.getOps<FModuleOp>()) if (m.getName() == "Target") target = m;
      require(dump(target) == targetBefore, "target body changed");
      unsigned modules = 0; for (auto m : c.getOps<FModuleOp>()) ++modules;
      require(modules == 3, "host wrappers remain or target was flattened");
      if (depth > 1) {
        unsigned anonymous = 0, named = 0;
        c.walk([&](AssertOp assertion) {
          if (assertion.getMessage().starts_with("anonymous protocol")) {
            require(assertion.getName().empty(),
                    "inlined anonymous assertion acquired label: " +
                        assertion.getName().str());
            ++anonymous;
          } else if (assertion.getMessage() == "named protocol") {
            require(assertion.getName() == "protocol_0",
                    "named assertion lost collision handling");
            ++named;
          }
          require(assertion.getPredicate().getDefiningOp<NEQPrimOp>() &&
                  assertion.getEnable().getDefiningOp<ConstantOp>().getValue() == 1 &&
                  assertion.getClock().getType() == ClockType::get(&ctx),
                  "assertion predicate, enable or clock changed");
        });
        require(anonymous == 2 && named == 1, "inlining lost protocol assertions");
      }
      auto normalized = dump(*root);
      require(succeeded(goldengate::normalizeHostHierarchy(*root, "/tmp/host-hierarchy-test.sv", count, error)) && count == 0 && dump(*root) == normalized, "normalization is not idempotent");
      if (depth == 98) require(normalized.find("sim_sim_") == std::string::npos, "unbounded instance name prefix remains");
    }
    // Preserve explicitly referenced XDC roots, including aggregate/reference
    // metadata: the writer must resolve the retained operation after inlining.
    auto referenced = fixture(ctx, 4);
    auto rc = *referenced->getOps<CircuitOp>().begin();
    OpBuilder b(&ctx);
    auto raw = rc->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> annotations(raw.begin(), raw.end());
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::InternalXDC)),
      b.getNamedAttr("destinationFile", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::XDCSynthesis))})),
      b.getNamedAttr("formatString", b.getStringAttr("reference:{}")),
      b.getNamedAttr("argumentList", b.getArrayAttr({b.getStringAttr("~FPGATop|GGStage2Wrapper>in")}))}));
    rc->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    require(succeeded(goldengate::normalizeHostHierarchy(*referenced, "/tmp/host-hierarchy-test.sv", count, error)) && count == 2, "explicit XDC root was inlined");
    require(succeeded(goldengate::prepareXDCOutput(rc, error)), error);
    bool resolved = false;
    for (auto attr : rc->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      auto d = cast<DictionaryAttr>(attr); auto body = d.getAs<StringAttr>("fileBody");
      resolved |= body && body.getValue().contains("reference:sim_sim/in");
    }
    require(resolved, "explicit XDC root did not follow the final hierarchy");
    // Inner symbols are identities in a module's namespace. A host wrapper
    // carrying one is a retained boundary, even without a serialized XDC use.
    for (bool portSymbol : {false, true}) {
      auto symbolic = fixture(ctx, 4);
      auto circuit = *symbolic->getOps<CircuitOp>().begin();
      FModuleOp boundary;
      for (auto module : circuit.getOps<FModuleOp>())
        if (module.getName() == "GGStage2Wrapper") boundary = module;
      if (portSymbol)
        boundary.setPortSymbol(1, "retained_input");
      else {
        auto instance = *boundary.getOps<InstanceOp>().begin();
        instance.setInnerSymAttr(circt::hw::InnerSymAttr::get(
            b.getStringAttr("retained_instance")));
      }
      require(succeeded(verify(*symbolic)), "invalid inner symbol fixture");
      require(succeeded(goldengate::normalizeHostHierarchy(*symbolic,
          "/tmp/host-hierarchy-test.sv", count, error)), error);
      bool retained = false;
      for (auto module : circuit.getOps<FModuleOp>())
        if (module.getName() == "GGStage2Wrapper") {
          retained = portSymbol ? module.getPortSymbolAttr(1).getSymName() == "retained_input"
              : (*module.getOps<InstanceOp>().begin()).getInnerSymAttr().getSymName() == "retained_instance";
        }
      require(retained, "inner symbol module identity was removed");
    }
    // Shared construction wrappers are ambiguous: reject rather than clone
    // the helper state into a different number of instances.
    auto shared = fixture(ctx, 3); auto sc = *shared->getOps<CircuitOp>().begin();
    FModuleOp sharedTop;
    for (auto m : sc.getOps<FModuleOp>()) if (m.getName() == "FPGATop") sharedTop = m;
    auto sim = *sharedTop.getOps<InstanceOp>().begin();
    b.setInsertionPointAfter(sim); auto copy = b.clone(*sim.getOperation());
    copy->setAttr("name", b.getStringAttr("extra"));
    auto sharedBefore = dump(*shared);
    require(failed(goldengate::normalizeHostHierarchy(*shared, "/tmp/host-hierarchy-test.sv", count, error)) && dump(*shared) == sharedBefore, "shared wrapper rejection mutated source");
    // A malformed boundary rejects atomically.
    auto invalid = fixture(ctx, 3); auto c = *invalid->getOps<CircuitOp>().begin();
    c->removeAttr("rawAnnotations"); auto before = dump(*invalid);
    require(failed(goldengate::normalizeHostHierarchy(*invalid, "/tmp/host-hierarchy-test.sv", count, error)) && dump(*invalid) == before, "rejection mutated source");
    llvm::outs() << "Host hierarchy depth 1/3/98, anonymous assertion controls/names, named assertion collisions, target preservation, bounded names, XDC references, port/instance symbol identities, idempotence and atomic rejections passed\n";
    if (argc == 4) {
      auto baseline = fixture(ctx, 98);
      require(succeeded(goldengate::emitSimulatorRTL(*baseline, "fixture", std::string(argv[3]) + "/baseline.sv", error)), error);
      require(succeeded(goldengate::normalizeHostHierarchy(*baseline, std::string(argv[3]) + "/collapsed.sv", count, error)), error);
      require(succeeded(goldengate::emitSimulatorRTL(*baseline, "fixture", std::string(argv[3]) + "/collapsed.sv", error)), error);
    }
    if (argc >= 3) {
      auto real = parseSourceFile<ModuleOp>(argv[1], &ctx);
      require(bool(real), "real Rocket boundary parse");
      require(succeeded(goldengate::normalizeHostHierarchy(*real, std::string(argv[2]) + "/FireSim-generated.sv", count, error)), error);
      llvm::outs() << "Rocket inlined wrappers: " << count << '\n';
      auto rc = *real->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::prepareXDCOutput(rc, error)), error);
      require(succeeded(goldengate::emitOutputFiles(rc, argv[2], "FireSim-generated", error)), error);
      std::error_code ec; llvm::raw_fd_ostream ir(std::string(argv[2]) + "/normalized.mlir", ec);
      require(!ec, "normalized IR output"); real->print(ir); ir.close();
      require(succeeded(goldengate::emitSimulatorRTL(*real, argv[1], std::string(argv[2]) + "/FireSim-generated.sv", error)), error);
      llvm::outs() << "Rocket normalized IR, final XDC and RTL emission passed\n";
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
