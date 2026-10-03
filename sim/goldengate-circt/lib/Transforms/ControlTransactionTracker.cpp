// See LICENSE for license details.
// CIRCT implementation of junctions/ReorderQueue.scala's small-tag storage.
// Required input invariants: U250 twelve-port, twelve-bit tag tracker; dequeue
// valid means accepted B or accepted final R, as wired by the owning wrapper.
// Annotations consumed/produced: none. Analyses required: none.
// IR mutations: create a tracker module with storage and NastiRouter retirement
// assertions. Existing modules, ports and annotations are preserved.
// Output invariants: assertions sample pre-edge matches on the tracker clock,
// disabled during reset; assertions do not change storage or handshake behavior.
#include "goldengate/ControlTransactionTracker.h"
#include "mlir/IR/Builders.h"
#include <map>
using namespace mlir;
using namespace circt::firrtl;
llvm::SmallVector<PortInfo> goldengate::controlTransactionTrackerPorts(MLIRContext *ctx) {
  OpBuilder b(ctx); std::map<std::string, unsigned> hpIndex;
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  SmallVector<PortInfo> hp;
  auto port = [&](std::string n, Type t, Direction d) {
    hpIndex[n] = hp.size(); hp.push_back({b.getStringAttr(n), t, d});
  };
  port("clock", ClockType::get(ctx), Direction::In); port("reset", uint(1), Direction::In);
  port("enq_valid", uint(1), Direction::In); port("enq_bits_tag", uint(12), Direction::In);
  port("enq_bits_data", uint(4), Direction::In); port("enq_ready", uint(1), Direction::Out);
  for (unsigned i = 0; i < 12; ++i) {
    std::string p = "deq_" + std::to_string(i) + "_";
    port(p + "valid", uint(1), Direction::In); port(p + "tag", uint(12), Direction::In);
    port(p + "data", uint(4), Direction::Out); port(p + "matches", uint(1), Direction::Out);
  }
  return hp;
}
FModuleOp goldengate::createControlTransactionTracker(CircuitOp circuit, llvm::StringRef name,
                                                     llvm::StringRef queueName) {
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto hp = controlTransactionTrackerPorts(ctx); std::map<std::string, unsigned> hpIndex;
  for (auto [i, p] : llvm::enumerate(hp)) hpIndex[p.name.getValue().str()] = i;
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto helper = b.create<FModuleOp>(loc, b.getStringAttr(name),
      ConventionAttr::get(ctx, Convention::Internal), hp);
  helper->setAttr("goldengate.trackerSlots", b.getI32IntegerAttr(64));
  helper->setAttr("goldengate.trackerTagWidth", b.getI32IntegerAttr(12));
  helper->setAttr("goldengate.trackerDequeuePorts", b.getI32IntegerAttr(12));
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](llvm::StringRef n) { return helper.getBodyBlock()->getArgument(hpIndex.at(n.str())); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto either = [&](Value a, Value c) -> Value { return b.create<OrPrimOp>(loc, a, c); };
  auto invert = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value s, Value y, Value n) -> Value { return b.create<MuxPrimOp>(loc, s, y, n); };
  auto low = [&](Value v) -> Value { return b.create<BitsPrimOp>(loc, v, 5, 0); };
  auto high = [&](Value v) -> Value { return b.create<BitsPrimOp>(loc, v, 11, 6); };
  SmallVector<Value> data, tags, free;
  for (unsigned i = 0; i < 64; ++i) {
    std::string n = std::to_string(i);
    data.push_back(b.create<RegOp>(loc, uint(4), arg("clock"), "roq_data_" + n).getResult());
    tags.push_back(b.create<RegOp>(loc, uint(6), arg("clock"), "roq_tags_" + n).getResult());
    free.push_back(b.create<RegResetOp>(loc, uint(1), arg("clock"), arg("reset"), constant(1, 1), "roq_free_" + n).getResult());
  }
  // Decode ordinary SSA values into fixed slot enables. An OR of gated values
  // implements a total selection because all indices are exactly six bits.
  auto select = [&](Value index, ArrayRef<Value> values, unsigned width) -> Value {
    SmallVector<Value> terms;
    for (unsigned i = 0; i < 64; ++i)
      terms.push_back(mux(b.create<EQPrimOp>(loc, index, constant(6, i)), values[i], constant(width, 0)));
    while (terms.size() > 1) {
      SmallVector<Value> reduced;
      for (unsigned i = 0; i < terms.size(); i += 2) reduced.push_back(either(terms[i], terms[i + 1]));
      terms = std::move(reduced);
    }
    return terms.front();
  };
  Value enqIndex = low(arg("enq_bits_tag")), enqReady = select(enqIndex, free, 1);
  Value push = both(arg("enq_valid"), enqReady);
  connect(arg("enq_ready"), enqReady);
  SmallVector<Value> deqIndex;
  Value assertionEnable = invert(arg("reset"));
  for (unsigned i = 0; i < 12; ++i) {
    std::string p = "deq_" + std::to_string(i) + "_";
    Value index = low(arg(p + "tag")); deqIndex.push_back(index);
    connect(arg(p + "data"), select(index, data, 4));
    Value matches = both(invert(select(index, free, 1)),
        b.create<EQPrimOp>(loc, select(index, tags, 6), high(arg(p + "tag"))));
    connect(arg(p + "matches"), matches);
    // NastiRouter checks !deq.valid || deq.matches. Use the same combinational
    // match value as the output: a simultaneous enqueue cannot authorize an
    // untracked response at this edge. Reset masks the check, not retirement.
    b.create<AssertOp>(loc, arg("clock"), either(invert(arg(p + "valid")), matches),
        assertionEnable, queueName.str() + " " + std::to_string(i) +
            " tried to dequeue untracked transaction", ValueRange{}, "");
  }
  for (unsigned i = 0; i < 64; ++i) {
    Value put = both(push, b.create<EQPrimOp>(loc, enqIndex, constant(6, i)));
    Value retire = constant(1, 0);
    for (unsigned j = 0; j < 12; ++j)
      retire = either(retire, both(arg("deq_" + std::to_string(j) + "_valid"),
          b.create<EQPrimOp>(loc, deqIndex[j], constant(6, i))));
    connect(data[i], mux(put, arg("enq_bits_data"), data[i]));
    connect(tags[i], mux(put, high(arg("enq_bits_tag")), tags[i]));
    // Dequeues are deliberately not gated by matches. Scala's later connects
    // override an enqueue to the same slot; reset overrides both for free only.
    connect(free[i], mux(retire, constant(1, 1), mux(put, constant(1, 0), free[i])));
  }
  return helper;
}
