// See LICENSE for license details.
#include "goldengate/CPUStreamCountBank.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/TracerVTokenEngine.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream stream(text); op->print(stream); return text;
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned wide = 4,
                             llvm::StringRef top = "CountFixture") {
  std::string text = "module { firrtl.circuit \"" + top.str() + "\" { firrtl.module @" + top.str() +
      "(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
      "in %countAInput: !firrtl.uint<13>, in %countBInput: !firrtl.uint<" + std::to_string(wide) +
      ">, in %payload: !firrtl.uint<8>, out %other: !firrtl.uint<8>, "
      "out %countA: !firrtl.uint<13>, out %countB: !firrtl.uint<" + std::to_string(wide) + ">) {} } }";
  auto root = parseSourceString<ModuleOp>(text, &ctx);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin(); auto inner = named(circuit, top);
  OpBuilder b(inner.getBodyBlock(), inner.getBodyBlock()->end());
  b.create<StrictConnectOp>(inner.getLoc(), inner.getArgument(5), inner.getArgument(4));
  b.create<StrictConnectOp>(inner.getLoc(), inner.getArgument(6), inner.getArgument(2));
  b.create<StrictConnectOp>(inner.getLoc(), inner.getArgument(7), inner.getArgument(3));
  SmallVector<Attribute> annos;
  for (auto target : {"~" + top.str(), "~" + top.str() + "|" + top.str() + ">other",
       "~" + top.str() + "|" + top.str() + ">countA", "~" + top.str() + "|" + top.str() + ">countB",
       "~" + top.str() + "|" + top.str() + ">payload[0]", "~" + top.str() + "|" + top.str() + "/leaf:Leaf>foo",
       std::string("~Foreign|Foreign>countA")})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.CountAnnotation")),
                                        b.getNamedAttr("target", b.getStringAttr(target))}));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("nested", b.getArrayAttr({
      b.getDictionaryAttr({b.getNamedAttr("target", b.getStringAttr("~" + top.str() + "|" + top.str() + ">hostReset"))}),
      b.getI32IntegerAttr(37)}))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
