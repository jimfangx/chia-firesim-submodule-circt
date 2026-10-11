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
  if (failed(prepareCPUManagedStreamHeader(*staged, error))) return failure();
  // This final emitter can still reject a stale memory allocation. Even such
  // a late failure must leave the original interface header and archive intact.
  if (failed(prepareFASEDHeader(*staged, error))) return failure();
  circuit->setAttr("rawAnnotations", staged->getOperation()->getAttr("rawAnnotations"));
  return success();
}
