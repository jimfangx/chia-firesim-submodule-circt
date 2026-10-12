// See LICENSE for license details.
// Bind existing post-FAME tokens; do not import a second target hierarchy.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/BridgeClockInfo.h"
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/AnnotationClasses.h"
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::bindPrintBridgeTracerV(CircuitOp circuit,
    std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  FModuleOp top;
  for (auto m : circuit.getOps<FModuleOp>())
    if (m.getName() == circuit.getName()) top = m;
  auto registry = top ? top->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr();
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (circuit.getName() != "GGPrintBridgeHostWrapper" || !registry || registry.empty() || !raw)
    return reject("Print TracerV binding requires the active bound Print hosts");
  DictionaryAttr bridge;
  for (auto attr : raw) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    if (!row) return reject("malformed Print TracerV annotation");
    auto widget = row.getAs<StringAttr>("widgetClass");
    if (row.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO && widget &&
        widget == "firechip.goldengateimplementations.TracerVBridgeModule") {
      if (bridge) return reject("ambiguous Print TracerV constructor");
      bridge = row;
    }
  }
  // BridgeIO.target still names the extracted target-side port. Validate the
  // rational channel domain first; the token mapper publishes the live hPort.
  if (failed(validateBridgeChannelClocks(circuit, bridge, error))) return failure();
  return attachTracerVTokenEngine(circuit, "GGPrintBridgeHostWrapper", error);
}
