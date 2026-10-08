// See LICENSE for license details.
// Requires: internal host module, exact RegfileModelIO aggregate ABI, known
// UInt widths, at least one read/write command, no body annotation identities.
// Consumes/produces annotations: none. Preserves module/port metadata.
// Mutates: selected body only, transactionally. Invalidates body analyses.
// Analyses required: none; hierarchy/port identity is preserved.
// Output: ordinary FIRRTL registers, muxes and one async-read/sync-write RAM.
// Oracle: models/sram/AsyncMemModel.scala. Host reset flushes protocol state,
// not memory/data buffers. Read enable is intentionally unused. Target reset
// suppresses writes but does not clear data. Writes wait for all reads and a
// reset token; advance waits for every response and write command.
#include "goldengate/AsyncRAMModel.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::emitAsyncRAMModel(FModuleOp module,
    RAMModelParameters &parameters, std::string &error) {
  parameters = {};
  auto reject = [&](const char *s) { error = s; return failure(); };
  if (!module) return reject("Async RAM needs an internal host module");
  auto *ctx = module.getContext(); OpBuilder b(ctx); auto loc = module.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w); };
  auto bit = uint(1);
  auto ports = module.getPorts();
  std::optional<unsigned> clkIndex, rstIndex, chIndex;
  for (auto [i, p] : llvm::enumerate(ports)) {
    if (p.name == "clock") clkIndex = i;
    if (p.name == "reset") rstIndex = i;
    if (p.name == "channels") chIndex = i;
  }
  if (ports.size() != 3 || !clkIndex || !rstIndex || !chIndex ||
      ports[*clkIndex].type != ClockType::get(ctx) || ports[*rstIndex].type != bit ||
      ports[*clkIndex].direction != Direction::In || ports[*rstIndex].direction != Direction::In ||
      ports[*chIndex].direction != Direction::Out)
    return reject("Async RAM needs clock/reset inputs and channels output");
  auto member = [&](Type t, StringRef name) -> FIRRTLBaseType {
    auto bundle = dyn_cast<BundleType>(t);
    auto index = bundle ? bundle.getElementIndex(name) : std::nullopt;
    return index ? bundle.getElements()[*index].type : FIRRTLBaseType{};
  };
  auto channels = ports[*chIndex].type;
  auto reads = dyn_cast_or_null<FVectorType>(member(channels, "read_cmds"));
  auto writes = dyn_cast_or_null<FVectorType>(member(channels, "write_cmds"));
  auto address = reads ? dyn_cast_or_null<UIntType>(member(member(reads.getElementType(), "bits"), "addr")) : UIntType{};
  auto data = writes ? dyn_cast_or_null<UIntType>(member(member(writes.getElementType(), "bits"), "data")) : UIntType{};
  if (!reads || !writes || !reads.getNumElements() || !writes.getNumElements() ||
      !address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 62 ||
      !data || !data.getWidth() || *data.getWidth() < 1)
    return reject("Async RAM needs nonempty vectors and positive known UInt address/data widths");
  RAMModelParameters planned{unsigned(*address.getWidth()), unsigned(*data.getWidth()),
      unsigned(reads.getNumElements()), unsigned(writes.getNumElements())};
  auto field = [&](StringRef name, bool flip, FIRRTLBaseType t) {
    return BundleType::BundleElement{b.getStringAttr(name), flip, t};
  };
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> fields) {
    return BundleType::get(ctx, ArrayRef<BundleType::BundleElement>(fields));
  };
  auto decoupled = [&](FIRRTLBaseType payload) {
    return bundle({field("ready", true, bit), field("valid", false, bit), field("bits", false, payload)});
  };
  auto readCmd = bundle({field("en", false, bit), field("addr", false, address)});
  auto writeCmd = bundle({field("en", false, bit), field("mask", false, bit),
      field("addr", false, address), field("data", false, data)});
  auto expected = bundle({field("reset", true, decoupled(bit)),
      field("read_cmds", true, FVectorType::get(decoupled(readCmd), planned.reads)),
      field("read_resps", false, FVectorType::get(decoupled(data), planned.reads)),
      field("write_cmds", true, FVectorType::get(decoupled(writeCmd), planned.writes))});
  if (channels != expected) return reject("Async RAM channels differ from RegfileModelIO ABI");
  bool identities = false;
  module.walk([&](Operation *op) {
    if (op == module) return;
    identities |= op->hasAttr("inner_sym");
    auto annotations = op->getAttrOfType<ArrayAttr>("annotations");
    identities |= annotations && !annotations.empty();
  });
  auto circuit = module->getParentOfType<CircuitOp>();
  // Retained body targets need a separate rename policy. Port targets remain.
  if (auto raw = circuit ? circuit->getAttrOfType<ArrayAttr>("rawAnnotations") : ArrayAttr{}) {
    std::string prefix = "~" + circuit.getName().str() + "|";
    std::function<void(Attribute)> check = [&](Attribute a) {
      if (auto s = dyn_cast<StringAttr>(a)) {
        auto ref = s.getValue();
        if (!ref.consume_front(prefix)) return;
        auto [scope, reference] = ref.split('>');
        SmallVector<StringRef> path;
        scope.split(path, '/');
        for (auto [i, step] : llvm::enumerate(path)) {
          auto name = i ? step.split(':').second : step;
          if (name != module.getName()) continue;
          // A path through a soon-to-be-erased instance is also a body identity.
          if (i + 1 != path.size()) { identities = true; continue; }
          if (reference.empty()) continue; // The module identity survives.
          auto root = reference.take_front(reference.find_first_of(".["));
          identities |= llvm::none_of(ports, [&](PortInfo p) { return p.name.getValue() == root; });
        }
      } else if (auto array = dyn_cast<ArrayAttr>(a)) { for (auto v : array) check(v); }
      else if (auto d = dyn_cast<DictionaryAttr>(a)) { for (auto v : d) check(v.getValue()); }
    };
    check(raw);
  }
  if (identities) return reject("Async RAM body identities require a rename policy");
  OwningOpRef<FModuleOp> candidate(cast<FModuleOp>(module->clone()));
  b.setInsertionPoint(module); b.insert(candidate->getOperation());
  auto block = candidate->getBodyBlock(); block->dropAllReferences(); block->getOperations().clear();
  b.setInsertionPointToEnd(block);
  Value clock = block->getArgument(*clkIndex), reset = block->getArgument(*rstIndex);
  Value channel = block->getArgument(*chIndex);
  auto sub = [&](Value v, StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto at = [&](Value v, unsigned i) -> Value { return b.create<SubindexOp>(loc, v, i); };
  auto k = [&](unsigned w, uint64_t v) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, v)); };
  auto put = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  auto neg = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto eq = [&](Value a, unsigned n) -> Value { return b.create<EQPrimOp>(loc, a, k(2, n)); };
  auto mux = [&](Value c, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, c, y, n); };
  auto reg = [&](FIRRTLBaseType t, StringRef name) -> Value { return b.create<RegOp>(loc, t, clock, name).getResult(); };
  Value resetFired = reg(bit, "target_reset_fired"), resetValue = reg(bit, "target_reset_reg");
  Value readData = reg(data, "read_data");
  Value states = reg(FVectorType::get(uint(2), planned.reads), "read_state");
  Value buffers = reg(FVectorType::get(data, planned.reads), "read_resp_data");
  Value completed = reg(FVectorType::get(bit, planned.writes), "write_complete");
  Value resetChannel = sub(channel, "reset");
  Value resetReady = neg(resetFired), resetFire = both(resetReady, sub(resetChannel, "valid"));
  Value resetAvailable = either(resetFired, sub(resetChannel, "valid"));
  Value targetReset = mux(resetFired, resetValue, sub(resetChannel, "bits"));
  put(sub(resetChannel, "ready"), resetReady);
  SmallVector<Value> state, buffer, cmd, resp, grant, responseFire, write, complete, writeFire;
  Value open = k(1, 1), readsDone = k(1, 1), responsesDone = k(1, 1);
  for (unsigned i = 0; i < planned.reads; ++i) {
    state.push_back(at(states, i)); buffer.push_back(at(buffers, i));
    cmd.push_back(at(sub(channel, "read_cmds"), i));
    resp.push_back(at(sub(channel, "read_resps"), i));
    Value start = eq(state.back(), 0), active = eq(state.back(), 1);
    Value request = both(start, sub(cmd.back(), "valid"));
    Value ready = both(start, open); grant.push_back(both(request, open));
    put(sub(cmd.back(), "ready"), ready); open = both(open, neg(request));
    Value valid = either(active, eq(state.back(), 2));
    put(sub(resp.back(), "valid"), valid);
    put(sub(resp.back(), "bits"), mux(active, readData, buffer.back()));
    responseFire.push_back(both(valid, sub(resp.back(), "ready")));
    readsDone = both(readsDone, neg(start));
    responsesDone = both(responsesDone, either(eq(state.back(), 3), responseFire.back()));
  }
  Value writesDone = k(1, 1);
  for (unsigned i = 0; i < planned.writes; ++i) {
    complete.push_back(at(completed, i)); write.push_back(at(sub(channel, "write_cmds"), i));
    Value prereq = both(both(i ? complete[i - 1] : k(1, 1), readsDone), resetAvailable);
    Value ready = both(prereq, neg(complete.back())); put(sub(write.back(), "ready"), ready);
    writeFire.push_back(both(ready, sub(write.back(), "valid")));
    writesDone = both(writesDone, either(complete.back(), writeFire.back()));
  }
  Value advance = both(responsesDone, writesDone), flush = either(advance, reset);
  put(resetFired, mux(flush, k(1, 0), mux(resetFire, k(1, 1), resetFired)));
  put(resetValue, mux(both(neg(flush), resetFire), sub(resetChannel, "bits"), resetValue));
  Value readAddress = sub(sub(cmd[0], "bits"), "addr");
  for (unsigned i = 0; i < planned.reads; ++i) {
    readAddress = mux(grant[i], sub(sub(cmd[i], "bits"), "addr"), readAddress);
    Value active = eq(state[i], 1);
    Value next = mux(both(eq(state[i], 2), responseFire[i]), k(2, 3), state[i]);
    next = mux(active, mux(responseFire[i], k(2, 3), k(2, 2)), next);
    next = mux(both(eq(state[i], 0), grant[i]), k(2, 1), next);
    put(state[i], mux(flush, k(2, 0), next));
    put(buffer[i], mux(both(neg(flush), active), readData, buffer[i]));
  }
  Value writeAddress = sub(sub(write[0], "bits"), "addr");
  Value writeData = sub(sub(write[0], "bits"), "data"), writeEnable = k(1, 0);
  for (unsigned i = 0; i < planned.writes; ++i) {
    Value bits = sub(write[i], "bits");
    writeAddress = mux(writeFire[i], sub(bits, "addr"), writeAddress);
    writeData = mux(writeFire[i], sub(bits, "data"), writeData);
    writeEnable = mux(writeFire[i], both(sub(bits, "en"), sub(bits, "mask")), writeEnable);
    put(complete[i], mux(flush, k(1, 0), mux(writeFire[i], k(1, 1), complete[i])));
  }
  uint64_t depth = uint64_t(1) << planned.addressWidth;
  SmallVector<Type> types{MemOp::getTypeForPort(depth, data, MemOp::PortKind::Read),
      MemOp::getTypeForPort(depth, data, MemOp::PortKind::Write)};
  SmallVector<Attribute> names{b.getStringAttr("read_data_async"), b.getStringAttr("MPORT")};
  auto memory = b.create<MemOp>(loc, types, 0, 1, depth, RUWAttr::Undefined, names, "data");
  Value reader = memory.getResult(0), writer = memory.getResult(1);
  put(sub(reader, "clk"), clock); put(sub(reader, "en"), k(1, 1)); put(sub(reader, "addr"), readAddress);
  put(readData, sub(reader, "data"));
  put(sub(writer, "clk"), clock); put(sub(writer, "addr"), writeAddress); put(sub(writer, "data"), writeData);
  put(sub(writer, "mask"), k(1, 1)); put(sub(writer, "en"), both(both(writeEnable, neg(targetReset)), neg(reset)));
  if (failed(verify(*candidate))) return reject("Async RAM emission produced invalid CIRCT FIRRTL");
  module.getBody().takeBody(candidate->getBody()); parameters = planned;
  return success();
}
