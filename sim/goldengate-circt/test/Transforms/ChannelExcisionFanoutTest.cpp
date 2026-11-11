// See LICENSE for license details.
#include "goldengate/ChannelExcision.h"
#include "goldengate/InferModelPorts.h"
#include "goldengate/HierarchyAnalysis.h"
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
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    auto root = parseSourceString<ModuleOp>(R"mlir(module {
      firrtl.circuit "Top" {
        firrtl.module @Top() {
          %out = firrtl.instance source @Source(out out: !firrtl.uint<1>)
          %a = firrtl.instance a @Sink(in in: !firrtl.uint<1>)
          %b = firrtl.instance b @Sink(in in: !firrtl.uint<1>)
          firrtl.strictconnect %a, %out : !firrtl.uint<1>
          firrtl.connect %b, %out : !firrtl.uint<1>, !firrtl.uint<1>
        }
        firrtl.module @Source(out %out: !firrtl.uint<1>) {
          %one = firrtl.constant 1 : !firrtl.uint<1>
          firrtl.strictconnect %out, %one : !firrtl.uint<1>
        }
        firrtl.module @Sink(in %in: !firrtl.uint<1>) {}
      }
    })mlir", &context);
    require(bool(root), "fanout fixture parse failed");
    auto circuit = *root->getOps<CircuitOp>().begin();Builder builder(&context);
    auto text = [&](StringRef s) {return builder.getStringAttr(s);};
    SmallVector<Attribute> annotations;
    for (StringRef module : {"Source", "Sink"})
      annotations.push_back(builder.getDictionaryAttr({
        builder.getNamedAttr("class", text(goldengate::AnnotationClasses::FAMETransform)),
        builder.getNamedAttr("target", text("~Top|" + module.str()))}));
    for (StringRef sink : {"a", "b"})
      annotations.push_back(builder.getDictionaryAttr({
        builder.getNamedAttr("class", text(goldengate::AnnotationClasses::ChannelConnection)),
        builder.getNamedAttr("globalName", text("to_" + sink.str())),
        builder.getNamedAttr("channelInfo", builder.getDictionaryAttr({
          builder.getNamedAttr("class", text(goldengate::AnnotationClasses::PipeChannel)),
          builder.getNamedAttr("latency", builder.getI32IntegerAttr(0))})),
        builder.getNamedAttr("sources", builder.getArrayAttr({text("~Top|Top>source.out")})),
        builder.getNamedAttr("sinks", builder.getArrayAttr({text("~Top|Top>" + sink.str() + ".in")}))}));
    circuit->setAttr("rawAnnotations", builder.getArrayAttr(annotations));
    std::string error;
    require(succeeded(verify(*root)), "fanout input verification failed");
    require(succeeded(goldengate::exciseChannels(circuit,error)),error);
    require(succeeded(verify(*root)), "fanout excision produced invalid IR");
    auto hierarchy = goldengate::analyzeTopHierarchy(circuit,error);
    require(bool(hierarchy), error);
    require(hierarchy->top.getNumPorts()==3 && hierarchy->connections.size()==3,
            "fanout must have one source and two independent sink connections");
    StringAttr sharedSource, firstSink;
    unsigned channels = 0;
    for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      Annotation annotation(attr);
      if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) continue;
      auto source = cast<StringAttr>(annotation.getMember<ArrayAttr>("sources")[0]);
      auto sink = cast<StringAttr>(annotation.getMember<ArrayAttr>("sinks")[0]);
      if (channels++)
        require(source == sharedSource && sink != firstSink,
                "fanout lost its shared source or independent sinks");
      else { sharedSource = source; firstSink = sink; }
    }
    require(channels == 2, "fanout channel annotations were dropped");
    require(succeeded(goldengate::inferModelPorts(circuit,error)),error);
    unsigned groups=0;
    for (auto a : circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
      if (Annotation(a).isClass(goldengate::AnnotationClasses::ChannelPorts)) ++groups;
    require(groups==2,"shared source/sink definitions did not deduplicate local channel groups");
    llvm::outs()<<"PASS shared model source fanout excision and model-port inference\n";
  } catch (const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
