// See LICENSE for license details.
#include "goldengate/FindDefaultClocks.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
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
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
void run(MLIRContext &context, unsigned scenario) {
  // Both satellite clocks reach distinct hub outputs. Only the alias case
  // connects those outputs to the same actual clock; names/frequencies do not
  // establish clock identity. Exercise aliases across a child module too.
  std::string body = scenario == 0 ? "firrtl.strictconnect %b, %alias : !firrtl.clock\n" :
                     scenario == 1 ? "firrtl.strictconnect %b, %c1 : !firrtl.clock\n" :
                     scenario == 2 ? "firrtl.strictconnect %b, %alias : !firrtl.clock\nfirrtl.strictconnect %b, %c1 : !firrtl.clock\n" :
                     "%asUInt = firrtl.asUInt %c0 : (!firrtl.clock) -> !firrtl.uint<1>\n%not = firrtl.not %asUInt : (!firrtl.uint<1>) -> !firrtl.uint<1>\n%inverse = firrtl.asClock %not : (!firrtl.uint<1>) -> !firrtl.clock\nfirrtl.strictconnect %b, %inverse : !firrtl.clock\n";
  if (scenario == 4 || scenario == 5) {
    body = "%gateIn, %gateOut = firrtl.instance gate @Opaque(in clock: !firrtl.clock, out out: !firrtl.clock)\n"
           "firrtl.strictconnect %gateIn, %c0 : !firrtl.clock\n"
           "firrtl.strictconnect %a, %gateOut : !firrtl.clock\n";
    if (scenario == 4)
      body += "firrtl.strictconnect %b, %gateOut : !firrtl.clock\n";
    else
      body += "%otherIn, %otherOut = firrtl.instance otherGate @Opaque(in clock: !firrtl.clock, out out: !firrtl.clock)\n"
              "firrtl.strictconnect %otherIn, %c0 : !firrtl.clock\n"
              "firrtl.strictconnect %b, %otherOut : !firrtl.clock\n";
  }
  if (scenario == 7)
    body = "firrtl.strictconnect %b, %alias : !firrtl.clock\n"
           "firrtl.connect %b, %alias : !firrtl.clock, !firrtl.clock\n";
  if (scenario == 6)
    body = "firrtl.strictconnect %b, %c1 : !firrtl.clock\n";
  auto source = std::string(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %clock: !firrtl.clock, in %other: !firrtl.clock) {
        %c0, %c1, %a, %b, %data = firrtl.instance hub @Hub(in c0: !firrtl.clock, in c1: !firrtl.clock, out a: !firrtl.clock, out b: !firrtl.clock, out data: !firrtl.uint<8>)
        %s0, %s1, %sink, %clockOut = firrtl.instance satellite @Satellite(in c0: !firrtl.clock, in c1: !firrtl.clock, in data: !firrtl.uint<8>, out clockOut: !firrtl.clock)
        firrtl.strictconnect %c0, %clock : !firrtl.clock
        firrtl.strictconnect %c1, %other : !firrtl.clock
        firrtl.strictconnect %s0, %a : !firrtl.clock
        firrtl.strictconnect %s1, %b : !firrtl.clock
        firrtl.strictconnect %sink, %data : !firrtl.uint<8>
      }
      firrtl.module @Hub(in %c0: !firrtl.clock, in %c1: !firrtl.clock, out %a: !firrtl.clock, out %b: !firrtl.clock, out %data: !firrtl.uint<8>) {
        %in, %out = firrtl.instance child @Alias(in clock: !firrtl.clock, out out: !firrtl.clock)
        firrtl.strictconnect %in, %c0 : !firrtl.clock
        %alias = firrtl.node %out : !firrtl.clock
        firrtl.strictconnect %a, %c0 : !firrtl.clock
        %zero = firrtl.constant 0 : !firrtl.uint<8>
        firrtl.strictconnect %data, %zero : !firrtl.uint<8>
  )mlir") + body + R"mlir(
      }
      firrtl.module @Alias(in %clock: !firrtl.clock, out %out: !firrtl.clock) {
        %wire = firrtl.wire : !firrtl.clock
        firrtl.strictconnect %wire, %clock : !firrtl.clock
        firrtl.strictconnect %out, %wire : !firrtl.clock
      }
      firrtl.extmodule @Opaque(in clock: !firrtl.clock, out out: !firrtl.clock)
      firrtl.module @Satellite(in %c0: !firrtl.clock, in %c1: !firrtl.clock, in %data: !firrtl.uint<8>, out %clockOut: !firrtl.clock) {
        firrtl.strictconnect %clockOut, %c0 : !firrtl.clock
      }
    }
  })mlir";
  if (scenario == 4 || scenario == 5) {
    auto driver = source.find("firrtl.strictconnect %a, %c0 : !firrtl.clock");
    source.erase(driver, std::string("firrtl.strictconnect %a, %c0 : !firrtl.clock").size());
  }
  if (scenario == 6) {
    auto driver = source.find("firrtl.strictconnect %c1, %other : !firrtl.clock");
    source.replace(driver, std::string("firrtl.strictconnect %c1, %other : !firrtl.clock").size(),
                   "firrtl.strictconnect %c1, %clockOut : !firrtl.clock");
  }
  auto root = parseSourceString<ModuleOp>(source, &context);
  require(bool(root) && succeeded(verify(*root)), "clock fixture invalid");
  auto circuit = *root->getOps<CircuitOp>().begin();
  Builder builder(&context);
  auto text = [&](StringRef value) { return builder.getStringAttr(value); };
  auto clocks = builder.getDictionaryAttr({builder.getNamedAttr("class", text(goldengate::AnnotationClasses::ChannelConnection)),
      builder.getNamedAttr("channelInfo", builder.getDictionaryAttr({builder.getNamedAttr("class", text(goldengate::AnnotationClasses::TargetClockChannel))})),
      builder.getNamedAttr("sinks", builder.getArrayAttr({text("~Top|Top>clock")}))});
  auto data = builder.getDictionaryAttr({builder.getNamedAttr("class", text(goldengate::AnnotationClasses::ChannelConnection)),
      builder.getNamedAttr("sources", builder.getArrayAttr({text("~Top|Top>hub.data")})),
      builder.getNamedAttr("sinks", builder.getArrayAttr({text("~Top|Top>satellite.data")}))});
  auto original = builder.getArrayAttr({clocks, data});
  circuit->setAttr("rawAnnotations", original);
  std::string error;
  auto result = goldengate::findDefaultClocks(circuit, error);
  if (scenario == 0 || scenario == 4 || scenario == 6 || scenario == 7) {
    require(succeeded(result), "proven clock aliases incorrectly rejected");
    auto updated = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    auto clock = Annotation(updated[1]).getMember<StringAttr>("clock");
    require(clock && clock.getValue() == "~Top|Top>hub.a", "wrong canonical hub clock");
    FModuleOp hub;
    for (auto model : circuit.getOps<FModuleOp>()) if (model.getName() == "Hub") hub = model;
    auto proofs = hub->getAttrOfType<ArrayAttr>("goldengate.fameClockAliases");
    if (scenario == 6) {
      require(proofs && proofs.size() == 1, "feedback input lost its pre-excision alias proof");
      auto proof = cast<DictionaryAttr>(proofs[0]);
      require(proof.getAs<StringAttr>("port").getValue() == "c1" &&
              proof.getAs<StringAttr>("clock").getValue() == "c0" &&
              proof.getAs<StringAttr>("instance").getValue() == "hub", "incorrect physical alias proof");
    } else require(!proofs, "independent input clock acquired an alias proof");
  } else {
    require(failed(result), "distinct, multiply driven or inverted clocks accepted");
    require(error.find("hub.a") != std::string::npos && error.find("hub.b") != std::string::npos,
            "clock ambiguity diagnostic omits candidate ports");
    require(circuit->getAttr("rawAnnotations") == original, "failed inference changed annotations");
  }
}
}
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    for (unsigned i = 0; i < 8; ++i) run(context, i);
    llvm::outs() << "PASS equivalent hierarchical clock aliases and distinct/ambiguous/inverted clock rejection\n";
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n'; return 1;
  }
}
