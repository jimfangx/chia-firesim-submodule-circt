// See LICENSE for license details.
// FPGATop's default AXI4Buffer has a depth-two, non-flowing, non-pipelined AR
// queue. Golden AXI4Buffer_2 shares Queue_51's AW/AR payload shape. Instantiate
// the same native helper with independent state, retaining the AR until the
// inner CPUManagedStreamEngine accepts its final read beat. R/B buffering and
// the FPGA shell remain subsequent stages. All validation precedes mutation.
#include "goldengate/TracerVTokenEngine.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addCPUStreamReadBuffer(CircuitOp circuit,
                                               std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGCPUStreamReadBufferWrapper";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGCPUStreamWriteBufferWrapper")
    return reject("CPU AR buffer requires the active CPU AW/W buffer wrapper");
  FModuleOp inner, queue;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName)
      return reject("CPU AR buffer wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGCPUStreamAWQueue2") queue = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw || !queue)
    return reject("CPU AR buffer needs a top, retained annotations and native AW/AR queue");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); };
  auto bit = uint(1);
  auto payload = BundleType::get(context, {{b.getStringAttr("id"), false, uint(16)},
      {b.getStringAttr("addr"), false, uint(64)}, {b.getStringAttr("len"), false, uint(8)},
      {b.getStringAttr("size"), false, uint(3)}});
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  auto depth = queue->getAttrOfType<IntegerAttr>("goldengate.queueDepth");
  auto flow = queue->getAttrOfType<BoolAttr>("goldengate.queueFlow");
  auto pipe = queue->getAttrOfType<BoolAttr>("goldengate.queuePipe");
  if (!depth || depth.getInt() != 2 || !flow || flow.getValue() || !pipe || pipe.getValue() ||
      queue.getNumPorts() != 4)
    return reject("CPU AR buffer needs the default depth-two non-flowing queue");
  const llvm::StringRef queueNames[]{"clock", "reset", "enq", "deq"};
  const Type queueTypes[]{ClockType::get(context), bit, token, token};
  for (unsigned i = 0; i < 4; ++i)
    if (queue.getPortName(i) != queueNames[i] || queue.getPortType(i) != queueTypes[i] ||
        queue.getPortDirection(i) != (i == 3 ? Direction::Out : Direction::In))
      return reject("CPU AR buffer queue interface differs from U250");
  unsigned memoryCount = 0;
  for (auto mem : queue.getOps<MemOp>()) {
    ++memoryCount;
    if (mem.getDepth() != 2 || mem.getDataType() != payload || mem.getReadLatency() != 0 ||
        mem.getWriteLatency() != 1 || mem.getRuw() != RUWAttr::Undefined)
      return reject("CPU AR buffer queue storage differs from U250");
  }
  if (memoryCount != 1)
    return reject("CPU AR buffer queue needs one payload memory");
  const llvm::StringRef names[]{"hostClock", "hostReset", "cpu_stream_ar_ready", "cpu_stream_ar_valid",
      "cpu_stream_ar_bits_id", "cpu_stream_ar_bits_addr", "cpu_stream_ar_bits_len", "cpu_stream_ar_bits_size"};
  const unsigned widths[]{0, 1, 1, 1, 16, 64, 8, 3};
  unsigned indices[8];
  for (unsigned j = 0; j < 8; ++j) {
    std::optional<unsigned> found;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.direction == (j == 2 ? Direction::Out : Direction::In) &&
          p.type == (j == 0 ? Type(ClockType::get(context)) : Type(uint(widths[j])))) found = i;
    if (!found) return reject("CPU AR buffer needs the exact U250 clock/reset and AR interface");
    indices[j] = *found;
  }
  for (auto p : inner.getPorts())
    if (p.name.getValue().starts_with("cpu_stream_ar_") &&
        !llvm::is_contained(ArrayRef<llvm::StringRef>(names), p.name.getValue()))
      return reject("unsupported CPU AR field");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("CPU AR buffer needs an uninstantiated top");

  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), inner.getPorts());
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), ar = b.create<InstanceOp>(loc, queue, "arQueue");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices).drop_front(2), i)) {
      copied.push_back(i);
      b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : outer(i),
          p.direction == Direction::In ? outer(i) : sim.getResult(i));
    }
  connect(ar.getResult(0), outer(indices[0])); connect(ar.getResult(1), outer(indices[1]));
  connect(outer(indices[2]), field(ar.getResult(2), "ready"));
  connect(field(ar.getResult(2), "valid"), outer(indices[3]));
  connect(field(ar.getResult(3), "ready"), sim.getResult(indices[2]));
  connect(sim.getResult(indices[3]), field(ar.getResult(3), "valid"));
  const llvm::StringRef fields[]{"id", "addr", "len", "size"};
  for (unsigned i = 0; i < 4; ++i) {
    connect(field(field(ar.getResult(2), "bits"), fields[i]), outer(indices[i + 4]));
    connect(sim.getResult(indices[i + 4]), field(field(ar.getResult(3), "bits"), fields[i]));
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
