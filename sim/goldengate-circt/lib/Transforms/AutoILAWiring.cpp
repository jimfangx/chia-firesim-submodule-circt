// See LICENSE for license details.
#include "goldengate/AutoILAWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/HostClockWiring.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>

using namespace mlir;
using namespace circt::firrtl;

namespace {
struct Route {
  goldengate::AutoILAProbe source;
  SmallVector<Operation *> path;
  unsigned port = 0, childPort = 0;
  StringAttr name;
};
struct ModuleRoutes {
  FModuleOp module;
  unsigned oldPorts;
  SmallVector<Route, 0> routes;
  SmallVector<std::pair<unsigned, PortInfo>> added;
};
bool isTemporaryILAAnnotation(Attribute attr) {
  auto dict = dyn_cast<DictionaryAttr>(attr);
  auto cls = dict ? dict.getAs<StringAttr>("class") : StringAttr();
  return cls && (cls.getValue() == goldengate::AnnotationClasses::InternalFpgaDebug ||
      cls.getValue() == "firrtl.transforms.TopWiring.TopWiringAnnotation" ||
      cls.getValue() == "firrtl.transforms.TopWiring.TopWiringOutputFilesAnnotation");
}
} // namespace

LogicalResult goldengate::prepareAutoILAAnnotations(
    CircuitOp circuit, bool internalizePublic, StringRef originalCircuit,
    std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoILA preparation requires retained annotations";
    return failure();
  }
  OpBuilder b(circuit.getContext()); SmallVector<Attribute> updated;
  for (auto attr : raw) {
    auto dict = dyn_cast<DictionaryAttr>(attr);
    auto cls = dict ? dict.getAs<StringAttr>("class") : StringAttr();
    bool convert = cls && internalizePublic && cls.getValue() == AnnotationClasses::FpgaDebug;
    bool carry = cls && !originalCircuit.empty() && cls.getValue() == AnnotationClasses::InternalFpgaDebug;
    if (!convert && !carry) { updated.push_back(attr); continue; }
    NamedAttrList fields(dict);
    if (convert) fields.set("class", b.getStringAttr(AnnotationClasses::InternalFpgaDebug));
    if (carry) {
      auto target = dict.getAs<StringAttr>("target");
      if (!target) { error = "AutoILA debug annotation lacks a string target"; return failure(); }
      StringRef spelling = target.getValue();
      bool modern = spelling.starts_with("~");
      auto split = modern ? spelling.drop_front().split('|') : spelling.split('.');
      if ((split.first != originalCircuit && split.first != circuit.getName()) || split.second.empty()) {
        error = "AutoILA debug target has an unexpected circuit identity";
        return failure();
      }
      fields.set("target", b.getStringAttr((modern ? "~" : "") + circuit.getName().str() +
          (modern ? "|" : ".") + split.second.str()));
    }
    updated.push_back(fields.getDictionary(circuit.getContext()));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(updated));
  return success();
}

