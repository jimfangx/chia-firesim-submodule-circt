// See LICENSE for license details.
// BlockDevBridgeModule.scala: rRespBuf Queue(BlockDeviceData, 32).
// Requires: active uninstantiated GGBlockDevDataQueueWrapper, exact response
// dequeue input, hostClock, qualified reset, and the earlier 32x65 FIFO module.
// Consumes: response dequeue boundary; no annotation classes are removed.
// Transfers: circuit prefix and copied port targets; consumed targets stay on sim.
// Mutates: adds a wrapper and rRespBuf instance using the verified data FIFO.
// Requires no cached analyses; hierarchy/port analyses are invalidated.
// Produces: host response enqueue input connected through an ordinary FIRRTL
// FIFO to the token engine. Reset is preserved for the acknowledgement FIFO.
#include "goldengate/BlockDevReadResponseQueue.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addBlockDevReadResponseQueue(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral queueName = "GGBlockDevDataQueue32";
  constexpr llvm::StringLiteral wrapperName = "GGBlockDevReadResponseQueueWrapper";
  auto reject = [&](llvm::StringRef reason) { error = reason.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevDataQueueWrapper")
    return reject("BlockDev read-response queue requires the active BlockDev data queue wrapper");
  FModuleOp inner, queue;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto m = dyn_cast<FModuleLike>(&op);
    if (!m) continue;
    if (m.getModuleName() == wrapperName)
      return reject("BlockDev read-response wrapper already exists");
    if (m.getModuleName() == queueName) queue = dyn_cast<FModuleOp>(&op);
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(&op);
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("BlockDev read-response queue needs a top module and retained annotations");
  auto *context = circuit.getContext();
  OpBuilder b(context);
  auto bit = UIntType::get(context, 1, false), word = UIntType::get(context, 64, false);
  auto payload = BundleType::get(context, {{b.getStringAttr("tag"), false, bit},
      {b.getStringAttr("data"), false, word}});
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  auto port = [&](llvm::StringRef name, Type type, Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction) return i;
    return std::nullopt;
  };
  auto clock = port("hostClock", ClockType::get(context), Direction::In);
  auto input = port("blockdev_rresp_deq", token, Direction::In);
  auto reset = port("blockdev_queue_reset", bit, Direction::Out);
  if (!clock || !input || !reset)
    return reject("BlockDev read-response queue needs hostClock, Decoupled response dequeue and queue reset");
  for (auto p : inner.getPorts())
    if (p.name == "blockdev_rresp_enq")
      return reject("BlockDev read-response queue boundary already exists");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("BlockDev read-response queue needs an uninstantiated top");

  // Validate the helper before mutating the circuit. It was built and verified
  // by addBlockDevDataQueue; rRespBuf has identical Queue_2 semantics in SFC.
  if (!queue || queue.getNumPorts() != 4)
    return reject("BlockDev read-response queue needs the existing 32-entry data FIFO");
  const llvm::StringRef names[]{"clock", "reset", "enq", "deq"};
  const Type types[]{ClockType::get(context), bit, token, token};
  for (unsigned i = 0; i < 4; ++i)
    if (queue.getPortName(i) != names[i] || queue.getPortType(i) != types[i] ||
        queue.getPortDirection(i) != (i == 3 ? Direction::Out : Direction::In))
      return reject("BlockDev read-response FIFO has an incompatible port contract");
  auto memories = queue.getOps<MemOp>();
  if (std::distance(memories.begin(), memories.end()) != 1)
    return reject("BlockDev read-response FIFO needs one 32x65 memory");
  auto ram = *memories.begin();
  if (ram.getDepth() != 32 || ram.getDataType() != UIntType::get(context, 65, false) ||
      ram.getReadLatency() != 0 || ram.getWriteLatency() != 1 ||
      ram.getRuw() != RUWAttr::Undefined || ram.getNumResults() != 2 ||
      ram.getPortKind(size_t(0)) != MemOp::PortKind::Read ||
      ram.getPortKind(size_t(1)) != MemOp::PortKind::Write)
    return reject("BlockDev read-response FIFO has incompatible memory semantics");
  Location loc = circuit.getLoc();
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };

  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (i != *input) {
      copied.push_back(i); ports.push_back(p);
    }
  unsigned hostInput = ports.size();
  ports.push_back({b.getStringAttr("blockdev_rresp_enq"), token, Direction::In});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto rRespBuf = b.create<InstanceOp>(loc, queue, "rRespBuf");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(rRespBuf.getResult(0), outer(*clock));
  connect(rRespBuf.getResult(1), sim.getResult(*reset));
  b.create<ConnectOp>(loc, sim.getResult(*input), rRespBuf.getResult(3));
  b.create<ConnectOp>(loc, rRespBuf.getResult(2), wrapper.getBodyBlock()->getArgument(hostInput));

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
