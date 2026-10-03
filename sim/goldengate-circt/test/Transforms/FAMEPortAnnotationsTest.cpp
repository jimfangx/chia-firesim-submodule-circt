// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEOutputChannel.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/InnerSymbolTable.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
unsigned portNamed(FModuleOp module, StringRef name) {
  for (unsigned i = 0; i < module.getNumPorts(); ++i)
    if (module.getPortName(i) == name) return i;
  throw std::runtime_error("expected port absent: " + name.str());
}
void checkReference(CircuitOp circuit, FModuleOp top,
                    circt::hw::InnerRefAttr reference, unsigned port,
                    unsigned fieldID, StringRef visibility) {
  // Port insertion/erasure invalidates cached symbol tables. Resolve the same
  // reference using fresh tables at each boundary, including after LowerTypes.
  SymbolTable modules(circuit);
  circt::hw::InnerSymbolTableCollection innerTables;
  circt::hw::InnerRefNamespace names{modules, innerTables};
  auto target = names.lookup(reference);
  require(target && target.isPort() && target.getOp() == top &&
              target.getPort() == port && target.getField() == fieldID,
          "stable InnerRef resolved to the wrong payload");
  bool found = false;
  auto symbols = top.getPorts()[port].sym;
  for (auto property : symbols)
    if (property.getName() == reference.getName()) {
      require(property.getFieldID() == fieldID &&
                  property.getSymVisibility().getValue() == visibility,
              "symbol field or visibility changed");
      found = true;
    }
  require(found, "payload symbol lost");
}
void run(MLIRContext &context, bool output, bool grouped, unsigned rejection,
         unsigned metadata = 0, bool withModelSymbols = false) {
  bool withAnnotations = metadata != 2;
  bool withSymbols = metadata != 0;
  std::string dir = output ? "out" : "in";
  auto fixture = "module { firrtl.circuit \"Top\" { firrtl.module @Top("
      "in %hostClock: !firrtl.clock, in %kept: !firrtl.uint<1>, " + dir +
      " %g_data: !firrtl.uint<8>" +
      (grouped ? ", " + dir + " %g_flag: !firrtl.uint<1>" : "") +
      ") {} firrtl.module @Model(in %clock: !firrtl.clock, " + dir +
      " %m_data: !firrtl.uint<8>" +
      (grouped ? ", " + dir + " %m_flag: !firrtl.uint<1>" : "") + ") {} } }";
  auto root = parseSourceString<ModuleOp>(fixture, &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>();
  auto top = *modules.begin();
  auto model = *std::next(modules.begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  instance.setInnerSymAttr(circt::hw::InnerSymAttr::get(b.getStringAttr("instance_id")));
  auto instanceRef = circt::hw::InnerRefAttr::get(b.getStringAttr("Top"),
                                                b.getStringAttr("instance_id"));
  auto checkInstance = [&] {
    SymbolTable modules(circuit);
    circt::hw::InnerSymbolTableCollection innerTables;
    circt::hw::InnerRefNamespace names{modules, innerTables};
    auto target = names.lookup(instanceRef);
    require(target && isa<InstanceOp>(target.getOp()) && !target.isPort(),
            "instance cloning lost its independent inner symbol");
  };
  checkInstance();
  b.create<StrictConnectOp>(top.getLoc(), instance.getResult(0),
                           top.getBodyBlock()->getArgument(0));
  goldengate::TopHierarchy hierarchy{top, {}};
  goldengate::ModelPortGroup group{"m_", model,
      output ? Direction::Out : Direction::In, {}, {1}};
  goldengate::ModelChannelBinding binding{"g_", &group, instance, {1}};
  if (grouped) { group.ports.push_back(2); binding.instancePorts.push_back(2); }
  for (unsigned i = 1; i <= (grouped ? 2u : 1u); ++i) {
    auto wrapperValue = top.getBodyBlock()->getArgument(i + 1);
    auto instanceValue = instance.getResult(i);
    b.create<StrictConnectOp>(top.getLoc(), output ? wrapperValue : instanceValue,
                             output ? instanceValue : wrapperValue);
    hierarchy.connections.push_back({i + 1, instance, i});
  }
  // Empty input-only model bodies are valid and need a safe insertion point.
  if (output) {
    b.setInsertionPointToEnd(model.getBodyBlock());
    for (unsigned i : group.ports) {
      auto value = model.getBodyBlock()->getArgument(i);
      auto constant = b.create<ConstantOp>(model.getLoc(), cast<IntType>(value.getType()),
                                          llvm::APInt(i == 1 ? 8 : 1, 0));
      b.create<StrictConnectOp>(model.getLoc(), value, constant);
    }
  }
  auto anno = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firrtl.transforms.DontTouchAnnotation")),
      b.getNamedAttr("test.marker", b.getStringAttr("data"))});
  auto flagAnno = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("firrtl.transforms.DontTouchAnnotation")),
      b.getNamedAttr("test.marker", b.getStringAttr("flag"))});
  SmallVector<Attribute> portAnnos(top.getNumPorts(), b.getArrayAttr({}));
  auto keptAnno = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Kept"))});
  portAnnos[1] = b.getArrayAttr({keptAnno});
  SmallVector<NamedAttribute> explicitZero(anno.getValue());
  explicitZero.push_back(b.getNamedAttr("circt.fieldID", b.getI32IntegerAttr(0)));
  // Both root encodings must become the same payload field, without dropping
  // duplicate protections or custom members.
  if (withAnnotations) {
    portAnnos[2] = b.getArrayAttr({anno, b.getDictionaryAttr(explicitZero)});
    if (grouped) portAnnos[3] = b.getArrayAttr({flagAnno});
  }
  unsigned badPort = grouped ? 3 : 2; // Late leaf failures must also be atomic.
  if (rejection && rejection < 5) {
    SmallVector<NamedAttribute> members(anno.getValue());
    if (rejection == 1) members[0] = b.getNamedAttr("class", b.getStringAttr("test.Unknown"));
    if (rejection == 2) members.push_back(b.getNamedAttr("circt.fieldID", b.getI32IntegerAttr(1)));
    if (rejection == 3) members.push_back(b.getNamedAttr("circt.fieldID", b.getStringAttr("bad")));
    if (rejection == 4) members.push_back(b.getNamedAttr("target", b.getStringAttr("~Top|Top>g_data")));
    portAnnos[badPort] = b.getArrayAttr({b.getDictionaryAttr(members)});
  }
  top.setPortAnnotationsAttr(b.getArrayAttr(portAnnos));
  top.setPortSymbolsAttr(1, circt::hw::InnerSymAttr::get(b.getStringAttr("kept_id")));
  auto dataRef = circt::hw::InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("data_id"));
  auto flagRef = circt::hw::InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("flag_id"));
  auto setSymbol = [&](FModuleOp module, unsigned port, StringAttr name, unsigned fieldID,
                       StringRef visibility) {
    auto property = circt::hw::InnerSymPropertiesAttr::get(
        &context, name, fieldID, b.getStringAttr(visibility));
    module.setPortSymbolsAttr(port, circt::hw::InnerSymAttr::get(&context, {property}));
  };
  SmallVector<Attribute> stableRefs{instanceRef};
  if (withSymbols) {
    setSymbol(top, 2, dataRef.getName(), 0, "private");
    if (grouped) setSymbol(top, 3, flagRef.getName(), 0, "public");
    checkReference(circuit, top, dataRef, 2, 0, "private");
    if (grouped) checkReference(circuit, top, flagRef, 3, 0, "public");
    stableRefs.push_back(dataRef);
    if (grouped) stableRefs.push_back(flagRef);
  }
  // Equal leaf names in different modules are independent identities. Model
  // symbols preserve references without retaining consumed model DontTouches.
  auto modelDataRef = circt::hw::InnerRefAttr::get(b.getStringAttr("Model"), dataRef.getName());
  auto modelFlagRef = circt::hw::InnerRefAttr::get(b.getStringAttr("Model"), flagRef.getName());
  auto modelClockRef = circt::hw::InnerRefAttr::get(b.getStringAttr("Model"), b.getStringAttr("clock_id"));
  if (withModelSymbols) {
    setSymbol(model, 0, modelClockRef.getName(), 0, "public");
    setSymbol(model, 1, modelDataRef.getName(), 0, "public");
    if (grouped) setSymbol(model, 2, modelFlagRef.getName(), 0, "private");
    checkReference(circuit, model, modelClockRef, 0, 0, "public");
    checkReference(circuit, model, modelDataRef, 1, 0, "public");
    if (grouped) checkReference(circuit, model, modelFlagRef, 2, 0, "private");
    stableRefs.append({modelClockRef, modelDataRef});
    if (grouped) stableRefs.push_back(modelFlagRef);
  }
  circuit->setAttr("test.stable_refs", b.getArrayAttr(stableRefs));
  auto references = circuit->getAttr("test.stable_refs");
  if (rejection == 5)
    setSymbol(top, badPort, grouped ? flagRef.getName() : dataRef.getName(), 1, "public");
  auto u1 = UIntType::get(&context, 1);
  if (rejection == 6)
    setSymbol(top, badPort, dataRef.getName(), 0, "public");
  unsigned badModelPort = grouped ? 2 : 1;
  if (rejection == 7)
    setSymbol(model, badModelPort, grouped ? modelFlagRef.getName() : modelDataRef.getName(),
              1, "private");
  if (rejection == 8)
    setSymbol(model, badModelPort, modelDataRef.getName(), 0, "private");
  if (rejection == 9) {
    SmallVector<Attribute> modelAnnos(model.getNumPorts(), b.getArrayAttr({}));
    modelAnnos[badModelPort] = b.getArrayAttr({anno});
    model.setPortAnnotationsAttr(b.getArrayAttr(modelAnnos));
  }
  Type payload = UIntType::get(&context, 8);
  if (grouped) {
    SmallVector<BundleType::BundleElement> fields{
        {b.getStringAttr("data"), false, cast<FIRRTLBaseType>(payload)},
        {b.getStringAttr("flag"), false, u1}};
    // The payload layout can differ from model/top port visitation order.
    // Symbols must use named leaf IDs and be sorted by the resulting IDs.
    if (metadata == 2) std::swap(fields[0], fields[1]);
    payload = BundleType::get(&context, fields);
  }
  auto type = BundleType::get(&context, {
      {b.getStringAttr("ready"), true, u1},
      {b.getStringAttr("valid"), false, u1},
      {b.getStringAttr("bits"), false, cast<FIRRTLBaseType>(payload)}});
  goldengate::FAMETopChannelPort channel{&binding, "g_channel", type};
  std::string before = dump(*root), error;
  auto result = output ? goldengate::rewriteFAMEOutputChannel(hierarchy, channel, error)
                       : goldengate::rewriteFAMEInputChannel(hierarchy, channel, error);
  if (rejection) {
    require(failed(result) && !error.empty(), "unsupported annotation accepted");
    require(dump(*root) == before, "rejected annotation mutated IR");
    return;
  }
  require(succeeded(result) && succeeded(verify(*root)), "channel rewrite failed verification");
  auto annotations = AnnotationSet::forPort(top, portNamed(top, "g_channel"));
  auto payloadBundle = dyn_cast<BundleType>(payload);
  auto dataID = type.getFieldID(2) +
      (grouped ? payloadBundle.getFieldID(*payloadBundle.getElementIndex("data")) : 0);
  auto flagID = grouped ? type.getFieldID(2) +
      payloadBundle.getFieldID(*payloadBundle.getElementIndex("flag")) : 0;
  require(annotations.size() == (withAnnotations ? (grouped ? 3 : 2) : 0), "annotations lost");
  if (withSymbols) {
    auto port = portNamed(top, "g_channel");
    auto symbols = top.getPorts()[port].sym;
    require(symbols.size() == (grouped ? 2u : 1u), "symbols lost or duplicated");
    unsigned previousID = 0;
    for (auto property : symbols) {
      require(property.getFieldID() > previousID, "payload symbols not in field order");
      previousID = property.getFieldID();
    }
    checkReference(circuit, top, dataRef, port, dataID, "private");
    if (grouped) checkReference(circuit, top, flagRef, port, flagID, "public");
    require(circuit->getAttr("test.stable_refs") == references, "InnerRefs changed");
  }
  std::string modelChannelName = output ? "m__source" : "m__sink";
  if (withModelSymbols) {
    unsigned port = portNamed(model, modelChannelName);
    require(model.getPorts()[port].sym.size() == (grouped ? 2u : 1u),
            "model payload symbols lost or duplicated");
    unsigned previousID = 0;
    auto symbols = model.getPorts()[port].sym;
    for (auto property : symbols) {
      require(property.getFieldID() > previousID, "model payload symbols not in field order");
      previousID = property.getFieldID();
    }
    checkReference(circuit, model, modelDataRef, port, dataID, "public");
    if (grouped) checkReference(circuit, model, modelFlagRef, port, flagID, "private");
    checkReference(circuit, model, modelClockRef, portNamed(model, "clock"), 0, "public");
  }
  checkInstance();
  unsigned index = 0;
  for (auto annotation : annotations) {
    require(annotation.getFieldID() == (index < 2 ? dataID : flagID), "wrong payload field ID");
    require(annotation.getMember<StringAttr>("test.marker").getValue() ==
                (index < 2 ? "data" : "flag"), "annotation member lost");
    ++index;
  }
  // CIRCT distributes the aggregate annotations onto flattened ground ports.
  PassManager pm(&context);
  pm.addNestedPass<CircuitOp>(createLowerFIRRTLTypesPass());
  require(succeeded(pm.run(*root)), "LowerTypes failed");
  unsigned payloadPorts = 0, modelPayloadPorts = 0;
  for (auto module : circuit.getOps<FModuleOp>())
    for (unsigned p = 0; p < module.getNumPorts(); ++p) {
      auto attached = AnnotationSet::forPort(module, p);
      if (module != top) {
        require(attached.empty(), "wrapper protection leaked to model");
        auto name = module.getPortName(p);
        if (name == modelChannelName + "_bits" ||
            name == modelChannelName + "_bits_data" ||
            name == modelChannelName + "_bits_flag") {
          ++modelPayloadPorts;
          bool flag = name.ends_with("flag");
          if (withModelSymbols)
            checkReference(circuit, model, flag ? modelFlagRef : modelDataRef, p, 0,
                           flag ? "private" : "public");
          else require(!module.getPorts()[p].sym || module.getPorts()[p].sym.empty(),
                       "unexpected model payload symbol");
        } else if (name == "clock" && withModelSymbols)
          checkReference(circuit, model, modelClockRef, p, 0, "public");
        else require(!module.getPorts()[p].sym || module.getPorts()[p].sym.empty(),
                     "model identity leaked onto handshake");
        continue;
      }
      auto name = module.getPortName(p);
      if (name == "g_channel_bits" || name == "g_channel_bits_data" || name == "g_channel_bits_flag") {
        ++payloadPorts;
        bool flag = name.ends_with("flag");
        require(attached.size() == (withAnnotations ? (flag ? 1 : 2) : 0), "lowered payload annotation lost");
        if (withSymbols)
          checkReference(circuit, top, flag ? flagRef : dataRef, p, 0,
                         flag ? "public" : "private");
        else require(!module.getPorts()[p].sym || module.getPorts()[p].sym.empty(),
                     "unexpected payload symbol");
        for (auto annotation : attached)
          require(annotation.getFieldID() == 0 &&
                      annotation.isClass("firrtl.transforms.DontTouchAnnotation"), "lowered leaf identity wrong");
      } else if (name == "kept") {
        require(attached.getArrayAttr() == portAnnos[1] && module.getPorts()[p].sym.getSymName() == "kept_id",
                "unrelated metadata changed");
      } else require(attached.empty() && (!module.getPorts()[p].sym || module.getPorts()[p].sym.empty()),
                     "payload metadata leaked onto handshake");
    }
  checkInstance();
  require(modelPayloadPorts == (grouped ? 2u : 1u), "flattened model payload missing");
  require(payloadPorts == (grouped ? 2u : 1u), "flattened payload port missing");
  require(circuit->getAttr("test.stable_refs") == references, "LowerTypes changed InnerRefs");
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (bool output : {false, true})
      for (bool grouped : {false, true}) {
        for (unsigned metadata = 0; metadata < 3; ++metadata)
          for (bool modelSymbols : {false, true})
            run(context, output, grouped, 0, metadata, modelSymbols);
        for (unsigned rejection = 1; rejection < (grouped ? 7u : 6u); ++rejection)
          run(context, output, grouped, rejection, 1, true);
        for (unsigned rejection : {7u, 9u})
          run(context, output, grouped, rejection, 1, true);
        if (grouped) run(context, output, grouped, 8, 1, true);
      }
    llvm::outs() << "24 scalar/grouped input/output metadata transfers and LowerTypes checks passed; "
                    "stable InnerRefs and symbol visibility retained; "
                    "32 unsupported metadata plans rejected without mutation\n";
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
