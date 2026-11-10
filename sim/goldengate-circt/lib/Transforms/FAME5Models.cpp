// See LICENSE for license details.
// Oracle: midas/passes/fame/MultiThreadFAME5Models.scala and MuxingMultithreader.scala.
#include "goldengate/FAME5Models.h"
#include "goldengate/FAME5ThreadedState.h"
#include "goldengate/XilinxHostSpecialization.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
#include <map>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

namespace {
bool payloadType(FIRRTLBaseType type) {
  if (auto integer = dyn_cast<IntType>(type))
    return integer.getWidthOrSentinel() >= 0;
  if (auto bundle = dyn_cast<BundleType>(type))
    return llvm::all_of(bundle.getElements(), [](auto field) {
      return !field.isFlip && payloadType(field.type);
    });
  if (auto vector = dyn_cast<FVectorType>(type))
    return payloadType(vector.getElementType());
  return false;
}
bool channelType(Type type) {
  auto bundle = dyn_cast<BundleType>(type);
  if (!bundle || bundle.getNumElements() != 3) return false;
  auto ready = bundle.getElement("ready"), valid = bundle.getElement("valid"), bits = bundle.getElement("bits");
  auto bit = UIntType::get(type.getContext(), 1);
  return ready && valid && bits && ready->isFlip && !valid->isFlip && !bits->isFlip &&
         ready->type == bit && valid->type == bit && payloadType(bits->type);
}
std::string fresh(std::set<std::string> &names, StringRef base) {
  std::string result = base.str();
  for (unsigned i = 0; !names.insert(result).second; ++i)
    result = (base + "_" + Twine(i)).str();
  return result;
}
std::set<std::string> localNames(FModuleOp module) {
  std::set<std::string> names;
  for (auto port : module.getPorts()) names.insert(port.getName().str());
  module.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.insert(name.str());
  });
  return names;
}
Value counter(OpBuilder &b, Location loc, Value clock, Value reset,
              unsigned threads, StringRef name) {
  unsigned width = llvm::Log2_64_Ceil(threads);
  auto type = UIntType::get(b.getContext(), width);
  Value zero = b.create<ConstantOp>(loc, type, APInt(width, 0));
  Value index = reset ? b.create<RegResetOp>(loc, type, clock, reset, zero, name).getResult()
                      : b.create<RegOp>(loc, type, clock, name).getResult();
  Value max = b.create<ConstantOp>(loc, type, APInt(width, threads - 1));
  Value one = b.create<ConstantOp>(loc, type, APInt(width, 1));
  Value inc = b.create<AddPrimOp>(loc, index, one);
  inc = b.create<BitsPrimOp>(loc, inc, width - 1, 0);
  Value next = b.create<MuxPrimOp>(loc, b.create<EQPrimOp>(loc, index, max), zero, inc);
  b.create<StrictConnectOp>(loc, index, next);
  return index;
}

// Rocket plusarg_reader is an elaboration-time configuration source, not a
// target state element. Reusing its unchanged external definition/instance in a
// threaded clone preserves FORMAT/DEFAULT in simulation and DEFAULT in FPGA
// synthesis. Do not generalize this exception to arbitrary inputless blackboxes.
bool sharedConfigurationSource(InstanceOp instance) {
  auto circuit = instance->getParentOfType<CircuitOp>();
  auto external = dyn_cast_or_null<FExtModuleOp>(
      SymbolTable::lookupSymbolIn(circuit, instance.getModuleName()));
  if (!external || external.getDefname() != "plusarg_reader" ||
      external.getNumPorts() != 1 || instance.getNumResults() != 1 ||
      external.getPortName(0) != "out" || external.getPortDirection(0) != Direction::Out ||
      !external.getLayers().empty()) return false;
  auto output = dyn_cast<UIntType>(external.getPortType(0));
  if (!output || output.getWidthOrSentinel() <= 0) return false;
  std::map<std::string, Attribute> parameters;
  for (auto parameter : external.getParameters()) {
    auto declaration = dyn_cast<ParamDeclAttr>(parameter);
    if (!declaration || !declaration.getValue() ||
        !parameters.emplace(declaration.getName().str(), declaration.getValue()).second) return false;
  }
  if (parameters.size() != 3) return false;
  auto width = dyn_cast_or_null<IntegerAttr>(parameters["WIDTH"]);
  auto initial = dyn_cast_or_null<IntegerAttr>(parameters["DEFAULT"]);
  auto format = dyn_cast_or_null<StringAttr>(parameters["FORMAT"]);
  return width && width.getInt() == output.getWidthOrSentinel() && initial &&
         format && !format.getValue().empty();
}

