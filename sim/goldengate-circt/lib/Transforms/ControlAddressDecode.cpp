// See LICENSE for license details.
// Golden Gate widget regions are half-open byte ranges. Construct address
// comparisons and one-hot/encoded selections directly as CIRCT FIRRTL ops.
// The encoder's no-match value selects NastiRouter's stateful error endpoint.
#include "goldengate/ControlAddressDecode.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::allocateControlMMIORegions(unsigned addressBits,
    ArrayRef<ControlMMIOWidget> widgets,
    SmallVectorImpl<ControlMMIORegion> &regions, std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (!addressBits || addressBits > 63 || widgets.empty() || widgets.size() > 63)
    return reject("control allocation needs 1..63 address bits and 1..63 widgets");
  uint64_t limit = uint64_t(1) << addressBits;
  SmallVector<ControlMMIORegion> allocated;
  for (auto [i, widget] : llvm::enumerate(widgets)) {
    if (widget.name.empty()) return reject("control widget name must be nonempty");
    for (auto prior : widgets.take_front(i))
      if (prior.name == widget.name) return reject("control widget names must be unique");
    uint64_t size;
    if (widget.customSize) {
      // Scala customSize replaces the register-derived size; WidgetRegion
      // requires the supplied size to be a power of two.
      size = *widget.customSize;
      if (!llvm::isPowerOf2_64(size))
        return reject("control custom region size must be a positive power of two");
    } else {
      // Check before multiplying or rounding. The largest supported address
      // space is 2^63 bytes, whose last endpoint still fits uint64_t.
      if (!widget.registerCount || widget.registerCount > limit / 4)
        return reject("control register bank exceeds the address space or is empty");
      size = uint64_t(1) << llvm::Log2_64_Ceil(widget.registerCount * 4);
    }
    if (size > limit) return reject("control widget region exceeds the address space");
    allocated.push_back({widget.name, 0, size});
  }
  std::stable_sort(allocated.begin(), allocated.end(),
      [](const auto &a, const auto &b) { return a.size > b.size; });
  uint64_t start = 0;
  for (auto &region : allocated) {
    if (region.size > limit - start)
      return reject("control widgets collectively exceed the address space");
    region.start = start;
    start += region.size;
  }
  regions.assign(allocated.begin(), allocated.end());
  return success();
}

LogicalResult goldengate::addControlAddressDecode(CircuitOp circuit,
    unsigned addressBits, ArrayRef<ControlMMIORegion> regions, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGControlDecodeWrapper";
  constexpr llvm::StringLiteral helperName = "GGControlAddressDecode";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (!addressBits || addressBits > 63 || regions.empty() || regions.size() > 63)
    return reject("control decoder needs 1..63 address bits and 1..63 regions");
  uint64_t limit = uint64_t(1) << addressBits;
  for (auto [i, region] : llvm::enumerate(regions)) {
    if (region.name.empty() || !region.size || region.start >= limit || region.size > limit - region.start)
      return reject("control decoder region name or bounds are invalid");
    for (auto prior : regions.take_front(i)) {
      if (prior.name == region.name) return reject("control decoder region names must be unique");
      if (region.start < prior.start + prior.size && prior.start < region.start + region.size)
        return reject("control decoder regions overlap");
    }
  }
  if (circuit.getName() != "GGControlErrorWrapper")
    return reject("control decoder requires the control error wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == helperName)
      return reject("control decoder helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("control decoder needs a top and retained annotations");
  for (auto port : inner.getPorts())
    if (port.name.getValue().starts_with("ctrl_decode_")) return reject("control decoder boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("control decoder requires an uninstantiated top");

  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(context, width, false); };
  unsigned count = regions.size(), indexBits = std::max(1u, llvm::Log2_64_Ceil(count + 1));
  SmallVector<PortInfo> helperPorts;
  for (auto name : {"aw_addr", "ar_addr"})
    helperPorts.push_back({b.getStringAttr(name), uint(addressBits), Direction::In});
  for (auto name : {"aw_route", "ar_route"})
    helperPorts.push_back({b.getStringAttr(name), uint(count), Direction::Out});
  for (auto name : {"aw_target", "ar_target"})
    helperPorts.push_back({b.getStringAttr(name), uint(indexBits), Direction::Out});
  for (auto name : {"aw_error", "ar_error"})
    helperPorts.push_back({b.getStringAttr(name), uint(1), Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(context, Convention::Internal), helperPorts);
  SmallVector<Attribute> entries;
  for (auto [i, r] : llvm::enumerate(regions)) entries.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(r.name)),
      b.getNamedAttr("start", b.getI64IntegerAttr(r.start)),
      b.getNamedAttr("size", b.getI64IntegerAttr(r.size)),
      b.getNamedAttr("slave", b.getI32IntegerAttr(i))}));
  helper->setAttr("goldengate.controlRegions", b.getArrayAttr(entries));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned width, uint64_t v) -> Value {
    return b.create<ConstantOp>(loc, uint(width), APInt(width, v));
  };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  for (unsigned channel = 0; channel < 2; ++channel) {
    // Widen by one bit so a region ending exactly at 2^addressBits is legal.
    Value address = b.create<PadPrimOp>(loc, arg(channel), addressBits + 1);
    Value target = constant(indexBits, count), route;
    for (auto [i, r] : llvm::enumerate(regions)) {
      Value low = b.create<NotPrimOp>(loc, b.create<LTPrimOp>(loc, address, constant(addressBits + 1, r.start)));
      Value high = b.create<LTPrimOp>(loc, address, constant(addressBits + 1, r.start + r.size));
      Value selected = b.create<AndPrimOp>(loc, low, high);
      route = route ? Value(b.create<CatPrimOp>(loc, selected, route)) : selected;
      target = b.create<MuxPrimOp>(loc, selected, constant(indexBits, i), target);
    }
    connect(arg(2 + channel), route); connect(arg(4 + channel), target);
    connect(arg(6 + channel), b.create<EQPrimOp>(loc, route, constant(count, 0)));
  }
  SmallVector<PortInfo> ports(inner.getPorts()); unsigned first = ports.size();
  for (auto port : helperPorts) {
    port.name = b.getStringAttr("ctrl_decode_" + port.name.getValue().str()); ports.push_back(port);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto decoder = b.create<InstanceOp>(loc, helper, "controlDecode");
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
        port.direction == Direction::In ? external : sim.getResult(i));
  }
  for (unsigned i = 0; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(first + i);
    if (helperPorts[i].direction == Direction::In) connect(decoder.getResult(i), external);
    else connect(external, decoder.getResult(i));
  }
  // Retain classes/constructors and internal model identities; transfer copied
  // top ports while changing the circuit prefix for all retained targets.
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto port : inner.getPorts()) if (name == port.name.getValue()) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}
