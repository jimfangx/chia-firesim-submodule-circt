// See LICENSE for license details.
// CPUManagedStreamEngine's AW/W/B defaults for Rocket's empty sinkParams.
// Requires the uninstantiated CPU control wrapper and its retained annotations.
// No stream sinks are implemented here: the native pipeline currently supports
// the sole outgoing TracerV stream. Reject incoming-stream boundaries and name
// collisions before mutation. U250 has no AXI user bits; no zero-width user
// port is synthesized. AW/W payload fields unused by the oracle are omitted.
// Copies the existing boundary and retargets its ports, adding a stateless
// write boundary with the two host-clock, reset-gated protocol assertions.
#include "goldengate/TracerVTokenEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addEmptyCPUStreamWrite(CircuitOp circuit,
                                                std::string &error) {
  constexpr llvm::StringLiteral helperName = "GGEmptyCPUStreamWrite";
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamWriteWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGCPUStreamControlWrapper")
    return reject("empty CPU stream write requires the CPU control wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == helperName || m.getModuleName() == wrapperName)
      return reject("empty CPU stream write helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("empty CPU stream write needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context);
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  std::optional<unsigned> clock, reset, readID;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    auto n = p.name.getValue();
    if (n.starts_with("cpu_stream_aw_") || n.starts_with("cpu_stream_w_") ||
        n.starts_with("cpu_stream_b_") || n.contains("from_cpu_stream"))
      return reject("CPU write or incoming stream boundary already exists");
    if (n == "hostClock" && p.type == ClockType::get(context) && p.direction == Direction::In) clock = i;
    if (n == "hostReset" && p.type == bit && p.direction == Direction::In) reset = i;
    if (n == "cpu_stream_r_bits_id" && p.type == uint(16) && p.direction == Direction::Out) readID = i;
  }
  if (!clock || !reset || !readID)
    return reject("empty CPU stream write needs host clock/reset and the U250 CPU read boundary");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("empty CPU stream write needs an uninstantiated top");

  Location loc = circuit.getLoc();
  SmallVector<PortInfo> helperPorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  auto append = [&](llvm::StringRef n, unsigned w, Direction d) {
    helperPorts.push_back({b.getStringAttr(n), uint(w), d});
  };
  append("aw_ready", 1, Direction::Out); append("aw_valid", 1, Direction::In);
  append("aw_bits_id", 16, Direction::In); append("aw_bits_size", 3, Direction::In);
  append("w_ready", 1, Direction::Out); append("w_valid", 1, Direction::In);
  append("w_bits_strb", 64, Direction::In); append("b_valid", 1, Direction::Out);
  append("b_bits_id", 16, Direction::Out); append("b_bits_resp", 2, Direction::Out);
  // Retain the handshake input for the subsequent B buffer. The empty sink
  // list does not use it to enable a response.
  append("b_ready", 1, Direction::In);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(helperName),
      ConventionAttr::get(context, Convention::Internal), helperPorts);
  helper->setAttr("goldengate.fromHostCPUStreamCount", b.getI32IntegerAttr(0));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  Value zero = constant(1, 0);
  connect(arg(2), zero); connect(arg(6), zero); connect(arg(9), zero);
  // B ID is the *live* AW ID even when B valid is false and during reset.
  connect(arg(10), arg(4)); connect(arg(11), constant(2, 0));
  Value enabled = b.create<NotPrimOp>(loc, arg(1));
  auto assertValid = [&](unsigned valid, unsigned bits, Value expected, llvm::StringRef message) {
    Value permitted = b.create<OrPrimOp>(loc, b.create<NotPrimOp>(loc, arg(valid)),
        b.create<EQPrimOp>(loc, arg(bits), expected));
    b.create<AssertOp>(loc, arg(0), permitted, enabled, message, ValueRange{}, "");
  };
  assertValid(3, 5, constant(3, 6), "CPUManagedStreamEngine requires 64-byte write beats");
  assertValid(7, 8, constant(64, ~uint64_t(0)), "CPUManagedStreamEngine requires all write strobes");

  SmallVector<PortInfo> ports(inner.getPorts()); unsigned firstAXI = ports.size();
  for (auto p : ArrayRef<PortInfo>(helperPorts).drop_front(2)) {
    p.name = b.getStringAttr("cpu_stream_" + p.name.getValue().str()); ports.push_back(p);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto transport = b.create<InstanceOp>(loc, helper, "cpuStreamWrite");
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    copied.push_back(i); Value external = wrapper.getBodyBlock()->getArgument(i);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
        p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(transport.getResult(0), wrapper.getBodyBlock()->getArgument(*clock));
  connect(transport.getResult(1), wrapper.getBodyBlock()->getArgument(*reset));
  for (unsigned i = 2; i < helperPorts.size(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(firstAXI + i - 2);
    if (helperPorts[i].direction == Direction::In) connect(transport.getResult(i), external);
    else connect(external, transport.getResult(i));
  }
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
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
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
