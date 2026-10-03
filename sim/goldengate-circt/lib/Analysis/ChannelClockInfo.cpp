// See LICENSE for license details.
#include "goldengate/ChannelClockInfo.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <functional>
#include <map>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct FieldProjection {
  Value base;
  SmallVector<StringAttr> fields;
};

FieldProjection describeField(Value value) {
  FieldProjection result;
  while (auto field = value.getDefiningOp<SubfieldOp>()) {
    result.fields.insert(result.fields.begin(),
                         StringAttr::get(value.getContext(),
                                         field.getFieldName()));
    value = field.getInput();
  }
  result.base = value;
  return result;
}

bool isInputField(FModuleOp module, Value value) {
  auto projection = describeField(value);
  auto argument = dyn_cast<BlockArgument>(projection.base);
  if (!argument || argument.getOwner() != module.getBodyBlock())
    return false;
  bool input = module.getPortDirection(argument.getArgNumber()) == Direction::In;
  Type type = argument.getType();
  for (StringAttr name : projection.fields) {
    auto bundle = dyn_cast<BundleType>(type);
    if (!bundle)
      return false;
    bool found = false;
    for (const auto &field : bundle.getElements()) {
      if (field.name != name)
        continue;
      input ^= field.isFlip;
      type = field.type;
      found = true;
      break;
    }
    if (!found)
      return false;
  }
  return input;
}

SmallVector<Value> matchingFields(Value base, ArrayRef<StringAttr> fields) {
  SmallVector<Value> current{base};
  for (StringAttr name : fields) {
    SmallVector<Value> next;
    for (Value parent : current)
      for (Operation *user : parent.getUsers())
        if (auto field = dyn_cast<SubfieldOp>(user);
            field && field.getFieldName() == name.getValue())
          next.push_back(field.getResult());
    current = std::move(next);
  }
  return current;
}

Value projectFieldAfter(Value base, ArrayRef<StringAttr> fields,
                        Operation *after) {
  OpBuilder builder(after);
  builder.setInsertionPointAfter(after);
  // A matching SubfieldOp elsewhere in the block may be defined after this
  // insertion point. Project here so every new operand dominates its use.
  for (StringAttr name : fields)
    base = builder.create<SubfieldOp>(after->getLoc(), base, name.getValue());
  return base;
}

Value projectPortField(FModuleOp module, Value base,
                       ArrayRef<StringAttr> fields) {
  OpBuilder builder(module.getContext());
  builder.setInsertionPointToStart(module.getBodyBlock());
  for (StringAttr name : fields)
    base = builder.create<SubfieldOp>(module.getLoc(), base, name.getValue());
  return base;
}

std::optional<Value> topClockPort(CircuitOp circuit, FModuleOp top,
                                  StringAttr spelling, std::string &error) {
  llvm::StringRef targetName = spelling.getValue();
  auto [portName, fieldName] = targetName.split('.');
  auto target = goldengate::resolveAnnotationTarget(circuit, portName, error);
  if (!target || target->module != top || !target->port) {
    if (error.empty())
      error = "channel clock target is not a top-level clock port: " +
              spelling.getValue().str();
    return std::nullopt;
  }
  Value port = top.getBodyBlock()->getArgument(*target->port);
  if (fieldName.empty()) {
    if (isa<ClockType>(port.getType()))
      return port;
  } else if (!fieldName.contains('.') && !fieldName.contains('[')) {
    if (auto bundle = dyn_cast<BundleType>(port.getType())) {
      for (const auto &field : bundle.getElements()) {
        if (field.name.getValue() != fieldName ||
            !isa<ClockType>(field.type))
          continue;
        // Bridge promotion already materializes a subfield for each flattened
        // leaf. Reuse that SSA value so clock-driver traversal sees its
        // connections rather than constructing a disconnected read.
        for (Operation *user : port.getUsers())
          if (auto subfield = dyn_cast<SubfieldOp>(user);
              subfield && subfield.getFieldName() == fieldName)
            return subfield.getResult();
      }
    }
  }
  error = "channel clock target has no clock-typed FIRRTL port field: " +
          spelling.getValue().str();
  return std::nullopt;
}
} // namespace

