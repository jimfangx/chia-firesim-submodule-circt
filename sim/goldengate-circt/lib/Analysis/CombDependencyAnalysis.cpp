// See LICENSE for license details.
#include "goldengate/CombDependencyAnalysis.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>

using namespace circt::firrtl;
using namespace mlir;

namespace {
using PortField = std::pair<unsigned, unsigned>;
using ModulePortField = std::tuple<Operation *, unsigned, unsigned>;

struct Trace {
  // Keep the FIRRTL operand traversal order for Scala's ordered ccDeps.
  std::vector<PortField> inputPorts;
  std::set<std::string> blockers;
};

class LocalPortTracer {
public:
  explicit LocalPortTracer(FModuleOp module) : module(module) {
    auto circuit = module->getParentOfType<CircuitOp>();
    for (auto &op : circuit.getBodyBlock()->getOperations())
      if (auto child = dyn_cast<FModuleLike>(&op))
        modules[child.getModuleName().str()] = child;
  }

  Trace trace(unsigned port) {
    return tracePort(module, port, 0);
  }

private:
  Trace tracePort(FModuleOp current, unsigned port, unsigned fieldID) {
    ModulePortField key{current.getOperation(), port, fieldID};
    if (auto cached = portCache.find(key); cached != portCache.end())
      return cached->second;
    if (!activePorts.insert(key).second)
      return {{}, {current.getName().str() + ":recursive module port"}};
    DenseSet<circt::FieldRef> active;
    Trace result = traceValue(current, current.getBodyBlock()->getArgument(port),
                              active, fieldID);
    activePorts.erase(key);
    portCache.emplace(key, result);
    return result;
  }

  void indexDrivers(FModuleOp current) {
    if (!indexed.insert(current.getOperation()).second)
      return;
    current.walk([&](StrictConnectOp op) {
      drivers[current.getOperation()][getFieldRefFromValue(op.getDest())]
          .push_back(op.getSrc());
    });
    current.walk([&](ConnectOp op) {
      drivers[current.getOperation()][getFieldRefFromValue(op.getDest())]
          .push_back(op.getSrc());
    });
  }

