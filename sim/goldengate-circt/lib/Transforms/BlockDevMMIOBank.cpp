// See LICENSE for license details.
// Materialization requires a free bank symbol; preserves circuit identity, top
// ports and annotations. Attachment requires the active uninstantiated
// GGBlockDevWriteAckQueueWrapper, retained annotations, exact one-tracker queue,
// host, geometry and timing boundaries and the uninstantiated ten-port bank.
// All attachment preflight precedes mutation; combined API rejects atomically.
// Consumes: four functional queue ports and geometry; annotation classes unchanged.
// Transfers: circuit/copied-port targets; consumed targets remain on inner sim.
// Produces: 26-word MMIO name/offset/permission metadata and ordinary FIRRTL ops.
// Mutates: add bank and active wrapper; invalidate hierarchy and port analyses.
// Analyses required: none. Preserves: inner hardware, timing/qualified-reset ports.
// Output: six host-reset registers, twenty unreset registers, write precedence
// over samples/pulse clearing; no wstrb or target-token qualification of writes.
// Latency outputs feed a later timing pass; decoded MCR transport is separate.
// Oracle: BlockDevBridgeModule.scala, Widget.gen{RO,WO}Reg and MCRIO.bindReg.
#include "goldengate/BlockDevMMIOBank.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

namespace {
constexpr llvm::StringLiteral bankName = "GGBlockDevMMIOBank";
constexpr llvm::StringLiteral wrapperName = "GGBlockDevMMIOWrapper";
constexpr llvm::StringLiteral controlName = "blockdevBridge_mcr";
SmallVector<PortInfo> blockDevBankPorts(MLIRContext *ctx) {
  OpBuilder b(ctx);
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  auto token = [&](FIRRTLBaseType payload) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  auto request = BundleType::get(ctx, {{b.getStringAttr("tag"), false, bit},
      {b.getStringAttr("len"), false, uint(32)}, {b.getStringAttr("offset"), false, uint(32)},
      {b.getStringAttr("write"), false, bit}});
  auto data = BundleType::get(ctx, {{b.getStringAttr("tag"), false, bit},
      {b.getStringAttr("data"), false, uint(64)}});
  auto status = BundleType::get(ctx, {{b.getStringAttr("wAckStallN"), false, bit},
      {b.getStringAttr("rRespStallN"), false, bit}});
  auto info = BundleType::get(ctx, {{b.getStringAttr("nsectors"), false, uint(32)},
      {b.getStringAttr("max_req_len"), false, uint(32)}});
  auto latency = BundleType::get(ctx, {{b.getStringAttr("read_latency"), false, uint(24)},
      {b.getStringAttr("write_latency"), false, uint(24)}});
  auto words = FVectorType::get(token(uint(32)), 26);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  return {{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("reqBuf"), token(request), Direction::In},
      {b.getStringAttr("dataBuf"), token(data), Direction::In},
      {b.getStringAttr("rRespBuf"), token(data), Direction::Out},
      {b.getStringAttr("wAckBuf"), token(bit), Direction::Out},
      {b.getStringAttr("timing"), status, Direction::In},
      {b.getStringAttr("info"), info, Direction::Out},
      {b.getStringAttr("latency"), latency, Direction::Out},
      {b.getStringAttr("mcr"), mcr, Direction::Out}};
}
LogicalResult attachmentTop(CircuitOp circuit, FModuleOp &inner,
    unsigned (&indices)[8], std::string &error) {
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGBlockDevWriteAckQueueWrapper")
    return reject("BlockDev MMIO requires the active BlockDev write-ack queue wrapper");
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == wrapperName) return reject("BlockDev MMIO wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  if (!inner || !circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("BlockDev MMIO needs a top and retained annotations");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto ports = blockDevBankPorts(ctx); auto bit = cast<FIRRTLBaseType>(ports[1].type);
  auto timing = BundleType::get(ctx, {{b.getStringAttr("returnWrite"), false, bit},
      {b.getStringAttr("readRespBusy"), false, bit}, {b.getStringAttr("wAckStallN"), true, bit},
      {b.getStringAttr("rRespStallN"), true, bit},
      {b.getStringAttr("tCycle"), true, UIntType::get(ctx, 24, false)}});
  const llvm::StringRef required[]{"hostClock", "hostReset", "blockdev_req_deq", "blockdev_data_deq",
      "blockdev_rresp_enq", "blockdev_wack_enq", "blockdev_timing", "blockdev_info"};
  const Type types[]{ports[0].type, bit, ports[2].type, ports[3].type, ports[4].type, ports[5].type, timing, ports[7].type};
  const Direction directions[]{Direction::In, Direction::In, Direction::Out, Direction::Out,
      Direction::In, Direction::In, Direction::In, Direction::In};
  for (auto p : inner.getPorts())
    if (p.name == controlName || p.name == "blockdev_latency")
      return reject("BlockDev decoded MCR or latency port already exists");
  for (unsigned j = 0; j < 8; ++j) {
    std::optional<unsigned> index;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == required[j] && p.type == types[j] && p.direction == directions[j]) index = i;
    if (!index) return reject("BlockDev MMIO needs exact host controls, queue, timing and geometry ports");
    indices[j] = *index;
  }
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("BlockDev MMIO needs an uninstantiated top");

