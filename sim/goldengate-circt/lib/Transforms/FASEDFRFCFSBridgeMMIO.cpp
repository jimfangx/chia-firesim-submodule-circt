// See LICENSE for license details.
// Oracle: FASEDMemoryTimingModel.scala attachIO(model.io.mmReg), functional
// register/error observation, Widget.genCRFile. FRFCFS exposes a distinct bank;
// the LatencyPipe latency/max-request fragments must never be attached here.
#include "goldengate/FASEDFRFCFSBridgeMMIO.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/FASEDFRFCFSMMIO.h"
#include "goldengate/FASEDRequestLimits.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include <functional>
#include <map>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::attachFASEDFRFCFSMMIO(CircuitOp circuit,
                                                FModuleOp bank,
                                                std::string &error) {
  auto reject = [&](StringRef why) {
    error = why.str();
    return failure();
  };
  bool selected =
      circuit.getName() == "GGSelectedPrintFASEDFRFCFSBridgeWrapper";
  StringRef output =
      selected ? "GGSelectedPrintFASEDMMIOWrapper" : "GGFASEDMMIOWrapper";
  if (!selected && circuit.getName() != "GGFASEDFRFCFSBridgeWrapper")
    return reject(
        "FRFCFS MMIO requires the active FRFCFS functional/timing bridge");
  auto base =
      circuit->getAttrOfType<IntegerAttr>("goldengate.fasedBaseCapacity");
  unsigned capacity, width;
  if (!base || base.getValue().getBitWidth() > 64 || base.getInt() != 32 ||
      failed(getFASEDRequestLimitProfile(circuit, capacity, width, error)))
    return reject(
        "FRFCFS MMIO requires the explicit base-32 functional profile");
  if (capacity != 10 && capacity != 20)
    return reject(
        "FRFCFS MMIO requires selected effective capacity ten or twenty");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == output)
      return reject("FASED MMIO wrapper already exists");
    if (m.getModuleName() == circuit.getName())
      inner = dyn_cast<FModuleOp>(m.getOperation());
    if (m.getModuleName() == "GGFASEDLatencyRegisters" ||
        m.getModuleName() == "GGFASEDRequestLimits")
      return reject("FRFCFS cannot contain LatencyPipe programming banks");
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw || !bank || bank->getParentOp() != circuit ||
      bank.getName() != "GGFASEDFRFCFSMMIO")
    return reject(
        "FRFCFS MMIO needs top, annotations and its materialized bank");
  if (failed(verify(bank)))
    return reject("FRFCFS MMIO bank is invalid");
  bool used = false;
  circuit.walk([&](InstanceOp i) {
    used |= i.getModuleName() == inner.getName() ||
            i.getModuleName() == bank.getName();
  });
  if (used)
    return reject("FRFCFS MMIO requires uninstantiated top and bank");
  ControlMMIOWidget descriptor;
  if (failed(deriveControlMMIOWidget(circuit,
                                     "FASEDMemoryTimingModel_0",
                                     bank.getName(),
                                     {bank.getName()},
                                     descriptor,
                                     error,
                                     Direction::Out)))
    return failure();
  if (descriptor.registerCount != 68)
    return reject("FRFCFS MMIO requires all 68 selected register words");

  auto rows = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  auto expected = getFASEDFRFCFSMMIORegisters();
  for (auto [i, row] : llvm::enumerate(rows)) {
    auto entry = cast<DictionaryAttr>(row);
    auto &reg = expected[entry.getAs<IntegerAttr>("offset").getInt() / 4];
    if (entry.getAs<StringAttr>("name").getValue() != reg.name ||
        !entry.getAs<BoolAttr>("readable").getValue() ||
        entry.getAs<BoolAttr>("writeable").getValue() != reg.writeable)
      return reject("FRFCFS MMIO register identity or permissions differ");
  }

  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned bits) { return UIntType::get(ctx, bits, false); };
  auto payload =
      [&](std::initializer_list<std::pair<StringRef, unsigned>> fields) {
        SmallVector<BundleType::BundleElement> result;
        for (auto [name, bits] : fields)
          result.push_back({b.getStringAttr(name), false, u(bits)});
        return BundleType::get(ctx, result);
      };
  auto token = [&](BundleType bits) {
    return BundleType::get(ctx,
                           {{b.getStringAttr("ready"), true, u(1)},
                            {b.getStringAttr("valid"), false, u(1)},
                            {b.getStringAttr("bits"), false, bits}});
  };
  auto rType = token(payload(
      {{"user", 1}, {"id", 4}, {"last", 1}, {"data", 64}, {"resp", 2}}));
  auto bType = token(payload({{"user", 1}, {"id", 4}, {"resp", 2}}));
  auto find = [&](StringRef name,
                  Type type,
                  Direction direction) -> std::optional<unsigned> {
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == name && p.type == type && p.direction == direction)
        return i;
    return std::nullopt;
  };
  auto clock = find("hostClock", ClockType::get(ctx), Direction::In);
  auto reset = find("hostReset", u(1), Direction::In);
  auto r = find("fased_host_read_response", rType, Direction::In);
  auto w = find("fased_host_write_response", bType, Direction::In);
  if (!clock || !reset || !r || !w)
    return reject(
        "FRFCFS MMIO needs host clock/reset and exact functional responses");
  std::map<unsigned, unsigned> routes;
  llvm::StringSet<> consumed;
  std::optional<unsigned> mcr;
  for (auto [i, p] : llvm::enumerate(bank.getPorts())) {
    StringRef name = p.getName();
    if (name == "mcr") {
      mcr = i;
      continue;
    }
    if (name == "clock" || name == "reset") {
      if (p.direction != Direction::In ||
          p.type != (name == "clock" ? Type(ClockType::get(ctx)) : Type(u(1))))
        return reject("FRFCFS bank host clock/reset types differ");
      continue;
    }
    if (name == "transactionQueueDepth") {
      // Scala retains this software register, but the selected scheduler's
      // elaboration-time FIFO depth does not use it (FRFCFSModel.scala:68).
      if (p.direction != Direction::Out || p.type != u(32))
        return reject("FRFCFS transaction depth register type differs");
      continue;
    }
    if (name == "rValid" || name == "rReady" || name == "bValid" ||
        name == "bReady" || name == "rResp" || name == "bResp") {
      if (p.direction != Direction::In ||
          p.type != u(name.ends_with("Resp") ? 2 : 1))
        return reject("FRFCFS host response observation types differ");
      continue;
    }
    std::string input = name == "relaxFunctionalModel" ? "fased_ingress_relaxed"
                        : name == "llc_peakMSHRsUsed"
                            ? "fased_frfcfs_llc_peakMSHRs"
                            : ("fased_frfcfs_" + name).str();
    auto direction =
        p.direction == Direction::In ? Direction::Out : Direction::In;
    auto index = find(input, p.type, direction);
    if (!index || !consumed.insert(input).second)
      return reject(
          ("FRFCFS bank port '" + name + "' does not match bridge").str());
    routes.emplace(i, *index);
  }
  if (!mcr)
    return reject("FRFCFS MMIO bank has no control port");
  for (auto p : inner.getPorts())
    if (p.name == "fasedBridge_mcr")
      return reject("FRFCFS aggregate MCR boundary already exists");

  SmallVector<PortInfo> ports;
  SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts()))
    if (!consumed.count(p.getName())) {
      copied.push_back(i);
      ports.push_back(p);
    }
  unsigned control = ports.size();
  ports.push_back({b.getStringAttr("fasedBridge_mcr"),
                   bank.getPortType(*mcr),
                   Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(
      loc, b.getStringAttr(output), inner.getConventionAttr(), ports);
  wrapper->setAttr("goldengate.mmioRegisters",
                   bank->getAttr("goldengate.mmioRegisters"));
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto regs = b.create<InstanceOp>(loc, bank, "registers");
  auto con = [&](Value dest, Value src) {
    b.create<ConnectOp>(loc, dest, src);
  };
  auto field = [&](Value value, StringRef name) -> Value {
    return b.create<SubfieldOp>(loc, value, name);
  };
  auto external = [&](unsigned index) -> Value {
    return wrapper.getArgument(llvm::find(copied, index) - copied.begin());
  };
  for (auto [i, old] : llvm::enumerate(copied)) {
    Value outer = wrapper.getArgument(i), nested = sim.getResult(old);
    con(inner.getPortDirection(old) == Direction::In ? nested : outer,
        inner.getPortDirection(old) == Direction::In ? outer : nested);
  }
  for (auto [i, p] : llvm::enumerate(bank.getPorts())) {
    StringRef name = p.getName();
    Value pin = regs.getResult(i);
    if (name == "clock")
      con(pin, external(*clock));
    else if (name == "reset")
      con(pin, external(*reset));
    else if (name == "mcr")
      con(wrapper.getArgument(control), pin);
    else if (name == "transactionQueueDepth")
      continue;
    else if (routes.count(i)) {
      Value bridge = sim.getResult(routes.at(i));
      con(p.direction == Direction::In ? pin : bridge,
          p.direction == Direction::In ? bridge : pin);
    } else {
      bool read = name.starts_with("r");
      auto response = name.ends_with("Ready") ? sim.getResult(read ? *r : *w)
                                              : external(read ? *r : *w);
      StringRef leaf = name.drop_front();
      if (leaf == "Resp")
        con(pin, field(field(response, "bits"), "resp"));
      else
        con(pin, field(response, leaf == "Valid" ? "valid" : "ready"));
    }
  }
  std::string oldPrefix = "~" + circuit.getName().str();
  std::string newPrefix = "~" + output.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute a) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      if (v == oldPrefix)
        return b.getStringAttr(newPrefix);
      if (!v.consume_front(oldPrefix + "|"))
        return a;
      std::string suffix = "|" + v.str();
      StringRef ref(suffix);
      if (ref.consume_front(modulePrefix))
        for (auto i : copied)
          if (ref.take_front(ref.find_first_of(".[")) == inner.getPortName(i)) {
            suffix = "|" + output.str() + ">" + ref.str();
            break;
          }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto xs = dyn_cast<ArrayAttr>(a)) {
      SmallVector<Attribute> values;
      for (auto x : xs)
        values.push_back(retarget(x));
      return b.getArrayAttr(values);
    }
    if (auto xs = dyn_cast<DictionaryAttr>(a)) {
      NamedAttrList values;
      for (auto x : xs)
        values.set(x.getName(), retarget(x.getValue()));
      return values.getDictionary(ctx);
    }
    return a;
  };
  circuit->setAttr("rawAnnotations", retarget(raw));
  circuit.setName(output);
  return success();
}