  Trace traceValue(FModuleOp current, Value value,
                   DenseSet<circt::FieldRef> &active,
                   unsigned subFieldID = 0) {
    // FIRRTL subfield/subindex operations may be recreated at each use.
    // Their SSA results differ, but the root value and field ID identify the
    // same electrical field. Keep drivers, memoization, and cycle detection
    // on that stable FIRRTL field identity.
    auto fieldRef = getFieldRefFromValue(value).getSubField(subFieldID);
    auto &cachedValues = valueCache[current.getOperation()];
    if (auto cached = cachedValues.find(fieldRef);
        cached != cachedValues.end())
      return cached->second;
    Trace result;
    if (!active.insert(fieldRef).second) {
      result.blockers.insert(current.getName().str() + ":combinational cycle");
      return result;
    }
    // FIRRTL registers have a connect on the D side. Their result is the Q
    // side, so check this before looking up connects or a synchronous path
    // will be incorrectly counted as combinational.
    if (auto *def = fieldRef.getValue().getDefiningOp();
        def && isa<RegOp, RegResetOp>(def)) {
      active.erase(fieldRef);
      cachedValues[fieldRef] = result;
      return result;
    }
    auto merge = [&](Trace other) {
      for (PortField input : other.inputPorts)
        if (std::find(result.inputPorts.begin(), result.inputPorts.end(), input) ==
            result.inputPorts.end())
          result.inputPorts.push_back(input);
      result.blockers.insert(other.blockers.begin(), other.blockers.end());
    };
    indexDrivers(current);
    auto &moduleDrivers = drivers[current.getOperation()];
    auto driver = moduleDrivers.find(fieldRef);
    if (driver != moduleDrivers.end()) {
      for (Value source : driver->second)
        merge(traceValue(current, source, active));
    } else if (auto arg = dyn_cast<BlockArgument>(fieldRef.getValue())) {
      unsigned port = arg.getArgNumber();
      if (arg.getOwner() == current.getBodyBlock() &&
          current.getPortDirection(port) == Direction::In)
        result.inputPorts.push_back({port, fieldRef.getFieldID()});
      else
        result.blockers.insert(current.getName().str() + ":undriven output");
    } else if (auto *op = fieldRef.getValue().getDefiningOp()) {
      // Map internal instance outputs through their child's input ports,
      // then follow the corresponding driven instance inputs. Preserve the
      // selected aggregate field across both sides of the instance boundary.
      if (auto instance = dyn_cast<InstanceOp>(op)) {
        unsigned port = cast<OpResult>(fieldRef.getValue()).getResultNumber();
        auto child = modules.find(instance.getModuleName().str());
        if (child == modules.end() ||
            child->second.getPortDirection(port) != Direction::Out) {
          result.blockers.insert(current.getName().str() + ":unresolved instance " +
                                 instance.getModuleName().str());
        } else if (auto body = dyn_cast<FModuleOp>(child->second.getOperation())) {
          auto childTrace = tracePort(body, port, fieldRef.getFieldID());
          result.blockers.insert(childTrace.blockers.begin(),
                                 childTrace.blockers.end());
          for (auto [inputPort, inputFieldID] : childTrace.inputPorts)
            merge(traceValue(current, instance.getResult(inputPort), active,
                             inputFieldID));
        } else if (auto external = dyn_cast<FExtModuleOp>(child->second.getOperation());
                   external && external.getDefname() == "plusarg_reader" &&
                   external.getNumPorts() == 1 &&
                   external.getPortName(0) == "out" &&
                   external.getPortDirection(0) == Direction::Out &&
                   isa<UIntType>(external.getPortType(0)) &&
                   cast<UIntType>(external.getPortType(0)).getWidth() > 0) {
          // Rocket's plusarg_reader has no target inputs: its output is a
          // configuration value initialized by $value$plusargs (DEFAULT in
          // synthesis). The immutable SFC RTL confirms that contract. Scala
          // CheckCombLoops therefore finds no input paths through this source.
          // Keep other blackboxes unresolved, including readers with inputs.
        } else {
          result.blockers.insert(current.getName().str() + ":external " +
                                 instance.getModuleName().str());
        }
      } else if (auto field = value.getDefiningOp<SubfieldOp>()) {
        if (auto memory = field.getInput().getDefiningOp<MemOp>();
            memory && field.isFieldFlipped() &&
            (field.getFieldName() == "data" ||
             field.getFieldName() == "rdata")) {
          // Stored bits are state. A zero-latency read can still depend on
          // this cycle's read address and enable. New read-under-write also
          // needs write-port dependencies, which remain explicitly unresolved.
          if (memory.getReadLatency() == 0) {
            if (memory.getRuw() == RUWAttr::New)
              result.blockers.insert(current.getName().str() +
                                     ":new read-under-write memory");
            bool hasAddr = false, hasEnable = false;
            for (Operation *user : field.getInput().getUsers()) {
              auto portField = dyn_cast<SubfieldOp>(user);
              if (!portField)
                continue;
              if (portField.getFieldName() == "addr") {
                hasAddr = true;
                merge(traceValue(current, portField.getResult(), active));
              } else if (portField.getFieldName() == "en") {
                hasEnable = true;
                merge(traceValue(current, portField.getResult(), active));
              }
            }
            if (!hasAddr || !hasEnable)
              result.blockers.insert(current.getName().str() +
                                     ":incomplete memory read port");
          }
        } else {
          // Other subfields retain their ordinary FIRRTL SSA dependency.
          merge(traceValue(current, field.getInput(), active));
        }
      } else if (auto port = dyn_cast<circt::chirrtl::MemoryPortOp>(op)) {
        // A CHIRRTL memory port's data is stored state, while an asynchronous
        // read still follows its access index. Infer ports are read-only when
        // no connect drives any field of the data result. Keep write and
        // mixed-direction ports unresolved instead of treating their writes
        // as combinational reads.
        bool written = false;
        for (const auto &entry : moduleDrivers)
          if (entry.first.getValue() == port.getData()) {
            written = true;
            break;
          }
        auto direction = port.getDirection();
        if (written || direction == MemDirAttr::Write ||
            direction == MemDirAttr::ReadWrite) {
          result.blockers.insert(current.getName().str() +
                                 ":written memory port");
        } else if (auto access = port.getAccess()) {
          if (isa<circt::chirrtl::CombMemOp>(port.getMemory().getDefiningOp()))
            merge(traceValue(current, access.getIndex(), active));
          else if (!isa<circt::chirrtl::SeqMemOp>(
                       port.getMemory().getDefiningOp()))
            result.blockers.insert(current.getName().str() +
                                   ":unrecognized memory port source");
        } else {
          result.blockers.insert(current.getName().str() +
                                 ":memory port without access");
        }
      } else if (isa<MemOp, circt::chirrtl::CombMemOp,
                     circt::chirrtl::SeqMemOp>(op)) {
        result.blockers.insert(current.getName().str() + ":memory result");
      } else if (isa<WireOp>(op)) {
        result.blockers.insert(current.getName().str() + ":undriven wire");
      } else {
        // Nodes, muxes, casts, and primitive expressions depend on every
        // operand, including a mux condition. An aggregate expression without
        // an explicit field connect remains conservative here.
        for (Value operand : op->getOperands())
          merge(traceValue(current, operand, active));
      }
    }
    active.erase(fieldRef);
    cachedValues[fieldRef] = result;
    return result;
  }

  FModuleOp module;
  std::map<std::string, FModuleLike> modules;
  DenseSet<Operation *> indexed;
  // Module maps must retain stable references as tracing enters a child.
  std::map<Operation *, DenseMap<circt::FieldRef, SmallVector<Value>>> drivers;
  std::map<Operation *, DenseMap<circt::FieldRef, Trace>> valueCache;
  std::map<ModulePortField, Trace> portCache;
  std::set<ModulePortField> activePorts;
};
} // namespace

std::vector<goldengate::LocalChannelDependency>
goldengate::analyzeLocalChannelDependencies(
    FModuleOp module, llvm::ArrayRef<ModelChannelBinding> bindings) {
  LocalPortTracer tracer(module);
  std::vector<LocalChannelDependency> result;
  for (const auto &output : bindings) {
    if (output.portGroup->module != module ||
        output.portGroup->direction != Direction::Out)
      continue;
    LocalChannelDependency dependency;
    dependency.outputChannel = output.portGroup->name;
    std::set<std::string> seenInputs;
    std::set<std::string> blockers;
    for (unsigned port : output.instancePorts) {
      auto traced = tracer.trace(port);
      if (!traced.blockers.empty())
        dependency.unresolvedPorts.push_back(module.getPortName(port).str());
      blockers.insert(traced.blockers.begin(), traced.blockers.end());
      for (PortField source : traced.inputPorts)
        for (const auto &input : bindings) {
          if (input.portGroup->module != module ||
              input.portGroup->direction != Direction::In ||
              !llvm::is_contained(input.instancePorts, source.first))
            continue;
          if (seenInputs.insert(input.portGroup->name).second)
            dependency.inputChannels.push_back(input.portGroup->name);
        }
    }
    dependency.unresolvedCauses.assign(blockers.begin(), blockers.end());
    result.push_back(std::move(dependency));
  }
  return result;
}
