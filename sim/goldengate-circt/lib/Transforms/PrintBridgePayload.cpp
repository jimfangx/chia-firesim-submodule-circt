// See LICENSE for license details.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringSet.h"
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializePrintBridgePayloads(
    CircuitOp circuit, llvm::SmallVectorImpl<FModuleOp> &modules,
    std::string &error) {
  auto reject = [&](llvm::StringRef reason) {
    error = reason.str();
    return failure();
  };
  struct Record {
    StringAttr name, format;
    BundleType type;
    uint64_t width;
    SmallVector<Attribute> argumentWidths;
  };
  struct Payload {
    StringAttr target, reset;
    DictionaryAttr key;
    SmallVector<Record> records;
    uint64_t tokenBits;
  };
  SmallVector<Payload> payloads;
  std::set<std::pair<std::string, std::string>> identities;
  llvm::StringSet<> moduleNames;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    if (auto m = dyn_cast<FModuleLike>(&op))
      moduleNames.insert(m.getModuleName());
    if (auto prior = op.getAttrOfType<DictionaryAttr>("goldengate.printPayload")) {
      auto target = prior.getAs<StringAttr>("bridgeTarget");
      auto reset = prior.getAs<StringAttr>("resetPortName");
      if (target && reset)
        identities.emplace(target.getValue().str(), reset.getValue().str());
    }
  }
  OpBuilder b(circuit.getContext());
  auto bit = UIntType::get(circuit.getContext(), 1);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return success();
  for (auto attr : raw) {
    Annotation anno(attr);
    auto widgetClass = anno.getMember<StringAttr>("widgetClass");
    if (!anno.isClass(AnnotationClasses::BridgeIO) || !widgetClass ||
        widgetClass.getValue() != AnnotationClasses::PrintBridgeModule)
      continue;
    auto dict = dyn_cast<DictionaryAttr>(attr);
    Payload p;
    p.target = dict.getAs<StringAttr>("target");
    p.key = dict.getAs<DictionaryAttr>("widgetConstructorKey");
    auto keyClass = p.key ? p.key.getAs<StringAttr>("class") : StringAttr();
    if (!p.target || p.target.getValue().empty() || !p.key ||
        !keyClass || keyClass.getValue() != AnnotationClasses::PrintBridgeParameters)
      return reject("PrintBridge payload requires a target and PrintBridgeParameters constructor");
    p.reset = p.key.getAs<StringAttr>("resetPortName");
    auto records = p.key.getAs<ArrayAttr>("printPorts");
    if (!p.reset || p.reset.getValue().empty() || !records || records.empty())
      return reject("PrintBridge payload requires resetPortName and nonempty printPorts");
    if (!identities.emplace(p.target.getValue().str(), p.reset.getValue().str()).second)
      return reject("PrintBridge payload target/reset identity already materialized or duplicated");
    llvm::StringSet<> recordNames;
    recordNames.insert(p.reset.getValue());
    uint64_t total = 0;
    for (auto recordAttr : records) {
      auto record = dyn_cast<DictionaryAttr>(recordAttr);
      auto name = record ? record.getAs<StringAttr>("name") : StringAttr();
      auto format = record ? record.getAs<StringAttr>("format") : StringAttr();
      auto fields = record ? record.getAs<ArrayAttr>("ports") : ArrayAttr();
      if (!name || name.getValue().empty() || !format || !fields || fields.empty() ||
          !recordNames.insert(name.getValue()).second)
        return reject("PrintBridge payload needs distinct named records with format and ordered ports");
      SmallVector<BundleType::BundleElement> elements;
      llvm::StringSet<> fieldNames;
      Record r{name, format, {}, 0, {}};
      for (auto [index, fieldAttr] : llvm::enumerate(fields)) {
        auto field = dyn_cast<DictionaryAttr>(fieldAttr);
        if (!field || field.size() != 1)
          return reject("PrintBridge ports must be ordered singleton type dictionaries");
        auto entry = *field.begin();
        auto typeAttr = dyn_cast<StringAttr>(entry.getValue());
        auto fieldName = entry.getName().getValue();
        if (!typeAttr || fieldName.empty() || fieldName == "clock" ||
            !fieldNames.insert(fieldName).second)
          return reject("PrintBridge record ports must have distinct names and integer types");
        llvm::StringRef typeText = typeAttr.getValue();
        bool isSigned = typeText.consume_front("SInt<");
        if (!isSigned && !typeText.consume_front("UInt<"))
          return reject("PrintBridge payload arguments require fixed-width UInt or SInt");
        uint64_t width;
        if (!typeText.consume_back(">") || typeText.empty() ||
            typeText.getAsInteger(10, width) || width > (1ULL << 30))
          return reject("PrintBridge payload integer width is missing or exceeds FIRRTL packing limits");
        if (index == 0 && (fieldName != "enable" || isSigned || width != 1))
          return reject("PrintBridge record must start with enable: UInt<1>");
        FIRRTLBaseType type = isSigned
            ? FIRRTLBaseType(SIntType::get(circuit.getContext(), width))
            : FIRRTLBaseType(UIntType::get(circuit.getContext(), width));
        elements.push_back({b.getStringAttr(fieldName), false, type});
        r.width += width;
        if (index) r.argumentWidths.push_back(b.getI64IntegerAttr(width));
      }
      total += r.width;
      // Leave room for PrintRecordBag.reset and the transmitted validity bit.
      if (total > (1ULL << 30) - 2)
        return reject("PrintBridge record widths exceed FIRRTL packing limits");
      r.type = BundleType::get(circuit.getContext(), elements);
      p.records.push_back(std::move(r));
    }
    // Chisel sizes from printPort.getWidth + 1, including the reset field,
    // although the payload itself transmits only records and validity.
    p.tokenBits = 8;
    while (p.tokenBits < total + 2) p.tokenBits <<= 1;
    payloads.push_back(std::move(p));
  }
  auto loc = circuit.getLoc();
  for (auto &p : payloads) {
    std::string name = "GGPrintBridgePayload";
    for (unsigned suffix = 1; moduleNames.count(name); ++suffix)
      name = "GGPrintBridgePayload_" + std::to_string(suffix);
    moduleNames.insert(name);
    SmallVector<BundleType::BundleElement> fields{{p.reset, false, bit}};
    for (auto &r : p.records) fields.push_back({r.name, false, r.type});
    SmallVector<PortInfo> ports{
        {b.getStringAttr("hBits"), BundleType::get(circuit.getContext(), fields), Direction::In},
        {b.getStringAttr("valid"), bit, Direction::Out},
        {b.getStringAttr("data"), UIntType::get(circuit.getContext(), p.tokenBits), Direction::Out}};
    b.setInsertionPointToStart(circuit.getBodyBlock());
    auto m = b.create<FModuleOp>(loc, b.getStringAttr(name),
        ConventionAttr::get(circuit.getContext(), Convention::Internal), ports);
    m->setAttr("goldengate.bridgeConstructor", p.key);
    b.setInsertionPointToStart(m.getBodyBlock());
    Value reset = b.create<SubfieldOp>(loc, m.getArgument(0), 0);
    Value enabled, packed;
    SmallVector<Attribute> layouts;
    uint64_t offset = 1;
    for (auto [index, r] : llvm::enumerate(p.records)) {
      Value record = b.create<SubfieldOp>(loc, m.getArgument(0), index + 1);
      Value enable = b.create<SubfieldOp>(loc, record, 0);
      enabled = enabled ? Value(b.create<OrPrimOp>(loc, enabled, enable)) : enable;
      for (auto [fieldIndex, field] : llvm::enumerate(r.type.getElements())) {
        auto fieldType = field.type;
        if (!fieldType.getBitWidthOrSentinel()) continue;
        Value value = b.create<SubfieldOp>(loc, record, fieldIndex);
        if (isa<SIntType>(value.getType())) value = b.create<AsUIntPrimOp>(loc, value);
        packed = packed ? Value(b.create<CatPrimOp>(loc, value, packed)) : value;
      }
      layouts.push_back(b.getDictionaryAttr({
          b.getNamedAttr("name", r.name), b.getNamedAttr("format", r.format),
          b.getNamedAttr("offset", b.getI64IntegerAttr(offset)),
          b.getNamedAttr("width", b.getI64IntegerAttr(r.width)),
          b.getNamedAttr("argumentWidths", b.getArrayAttr(r.argumentWidths))}));
      offset += r.width;
    }
    Value notReset = b.create<NotPrimOp>(loc, reset);
    Value valid = b.create<AndPrimOp>(loc, enabled, notReset);
    Value data = b.create<CatPrimOp>(loc, packed, valid);
    data = b.create<PadPrimOp>(loc, data, p.tokenBits);
    b.create<StrictConnectOp>(loc, m.getArgument(1), valid);
    b.create<StrictConnectOp>(loc, m.getArgument(2), data);
    uint64_t idleBits = std::min<uint64_t>(16, p.tokenBits) - 1;
    m->setAttr("goldengate.printPayload", b.getDictionaryAttr({
        b.getNamedAttr("bridgeTarget", p.target), b.getNamedAttr("resetPortName", p.reset),
        b.getNamedAttr("tokenBits", b.getI64IntegerAttr(p.tokenBits)),
        b.getNamedAttr("tokenBytes", b.getI64IntegerAttr(p.tokenBits / 8)),
        b.getNamedAttr("idleCycleBits", b.getI64IntegerAttr(idleBits)),
        b.getNamedAttr("idleCycleMask", b.getI64IntegerAttr(((1ULL << idleBits) - 1) << 1)),
        b.getNamedAttr("records", b.getArrayAttr(layouts))}));
    modules.push_back(m);
  }
  return success();
}
