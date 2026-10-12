// See LICENSE for license details.
// Oracle: FPGATop.genHeader and Widget.genWidgetHeaders/genConstructor.
// Input: assembled Rocket F1Shim, retained annotations, allocated live banks,
// and an interface .const.h.
// Consumes no annotations; extends the sole .const.h OutputFile.
// Mutates only rawAnnotations, after all emitters succeed on a private clone.
// InstanceGraph/SSA checks belong to the individual native header emitters.
// Hardware, targets, clocks, channels and hierarchy analyses are preserved.
#include "goldengate/RocketDriverHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/LoadMemHeader.h"
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/UARTHeader.h"
#include "goldengate/TSIHeader.h"
#include "goldengate/BlockDevHeader.h"
#include "goldengate/TracerVHeader.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/FASEDHeader.h"
#include "goldengate/PrintBridgeHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareRocketDriverHeaders(CircuitOp circuit,
    std::string &error) {
  auto reject = [&](StringRef reason) { error = reason.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (circuit.getName() != "F1Shim" || !raw)
    return reject("Rocket driver headers require the assembled F1Shim and retained annotations");
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  // Retain the established production constructor order. Each emitter checks
  // the live allocated registry, rather than assuming an SFC absolute address.
  using Emitter = LogicalResult (*)(CircuitOp, std::string &);
  for (auto [name, emit] : {
      std::pair<StringRef, Emitter>{"SimulationMaster", prepareSimulationMasterHeader},
      {"ClockBridge", prepareClockBridgeHeader}, {"ResetPulse", prepareResetPulseHeader},
      {"LoadMem", prepareLoadMemHeader}, {"PeekPoke", preparePeekPokeHeader},
      {"UART", prepareUARTHeader}, {"TSI", prepareTSIHeader},
      {"BlockDev", prepareBlockDevHeader}, {"TracerV", prepareTracerVHeader}}) {
    if (failed(emit(*staged, error))) {
      error = name.str() + " driver header: " + error; return failure();
    }
  }
  FModuleOp printWrapper;
  for (auto m : staged->getOps<FModuleOp>()) if (m.getName() == "GGPrintBridgeHostWrapper") printWrapper = m;
  bool hasPrint = bool(printWrapper);
  for (auto row : raw) {
    auto d = dyn_cast<DictionaryAttr>(row);
    if (d && d.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO &&
        d.getAs<StringAttr>("widgetClass") == AnnotationClasses::PrintBridgeModule) hasPrint = true;
  }
  if (!hasPrint) {
    if (failed(prepareCPUManagedStreamHeader(*staged, error))) return failure();
  } else {
    auto bindings = printWrapper ? printWrapper->getAttrOfType<ArrayAttr>("goldengate.printHostBindings") : ArrayAttr();
    if (!bindings || bindings.empty()) return reject("Rocket Print driver needs the complete bound host catalog");
    SmallVector<FModuleOp> hosts;
    for (auto row : bindings) {
      auto d = dyn_cast<DictionaryAttr>(row); auto name = d ? d.getAs<StringAttr>("hostModule") : StringAttr();
      FModuleOp host;
      if (name) for (auto m : staged->getOps<FModuleOp>()) if (m.getName() == name) {
        if (host) return reject("Rocket Print driver has ambiguous host definitions"); host = m;
      }
      if (!host) return reject("Rocket Print driver host is missing from the live catalog");
      hosts.push_back(host);
    }
    std::string fragment, decoder;
    if (failed(preparePrintBridgeAllocatedHeader(*staged, hosts, fragment, error)) ||
        failed(preparePrintBridgeDecoderHeader(*staged, hosts, decoder, error))) return failure();
    // Embed the decoder under GET_INCLUDES so the ordinary OutputFile emitter
    // needs no special filename or driver build dependency. This header is
    // included again under each constructor phase, so omit its pragma once.
    StringRef include = "#include \"print-bridge-decoders.h\"";
    auto position = fragment.find(include.str());
    auto pragma = decoder.find("#pragma once\n");
    if (position == std::string::npos || pragma == std::string::npos)
      return reject("Rocket Print decoder/constructor header contract differs");
    decoder.erase(pragma, StringRef("#pragma once\n").size());
    fragment.replace(position, include.size(),
        "#ifndef GOLDENGATE_PRINT_BRIDGE_DECODERS\n#define GOLDENGATE_PRINT_BRIDGE_DECODERS\n" +
        decoder + "#endif // GOLDENGATE_PRINT_BRIDGE_DECODERS");
    OpBuilder b(circuit.getContext());
    auto annotations = staged->getOperation()->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> updated(annotations.begin(), annotations.end()); bool found = false;
    for (auto &row : updated) {
      auto d = dyn_cast<DictionaryAttr>(row); auto suffix = d ? d.getAs<StringAttr>("fileSuffix") : StringAttr();
      if (!d || d.getAs<StringAttr>("class") != AnnotationClasses::OutputFile || !suffix || suffix != ".const.h") continue;
      auto body = d.getAs<StringAttr>("body");
      if (found || !body || d.get("goldengate.cpuManagedStreamHeader") || d.get("goldengate.printBridgeHeader"))
        return reject("Rocket Print driver requires one unextended CPU/Print header");
      found = true; NamedAttrList output(d); output.set("body", b.getStringAttr(body.getValue().str() + fragment));
      output.set("goldengate.cpuManagedStreamHeader", b.getBoolAttr(true));
      output.set("goldengate.printBridgeHeader", b.getBoolAttr(true)); row = output.getDictionary(circuit.getContext());
    }
    if (!found) return reject("Rocket Print driver interface header is missing");
    staged->getOperation()->setAttr("rawAnnotations", b.getArrayAttr(updated));
  }
  // This final emitter can still reject a stale memory allocation. Even such
  // a late failure must leave the original interface header and archive intact.
  if (failed(prepareFASEDHeader(*staged, error))) return failure();
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  return success();
}
