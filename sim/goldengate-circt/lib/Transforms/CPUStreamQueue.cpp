// See LICENSE for license details.
// CPUManagedStreamEngine's Queue(UInt(512.W), 6144, flow=false, pipe=false).
// The explicit unreset read address implements the SFC synchronous lookahead;
// RAM contents and address continue to update during reset.
#include "goldengate/CPUStreamQueue.h"
#include "mlir/IR/Builders.h"
using namespace mlir;
using namespace circt::firrtl;

FModuleOp goldengate::createCPUStreamQueue6144(CircuitOp circuit, StringRef name) {
  auto *context = circuit.getContext();
  OpBuilder b(context); Location loc = circuit.getLoc();
  auto bit = UIntType::get(context, 1, false), word = UIntType::get(context, 512, false);
  auto token = BundleType::get(context, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, word}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> queuePorts{{b.getStringAttr("clock"), ClockType::get(context), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("enq"), token, Direction::In}, {b.getStringAttr("deq"), token, Direction::Out},
      {b.getStringAttr("count"), UIntType::get(context, 13, false), Direction::Out}};
  auto queue = b.create<FModuleOp>(loc, b.getStringAttr(name), ConventionAttr::get(context, Convention::Internal), queuePorts);
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
  Value enqPtr = reg(13, "enq_ptr_value"), deqPtr = reg(13, "deq_ptr_value");
  Value maybeFull = reg(1, "maybe_full");
  Value equal = b.create<EQPrimOp>(loc, enqPtr, deqPtr);
  Value full = both(equal, maybeFull), empty = both(equal, b.create<NotPrimOp>(loc, maybeFull));
  Value ready = b.create<NotPrimOp>(loc, full), valid = b.create<NotPrimOp>(loc, empty);
  Value push = both(ready, field(arg(2), "valid")), pop = both(valid, field(arg(3), "ready"));
  connect(field(arg(2), "ready"), ready); connect(field(arg(3), "valid"), valid);
  auto increment = [&](Value pointer) -> Value {
    Value plus = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, pointer, constant(13, 1)), 12, 0);
    return mux(b.create<EQPrimOp>(loc, pointer, constant(13, 6143)), constant(13, 0), plus);
  };
  connect(enqPtr, mux(push, increment(enqPtr), enqPtr));
  connect(deqPtr, mux(pop, increment(deqPtr), deqPtr));
  connect(maybeFull, mux(b.create<XorPrimOp>(loc, push, pop), push, maybeFull));

  SmallVector<Type> memoryTypes{MemOp::getTypeForPort(6144, word, MemOp::PortKind::Read),
      MemOp::getTypeForPort(6144, word, MemOp::PortKind::Write)};
  SmallVector<Attribute> memoryNames{b.getStringAttr("read"), b.getStringAttr("write")};
  auto ram = b.create<MemOp>(loc, memoryTypes, 0, 1, 6144,
      RUWAttr::Undefined, memoryNames, "ram");
  Value reader = ram.getResult(0), writer = ram.getResult(1);
  connect(field(reader, "clk"), arg(0)); connect(field(reader, "en"), constant(1, 1));
  Value readAddress = b.create<RegOp>(loc, UIntType::get(context, 13, false), arg(0), "ram_read_addr").getResult();
  connect(readAddress, mux(pop, increment(deqPtr), deqPtr));
  connect(field(reader, "addr"), readAddress); connect(field(arg(3), "bits"), field(reader, "data"));
  connect(field(writer, "clk"), arg(0)); connect(field(writer, "en"), push);
  connect(field(writer, "addr"), enqPtr); connect(field(writer, "mask"), constant(1, 1));
  connect(field(writer, "data"), field(arg(2), "bits"));
  // Explicit unreset address register plus RAM reproduces the golden's
  // read-during-write behavior: data observes the updated memory after the edge.
  // Reset flushes pointers, leaving RAM and the read-address capture active.
  Value diff = b.create<BitsPrimOp>(loc, b.create<SubPrimOp>(loc, enqPtr, deqPtr), 12, 0);
  Value wrapped = b.create<BitsPrimOp>(loc, b.create<AddPrimOp>(loc, constant(13, 6144), diff), 12, 0);
  connect(arg(4), mux(equal, mux(maybeFull, constant(13, 6144), constant(13, 0)),
      mux(b.create<GTPrimOp>(loc, deqPtr, enqPtr), wrapped, diff)));
  // Retain platform intent for the later XDC emitter, separate from queue logic.
  ram->setAttr("goldengate.ramStyle", b.getStringAttr("ULTRA"));
  return queue;
}