  return success();
}
} // namespace

LogicalResult goldengate::materializeBlockDevMMIOBank(CircuitOp circuit,
    FModuleOp &result, std::string &error) {
  for (auto module : circuit.getOps<FModuleLike>())
    if (module.getName() == bankName) {
      error = "BlockDev MMIO bank module already exists";
      return failure();
    }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1); auto bankPorts = blockDevBankPorts(ctx);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  const llvm::StringRef names[]{"read_latency", "write_latency", "bdev_nsectors", "bdev_max_req_len",
      "bdev_req_valid", "bdev_req_write", "bdev_req_offset", "bdev_req_len", "bdev_req_tag", "bdev_req_ready",
      "bdev_data_valid", "bdev_data_data_upper", "bdev_data_data_lower", "bdev_data_tag", "bdev_data_ready",
      "bdev_rresp_data_upper", "bdev_rresp_data_lower", "bdev_rresp_tag", "bdev_rresp_valid", "bdev_rresp_ready",
      "bdev_wack_tag", "bdev_wack_valid", "bdev_wack_ready", "bdev_reqs_pending", "bdev_wack_stalled", "bdev_rresp_stalled"};
  const unsigned widths[]{24, 24, 32, 32, 1, 1, 32, 32, 1, 1, 1, 32, 32, 1, 1, 32, 32, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  SmallVector<Attribute> registers;
  for (unsigned i = 0; i < 26; ++i)
    registers.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr(names[i])),
        b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)),
        b.getNamedAttr("readable", b.getBoolAttr(i != 2 && i != 3)), b.getNamedAttr("writeable", b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(9), group), i);
  };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value zero = b.create<ConstantOp>(loc, bit, APInt(1, 0));
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value enabled = b.create<NotPrimOp>(loc, arg(1));
  auto path = [&](Value v, llvm::StringRef a, llvm::StringRef c) { return field(field(v, a), c); };
  SmallVector<Value> values;
  for (unsigned i = 0; i < 26; ++i) {
    bool pulse = i == 9 || i == 14 || i == 18 || i == 21;
    auto name = i == 2 ? llvm::StringRef("nsectorReg") : i == 3 ? llvm::StringRef("max_req_lenReg") : names[i];
    Value initial = b.create<ConstantOp>(loc, uint(widths[i]), APInt(widths[i], i < 2 ? 256 : 0));
    Value reg = (i < 2 || pulse)
        ? b.create<RegResetOp>(loc, uint(widths[i]), arg(0), arg(1), initial, name).getResult()
        : b.create<RegOp>(loc, uint(widths[i]), arg(0), name).getResult();
    values.push_back(reg);
    Value sample = pulse ? zero : reg;
    switch (i) {
    case 4: sample = field(arg(2), "valid"); break;
    case 5: sample = path(arg(2), "bits", "write"); break;
    case 6: sample = path(arg(2), "bits", "offset"); break;
    case 7: sample = path(arg(2), "bits", "len"); break;
    case 8: sample = path(arg(2), "bits", "tag"); break;
    case 10: sample = field(arg(3), "valid"); break;
    case 11: sample = b.create<BitsPrimOp>(loc, path(arg(3), "bits", "data"), 63, 32); break;
    case 12: sample = b.create<BitsPrimOp>(loc, path(arg(3), "bits", "data"), 31, 0); break;
    case 13: sample = path(arg(3), "bits", "tag"); break;
    case 19: sample = field(arg(4), "ready"); break;
    case 22: sample = field(arg(5), "ready"); break;
    case 23: sample = b.create<OrPrimOp>(loc, field(arg(2), "valid"), field(arg(3), "valid")); break;
    case 24: sample = b.create<NotPrimOp>(loc, field(arg(6), "wAckStallN")); break;
    case 25: sample = b.create<NotPrimOp>(loc, field(arg(6), "rRespStallN")); break;
    default: break;
    }
    Value write = slot("write", i), read = slot("read", i);
    Value data = b.create<BitsPrimOp>(loc, field(write, "bits"), widths[i] - 1, 0);
    // MCR bindReg writes override RO samples and pulses. Byte strobes do not
    // participate, and host reset dominates only the six RegInit registers.
    connect(reg, b.create<MuxPrimOp>(loc, field(write, "valid"), data, sample));
    connect(field(read, "bits"), b.create<PadPrimOp>(loc, i == 2 || i == 3 ? zero : reg, 32));
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
    if (i == 2 || i == 3)
      b.create<AssertOp>(loc, arg(0), b.create<NotPrimOp>(loc, field(read, "ready")), enabled,
          "Register is write only", ValueRange{}, "");
  }
  connect(field(arg(2), "ready"), values[9]); connect(field(arg(3), "ready"), values[14]);
  connect(path(arg(4), "bits", "data"), b.create<CatPrimOp>(loc, values[15], values[16]));
  connect(path(arg(4), "bits", "tag"), values[17]); connect(field(arg(4), "valid"), values[18]);
  connect(field(arg(5), "bits"), values[20]); connect(field(arg(5), "valid"), values[21]);
  connect(field(arg(7), "nsectors"), values[2]); connect(field(arg(7), "max_req_len"), values[3]);
  connect(field(arg(8), "read_latency"), values[0]); connect(field(arg(8), "write_latency"), values[1]);

  result = bank;
  return success();
}