LogicalResult apply(CircuitOp circuit, unsigned &groups, std::string &error,
                    goldengate::FAME5ClockSemantics semantics) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("FAME5 grouping needs retained annotations");
  if (circuit->hasAttr("goldengate.fame5.grouped"))
    return reject("FAME5 grouping was already applied");
  FModuleOp top;
  std::map<std::string, FModuleOp> modules;
  std::set<std::string> selected;
  for (auto module : circuit.getOps<FModuleOp>()) {
    modules.emplace(module.getName().str(), module);
    if (module.getName() == circuit.getName()) top = module;
  }
  if (!top) return reject("FAME5 grouping requires an internal circuit top");
  std::string prefix = "~" + circuit.getName().str() + "|" + top.getName().str() + "/";
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::EnableModelMultiThreading)) continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (!target || !target.getValue().starts_with(prefix))
      return reject("FAME5 selection must name a direct top-level model instance");
    auto path = target.getValue().drop_front(prefix.size());
    auto pair = path.split(':');
    if (pair.first.empty() || pair.second.empty() || path.contains('/') || path.contains('>') ||
        pair.second.contains(':') || !modules.count(pair.second.str()))
      return reject("FAME5 selection has an unsupported instance target");
    bool found = false;
    for (auto instance : top.getOps<InstanceOp>())
      found |= instance.getName() == pair.first && instance.getModuleName() == pair.second;
    if (!found) return reject("FAME5 selection does not resolve to a live top instance");
    selected.insert(pair.second.str());
  }
  std::map<std::string, SmallVector<InstanceOp>> repeated;
  for (auto instance : top.getOps<InstanceOp>())
    if (selected.count(instance.getModuleName().str()))
      repeated[instance.getModuleName().str()].push_back(instance);
  for (auto it = repeated.begin(); it != repeated.end();)
    if (it->second.size() < 2) it = repeated.erase(it); else ++it;
  if (repeated.empty()) { groups = 0; return success(); }
  SmallVector<Attribute> allocations;
  for (auto &[moduleName, instances] : repeated) {
  FModuleOp model = modules.at(moduleName);
  // SFC threads every internal descendant with the same thread count. Preserve
  // each child instance while reusing one threaded definition per source module.
  // Preflight the complete reachable hierarchy before mutating the staged IR.
  SmallVector<FModuleOp> hierarchy;
  std::map<std::string, unsigned> visited;
  std::function<LogicalResult(FModuleOp)> visit = [&](FModuleOp current) {
    auto &mark = visited[current.getName().str()];
    if (mark == 1) return reject("FAME5 recursive module cycles are unsupported");
    if (mark == 2) return success();
    mark = 1;
    if (current == top) return reject("FAME5 grouped hierarchy cannot instantiate the simulator top");
    if (auto annotations = current->getAttrOfType<ArrayAttr>("annotations"); annotations && !annotations.empty())
      return reject("FAME5 grouping needs explicit module annotation renames");
    for (unsigned p = 0; p < current.getNumPorts(); ++p)
      if (!current.getAnnotationsForPort(p).empty() || current.hasPortSymbolAttr(p))
        return reject("FAME5 grouping needs explicit model-port annotation/symbol renames");
    bool nestedInstance = false;
    current.walk([&](InstanceOp instance) { nestedInstance |= instance->getBlock() != current.getBodyBlock(); });
    if (nestedInstance) return reject("FAME5 grouped hierarchy requires flat child declarations");
    for (auto instance : current.getOps<InstanceOp>()) {
      if (instance->getBlock() != current.getBodyBlock() || instance->hasAttr("inner_sym") ||
          !instance.getAnnotations().empty() || instance.getLowerToBind() || !instance.getLayers().empty())
        return reject("FAME5 grouped hierarchy needs flat, unannotated child instances without bind/layer semantics");
      if (auto portAnnotations = instance->getAttrOfType<ArrayAttr>("portAnnotations"))
        for (auto annotation : portAnnotations)
          if (auto array = dyn_cast<ArrayAttr>(annotation); !array || !array.empty())
            return reject("FAME5 grouped hierarchy needs explicit child-port annotation renames");
      auto child = modules.find(instance.getModuleName().str());
      if (child != modules.end()) {
        if (failed(visit(child->second))) return failure();
      } else {
        bool dynamic = goldengate::isFAME5VirtualClockController(instance);
        if (!goldengate::isFAME5ClockGate(instance) && !sharedConfigurationSource(instance) &&
            !(dynamic && semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE)) {
          if (dynamic)
            return reject("FAME5 virtual-clock controller recognized, but dynamic target-edge/local-thread phase correspondence is unsupported");
          error = "FAME5 unsupported external instance: parent=" + current.getName().str() +
                  " instance=" + instance.getName().str() +
                  " module=" + instance.getModuleName().str() +
                  "; only canonical AbstractClockGate and read-only plusarg_reader infrastructure is supported";
          return failure();
        }
      }
    }
    mark = 2;
    hierarchy.push_back(current);
    return success();
  };
  if (failed(visit(model))) return failure();
  auto namedPort = [](FModuleOp module, StringRef name) -> std::optional<unsigned> {
    for (unsigned p = 0; p < module.getNumPorts(); ++p)
      if (module.getPortName(p) == name) return p;
    return std::nullopt;
  };
  auto hostClock = namedPort(top, "hostClock"), hostReset = namedPort(top, "hostReset");
  auto modelClock = namedPort(model, "hostClock"), modelReset = namedPort(model, "hostReset");
  auto bit = UIntType::get(circuit.getContext(), 1);
  if (!hostClock || !hostReset || !modelClock || !modelReset ||
      top.getPortType(*hostClock) != ClockType::get(circuit.getContext()) ||
      model.getPortType(*modelClock) != ClockType::get(circuit.getContext()) ||
      top.getPortType(*hostReset) != bit || model.getPortType(*modelReset) != bit ||
      top.getPortDirection(*hostClock) != Direction::In || top.getPortDirection(*hostReset) != Direction::In ||
      model.getPortDirection(*modelClock) != Direction::In || model.getPortDirection(*modelReset) != Direction::In)
    return reject("FAME5 grouping requires input hostClock/hostReset on top and model");

  // The FIRRTL importer expands flipped channel bulk-connects into exactly
  // ready/valid/bits connects. Recover that one channel topology without
  // accepting partial payloads, aliases, fanout or arbitrary field wiring.
  SmallVector<SmallVector<Value>> channels(model.getNumPorts());
  SmallVector<Operation *> prune, pruneFields;
  std::set<unsigned> usedPorts;
  for (auto instance : instances) {
    if (instance->hasAttr("inner_sym") || !instance.getAnnotations().empty())
      return reject("FAME5 grouping needs explicit instance annotation/symbol renames");
    if (auto portAnnotations = instance->getAttrOfType<ArrayAttr>("portAnnotations"))
      for (auto annotation : portAnnotations)
        if (auto array = dyn_cast<ArrayAttr>(annotation); !array || !array.empty())
          return reject("FAME5 grouping needs explicit instance-port annotation renames");
    for (unsigned p = 0; p < model.getNumPorts(); ++p) {
      Value result = instance.getResult(p);
      bool input = model.getPortDirection(p) == Direction::In;
      bool host = p == *modelClock || p == *modelReset;
      Value peer;
      if (result.hasOneUse() && isa<ConnectOp, StrictConnectOp>(*result.getUsers().begin())) {
        Operation *connect = *result.getUsers().begin();
        if (connect->getBlock() != top.getBodyBlock() || connect->getOperand(input ? 0 : 1) != result)
          return reject("FAME5 channel direction does not match model port direction");
        peer = connect->getOperand(input ? 1 : 0);
        if (!host && !peer.hasOneUse()) return reject("FAME5 channel top port has extra uses");
        prune.push_back(connect);
      } else {
        if (host || !channelType(model.getPortType(p)))
          return reject("FAME5 channels require canonical ready/valid/bits connects");
        std::set<std::string> fields;
        for (Operation *user : result.getUsers()) {
          auto field = dyn_cast<SubfieldOp>(user);
          if (!field || field->getBlock() != top.getBodyBlock() || !field.getResult().hasOneUse() ||
              !fields.insert(field.getFieldName().str()).second)
            return reject("FAME5 channel fields have partial, duplicate or noncanonical uses");
          Operation *connect = *field.getResult().getUsers().begin();
          bool fieldInput = input != (field.getFieldName() == "ready");
          if (!isa<ConnectOp, StrictConnectOp>(connect) || connect->getBlock() != top.getBodyBlock() ||
              connect->getOperand(fieldInput ? 0 : 1) != field.getResult())
            return reject("FAME5 channel field direction or connect is noncanonical");
          auto other = connect->getOperand(fieldInput ? 1 : 0).getDefiningOp<SubfieldOp>();
          if (!other || other.getFieldName() != field.getFieldName() || other->getBlock() != top.getBodyBlock() ||
              !other.getResult().hasOneUse() || (peer && peer != other.getInput()))
            return reject("FAME5 channel fields must connect to corresponding fields of one top port");
          peer = other.getInput();
          prune.push_back(connect); pruneFields.push_back(field); pruneFields.push_back(other);
        }
        if (fields != std::set<std::string>{"ready", "valid", "bits"} || !peer ||
            std::distance(peer.use_begin(), peer.use_end()) != 3)
          return reject("FAME5 channel must have exactly one complete ready/valid/bits top-port connection");
      }
      auto argument = dyn_cast<BlockArgument>(peer);
      if (!argument || argument.getOwner() != top.getBodyBlock())
        return reject("FAME5 channels must connect directly to top ports");
      unsigned index = argument.getArgNumber();
      if (host) {
        if (index != (p == *modelClock ? *hostClock : *hostReset))
          return reject("FAME5 models must share the top host clock/reset");
      } else {
        if (!channelType(model.getPortType(p)) || peer.getType() != result.getType() ||
            top.getPortDirection(index) != model.getPortDirection(p) || !usedPorts.insert(index).second)
          return reject("FAME5 channels need distinct direction-matched ready/valid/bits top ports");
        channels[p].push_back(peer);
      }
    }
  }
  bool hasChannel = llvm::any_of(channels, [](auto &ports) { return !ports.empty(); });
  if (!hasChannel) return reject("FAME5 model has no channels to schedule");

  // SFC retains the consumed model selectors (its TODO: Renames). They are
  // selection provenance. FAMETransform and FAMEChannelPorts are likewise
  // consumed by the preceding FAME transform: their original-definition targets
  // stay attached to the unchanged source definitions, just as in SFC. Runtime
  // transport instead uses ChannelConnection targets on the unchanged top ABI.
  // All other retained targets into the shared definition
  // or removed instance paths must reject rather than silently change meaning.
  bool retainedTarget = false;
  std::string retainedSpelling;
  std::function<void(Attribute)> inspect = [&](Attribute attr) {
    if (auto string = dyn_cast<StringAttr>(attr)) {
      auto spelling = string.getValue();
      if (!spelling.starts_with("~") || !spelling.contains('|')) return;
      auto owner = spelling.split('|').second;
      bool affected = false;
      for (auto source : hierarchy)
        affected |= owner.starts_with(source.getName().str() + ">") ||
                    owner.starts_with(source.getName().str() + "/") || owner == source.getName();
      for (auto instance : instances)
        affected |= owner.starts_with(top.getName().str() + "/" + instance.getName().str() + ":");
      if (affected) { retainedTarget = true; retainedSpelling = spelling.str(); }
    } else if (auto array = dyn_cast<ArrayAttr>(attr)) {
      for (auto entry : array) inspect(entry);
    } else if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
      for (auto entry : dict) inspect(entry.getValue());
    }
  };
  SmallVector<DictionaryAttr> cloneLocalMetadata;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(goldengate::AnnotationClasses::DontTouch) ||
        annotation.isClass("chisel3.util.experimental.decode.DecodeTableAnnotation")) {
      auto target = annotation.getMember<StringAttr>("target");
      if (!target) return reject("FAME5 local declaration metadata needs a reference target");
      auto owner = target.getValue().split('|').second.split('>');
      bool localDefinition = llvm::any_of(hierarchy, [&](FModuleOp source) {
        return owner.first == source.getName();
      });
      if (localDefinition) {
        // Preserve this optimization directive on both the original and the
        // cloned declaration. A banked register is protected as a whole bank;
        // scalar port/wire names and all target data paths stay unchanged.
        if (owner.second.empty() || owner.second.contains('/') ||
            owner.second.contains('.') || owner.second.contains('[') || owner.second.contains('>'))
          return reject("FAME5 metadata requires a lowered local whole-declaration target");
        cloneLocalMetadata.push_back(cast<DictionaryAttr>(attr));
        continue;
      }
    }
    if (annotation.isClass(goldengate::AnnotationClasses::EnableModelMultiThreading) ||
        annotation.isClass(goldengate::AnnotationClasses::FAMEModel) ||
        annotation.isClass(goldengate::AnnotationClasses::FAMETransform) ||
        annotation.isClass(goldengate::AnnotationClasses::ChannelPorts) ||
        // Native dedup already consumed this definition-level policy. Original
        // definitions remain unchanged; the annotation is source provenance,
        // never an instruction to merge newly banked physical definitions.
        annotation.isClass("firrtl.transforms.DedupGroupAnnotation") ||
        // MemModel selects the optional pre-FAME SRAM extraction boundary.
        // When that mode is disabled, it stays on the unchanged source memory
        // as selection provenance; it must not select physical bank storage.
        annotation.isClass(goldengate::AnnotationClasses::MemModel)) continue;
    inspect(attr);
    if (retainedTarget) {
      error = "FAME5 grouping needs explicit renames for retained model/instance target " + retainedSpelling +
              " in " + annotation.getClass().str();
      return failure();
    }
  }

  unsigned threads = instances.size();
  auto *context = circuit.getContext();
  Builder builder(context);
  std::set<std::string> moduleNames;
  for (auto module : circuit.getOps<FModuleLike>()) moduleNames.insert(module.getModuleName().str());
  std::map<std::string, FModuleOp> threadedModules;
  for (auto source : hierarchy) {
    std::string name = fresh(moduleNames, source.getName().str() + "_threaded");
    auto clone = cast<FModuleOp>(source->clone());
    SymbolTable::setSymbolName(clone, name);
    OpBuilder moduleBuilder(source);
    moduleBuilder.setInsertionPointAfter(source);
    moduleBuilder.insert(clone);
    // AddHostClockAndReset appends only absent host ports. Existing ports must
    // have the exact host ABI; target clock/reset ports retain their wiring.
    for (auto name : {StringRef("hostClock"), StringRef("hostReset")}) {
      Type type = name == "hostClock" ? Type(ClockType::get(context)) : Type(bit);
      auto port = namedPort(clone, name);
      if (port) {
        if (clone.getPortDirection(*port) != Direction::In || clone.getPortType(*port) != type)
          return reject("FAME5 child hostClock/hostReset port has an incompatible ABI");
      } else {
        clone.insertPorts({{clone.getNumPorts(), PortInfo(builder.getStringAttr(name), type, Direction::In)}});
      }
    }
    threadedModules.emplace(source.getName().str(), clone);
    auto clockPort = *namedPort(clone, "hostClock");
    auto resetPort = *namedPort(clone, "hostReset");
    SmallVector<InstanceOp> children(clone.getOps<InstanceOp>());
    for (auto child : children) {
      auto found = threadedModules.find(child.getModuleName().str());
      if (found == threadedModules.end()) continue; // checked clock primitive
      auto target = found->second;
      SmallVector<Operation *> oldHostConnections;
      for (StringRef hostName : {StringRef("hostClock"), StringRef("hostReset")}) {
        auto sourcePort = namedPort(modules.at(child.getModuleName().str()), hostName);
        if (!sourcePort) continue;
        Value host = clone.getArgument(hostName == "hostClock" ? clockPort : resetPort);
        Value old = child.getResult(*sourcePort);
        if (!old.hasOneUse())
          return reject("FAME5 child host ports require one canonical parent host connection");
        Operation *connection = *old.getUsers().begin();
        if (!isa<ConnectOp, StrictConnectOp>(connection) || connection->getBlock() != clone.getBodyBlock() ||
            connection->getOperand(0) != old || connection->getOperand(1) != host)
          return reject("FAME5 child host ports must share the parent host clock/reset");
        oldHostConnections.push_back(connection);
      }
      OpBuilder b(child);
      auto replacement = b.create<InstanceOp>(child.getLoc(), target, child.getName(), child.getNameKind());
      for (auto [old, next] : llvm::zip(child.getResults(), replacement.getResults()))
        old.replaceAllUsesWith(next);
      for (auto connection : oldHostConnections) connection->erase();
      b.create<StrictConnectOp>(child.getLoc(), replacement.getResult(*namedPort(target, "hostClock")), clone.getArgument(clockPort));
      b.create<StrictConnectOp>(child.getLoc(), replacement.getResult(*namedPort(target, "hostReset")), clone.getArgument(resetPort));
      child.erase();
    }
    // Abstract mode retains the exact SFC unreset selector relation. Production
    // FPGA mode establishes that relation with hostReset on every selector,
    // matching the reset arbiter. This resets scheduling only: target registers
    // and RAM contents keep their original reset/token semantics.
    unsigned indexPort = clone.getNumPorts();
    auto names = localNames(clone);
    auto indexType = UIntType::get(context, llvm::Log2_64_Ceil(threads));
    clone.insertPorts({{indexPort, PortInfo(builder.getStringAttr(fresh(names, "gg_fame5_activeThread")), indexType, Direction::In)}});
    Value indexInput = clone.getArgument(indexPort);
    if (failed(goldengate::threadFAME5Registers(clone, threads, indexInput, error,
            semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE))) return failure();
    bool hasTargetMemory = false;
    clone.walk([&](MemOp mem) { hasTargetMemory |= !mem->hasAttr("goldengate.fame5.registerState"); });
    if (hasTargetMemory && failed(goldengate::threadFAME5Memories(
            clone, threads, indexInput, error,
            semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE
                ? goldengate::FAME5MemoryClockSemantics::XilinxBUFGCE
                : goldengate::FAME5MemoryClockSemantics::Abstract))) return failure();
    OpBuilder local(clone.getBodyBlock(), clone.getBodyBlock()->begin());
    Value localReset = semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE
                           ? clone.getArgument(resetPort) : Value();
    Value localIndex = counter(local, clone.getLoc(), clone.getArgument(clockPort), localReset, threads, fresh(names, "threadIdx"));
    indexInput.replaceAllUsesWith(localIndex);
    llvm::BitVector erase(clone.getNumPorts()); erase.set(indexPort);
    clone.erasePorts(erase);
    clone->setAttr("goldengate.fame5.sharedThreads", builder.getI32IntegerAttr(threads));
  }
  if (!cloneLocalMetadata.empty()) {
    SmallVector<Attribute> updated(raw.begin(), raw.end());
    for (auto annotation : cloneLocalMetadata) {
      auto target = annotation.getAs<StringAttr>("target").getValue();
      auto owner = target.split('|').second.split('>');
      auto clone = threadedModules.at(owner.first.str());
      if (!localNames(clone).count(owner.second.str()))
        return reject("FAME5 cloned metadata declaration no longer resolves");
      NamedAttrList renamed(annotation);
      renamed.set("target", builder.getStringAttr("~" + circuit.getName().str() + "|" +
                    clone.getName().str() + ">" + owner.second.str()));
      updated.push_back(renamed.getDictionary(context));
    }
    circuit->setAttr("rawAnnotations", builder.getArrayAttr(updated));
    raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  }
  auto threaded = threadedModules.at(moduleName);
  auto threadedName = threaded.getName();
  auto indexType = UIntType::get(context, llvm::Log2_64_Ceil(threads));

  auto topNames = localNames(top);
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->begin());
  auto shared = b.create<InstanceOp>(top.getLoc(), threaded, fresh(topNames, threadedName));
  Value index = counter(b, top.getLoc(), top.getArgument(*hostClock), top.getArgument(*hostReset), threads, fresh(topNames, "threadIdx"));
  auto field = [&](Value value, StringRef name) -> Value { return b.create<SubfieldOp>(top.getLoc(), value, name); };
  auto connect = [&](Value dest, Value src) { b.create<ConnectOp>(top.getLoc(), dest, src); };
  connect(shared.getResult(*modelClock), top.getArgument(*hostClock));
  connect(shared.getResult(*modelReset), top.getArgument(*hostReset));
  auto selectedMask = [&](unsigned slot) -> Value {
    Value literal = b.create<ConstantOp>(top.getLoc(), indexType, APInt(llvm::Log2_64_Ceil(threads), slot));
    return b.create<EQPrimOp>(top.getLoc(), index, literal);
  };
  auto mux = [&](SmallVector<Value> values) -> Value {
    Value value = values.front();
    for (unsigned i = 1; i < values.size(); ++i)
      value = b.create<MuxPrimOp>(top.getLoc(), selectedMask(i), values[i], value);
    return value;
  };
  for (unsigned p = 0; p < channels.size(); ++p) {
    if (channels[p].empty()) continue;
    Value port = shared.getResult(p);
    SmallVector<Value> values;
    if (model.getPortDirection(p) == Direction::In) {
      for (auto peer : channels[p]) values.push_back(field(peer, "valid"));
      connect(field(port, "valid"), mux(values)); values.clear();
      for (auto peer : channels[p]) values.push_back(field(peer, "bits"));
      connect(field(port, "bits"), mux(values));
      for (auto [slot, peer] : llvm::enumerate(channels[p]))
        connect(field(peer, "ready"), b.create<AndPrimOp>(top.getLoc(), field(port, "ready"), selectedMask(slot)));
    } else {
      for (auto peer : channels[p]) values.push_back(field(peer, "ready"));
      connect(field(port, "ready"), mux(values));
      for (auto [slot, peer] : llvm::enumerate(channels[p])) {
        connect(field(peer, "bits"), field(port, "bits"));
        connect(field(peer, "valid"), b.create<AndPrimOp>(top.getLoc(), field(port, "valid"), selectedMask(slot)));
      }
    }
  }
  // Retain the actual allocation, not only a Boolean feature marker. Consumers
  // can bind original logical instances and channel endpoints to the physical
  // shared definition without interpreting stale selection provenance.
  SmallVector<Attribute> channelAllocation;
  for (unsigned p = 0; p < channels.size(); ++p) {
    if (channels[p].empty()) continue;
    SmallVector<Attribute> peers;
    for (auto peer : channels[p]) {
      auto index = cast<BlockArgument>(peer).getArgNumber();
      peers.push_back(builder.getStringAttr(top.getPortName(index)));
    }
    channelAllocation.push_back(builder.getDictionaryAttr({
        builder.getNamedAttr("modelPort", builder.getStringAttr(model.getPortName(p))),
        builder.getNamedAttr("direction", builder.getStringAttr(
            model.getPortDirection(p) == Direction::In ? "input" : "output")),
        builder.getNamedAttr("threadPorts", builder.getArrayAttr(peers))}));
  }
  SmallVector<Attribute> definitions;
  for (auto source : hierarchy)
    definitions.push_back(builder.getDictionaryAttr({
        builder.getNamedAttr("original", builder.getStringAttr(source.getName())),
        builder.getNamedAttr("threaded", builder.getStringAttr(
            threadedModules.at(source.getName().str()).getName()))}));
  for (auto connect : prune) connect->erase();
  for (auto field : pruneFields) field->erase();
  SmallVector<Attribute> members, memberMetadata;
  for (auto instance : instances) {
    members.push_back(builder.getStringAttr(instance.getName()));
    NamedAttrList metadata;
    for (auto attr : instance->getAttrs()) {
      auto name = attr.getName().strref();
      if (name != "moduleName" && name != "name" && name != "portNames" &&
          name != "portDirections" && name != "portAnnotations")
        metadata.set(attr.getName(), attr.getValue());
    }
    memberMetadata.push_back(metadata.getDictionary(context));
    instance.erase();
  }
  shared->setAttr("goldengate.fame5.members", builder.getArrayAttr(members));
  shared->setAttr("goldengate.fame5.memberMetadata", builder.getArrayAttr(memberMetadata));
  allocations.push_back(builder.getDictionaryAttr({
      builder.getNamedAttr("originalModule", builder.getStringAttr(moduleName)),
      builder.getNamedAttr("sharedModule", builder.getStringAttr(threadedName)),
      builder.getNamedAttr("sharedInstance", builder.getStringAttr(shared.getName())),
      builder.getNamedAttr("threads", builder.getI32IntegerAttr(threads)),
      builder.getNamedAttr("members", builder.getArrayAttr(members)),
      builder.getNamedAttr("definitions", builder.getArrayAttr(definitions)),
      builder.getNamedAttr("channels", builder.getArrayAttr(channelAllocation)),
      builder.getNamedAttr("scheduler", builder.getStringAttr("static-host-cycle-round-robin")),
      builder.getNamedAttr("localSelectorReset", builder.getBoolAttr(
          semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE)),
      builder.getNamedAttr("arbiterSelectorReset", builder.getStringAttr("hostReset")),
      builder.getNamedAttr("hostControlReset", builder.getStringAttr(
          semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE
              ? "all-thread-banks-for-canonical-host-controls" : "selected-thread-bank")),
      builder.getNamedAttr("startupPhaseObligation", builder.getStringAttr(
          semantics == goldengate::FAME5ClockSemantics::XilinxBUFGCE
              ? "hostReset aligns local selectors and arbiter before target tokens"
              : "local selectors and arbiter start aligned; host reset preserves whole-thread rotations"))}));
  }
  Builder builder(circuit.getContext());
  circuit->setAttr("goldengate.fame5.groups", builder.getArrayAttr(allocations));
  circuit->setAttr("goldengate.fame5.grouped", builder.getUnitAttr());
  groups = allocations.size();
  return success();
}
} // namespace

LogicalResult goldengate::multiThreadFAME5Models(CircuitOp circuit,
                                               unsigned &threadedGroups,
                                               std::string &error,
                                               FAME5ClockSemantics semantics) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  unsigned groups = 0;
  if (failed(apply(*staged, groups, error, semantics))) return failure();
  if (!groups) { threadedGroups = 0; return success(); }
  // MidasTransforms applies grouping before xilinx.HostSpecialization. The
  // abstract simulation gate samples CE on a rising edge, while production
  // BUFGCE defaults to a synchronous low-phase latch. Preserve that explicit
  // platform distinction: only this requested mode may accept dynamic gates.
  if (semantics == FAME5ClockSemantics::XilinxBUFGCE &&
      failed(specializeXilinxClockGates(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "FAME5 grouping produced invalid FIRRTL IR";
    return failure();
  }
  circuit->setAttrs(staged->getOperation()->getAttrs());
  circuit.getBody().takeBody(staged->getBody());
  threadedGroups = groups;
  return success();
}
