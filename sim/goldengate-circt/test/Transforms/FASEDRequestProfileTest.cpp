// See LICENSE for license details.
#include "circt/Dialect/HW/HWDialect.h"
#include "goldengate/FASEDRequestLimits.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, StringRef message) {
  if (!value)
    throw std::runtime_error(message.str());
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context,
                              bool mapped,
                              Attribute flight,
                              Attribute base = {}) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "Top" { firrtl.module @Top() {} }
  })",
                                          &context);
  require(bool(root), "profile fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  NamedAttrList edge;
  if (flight)
    edge.set("maxFlight", flight);
  auto key = b.getDictionaryAttr(
      {b.getNamedAttr("axi4Edge", edge.getDictionary(&context))});
  auto bridge = b.getDictionaryAttr(
      {b.getNamedAttr(
           "class",
           b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
       b.getNamedAttr("widgetClass",
                      b.getStringAttr("midas.models.FASEDMemoryTimingModel")),
       b.getNamedAttr("widgetConstructorKey", key)});
  circuit->setAttr("rawAnnotations", b.getArrayAttr({bridge}));
  if (base)
    circuit->setAttr("goldengate.fasedBaseCapacity", base);
  if (mapped) {
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    auto engine =
        b.create<FModuleOp>(circuit.getLoc(),
                            b.getStringAttr("GGFASEDTokenEngine"),
                            ConventionAttr::get(&context, Convention::Internal),
                            ArrayRef<PortInfo>{});
    engine->setAttr("goldengate.bridgeConstructor", key);
  }
  return root;
}
void rejects(CircuitOp circuit) {
  unsigned limit = 91, width = 92;
  auto before = dump(circuit);
  std::string error;
  require(failed(goldengate::getFASEDRequestLimitProfile(
              circuit, limit, width, error)) &&
              !error.empty() && limit == 91 && width == 92 &&
              dump(circuit) == before,
          "malformed profile changed IR or result outputs");
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    OpBuilder b(&context);
    unsigned cases = 0, rejections = 0;
    for (bool mapped : {false, true}) {
      for (unsigned base : {0u, 16u, 32u})
        for (int64_t flight : {int64_t(1),
                               int64_t(3),
                               int64_t(10),
                               int64_t(16),
                               int64_t(20),
                               int64_t(32),
                               int64_t(64),
                               INT64_MAX}) {
          auto root = fixture(context,
                              mapped,
                              b.getI64IntegerAttr(flight),
                              base ? b.getI32IntegerAttr(base) : Attribute());
          auto circuit = *root->getOps<CircuitOp>().begin();
          auto before = dump(circuit);
          unsigned limit = 0, width = 0;
          std::string error;
          unsigned expected = std::min<int64_t>(base ? base : 16, flight);
          require(succeeded(goldengate::getFASEDRequestLimitProfile(
                      circuit, limit, width, error)) &&
                      limit == expected &&
                      width == llvm::Log2_64_Ceil(expected + 1) &&
                      dump(circuit) == before,
                  "functional profile or atomicity mismatch");
          if (base == 32) {
            // These words are absent in FRFCFS even when the effective limit is
            // ten.
            auto top = *circuit.getOps<FModuleOp>().begin();
            auto result = top;
            require(failed(goldengate::materializeFASEDRequestLimits(
                        circuit, result, error)) &&
                        result == top && dump(circuit) == before,
                    "FRFCFS incorrectly materialized LatencyPipe MMIO words");
            require(
                failed(
                    goldengate::attachFASEDRequestLimits(circuit, {}, error)) &&
                    failed(goldengate::addFASEDRequestLimits(circuit, error)) &&
                    dump(circuit) == before,
                "FRFCFS incorrectly attached LatencyPipe MMIO words");
          }
          ++cases;
        }
      SmallVector<Attribute> badBases{
          b.getStringAttr("32"),
          b.getBoolAttr(true),
          b.getI64IntegerAttr(-1),
          b.getI32IntegerAttr(0),
          b.getI32IntegerAttr(10),
          b.getI32IntegerAttr(20),
          b.getI32IntegerAttr(64),
          b.getIntegerAttr(IntegerType::get(&context, 128), APInt(128, 32))};
      for (auto bad : badBases) {
        auto root = fixture(context, mapped, b.getI64IntegerAttr(20), bad);
        rejects(*root->getOps<CircuitOp>().begin());
        ++rejections;
      }
      SmallVector<Attribute> badFlights{
          Attribute(),
          b.getStringAttr("20"),
          b.getI64IntegerAttr(-1),
          b.getI32IntegerAttr(0),
          b.getIntegerAttr(IntegerType::get(&context, 128), APInt(128, 20))};
      for (auto bad : badFlights) {
        auto root = fixture(context, mapped, bad, b.getI32IntegerAttr(32));
        rejects(*root->getOps<CircuitOp>().begin());
        ++rejections;
      }
    }
    // A mapped engine is authoritative, including a missing key. A conflicting
    // early annotation must not silently replace its constructor.
    auto root = fixture(
        context, true, b.getI64IntegerAttr(20), b.getI32IntegerAttr(32));
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto original = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    NamedAttrList conflicting(cast<DictionaryAttr>(original[0]));
    conflicting.set("widgetConstructorKey", b.getDictionaryAttr({b.getNamedAttr(
        "axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI64IntegerAttr(1))}))}));
    circuit->setAttr("rawAnnotations", b.getArrayAttr({conflicting.getDictionary(&context)}));
    unsigned limit = 0, width = 0;
    std::string error;
    require(succeeded(goldengate::getFASEDRequestLimitProfile(
                circuit, limit, width, error)) &&
                limit == 20 && width == 5,
            "mapped profile did not remain authoritative");
    for (auto m : circuit.getOps<FModuleOp>())
      if (m.getName() == "GGFASEDTokenEngine")
        m->removeAttr("goldengate.bridgeConstructor");
    rejects(circuit);
    ++rejections;
    // Before mapping, there must be exactly one retained FASED constructor.
    root = fixture(
        context, false, b.getI64IntegerAttr(20), b.getI32IntegerAttr(32));
    circuit = *root->getOps<CircuitOp>().begin();
    auto annotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    circuit->setAttr("rawAnnotations",
                     b.getArrayAttr({annotations[0], annotations[0]}));
    rejects(circuit);
    ++rejections;
    llvm::outs() << "PASS functional capacity profiles=" << cases
                 << " atomic rejections=" << rejections
                 << " selected Shuttle/Rocket=10/4 MegaBoom/Saturn=20/5; LBP "
                    "cap16 retained\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