LogicalResult goldengate::wireAutoILAProbesToTop(
    CircuitOp circuit, SmallVectorImpl<WiredILAProbe> &outputs,
    std::string &error) {
  SmallVector<AutoILAProbe> probes;
  if (failed(analyzeAutoILAProbes(circuit, probes, error))) return failure();
  if (probes.empty()) return success();
  llvm::DenseMap<Value, AutoILAProbe> selected;
  for (auto probe : probes) {
    probe.path.clear();
    selected.try_emplace(probe.value, std::move(probe));
  }
  std::map<Operation *, ModuleRoutes> modules;
  SmallVector<Operation *> order;
  SmallVector<std::pair<InstanceOp, Operation *>> uses;
  FModuleOp top;
  {
    // All graph queries precede port insertion and instance replacement.
    circt::igraph::InstanceGraph graph(circuit);
    top = cast<FModuleOp>(graph.lookup(
        StringAttr::get(circuit.getContext(), circuit.getName()))->getModule());
    llvm::DenseSet<Operation *> active;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
      if (!module) return success();
      auto *key = module.getOperation();
      if (modules.count(key)) return success();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in AutoILA top wiring";
        return failure();
      }
      ModuleRoutes info{module, unsigned(module.getNumPorts()), {}, {}};
      auto addLocal = [&](Value value) {
        auto found = selected.find(value);
        if (found != selected.end()) info.routes.push_back({found->second, {}});
      };
      for (auto &op : module.getBodyBlock()->getOperations())
        if (op.getNumResults() == 1) addLocal(op.getResult(0));
      for (auto value : module.getBodyBlock()->getArguments()) addLocal(value);
      for (auto *record : *node) {
        if (failed(plan(record->getTarget()))) return failure();
        auto child = modules.find(record->getTarget()->getModule().getOperation());
        if (child == modules.end() || child->second.routes.empty()) continue;
        auto instance = record->getInstance<InstanceOp>();
        if (!instance || instance->getBlock() != module.getBodyBlock()) {
          error = "AutoILA top wiring requires module-scope FIRRTL instances";
          return failure();
        }
        for (auto route : child->second.routes) {
          route.childPort = route.port;
          route.path.insert(route.path.begin(), instance.getOperation());
          info.routes.push_back(std::move(route));
        }
      }
      circt::Namespace names;
      for (auto name : module.getPortNamesAttr())
        names.newName(cast<StringAttr>(name).getValue());
      module.walk([&](Operation *op) {
        if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
      });
      for (auto [index, route] : llvm::enumerate(info.routes)) {
        std::string suggested = "ila_";
        for (auto *instance : route.path)
          suggested += cast<InstanceOp>(instance).getName().str() + "_";
        suggested += route.source.leafName;
        route.port = info.oldPorts + index;
        route.name = StringAttr::get(circuit.getContext(), names.newName(suggested));
        info.added.push_back({info.oldPorts,
            PortInfo(route.name, route.source.value.getType(), Direction::Out)});
      }
      active.erase(key);
      modules.emplace(key, std::move(info));
      order.push_back(key);
      return success();
    };
    // Changing a shared signature must also update uses in unused parents.
    for (auto module : circuit.getOps<FModuleOp>())
      if (failed(plan(graph.lookup(module)))) return failure();
    for (auto *key : order) {
      auto &info = modules.at(key);
      if (info.routes.empty()) continue;
      for (auto *record : graph.lookup(info.module)->uses()) {
        auto instance = record->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        if (!instance || !parent || instance.getNumResults() != info.oldPorts ||
            instance->getBlock() != parent.getBodyBlock()) {
          error = "AutoILA source module has an incompatible instance use";
          return failure();
        }
        uses.push_back({instance, key});
      }
    }
  }
  for (auto *key : order) {
    auto &info = modules.at(key);
    if (!info.added.empty()) info.module.insertPorts(info.added);
  }
  llvm::DenseMap<Operation *, InstanceOp> replacements;
  for (auto [instance, child] : uses) {
    auto &info = modules.at(child);
    auto replacement = instance.cloneAndInsertPorts(info.added);
    for (auto attr : instance->getAttrs())
      if (!replacement->hasAttr(attr.getName()))
        replacement->setAttr(attr.getName(), attr.getValue());
    for (unsigned i = 0; i < info.oldPorts; ++i)
      instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
    replacements[instance.getOperation()] = replacement;
    instance.erase();
  }
  OpBuilder b(circuit.getContext());
  for (auto *key : order) {
    auto &info = modules.at(key);
    b.setInsertionPointToEnd(info.module.getBodyBlock());
    for (auto &route : info.routes) {
      Value driver = route.source.value;
      if (!route.path.empty())
        driver = replacements.lookup(route.path.front()).getResult(route.childPort);
      b.create<StrictConnectOp>(info.module.getLoc(),
          info.module.getBodyBlock()->getArgument(route.port), driver);
    }
  }
  SmallVector<WiredILAProbe> result;
  for (auto [index, route] : llvm::enumerate(modules.at(top.getOperation()).routes)) {
    auto probe = route.source;
    probe.index = index;
    probe.suggestedName = "ila_";
    for (auto *old : route.path) {
      auto instance = replacements.lookup(old);
      probe.path.push_back(instance);
      probe.suggestedName += instance.getName().str() + "_";
    }
    probe.suggestedName += probe.leafName;
    result.push_back({std::move(probe), top.getBodyBlock()->getArgument(route.port),
        "~" + circuit.getName().str() + "|" + top.getName().str() + ">" + route.name.getValue().str()});
  }
  outputs.append(result.begin(), result.end());
  return success();
}