LogicalResult goldengate::analyzeChannelClocksAndUpdateBridges(
    CircuitOp circuit, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  if (!raw || !top) {
    error = "clock analysis needs a top module and retained annotations";
    return failure();
  }

  Annotation clockChannel;
  unsigned clockChannelCount = 0;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    if (info && info.getAs<StringAttr>("class") ==
                    AnnotationClasses::TargetClockChannel) {
      clockChannel = annotation;
      ++clockChannelCount;
    }
  }
  if (clockChannelCount != 1) {
    error = "expected exactly one target clock channel";
    return failure();
  }
  auto clockInfo = clockChannel.getMember<DictionaryAttr>("channelInfo")
                       .getAs<ArrayAttr>("clockInfo");
  auto sources = clockChannel.getMember<ArrayAttr>("sources");
  auto sinks = clockChannel.getMember<ArrayAttr>("sinks");
  unsigned sourceCount = sources ? sources.size() : 0;
  unsigned sinkCount = sinks ? sinks.size() : 0;
  if (!clockInfo || clockInfo.size() != sourceCount + sinkCount) {
    error = "target clock endpoint and clockInfo counts differ";
    return failure();
  }

  DenseMap<Value, Attribute> rootClocks;
  unsigned clockIndex = 0;
  for (ArrayAttr endpoints : {sources, sinks}) {
    if (!endpoints)
      continue;
    for (Attribute endpoint : endpoints) {
      auto spelling = dyn_cast<StringAttr>(endpoint);
      auto record = dyn_cast<DictionaryAttr>(clockInfo[clockIndex++]);
      if (!spelling || !record || !record.getAs<StringAttr>("name") ||
          !record.getAs<IntegerAttr>("multiplier") ||
          !record.getAs<IntegerAttr>("divisor")) {
        error = "invalid target clock endpoint or ratio";
        return failure();
      }
      auto port = topClockPort(circuit, top, spelling, error);
      if (!port)
        return failure();
      if (rootClocks.count(*port)) {
        error = "target clock endpoint is duplicated";
        return failure();
      }
      rootClocks[*port] = record;
    }
  }

  // FIRRTL connects use last-connect precedence. A clock leaf can be driven
  // by a connect to an enclosing bundle (for example clockNodeIn <-
  // auto.clock_in), so compare the complete field path before projecting the
  // source. An instance output is traced through its module to an input port.
  std::map<std::string, FModuleOp> modules;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op))
      modules.emplace(module.getName().str(), module);
  auto getDriver = [&](FModuleOp module, Value value) -> std::optional<Value> {
    auto wanted = describeField(value);
    for (Operation &op : llvm::reverse(module.getBodyBlock()->getOperations())) {
      Value dest, source;
      if (auto connect = dyn_cast<ConnectOp>(&op)) {
        dest = connect.getDest();
        source = connect.getSrc();
      } else if (auto connect = dyn_cast<StrictConnectOp>(&op)) {
        dest = connect.getDest();
        source = connect.getSrc();
      } else {
        continue;
      }
      auto assigned = describeField(dest);
      if (assigned.base != wanted.base ||
          assigned.fields.size() > wanted.fields.size() ||
          !std::equal(assigned.fields.begin(), assigned.fields.end(),
                      wanted.fields.begin()))
        continue;
      // The suffix identifies the same leaf in the aggregate source. Create
      // projections only after finding the winning connect so that repeated
      // aggregate assignments do not manufacture unused FIRRTL operations.
      OpBuilder builder(&op);
      builder.setInsertionPointAfter(&op);
      for (StringAttr field :
           ArrayRef<StringAttr>(wanted.fields).drop_front(assigned.fields.size())) {
        source = builder.create<SubfieldOp>(op.getLoc(), source,
                                            field.getValue());
      }
      if (isa<ClockType>(source.getType()))
        return source;
    }
    return std::nullopt;
  };
  DenseSet<Value> active;
  std::function<std::optional<Value>(FModuleOp, Value)> trace =
      [&](FModuleOp module, Value value) -> std::optional<Value> {
    if (!active.insert(value).second) {
      error = "cycle in clock connections";
      std::string rendered;
      llvm::raw_string_ostream stream(rendered);
      value.print(stream);
      error += " in " + module.getName().str() + " at " + rendered;
      return std::nullopt;
    }
    auto finish = [&](std::optional<Value> result) {
      active.erase(value);
      return result;
    };
    // A promoted clock bridge drives a flipped Clock field of a top-level
    // bundle. Its SubfieldOp is the root, even though the enclosing bundle is
    // an input port and the field itself has no incoming FIRRTL connect.
    if (module == top) {
      auto projection = describeField(value);
      for (Value equivalent :
           matchingFields(projection.base, projection.fields))
        if (rootClocks.count(equivalent))
          return finish(equivalent);
    }
    if (auto argument = dyn_cast<BlockArgument>(value)) {
      if (argument.getOwner() != module.getBodyBlock())
        return finish(std::nullopt);
      if (module.getPortDirection(argument.getArgNumber()) == Direction::In)
        return finish(value);
    }
    // A clock may be a field of an instance's aggregate output. Follow the
    // same field in the child's output port, then project the traced child
    // input back onto the parent-side instance input.
    auto projected = describeField(value);
    if (!projected.fields.empty())
      if (auto instance = projected.base.getDefiningOp<InstanceOp>()) {
        unsigned port = cast<OpResult>(projected.base).getResultNumber();
        auto child = modules.find(instance.getModuleName().str());
        if (child == modules.end() ||
            child->second.getPortDirection(port) != Direction::Out) {
          error = "clock field is not on an internal instance output: " +
                  instance.getName().str();
          return finish(std::nullopt);
        }
        Value childOutput = projectPortField(
            child->second, child->second.getBodyBlock()->getArgument(port),
            projected.fields);
        // A flipped field of an output bundle is an instance input. Its
        // driver is in this parent module, not in the child's output logic.
        if (!isInputField(child->second, childOutput)) {
          auto childRoot = trace(child->second, childOutput);
          if (!childRoot)
            return finish(std::nullopt);
          auto inputProjection = describeField(*childRoot);
          auto childInput = dyn_cast<BlockArgument>(inputProjection.base);
          if (!childInput || !isInputField(child->second, *childRoot)) {
            error = "clock field has no child input root: " +
                    instance.getName().str();
            return finish(std::nullopt);
          }
          Value parentInput = projectFieldAfter(
              instance.getResult(childInput.getArgNumber()),
              inputProjection.fields, instance.getOperation());
          return finish(trace(module, parentInput));
        }
      }
    if (auto instance = value.getDefiningOp<InstanceOp>()) {
      unsigned port = cast<OpResult>(value).getResultNumber();
      auto child = modules.find(instance.getModuleName().str());
      if (child == modules.end())
        return finish(std::nullopt);
      if (child->second.getPortDirection(port) == Direction::Out) {
        auto childRoot = trace(child->second,
                               child->second.getBodyBlock()->getArgument(port));
        if (!childRoot || !isInputField(child->second, *childRoot))
          return finish(std::nullopt);
        auto childInput = describeField(*childRoot);
        auto argument = cast<BlockArgument>(childInput.base);
        Value parentInput = projectFieldAfter(
            instance.getResult(argument.getArgNumber()), childInput.fields,
            instance.getOperation());
        return finish(trace(module, parentInput));
      }
    }
    if (auto node = value.getDefiningOp<NodeOp>())
      return finish(trace(module, node->getOperand(0)));
    auto driver = getDriver(module, value);
    if (!driver) {
      auto inputProjection = describeField(value);
      if (isInputField(module, value))
        return finish(value);
      error = "missing clock driver in " + module.getName().str();
      if (auto field = value.getDefiningOp<SubfieldOp>())
        error += " for field " + field.getFieldName().str();
      else if (Operation *op = value.getDefiningOp())
        error += " for " + op->getName().getStringRef().str();
      else if (auto argument = dyn_cast<BlockArgument>(value))
        error += " for port " +
                 module.getPortName(argument.getArgNumber()).str();
      return finish(std::nullopt);
    }
    return finish(trace(module, *driver));
  };

  std::map<std::string, Attribute> channelClocks;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto clock = annotation.getMember<StringAttr>("clock");
    if (!clock)
      continue;
    auto name = annotation.getMember<StringAttr>("globalName");
    if (!name) {
      error = "clocked channel has no global name";
      return failure();
    }
    auto port = topClockPort(circuit, top, clock, error);
    if (!port)
      return failure();
    auto rootPort = trace(top, *port);
    auto root = rootPort ? rootClocks.find(*rootPort) : rootClocks.end();
    if (root == rootClocks.end()) {
      if (error.empty())
        error = "clock has no unique root at the target clock channel";
      if (rootPort) {
        std::string rendered;
        llvm::raw_string_ostream stream(rendered);
        (*rootPort).print(stream);
        error += ": traced " + rendered;
      }
      error = "channel " + name.getValue().str() + ": " + error;
      return failure();
    }
    if (!channelClocks.emplace(name.getValue().str(), root->second).second) {
      error = "duplicate channel clock name: " + name.getValue().str();
      return failure();
    }
  }

  Builder builder(circuit.getContext());
  SmallVector<NamedAttribute> infoMembers;
  for (const auto &[name, record] : channelClocks)
    infoMembers.push_back(builder.getNamedAttr(name, record));
  auto infoMap = DictionaryAttr::get(circuit.getContext(), infoMembers);
  SmallVector<Attribute> rewritten;
  rewritten.push_back(DictionaryAttr::get(
      circuit.getContext(),
      {builder.getNamedAttr("class", builder.getStringAttr(
                                        AnnotationClasses::ChannelClockInfo)),
       builder.getNamedAttr("infoMap", infoMap)}));
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::BridgeIO) ||
        annotation.getMember<DictionaryAttr>("clockInfo")) {
      rewritten.push_back(attr);
      continue;
    }
    auto mapping = annotation.getMember<DictionaryAttr>("channelMapping");
    if (!mapping || mapping.empty()) {
      error = "bridge has no channel mapping";
      return failure();
    }
    Attribute bridgeClock;
    for (NamedAttribute member : mapping) {
      auto channel = dyn_cast<StringAttr>(member.getValue());
      if (!channel) {
        error = "bridge channel mapping contains a non-string";
        return failure();
      }
      auto found = channelClocks.find(channel.getValue().str());
      if (found == channelClocks.end())
        continue;
      if (bridgeClock && bridgeClock != found->second) {
        error = "bridge channels have different clock domains";
        return failure();
      }
      bridgeClock = found->second;
    }
    if (!bridgeClock) {
      rewritten.push_back(attr); // The clock bridge has no channel clock.
      continue;
    }
    SmallVector<NamedAttribute> members;
    for (NamedAttribute member : cast<DictionaryAttr>(attr))
      members.push_back(member);
    members.push_back(builder.getNamedAttr("clockInfo", bridgeClock));
    rewritten.push_back(DictionaryAttr::get(circuit.getContext(), members));
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(),
                                                      rewritten));
  return success();
}
