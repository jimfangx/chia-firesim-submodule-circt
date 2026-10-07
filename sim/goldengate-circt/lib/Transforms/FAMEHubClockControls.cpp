// See LICENSE for license details.
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/XDCEmission.h"
#include "mlir/IR/Builders.h"
#include <set>

using namespace mlir;
using namespace circt::firrtl;

std::optional<llvm::SmallVector<goldengate::FAMEHubClockControl>>
goldengate::constructFAMEHubClockControls(
    CircuitOp circuit, FModuleOp model, StringRef clockChannelName,
    ArrayRef<FAMEHubClockDomain> domains, std::string &error) {
  auto reject = [&](StringRef reason)
      -> std::optional<SmallVector<FAMEHubClockControl>> {
    error = "FAME hub clock controls: " + reason.str();
    return std::nullopt;
  };
  if (!model || model->getParentOp() != circuit.getOperation() ||
      clockChannelName.empty() || domains.empty())
    return reject("missing circuit, model, clock channel or domain metadata");
  std::optional<unsigned> port;
  for (unsigned i = 0; i < model.getNumPorts(); ++i)
    if (model.getPortName(i) == (clockChannelName + "_sink").str()) port = i;
  if (!port || model.getPortDirection(*port) != Direction::In)
    return reject("missing model clock sink");
  auto channel = dyn_cast<BundleType>(model.getPortType(*port));
  auto bit = UIntType::get(model.getContext(), 1);
  if (!channel || channel.getElements().size() != 3 ||
      !channel.getElement("bits") || channel.getElement("bits")->isFlip ||
      !channel.getElement("ready") || !channel.getElement("ready")->isFlip ||
      channel.getElement("ready")->type != bit ||
      !channel.getElement("valid") || channel.getElement("valid")->isFlip ||
      channel.getElement("valid")->type != bit)
    return reject("incompatible decoupled clock sink");
  auto payload = channel.getElement("bits")->type;
  auto record = dyn_cast<BundleType>(payload);
  std::set<std::string> identities;
  for (auto [i, domain] : llvm::enumerate(domains)) {
    if (domain.modelClockName.empty() ||
        !identities.insert(domain.modelClockName).second)
      return reject("empty or duplicate model clock identity");
    if (domains.size() == 1) {
      if (!domain.payloadField.empty() || !isa<ClockType>(payload))
        return reject("single clock requires scalar bits");
    } else if (!record || record.getElements().size() != domains.size() ||
               record.getElements()[i].name.getValue() != domain.payloadField ||
               record.getElements()[i].isFlip ||
               !isa<ClockType>(record.getElements()[i].type)) {
      return reject("clock record fields disagree with ordered domain metadata");
    }
  }

  // Keep each raw flag before replacing Clock reads with the gate output.
  // The returned flag remains the next token, while outputEnable denotes the
  // current simulated cycle, matching TargetClockMetadata in FAMETransform.
  OpBuilder b(&model.getBodyBlock()->front());
  auto loc = model.getLoc();
  Value bits = b.create<SubfieldOp>(loc, model.getArgument(*port), "bits");
  SmallVector<Value> rawClocks;
  SmallVector<FAMEHubClockControl> controls;
  for (const auto &domain : domains) {
    Value raw = bits;
    if (!domain.payloadField.empty())
      raw = b.create<SubfieldOp>(loc, bits, domain.payloadField);
    Value flag = b.create<AsUIntPrimOp>(loc, raw);
    rawClocks.push_back(raw);
    if (failed(addFAMEClockEnable(model, domain.modelClockName, flag, error)))
      return std::nullopt;
    Value enabled = lookupFAMEClockEnable(model, domain.modelClockName, error);
    if (!enabled) return std::nullopt;
    controls.push_back({domain.modelClockName, flag, enabled});
  }
  for (auto [i, domain] : llvm::enumerate(domains))
    if (failed(addFAMEClockGate(circuit, model, domain.modelClockName, error,
                                rawClocks[i])) ||
        failed(addFAMEClockConstraint(circuit, model, domain.modelClockName,
                                      domain.clockInfo, error)))
      return std::nullopt;
  return controls;
}