LogicalResult goldengate::attachAutoILAWrapper(
    CircuitOp circuit, ArrayRef<WiredILAProbe> routes,
    const ILAWrapperOptions &options, InstanceOp &wrapper, std::string &error) {
  if (routes.empty()) return success();
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  FModuleOp top;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == circuit.getName()) top = module;
  if (!top || !raw || routes.size() > top.getNumPorts() ||
      options.outputBaseFilename.empty() || !options.dataDepth || !options.probeTriggers) {
    error = "AutoILA wrapper requires top, annotations, output filename and positive IP parameters";
    return failure();
  }
  unsigned oldPorts = top.getNumPorts() - routes.size();
  SmallVector<PortInfo> ports;
  circt::Namespace portNames, moduleNames, topNames;
  for (auto [i, route] : llvm::enumerate(routes)) {
    unsigned port = oldPorts + i;
    if (route.topPort != top.getBodyBlock()->getArgument(port) ||
        top.getPortDirection(port) != Direction::Out || route.source.index != i ||
        route.topPort.getType() != route.source.value.getType() ||
        !route.source.width || route.topTarget != "~" + circuit.getName().str() +
            "|" + top.getName().str() + ">" + top.getPortName(port).str()) {
      error = "AutoILA wrapper routes must cover the complete appended top port suffix";
      return failure();
    }
    // Actual allocated names, not flattened suggestions, also name collateral.
    // Distinct hierarchy paths can have the same flattened spelling.
    portNames.newName(top.getPortName(port));
    ports.emplace_back(top.getPortNameAttr(port), route.topPort.getType(), Direction::In);
  }
  for (auto &op : *circuit.getBodyBlock())
    if (auto name = op.getAttrOfType<StringAttr>("sym_name")) moduleNames.newName(name.getValue());
  for (auto name : top.getPortNamesAttr()) topNames.newName(cast<StringAttr>(name).getValue());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) topNames.newName(name.getValue());
  });
  auto ipName = moduleNames.newName("ila_firesim");
  auto moduleName = moduleNames.newName("ila_wrapper");
  auto instanceName = topNames.newName("ila_wrapper_inst");
  auto clockName = portNames.newName("clock");
  OpBuilder b(circuit.getContext());
  ports.insert(ports.begin(), PortInfo(b.getStringAttr(clockName), ClockType::get(circuit.getContext()), Direction::In));
  std::string tcl, verilog;
  llvm::raw_string_ostream t(tcl), v(verilog);
  t << "create_ip -name ila \\\n  -vendor xilinx.com \\\n  -library ip \\\n  -version  6.2 \\\n  -module_name " << ipName << "\nset_property -dict [list \\\n  ";
  for (auto [i, route] : llvm::enumerate(routes)) {
    if (i) t << " \\\n  ";
    t << "CONFIG.C_PROBE" << i << "_WIDTH {" << route.source.width
      << "}  CONFIG.C_PROBE" << i << "_MU_CNT {" << options.probeTriggers << "}";
  }
  t << " \\\n  CONFIG.C_NUM_OF_PROBES {" << routes.size()
    << "} \\\n  CONFIG.C_DATA_DEPTH {" << options.dataDepth
    << "} \\\n  CONFIG.C_TRIGOUT_EN {false} \\\n  CONFIG.C_EN_STRG_QUAL {1} \\\n  CONFIG.C_ADV_TRIGGER {true} \\\n  CONFIG.C_TRIGIN_EN {false} \\\n  CONFIG.ALL_PROBE_SAME_MU_CNT {" << options.probeTriggers << "}] [get_ips " << ipName << "]\n";
  v << "// A wrapper module around the ILA IP instance. This serves two purposes:\n"
       "// 1. It gives the probes reasonable names in the GUI\n"
       "// 2. Verilog ifdefs the remove the ILA instantiation in metasimulation.\nmodule "
    << moduleName << " (\n    input " << clockName;
  for (auto [i, route] : llvm::enumerate(routes))
    v << ",\ninput [" << route.source.width - 1 << ":0] " << ports[i + 1].name.getValue();
  v << "\n);\n// Don't instantiate the ILA when running under metasimulation\n`ifdef SYNTHESIS\n  "
    << ipName << " CL_FIRESIM_DEBUG_WIRING_TRANSFORM (\n    .clk(" << clockName << ")";
  for (unsigned i = 0; i < routes.size(); ++i)
    v << ",\n    .probe" << i << " (" << ports[i + 1].name.getValue() << ")";
  v << "\n  );\n`endif\nendmodule\n";
  auto annotation = [&](StringRef cls, ArrayRef<NamedAttribute> fields) {
    SmallVector<NamedAttribute> attrs{b.getNamedAttr("class", b.getStringAttr(cls))};
    attrs.append(fields.begin(), fields.end()); return b.getDictionaryAttr(attrs);
  };
  SmallVector<Attribute> annotations{
    annotation("midas.stage.GoldenGateOutputFileAnnotation", {
        b.getNamedAttr("body", b.getStringAttr(tcl)),
        b.getNamedAttr("fileSuffix", b.getStringAttr("." + ipName + ".ipgen.tcl"))}),
    annotation("firrtl.transforms.BlackBoxInlineAnno", {
        b.getNamedAttr("target", b.getStringAttr(circuit.getName().str() + "." + moduleName)),
        b.getNamedAttr("name", b.getStringAttr(options.outputBaseFilename + ".ila_wrapper_inst.v")),
        b.getNamedAttr("text", b.getStringAttr(verilog))}),
    annotation(AnnotationClasses::HostClockSink, {
        b.getNamedAttr("target", b.getStringAttr("~" + circuit.getName().str() + "|" + top.getName().str() + ">" + instanceName + "." + clockName))})};
  for (auto attr : raw) {
    if (isTemporaryILAAnnotation(attr)) continue;
    annotations.push_back(attr);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto external = b.create<FExtModuleOp>(top.getLoc(), b.getStringAttr(moduleName),
      ConventionAttr::get(circuit.getContext(), Convention::Internal), ports, moduleName);
  b.setInsertionPointToStart(top.getBodyBlock());
  auto instance = b.create<InstanceOp>(top.getLoc(), external, instanceName);
  for (auto [i, route] : llvm::enumerate(routes)) {
    Value port = route.topPort;
    port.replaceAllUsesWith(instance.getResult(i + 1));
  }
  llvm::BitVector removed(top.getNumPorts());
  removed.set(oldPorts, top.getNumPorts());
  top.erasePorts(removed);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  wrapper = instance;
  return success();
}

