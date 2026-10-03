// See LICENSE for license details.
// Oracle: FASEDMemoryTimingModel.scala:580-587 and MCRIO.bindReg.
// Required input invariants: uninstantiated functional-model wrapper, exact
// host clock/reset and recorded 64/4-bit host response bundles, ten flights.
// Annotations consumed: none.
// Annotations produced: none; all retained targets explicitly retargeted.
// IR mutations: two host-clock UInt<2> reset registers, read-only decoded
// MCR lanes at byte offsets 76/80 and a wrapper observing response handshakes.
// Analyses required: preceding FASED read/write response mapping; no cache.
// Analyses preserved: existing model/channel identity and constructor key.
// Output invariants: accepted errors capture the current R response code,
// including the SFC B-error capture behavior; zero-extended readback, host
// reset priority, assertions on writes outside reset, copied ports unchanged.
#include "goldengate/FASEDResponseErrors.h"
#include "mlir/IR/Builders.h"
#include <functional>
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDResponseErrors(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral bankName = "GGFASEDResponseErrors";
  constexpr llvm::StringLiteral wrapperName = "GGFASEDResponseErrorsWrapper";
  constexpr llvm::StringLiteral controlName = "fased_response_errors_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGFASEDFunctionalModelRegisterWrapper")
    return reject("FASED response errors require the active functional-model register wrapper");
  FModuleOp inner, engine;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == bankName || m.getName() == wrapperName)
      return reject("FASED response errors module or wrapper already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getName() == "GGFASEDTokenEngine") engine = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("FASED response errors need a top and retained annotations");
  auto key = engine ? engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") : DictionaryAttr();
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  auto flight = edge ? edge.getAs<IntegerAttr>("maxFlight") : IntegerAttr();
  if (!flight || flight.getInt() != 10)
    return reject("FASED response errors currently require the recorded ten-flight constructor");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = uint(1);
  auto token = [&](FIRRTLBaseType payload) {
    return BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
  };
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> elements;
    for (auto [name, width] : fields) elements.push_back({b.getStringAttr(name), false, uint(width)});
    return BundleType::get(ctx, elements);
  };
  auto readType = token(payload({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}}));
  auto writeType = token(payload({{"user",1},{"id",4},{"resp",2}}));
  std::optional<unsigned> clock, reset, readResponse, writeResponse;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    if (port.name == controlName) return reject("FASED response error MCR port already exists");
    if (port.name == "hostClock" && isa<ClockType>(port.type) && port.direction == Direction::In) clock = i;
    if (port.name == "hostReset" && port.type == bit && port.direction == Direction::In) reset = i;
    if (port.name == "fased_host_read_response" && port.type == readType && port.direction == Direction::In) readResponse = i;
    if (port.name == "fased_host_write_response" && port.type == writeType && port.direction == Direction::In) writeResponse = i;
  }
  if (!clock || !reset || !readResponse || !writeResponse)
    return reject("FASED response errors need host clock/reset and exact host response bundles");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("FASED response errors need an uninstantiated top");

  // Preflight is complete. Local lanes 0/1 map to global words 19/20.
  auto words = FVectorType::get(token(uint(32)), 2);
  auto mcr = BundleType::get(ctx, {{b.getStringAttr("read"), false, words},
      {b.getStringAttr("write"), true, words}, {b.getStringAttr("wstrb"), true, uint(4)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  for (auto name : {"rValid", "rReady", "bValid", "bReady"})
    bankPorts.push_back({b.getStringAttr(name), bit, Direction::In});
  for (auto name : {"rResp", "bResp"})
    bankPorts.push_back({b.getStringAttr(name), uint(2), Direction::In});
  bankPorts.push_back({b.getStringAttr("mcr"), mcr, Direction::Out});
  auto bank = b.create<FModuleOp>(loc, b.getStringAttr(bankName),
      ConventionAttr::get(ctx, Convention::Internal), bankPorts);
  SmallVector<Attribute> registers;
  for (auto [name, offset] : {std::pair<llvm::StringRef, unsigned>{"rrespError",76}, {"brespError",80}})
    registers.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(name)),
      b.getNamedAttr("offset", b.getI32IntegerAttr(offset)),
      b.getNamedAttr("readable", b.getBoolAttr(true)),
      b.getNamedAttr("writeable", b.getBoolAttr(false))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registers));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, field(arg(8), group), i);
  };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value one = b.create<ConstantOp>(loc, bit, APInt(1, 1));
  Value zero = b.create<ConstantOp>(loc, uint(2), APInt(2, 0));
  Value enabled = b.create<NotPrimOp>(loc, arg(1));
  for (unsigned i = 0; i < 2; ++i) {
    auto name = i ? "brespError" : "rrespError";
    Value reg = b.create<RegResetOp>(loc, uint(2), arg(0), arg(1), zero, name).getResult();
    Value fire = b.create<AndPrimOp>(loc, arg(2 + 2*i), arg(3 + 2*i));
    Value errorCode = b.create<NEQPrimOp>(loc, arg(6 + i), zero);
    Value capture = b.create<AndPrimOp>(loc, fire, errorCode);
    // Preserve the executable SFC oracle: B errors also capture R bits.resp.
    connect(reg, b.create<MuxPrimOp>(loc, capture, arg(6), reg));
    Value write = slot("write", i), read = slot("read", i);
    connect(field(read, "bits"), b.create<PadPrimOp>(loc, reg, 32));
    connect(field(read, "valid"), one); connect(field(write, "ready"), one);
    Value permitted = b.create<NotPrimOp>(loc, field(write, "valid"));
    b.create<AssertOp>(loc, arg(0), permitted, enabled,
        std::string("Register ") + name + " is read only", ValueRange{}, "");
  }

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, port] : llvm::enumerate(inner.getPorts())) {
    copied.push_back(i); ports.push_back(port);
  }
  ports.push_back({b.getStringAttr(controlName), mcr, Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim"), mmio = b.create<InstanceOp>(loc, bank, "responseErrors");
  auto outer = [&](unsigned i) { return wrapper.getBodyBlock()->getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto [j, i] : llvm::enumerate(copied)) {
    auto port = inner.getPorts()[i]; Value external = wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc, port.direction == Direction::In ? sim.getResult(i) : external,
                             port.direction == Direction::In ? external : sim.getResult(i));
  }
  connect(mmio.getResult(0), outer(*clock)); connect(mmio.getResult(1), outer(*reset));
  for (auto [response, valid, ready, resp] : {
      std::tuple<unsigned,unsigned,unsigned,unsigned>{*readResponse,2,3,6},
      {*writeResponse,4,5,7}}) {
    connect(mmio.getResult(valid), field(outer(response), "valid"));
    connect(mmio.getResult(ready), field(sim.getResult(response), "ready"));
    connect(mmio.getResult(resp), field(field(outer(response), "bits"), "resp"));
  }
  b.create<ConnectOp>(loc, wrapper.getBodyBlock()->getArguments().back(), mmio.getResult(8));

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
