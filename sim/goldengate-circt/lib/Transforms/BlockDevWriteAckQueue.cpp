// See LICENSE for license details.
// Requires: active uninstantiated GGBlockDevReadResponseQueueWrapper with
// scalar UInt<1> Decoupled acknowledgement dequeue, hostClock and queue reset.
// Consumes: acknowledgement dequeue boundary; no annotation classes removed.
// Transfers: circuit prefix and copied port targets; consumed targets stay on sim.
// Mutates: adds a 4-entry scalar tag FIFO and a host-enqueue wrapper.
// Analyses required: none. Hierarchy and port analyses are invalidated.
// Produces: ordinary FIRRTL registers and unreset async RAM, with no flow/pipe.
// Preserves: all other ports and qualified reset for the timing model.
// Oracle: BlockDevBridgeModule.scala wAckBuf Queue(UInt(tagBits.W), 4), nTrackers=1.
#include "goldengate/BlockDevWriteAckQueue.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addBlockDevWriteAckQueue(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGBlockDevWriteAckQueue4";
  constexpr llvm::StringLiteral wrapperName = "GGBlockDevWriteAckQueueWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevReadResponseQueueWrapper")
    return reject("BlockDev write-acknowledgement queue requires the active BlockDev read-response queue wrapper");
  FModuleOp inner;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (!m) continue;
    if (m.getModuleName() == queueName || m.getModuleName() == wrapperName)
      return reject("BlockDev write-acknowledgement queue or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("BlockDev write-acknowledgement queue needs a top module and retained annotations");
  auto *context = circuit.getContext();
  OpBuilder b(context);
  auto bit = UIntType::get(context, 1, false);
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, bit}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(context), Direction::In);
  auto input = port("blockdev_wack_deq", token, Direction::In);
  auto reset = port("blockdev_queue_reset", bit, Direction::Out);
  if (!clock || !input || !reset)
    return reject("BlockDev write-acknowledgement queue needs hostClock, Decoupled acknowledgement and queue reset");
  for (auto p : inner.getPorts())
    if (p.name == "blockdev_wack_enq")
      return reject("BlockDev write-acknowledgement queue boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("BlockDev write-acknowledgement queue needs an uninstantiated top");

  // Create hardware only after all boundary invariants have been checked.
  Location loc = circuit.getLoc();
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> queuePorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq"), token, Direction::In}, {b.getStringAttr("deq"), token, Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(queueName), ConventionAttr::get(context, Convention::Internal), queuePorts);
  b.setInsertionPointToStart(queue.getBodyBlock());
  auto arg = [&](unsigned i) { return queue.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  auto constant = [&](unsigned width, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, UIntType::get(context, width, false), APInt(width, n));
  };
  auto reg = [&](unsigned width, llvm::StringRef name) -> Value {
    return b.create<RegResetOp>(loc, UIntType::get(context, width, false), arg(0), arg(1), constant(width, 0), name).getResult();
  };
  auto mux = [&](Value c, Value yes, Value no) -> Value { return b.create<MuxPrimOp>(loc, c, yes, no); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  Value enqPtr = reg(2, "enq_ptr_value"), deqPtr = reg(2, "deq_ptr_value");
  Value maybeFull = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value full = both(equal, maybeFull), empty = both(equal, b.create<NotPrimOp>(loc, maybeFull));
  Value ready = b.create<NotPrimOp>(loc, full), valid = b.create<NotPrimOp>(loc, empty);
  Value push = both(ready, field(arg(2), "valid")), pop = both(valid, field(arg(3), "ready"));
  connect(field(arg(2), "ready"), ready); connect(field(arg(3), "valid"), valid);
  auto increment = [&](Value pointer) -> Value {
    return b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, pointer, constant(2, 1)), 1, 0);
  };
  connect(enqPtr, mux(push, increment(enqPtr), enqPtr));
  connect(deqPtr, mux(pop, increment(deqPtr), deqPtr));
  connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));

  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(4, bit, MemOp::PortKind::Read),
      MemOp::getTypeForPort(4, bit, MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 4,
      RUWAttr::Undefined, memoryNames, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), arg(0)); connect(field(reader, "en"), constant(1, 1));
  connect(field(reader, "addr"), deqPtr);
  connect(field(arg(3), "bits"), field(reader, "data"));
  connect(field(writer, "clk"), arg(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), constant(1, 1));
  connect(field(writer, "data"), field(arg(2), "bits"));
  // Reset flushes pointers and maybe_full, not RAM. A pre-edge accepted push
  // still writes during reset, just as the SFC BlockDev wAckBuf does.

  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (i != *input) {
      copied.push_back(i); ports.push_back(p);
    }
  unsigned hostInput = ports.size();
  ports.push_back({b.getStringAttr("blockdev_wack_enq"), token, Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto wAckBuf = b.create<InstanceOp>(loc, queue, "wAckBuf");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(wAckBuf.getResult(0), outer(*clock));
  connect(wAckBuf.getResult(1), sim.getResult(*reset));
  b.create<ConnectOp>(loc, sim.getResult(*input), wAckBuf.getResult(3));
  b.create<ConnectOp>(loc, wAckBuf.getResult(2), wrapper.getBodyBlock()->getArgument(hostInput));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue();
      if (value == oldPrefix) return b.getStringAttr(newPrefix);
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