LogicalResult goldengate::runAutoILA(
    CircuitOp circuit, bool enabled, const ILAWrapperOptions &options,
    unsigned &probeCount, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoILA requires retained annotations";
    return failure();
  }
  bool selected = llvm::any_of(raw, [](Attribute attr) {
    auto dict = dyn_cast<DictionaryAttr>(attr);
    auto cls = dict ? dict.getAs<StringAttr>("class") : StringAttr();
    return cls && cls.getValue() == AnnotationClasses::InternalFpgaDebug;
  });
  if (!enabled || !selected) {
    SmallVector<Attribute> retained;
    for (auto attr : raw)
      if (!isTemporaryILAAnnotation(attr)) retained.push_back(attr);
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
    probeCount = 0;
    return success();
  }
  // Validate options before hierarchy wiring changes any module signature.
  StringRef filename(options.outputBaseFilename);
  if (filename.empty() || filename == "." || filename == ".." ||
      filename.find_first_of("/\\") != StringRef::npos ||
      filename.contains('\0') || !options.dataDepth || !options.probeTriggers) {
    error = "AutoILA requires a local output filename and positive IP parameters";
    return failure();
  }
  SmallVector<WiredILAProbe> routes;
  if (failed(wireAutoILAProbesToTop(circuit, routes, error))) return failure();
  InstanceOp wrapper;
  if (failed(attachAutoILAWrapper(circuit, routes, options, wrapper, error))) return failure();
  unsigned clocks = 0;
  if (wrapper && failed(wireHostClock(circuit, clocks, error, true))) return failure();
  probeCount = routes.size();
  return success();
}
