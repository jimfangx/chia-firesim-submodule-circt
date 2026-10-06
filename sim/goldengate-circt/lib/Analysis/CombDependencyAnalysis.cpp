// See LICENSE for license details.
#include "goldengate/CombDependencyAnalysis.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "llvm/ADT/APSInt.h"
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
    indexDrivers(current);
    if (conditionalModules.contains(current.getOperation()))
      return {{}, {current.getName().str() +
                   ":unresolved when semantics; run FIRRTL ExpandWhens"}};
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
    current.walk([&](WhenOp) {
      conditionalModules.insert(current.getOperation());
    });
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
      if (driver->second.size() != 1)
        result.blockers.insert(current.getName().str() +
            ":unresolved last-connect semantics; run FIRRTL ExpandWhens");
      else
        merge(traceValue(current, driver->second.front(), active));
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
      } else if (auto memory = dyn_cast<MemOp>(op)) {
        // Recognize a read by its canonical memory port and field identity,
        // including nested bundle/vector selections. The original SSA value
        // may be a SubindexOp rather than the port's data SubfieldOp.
        auto port = fieldRef.getValue();
        auto type = cast<BundleType>(port.getType());
        if (!fieldRef.getFieldID()) {
          result.blockers.insert(current.getName().str() +
                                 ":unselected memory port");
        } else if (auto element = type.getElement(
                       type.getIndexForFieldID(fieldRef.getFieldID()));
                   element.isFlip && (element.name.getValue() == "data" ||
                                      element.name.getValue() == "rdata")) {
          // Stored bits are state. A zero-latency read can still depend on
          // this cycle's read address and enable. New read-under-write also
          // needs write-port dependencies, which remain explicitly unresolved.
          if (memory.getReadLatency() == 0) {
            // Scala's reader-port connectivity does not establish a contract
            // for combinational readwrite ports. Their write mode must not be
            // silently omitted from an apparently complete dependency set.
            if (element.name.getValue() == "rdata")
              result.blockers.insert(current.getName().str() +
                                     ":asynchronous readwrite memory");
            if (memory.getRuw() == RUWAttr::New)
              result.blockers.insert(current.getName().str() +
                                     ":new read-under-write memory");
            // CheckCombLoops adds address and enable edges to an async read.
            // Resolve them directly from field IDs, without depending on the
            // presence or use-list order of SSA subfield operations.
            for (StringRef control : {"addr", "en"}) {
              if (auto index = type.getElementIndex(control))
                merge(traceValue(current, port, active, type.getFieldID(*index)));
              else
                result.blockers.insert(current.getName().str() +
                                       ":incomplete memory read port");
            }
          }
        } else {
          // A memory input field must have a resolved driver, found above.
          result.blockers.insert(current.getName().str() +
                                 ":undriven memory field");
        }
      } else if (auto node = dyn_cast<NodeOp>(op)) {
        // Nodes preserve their input's aggregate layout. Forward the selected
        // field, rather than tracing the entire aggregate and losing the
        // individual field drivers (or adding unrelated input channels).
        merge(traceValue(current, node.getInput(), active,
                         fieldRef.getFieldID()));
      } else if (auto mux = dyn_cast<MuxPrimOp>(op)) {
        // A selected mux result depends on its condition and the matching
        // field of each arm. Static selections have already been folded into
        // fieldRef, so traversing the immediate SubfieldOp would discard them.
        merge(traceValue(current, mux.getSel(), active));
        merge(traceValue(current, mux.getHigh(), active,
                         fieldRef.getFieldID()));
        merge(traceValue(current, mux.getLow(), active,
                         fieldRef.getFieldID()));
      } else if (auto access = dyn_cast<SubaccessOp>(op)) {
        // Dynamic reads select the same relative field from every reachable
        // vector element. Tracing the whole vector loses its leaf drivers and
        // can introduce unrelated bundle fields. Static selections following
        // this access are already represented by fieldRef's relative field ID.
        auto vector = access.getInput().getType().base();
        auto count = vector.getNumElements();
        auto traceElement = [&](unsigned index) {
          merge(traceValue(current, access.getInput(), active,
                           vector.getFieldID(index) + fieldRef.getFieldID()));
        };
        if (auto constant = access.getIndex().getDefiningOp<ConstantOp>()) {
          auto index = constant.getValue().getLimitedValue(count);
          if (index < count)
            traceElement(index);
          else
            result.blockers.insert(current.getName().str() +
                                   ":out-of-range vector selection");
        } else if (!count) {
          result.blockers.insert(current.getName().str() +
                                 ":zero-length vector selection");
        } else {
          merge(traceValue(current, access.getIndex(), active));
          // A narrow index cannot select higher elements. Avoid dependencies
          // on unreachable fields even if they have no driver.
          auto width = access.getIndex().getType().base().getWidthOrSentinel();
          if (width >= 0 && width < 64)
            count = std::min<uint64_t>(count, uint64_t{1} << width);
          // SFC RemoveAccesses emits guarded connects in ascending index
          // order. ExpandWhens gives the last connect priority, hence the
          // resulting mux expression visits higher elements first.
          for (auto index = count; index > 0; --index)
            traceElement(index - 1);
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
      } else if (isa<circt::chirrtl::CombMemOp,
                     circt::chirrtl::SeqMemOp>(op)) {
        result.blockers.insert(current.getName().str() + ":memory result");
      } else if (isa<WireOp>(op)) {
        result.blockers.insert(current.getName().str() + ":undriven wire");
      } else {
        // Casts and primitive expressions depend on every operand. Aggregate
        // expressions without an explicit field connect remain conservative.
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
  DenseSet<Operation *> conditionalModules;
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