LogicalResult goldengate::attachBlockDevMMIOBank(CircuitOp circuit,
    FModuleOp bank, std::string &error) {
  FModuleOp inner; unsigned indices[8];
  if (failed(attachmentTop(circuit, inner, indices, error))) return failure();
  auto reject = [&](llvm::StringRef why) { error = why.str(); return failure(); };
  if (!bank || bank->getParentOp() != circuit.getOperation() || bank.getName() != bankName)
    return reject("BlockDev MMIO attachment requires its materialized bank in this circuit");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto expected = blockDevBankPorts(ctx); auto actual = bank.getPorts();
  if (actual.size() != expected.size()) return reject("BlockDev MMIO bank needs exactly ten ports");
  for (auto [i, port] : llvm::enumerate(actual))
    if (port.name != expected[i].name || port.type != expected[i].type ||
        port.direction != expected[i].direction)
      return reject("BlockDev MMIO bank ports differ from the queue/status/geometry/latency/26-word MCR boundary");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == bank.getName(); });
  if (used) return reject("BlockDev MMIO attachment requires an uninstantiated bank");
  auto latency = expected[8].type, mcr = expected[9].type;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  SmallVector<unsigned> consumed{indices[2], indices[3], indices[4], indices[5], indices[7]};
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!llvm::is_contained(consumed, i)) {
    copied.push_back(i); ports.push_back(p);
  }
  unsigned latencyPort = ports.size();
  ports.push_back({b.getStringAttr("blockdev_latency"), latency, Direction::Out});
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "blockdevRegisters");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto p = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
                             p.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(indices[0])); connect(mmio.getResult(1), outer(indices[1]));
  for (unsigned j : {2u, 3u}) b.create<ConnectOp>(loc, mmio.getResult(j), sim.getResult(indices[j]));
  for (unsigned j : {4u, 5u, 7u}) b.create<ConnectOp>(loc, sim.getResult(indices[j]), mmio.getResult(j));
  for (auto n : {"wAckStallN", "rRespStallN"})
    connect(field(mmio.getResult(6), n), field(sim.getResult(indices[6]), n));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArgument(latencyPort), mmio.getResult(8));
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(9));

  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      if (v == oldPrefix) return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|")) return a;
      std::string suffix = "|" + v.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto local = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (local == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) { SmallVector<Attribute> vs; for (auto v : arr) vs.push_back(retarget(v)); return b.getArrayAttr(vs); }
    if (auto d = dyn_cast<DictionaryAttr>(a)) { NamedAttrList vs; for (auto v : d) vs.set(v.getName(), retarget(v.getValue())); return vs.getDictionary(ctx); }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw)); circuit.setName(wrapperName);
  return success();
}

LogicalResult goldengate::addBlockDevMMIOBank(CircuitOp circuit, std::string &error) {
  FModuleOp inner, bank; unsigned indices[8];
  // Validate the top before constructing hardware for atomic combined rejection.
  if (failed(attachmentTop(circuit, inner, indices, error)) ||
      failed(materializeBlockDevMMIOBank(circuit, bank, error))) return failure();
  return attachBlockDevMMIOBank(circuit, bank, error);
}