// Evaluate the bank's emitted FIRRTL SSA, rather than a copy of its register map.
struct Interpreter {
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> values;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto i = v.getDefiningOp<SubindexOp>()) return key(i.getInput()) + "[" + std::to_string(i.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp bank) {
    for (auto c : bank.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple bank drivers");
  }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (values.count(k)) return values.at(k);
    if (drivers.count(k)) return eval(drivers.at(k));
    auto *op = v.getDefiningOp();
    if (auto c = dyn_cast_or_null<ConstantOp>(op)) return c.getValue().getZExtValue();
    if (isa_and_nonnull<PadPrimOp>(op)) return eval(op->getOperand(0));
    if (isa_and_nonnull<NotPrimOp>(op)) return !eval(op->getOperand(0));
    throw std::runtime_error("unsupported count-bank operation or missing input");
  }
};
void check(CircuitOp circuit, ArrayRef<goldengate::CPUStreamCountPort> specs) {
  auto bank = named(circuit, "GGCPUStreamCountBank");
  auto wrapper = named(circuit, "GGCPUStreamCountWrapper");
  auto inner = named(circuit, "CountFixture");
  require(circuit.getName() == wrapper.getName(), "wrapper is not active top");
  require(wrapper.getNumPorts() == 7 && wrapper.getPortName(6) == "cpuStream_mcr", "count outputs were not consumed");
  require(inner.getNumPorts() == 8, "original interface was mutated");
  for (unsigned i = 0; i < 6; ++i)
    require(wrapper.getPortName(i) == inner.getPortName(i) && wrapper.getPortType(i) == inner.getPortType(i) &&
        wrapper.getPortDirection(i) == inner.getPortDirection(i), "copied port changed");
  require(bank.getNumPorts() == specs.size() + 3, "wrong bank port count");
  auto mcr = cast<BundleType>(bank.getPorts().back().type);
  require(mcr.getElements()[0].name == "read" && mcr.getElements()[1].name == "write" &&
      !mcr.getElements()[0].isFlip && mcr.getElements()[1].isFlip &&
      cast<FVectorType>(mcr.getElements()[0].type).getNumElements() == specs.size(), "wrong MCR vector/order");
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(registers.size() == specs.size(), "missing ordered count register metadata");
  for (auto [i, spec] : llvm::enumerate(specs)) {
    auto reg = cast<DictionaryAttr>(registers[i]);
    require(reg.getAs<StringAttr>("name").getValue() == spec.streamName + "_count" &&
        reg.getAs<IntegerAttr>("offset").getInt() == 4 * i &&
        reg.getAs<BoolAttr>("readable").getValue() && !reg.getAs<BoolAttr>("writeable").getValue(), "wrong count collateral");
    require(bank.getPortName(i + 2) == (specs.size() == 1 ? "count" : "count_" + std::to_string(i)) &&
        cast<UIntType>(bank.getPortType(i + 2)).getWidth() == spec.countBits, "wrong count port width/name");
  }
  auto annos = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  const char *expected[]{"~GGCPUStreamCountWrapper", "~GGCPUStreamCountWrapper|GGCPUStreamCountWrapper>other",
      "~GGCPUStreamCountWrapper|CountFixture>countA", "~GGCPUStreamCountWrapper|CountFixture>countB",
      "~GGCPUStreamCountWrapper|GGCPUStreamCountWrapper>payload[0]",
      "~GGCPUStreamCountWrapper|CountFixture/leaf:Leaf>foo", "~Foreign|Foreign>countA"};
  for (unsigned i = 0; i < 7; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue() == expected[i], "target retargeting changed identity");
  auto nested = cast<DictionaryAttr>(annos[7]).getAs<ArrayAttr>("nested");
  require(cast<DictionaryAttr>(nested[0]).getAs<StringAttr>("target").getValue() ==
      "~GGCPUStreamCountWrapper|GGCPUStreamCountWrapper>hostReset" && cast<IntegerAttr>(nested[1]).getInt() == 37,
      "nested annotations were not preserved/retargeted");
  InstanceOp sim, mmio;
  for (auto inst : wrapper.getOps<InstanceOp>()) {
    if (inst.getName() == "sim") sim = inst;
    if (inst.getName() == "streamCount") mmio = inst;
  }
  require(sim && mmio, "missing wrapper instances");
  llvm::DenseMap<Value, Value> connections;
  for (auto op : wrapper.getOps<ConnectOp>()) connections[op.getDest()] = op.getSrc();
  for (auto op : wrapper.getOps<StrictConnectOp>()) connections[op.getDest()] = op.getSrc();
  require(connections.lookup(mmio.getResult(0)) == wrapper.getArgument(0) &&
      connections.lookup(mmio.getResult(1)) == wrapper.getArgument(1) &&
      connections.lookup(wrapper.getArgument(6)) == mmio.getResults().back(), "clock/reset/MCR wiring incorrect");
  for (auto [i, spec] : llvm::enumerate(specs))
    require(connections.lookup(mmio.getResult(i + 2)) == sim.getResult(spec.portName == "countA" ? 6 : 7), "count ordering follows source ports instead of supplied streams");

  Interpreter interp(bank); std::mt19937_64 rng(0x1251);
  require(std::distance(bank.getOps<AssertOp>().begin(), bank.getOps<AssertOp>().end()) == specs.size(), "wrong read-only assertion count");
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    interp.values.clear(); interp.values[interp.key(bank.getArgument(1))] = cycle % 3 == 0;
    auto root = interp.key(bank.getArguments().back());
    for (auto [i, spec] : llvm::enumerate(specs)) {
      uint64_t mask = (uint64_t(1) << spec.countBits) - 1;
      uint64_t count = cycle < 3 ? (cycle == 0 ? 0 : cycle == 1 ? mask : uint64_t(1) << (spec.countBits - 1)) : rng() & mask;
      interp.values[interp.key(bank.getArgument(i + 2))] = count;
      auto read = root + ".read[" + std::to_string(i) + "]", write = root + ".write[" + std::to_string(i) + "]";
      interp.values[read + ".ready"] = cycle & 1; // Reads must remain live when stalled.
      interp.values[write + ".valid"] = (cycle >> i) & 1;
      require(interp.eval(interp.drivers.at(read + ".bits")) == count &&
          interp.eval(interp.drivers.at(read + ".valid")) == 1 && interp.eval(interp.drivers.at(write + ".ready")) == 1,
          "live count/padding or permanent handshake differs");
    }
    unsigned i = 0;
    for (auto assertion : bank.getOps<AssertOp>()) {
      require(assertion.getClock() == bank.getArgument(0) &&
          interp.eval(assertion.getEnable()) == (cycle % 3 != 0) &&
          interp.eval(assertion.getPredicate()) == !((cycle >> i) & 1), "read-only write assertion/reset gating changed"); ++i;
    }
  }
}
void rejected(MLIRContext &ctx, ArrayRef<goldengate::CPUStreamCountPort> specs,
              std::function<void(CircuitOp)> mutate = {}) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  if (mutate) mutate(c);
  auto before = dump(*root); std::string error;
  require(failed(goldengate::addCPUStreamCountBank(c, specs, error)) && !error.empty(), "invalid count binding accepted");
  require(dump(*root) == before, "rejected count binding mutated circuit or annotations");
}
void invalid(MLIRContext &ctx) {
  using Spec = goldengate::CPUStreamCountPort;
  const SmallVector<Spec> valid{{"a", "countA", 13}, {"b", "countB", 4}};
  rejected(ctx, {});
  for (SmallVector<Spec> specs : {SmallVector<Spec>{{"", "countA", 13}}, {{"a", "", 13}},
       {{"a", "missing", 13}}, {{"a", "countA", 0}}, {{"a", "countA", 33}}, {{"a", "countA", 12}},
       {{"a", "countA", 13}, {"a", "countB", 4}}, {{"a", "countA", 13}, {"b", "countA", 13}}}) rejected(ctx, specs);
  rejected(ctx, valid, [](CircuitOp c) { c->removeAttr("rawAnnotations"); });
  rejected(ctx, valid, [](CircuitOp c) { c->setAttr("rawAnnotations", StringAttr::get(c.getContext(), "bad")); });
  rejected(ctx, valid, [](CircuitOp c) { c.setName("Missing"); });
  for (auto name : {"GGCPUStreamCountBank", "GGCPUStreamCountWrapper"})
    rejected(ctx, valid, [=](CircuitOp c) { OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->end());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(name), ConventionAttr::get(c.getContext(), Convention::Internal), ArrayRef<PortInfo>{}); });
  for (unsigned index : {0U, 1U, 6U, 7U})
    rejected(ctx, valid, [=](CircuitOp c) { auto m = named(c, "CountFixture");
      m.getBodyBlock()->getArgument(index).setType(SIntType::get(c.getContext(), index == 0 || index == 1 ? 1 : index == 6 ? 13 : 4, false));
      auto types = llvm::to_vector(m.getPortTypes()); types[index] = TypeAttr::get(m.getArgument(index).getType());
      m->setAttr("portTypes", ArrayAttr::get(c.getContext(), types)); });
  for (unsigned index : {0U, 1U, 6U, 7U})
    rejected(ctx, valid, [=](CircuitOp c) { auto m = named(c, "CountFixture");
      SmallVector<bool> directions(m.getPortDirections().begin(), m.getPortDirections().end());
      directions[index] = !directions[index]; m.setPortDirections(directions); });
  rejected(ctx, valid, [](CircuitOp c) { auto m = named(c, "CountFixture");
    auto names = llvm::to_vector(m.getPortNames()); names[5] = StringAttr::get(c.getContext(), "cpuStream_mcr");
    m->setAttr("portNames", ArrayAttr::get(c.getContext(), names)); });
  rejected(ctx, valid, [](CircuitOp c) { OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->end());
    auto owner = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Owner"), ConventionAttr::get(c.getContext(), Convention::Internal), ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(owner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), named(c, "CountFixture"), "topInstance"); });
}
void mapControl(CircuitOp c, ModuleOp root) {
  std::string error;
  require(succeeded(goldengate::mapCPUStreamControl(c, 25, 12, error)), error);
  require(succeeded(verify(root)), "multi-stream MCR control verification failed");
  auto adapter = named(c, "GGCPUStreamMCRFile");
  bool bank = false; unsigned indices = 0;
  for (auto p : adapter.getPorts()) if (p.name == "mcr") {
    auto mcr = cast<BundleType>(p.type);
    require(cast<FVectorType>(mcr.getElement("read")->type).getNumElements() == 2 &&
        cast<FVectorType>(mcr.getElement("write")->type).getNumElements() == 2,
        "AXI mapper truncated count bank to one word"); bank = true;
  }
  for (auto reg : adapter.getOps<RegOp>()) if (reg.getName() == "wIndex" || reg.getName() == "rIndex") {
    require(cast<UIntType>(reg.getResult().getType()).getWidth() == 1, "two-word bank index width differs"); ++indices;
  }
  require(bank && indices == 2, "missing two-word adapter bank/indices");
}
void invalidControl(MLIRContext &ctx) {
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
    require(succeeded(goldengate::addCPUStreamCountBank(c, {{"print", "countB", 4}, {"tracerv", "countA", 13}}, error)), error);
    auto top = named(c, "GGCPUStreamCountWrapper");
    auto original = cast<BundleType>(top.getPortType(6));
    SmallVector<BundleType::BundleElement> fields(original.getElements());
    if (mode < 2) {
      unsigned index = mode == 0 ? 0 : 1;
      fields[index].type = FVectorType::get(cast<FVectorType>(fields[index].type).getElementType(), mode == 0 ? 0 : 1);
    } else if (mode == 2) {
      auto token = cast<BundleType>(cast<FVectorType>(fields[0].type).getElementType());
      SmallVector<BundleType::BundleElement> payload(token.getElements()); payload[2].type = UIntType::get(&ctx, 31, false);
      fields[0].type = FVectorType::get(BundleType::get(&ctx, payload), 2);
    } else if (mode == 3) fields[1].isFlip = false;
    else fields[2].type = UIntType::get(&ctx, 3, false);
    auto malformed = BundleType::get(&ctx, fields);
    auto types = llvm::to_vector(top.getPortTypes()); types[6] = TypeAttr::get(malformed);
    top->setAttr("portTypes", ArrayAttr::get(&ctx, types)); top.getArgument(6).setType(malformed);
    auto before = dump(*root);
    require(failed(goldengate::mapCPUStreamControl(c, 25, 12, error)) && !error.empty() && dump(*root) == before,
        "malformed multiword MCR bank accepted or mutated");
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); std::string error;
    const SmallVector<goldengate::CPUStreamCountPort> specs{{"print", "countB", 4}, {"tracerv", "countA", 13}};
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addCPUStreamCountBank(c, specs, error)), error);
    require(succeeded(verify(*root)), "multi-stream count-bank verification failed"); check(c, specs);
    auto before = dump(*root); require(failed(goldengate::addCPUStreamCountBank(c, specs, error)) && dump(*root) == before, "repeated materialization was not atomic");
    if (argc > 1) { std::error_code ec; llvm::raw_fd_ostream out(argv[1], ec); require(!ec, "cannot write MLIR fixture"); root->print(out); out << '\n'; }
    mapControl(c, *root);
    auto wideRoot = fixture(ctx, 32); auto wide = *wideRoot->getOps<CircuitOp>().begin();
    SmallVector<goldengate::CPUStreamCountPort> wideSpecs{{"wide", "countB", 32}, {"narrow", "countA", 13}};
    require(succeeded(goldengate::addCPUStreamCountBank(wide, wideSpecs, error)), error);
    require(succeeded(verify(*wideRoot)), "32-bit count-bank verification failed"); check(wide, wideSpecs);
    auto oneRoot = fixture(ctx); auto one = *oneRoot->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addCPUStreamCountBank(one, {{"single", "countA", 13}}, error)), error);
    require(named(one, "GGCPUStreamCountBank").getPortName(2) == "count", "single-count bank compatibility changed");
    auto legacyRoot = fixture(ctx, 4, "GGCPUStreamReadWrapper"); auto legacy = *legacyRoot->getOps<CircuitOp>().begin();
    auto legacyTop = named(legacy, "GGCPUStreamReadWrapper"); auto names = llvm::to_vector(legacyTop.getPortNames()); names[6] = StringAttr::get(&ctx, "tracerv_stream_count"); legacyTop->setAttr("portNames", ArrayAttr::get(&ctx, names));
    require(succeeded(goldengate::addCPUStreamCountBank(legacy, error)), error);
    require(cast<DictionaryAttr>(named(legacy, "GGCPUStreamCountBank")->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters")[0]).getAs<StringAttr>("name").getValue() == "TRACERVBRIDGEMODULE_0_to_cpu_stream_count", "legacy register identity changed");
    invalid(ctx); invalidControl(ctx);
    llvm::outs() << "PASS ordered multi-stream live count bank, 2000 SSA cycles, 24 count-bank + 5 control atomic preflight rejections, two-word AXI mapping, repeated call and legacy API\n";
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
  return 0;
}
