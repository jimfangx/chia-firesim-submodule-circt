// See LICENSE for license details.
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include <set>

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::addFAMEClockChannel(CircuitOp circuit,
                                             std::string &error) {
  auto reject = [&](llvm::StringRef reason) {
    error = reason.str();
    return failure();
  };
  FModuleOp top, wrapper;
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module.getName() == circuit.getName()) top = module;
    if (module.getName() == "GGFAMEPipeWrapper") wrapper = module;
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!top || !wrapper || top == wrapper || !raw)
    return reject("clock channel requires an inactive simulator wrapper and annotations");
  DictionaryAttr clockAnnotation;
  unsigned annotationIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    Annotation annotation(attr);
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    if (!annotation.isClass(AnnotationClasses::ChannelConnection) || !info ||
        info.getAs<StringAttr>("class") != AnnotationClasses::TargetClockChannel)
      continue;
    if (clockAnnotation)
      return reject("clock channel has multiple TargetClockChannel annotations");
    clockAnnotation = cast<DictionaryAttr>(attr);
    annotationIndex = i;
  }
  if (!clockAnnotation)
    return reject("clock channel is missing its TargetClockChannel annotation");
  auto sinks = clockAnnotation.getAs<ArrayAttr>("sinks");
  auto sources = clockAnnotation.getAs<ArrayAttr>("sources");
  auto info = clockAnnotation.getAs<DictionaryAttr>("channelInfo");
  auto clocks = info.getAs<ArrayAttr>("clockInfo");
  auto ratios = info.getAs<ArrayAttr>("perClockMFMR");
  if (!sinks || sinks.empty() || (sources && !sources.empty()) ||
      clockAnnotation.get("clock") || !clocks || !ratios ||
      clocks.size() != sinks.size() || ratios.size() != sinks.size())
    return reject("clock channel needs boundary sinks and matching clock metadata");

  std::optional<unsigned> port;
  SmallVector<uint64_t> fieldIDs;
  std::set<uint64_t> uniqueFields;
  for (auto attr : sinks) {
    auto spelling = dyn_cast<StringAttr>(attr);
    auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error)
                           : std::nullopt;
    if (!target || target->module != top || !target->port || !target->fieldID)
      return reject("clock channel sink must resolve to a target top port field");
    if (port && *port != *target->port)
      return reject("clock channel sinks must share one Decoupled port");
    port = *target->port;
    if (!uniqueFields.insert(*target->fieldID).second)
      return reject("clock channel has duplicate sink fields");
    fieldIDs.push_back(*target->fieldID);
  }
  auto bit = UIntType::get(circuit.getContext(), 1, false);
  auto type = dyn_cast<BundleType>(top.getPortType(*port));
  if (!type || type.getElements().size() != 3 ||
      !type.getElementIndex("ready") || !type.getElementIndex("valid") ||
      !type.getElementIndex("bits") ||
      type.getElement("ready")->type != bit || !type.getElement("ready")->isFlip ||
      type.getElement("valid")->type != bit || type.getElement("valid")->isFlip ||
      type.getElement("bits")->isFlip || top.getPortDirection(*port) != Direction::In ||
      wrapper.getPorts().size() != top.getPorts().size() ||
      wrapper.getPortName(*port) != top.getPortName(*port) ||
      wrapper.getPortType(*port) != type || wrapper.getPortDirection(*port) != Direction::In)
    return reject("clock channel has an incompatible Decoupled Clock port");

  uint64_t bitsID = type.getFieldID(*type.getElementIndex("bits"));
  auto payload = type.getElement("bits")->type;
  auto record = dyn_cast<BundleType>(payload);
  SmallVector<std::optional<unsigned>> recordFields;
  if (isa<ClockType>(payload)) {
    if (fieldIDs.size() != 1 || fieldIDs.front() != bitsID)
      return reject("scalar clock channel requires exactly its bits sink");
    recordFields.push_back(std::nullopt);
  } else if (record && record.getElements().size() == fieldIDs.size()) {
    for (auto element : record.getElements())
      if (element.isFlip || !isa<ClockType>(element.type))
        return reject("clock record must contain only passive Clock fields");
    // SFC zips ClockRecord.elements with clockTokens.bits. Require the same
    // order in retained sink identities so metadata cannot silently swap lanes.
    for (auto [i, id] : llvm::enumerate(fieldIDs)) {
      if (bitsID + record.getFieldID(i) != id)
        return reject("clock sink order must match clock record element order");
      recordFields.push_back(i);
    }
  } else {
    return reject("clock payload must be a Clock or a complete flat clock record");
  }

  InstanceOp child;
  for (auto instance : wrapper.getOps<InstanceOp>())
    if (instance.getModuleName() == top.getName()) {
      if (child) return reject("clock wrapper has multiple target instances");
      child = instance;
    }
  if (!child) return reject("clock wrapper lacks its target instance");
  bool wrapperInstantiated = false;
  circuit.walk([&](InstanceOp instance) {
    wrapperInstantiated |= instance.getModuleName() == wrapper.getName();
  });
  if (wrapperInstantiated)
    return reject("clock wrapper port must be converted before instantiation");
  Value external = wrapper.getBodyBlock()->getArgument(*port);
  Value internal = child.getResult(*port);
  ConnectOp bulk;
  for (auto *use : external.getUsers()) {
    auto connect = dyn_cast<ConnectOp>(use);
    if (!connect || connect.getDest() != internal || connect.getSrc() != external || bulk)
      return reject("clock wrapper needs one untouched target passthrough connection");
    bulk = connect;
  }
  if (!bulk) return reject("clock wrapper passthrough connection is missing");
  if (!internal.hasOneUse())
    return reject("clock target payload has unexpected wrapper uses");

  // Validate everything before changing a port type. SimWrapperChannels uses
  // Vec[Bool] even for one clock; the target still uses Clock or ClockRecord.
  SmallVector<BundleType::BundleElement> elements(type.getElements().begin(), type.getElements().end());
  elements[*type.getElementIndex("bits")].type = FVectorType::get(bit, sinks.size());
  auto tokenType = BundleType::get(circuit.getContext(), elements);
  SmallVector<Attribute> portTypes(wrapper.getPortTypes().begin(), wrapper.getPortTypes().end());
  portTypes[*port] = TypeAttr::get(tokenType);
  bulk.erase();
  wrapper.setPortTypes(portTypes);
  external.setType(tokenType);
  OpBuilder builder(wrapper.getBodyBlock(), wrapper.getBodyBlock()->end());
  Location loc = wrapper.getLoc();
  auto field = [&](Value value, llvm::StringRef name) {
    return builder.create<SubfieldOp>(loc, value, name).getResult();
  };
  builder.create<ConnectOp>(loc, field(internal, "valid"), field(external, "valid"));
  builder.create<ConnectOp>(loc, field(external, "ready"), field(internal, "ready"));
  Value tokenBits = field(external, "bits"), targetBits = field(internal, "bits");
  SmallVector<Attribute> newSinks;
  for (auto [i, index] : llvm::enumerate(recordFields)) {
    Value destination = index ? builder.create<SubfieldOp>(loc, targetBits, *index).getResult()
                              : targetBits;
    Value token = builder.create<SubindexOp>(loc, tokenBits, i);
    builder.create<ConnectOp>(loc, destination, builder.create<AsClockPrimOp>(loc, token));
    newSinks.push_back(builder.getStringAttr("~" + circuit.getName().str() + "|" +
        wrapper.getName().str() + ">" + wrapper.getPortName(*port).str() +
        ".bits[" + std::to_string(i) + "]"));
  }
  // Boundary endpoints now refer to Boolean tokens; clockInfo and MFMR stay
  // unchanged. Other channel clock references still identify target domains.
  NamedAttrList attrs(clockAnnotation);
  attrs.set("sinks", builder.getArrayAttr(newSinks));
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  annotations[annotationIndex] = attrs.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations", builder.getArrayAttr(annotations));
  return success();
}
