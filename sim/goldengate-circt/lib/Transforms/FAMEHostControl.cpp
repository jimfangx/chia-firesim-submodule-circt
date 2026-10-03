// See LICENSE for license details.
#include "goldengate/FAMEHostControl.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct ParentInstance {
  InstanceOp instance;
  Value clock;
  Value reset;
};

bool isBit(Type type) {
  auto uint = dyn_cast<UIntType>(type);
  return uint && uint.getWidth() == 1;
}
} // namespace

LogicalResult goldengate::addFAMEHostControl(CircuitOp circuit,
                                              FModuleOp model,
                                              std::string &error) {
  if (!model || model->getParentOp() != circuit.getOperation()) {
    error = "FAME model is not in the supplied circuit";
    return failure();
  }
  std::set<std::string> names;
  for (auto port : model.getPorts())
    names.insert(port.getName().str());
  model.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"))
      names.insert(name.str());
  });
  for (auto name : {"hostClock", "hostReset", "targetCycleFinishing"})
    if (names.count(name)) {
      error = "FAME host-control name already exists: " + std::string(name);
      return failure();
    }

  SmallVector<ParentInstance> parents;
  bool badParent = false;
  circuit.walk([&](InstanceOp instance) {
    if (instance.getModuleName() != model.getName())
      return;
    auto parent = instance->getParentOfType<FModuleOp>();
    if (!parent) {
      badParent = true;
      return;
    }
    Value clock, reset;
    for (unsigned i = 0, n = parent.getPorts().size(); i < n; ++i) {
      if (parent.getPortDirection(i) != Direction::In)
        continue;
      if (parent.getPortName(i) == "hostClock")
        clock = parent.getBodyBlock()->getArgument(i);
      if (parent.getPortName(i) == "hostReset")
        reset = parent.getBodyBlock()->getArgument(i);
    }
    if (!clock || !reset || !isa<ClockType>(clock.getType()) ||
        !isBit(reset.getType())) {
      badParent = true;
      return;
    }
    parents.push_back({instance, clock, reset});
  });
  if (badParent) {
    error = "a FAME model instance has no host clock and reset inputs";
    return failure();
  }

  auto *context = model.getContext();
  SmallVector<std::pair<unsigned, PortInfo>> addedPorts;
  addedPorts.emplace_back(0, PortInfo(StringAttr::get(context, "hostClock"),
                                      ClockType::get(context), Direction::In));
  // Insertion indices refer to the original port list.  Both new ports go
  // before its first port, in this order.
  addedPorts.emplace_back(0, PortInfo(StringAttr::get(context, "hostReset"),
                                      UIntType::get(context, 1, false),
                                      Direction::In));
  model.insertPorts(addedPorts);
  OpBuilder declarations(&model.getBodyBlock()->front());
  declarations.create<WireOp>(model.getLoc(),
                              UIntType::get(context, 1, false),
                              "targetCycleFinishing");

  for (auto &parent : parents) {
    InstanceOp replacement = parent.instance.cloneAndInsertPorts(addedPorts);
    if (replacement.getPortNameStr(0) != "hostClock" ||
        replacement.getPortNameStr(1) != "hostReset" ||
        !isa<ClockType>(replacement.getResult(0).getType()) ||
        !isBit(replacement.getResult(1).getType())) {
      llvm::raw_string_ostream out(error);
      out << "cloned FAME model instance host ports: "
          << replacement.getPortNameStr(0) << " "
          << replacement.getResult(0).getType() << ", "
          << replacement.getPortNameStr(1) << " "
          << replacement.getResult(1).getType();
      return failure();
    }
    // Existing ports retain their names, types, and order after the new inputs.
    for (unsigned i = 0, n = parent.instance.getNumResults(); i < n; ++i) {
      if (parent.instance.getPortNameStr(i) !=
              replacement.getPortNameStr(i + 2) ||
          parent.instance.getResult(i).getType() !=
              replacement.getResult(i + 2).getType()) {
        error = "cloned FAME model instance changed an existing port";
        return failure();
      }
      parent.instance.getResult(i).replaceAllUsesWith(replacement.getResult(i + 2));
    }
    OpBuilder connections(replacement);
    connections.setInsertionPointAfter(replacement);
    connections.create<StrictConnectOp>(replacement.getLoc(),
                                        replacement.getResult(0), parent.clock);
    connections.create<StrictConnectOp>(replacement.getLoc(),
                                        replacement.getResult(1), parent.reset);
    parent.instance.erase();
  }
  return success();
}
