// See LICENSE for license details.
#include "goldengate/FASEDHostWidthAdapter.h"
#include "goldengate/FASEDGeometry.h"
#include "mlir/IR/Builders.h"
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::materializeFASEDHostWidthAdapter(CircuitOp circuit,
                                                           BundleType target,
                                                           BundleType host,
                                                           FModuleOp &result,
                                                           std::string &error) {
  auto g = getFASEDGeometry(circuit);
  if (!validFASEDGeometry(g) || g.dataBits == 64 ||
      uint64_t(std::max(g.readBeats, g.writeBeats)) *
              std::max(1u, g.dataBits / 64) >
          256) {
    error = "FASED host width adapter requires unequal target/host beat widths "
            "and host bursts of at most 256 beats";
    return failure();
  }
  for (auto m : circuit.getOps<FModuleLike>())
    if (m.getModuleName() == "GGFASEDHostWidthAdapter") {
      error = "FASED host width adapter symbol already exists";
      return failure();
    }
  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  bool downsize = g.dataBits > 64;
  unsigned ratio = downsize ? g.dataBits / 64 : 64 / g.dataBits;
  unsigned laneBits = llvm::Log2_64(ratio),
           offsetBits = std::max(3u, g.byteBits());
  unsigned readSlots = llvm::PowerOf2Ceil(g.idReuse);
  unsigned readPtrBits = std::max(1u, llvm::Log2_64(readSlots));
  unsigned readCountBits = llvm::Log2_64_Ceil(readSlots + 1);
  unsigned writeSlots = llvm::PowerOf2Ceil(getFASEDEffectiveCapacity(circuit));
  unsigned writePtrBits = std::max(1u, llvm::Log2_64(writeSlots));
  unsigned writeCountBits = llvm::Log2_64_Ceil(writeSlots + 1);
  SmallVector<PortInfo> ports{
      {b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), u(1), Direction::In},
      {b.getStringAttr("in"), target, Direction::In},
      {b.getStringAttr("out"), host, Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto m = b.create<FModuleOp>(loc,
                               b.getStringAttr("GGFASEDHostWidthAdapter"),
                               ConventionAttr::get(ctx, Convention::Internal),
                               ports);
  b.setInsertionPointToStart(m.getBodyBlock());
  auto arg = [&](unsigned i) { return m.getArgument(i); };
  auto f = [&](Value v, StringRef n) -> Value {
    return b.create<SubfieldOp>(loc, v, n);
  };
  auto c = [&](unsigned w, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  auto con = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto a = [&](Value x, Value y) -> Value {
    return b.create<AndPrimOp>(loc, x, y);
  };
  auto o = [&](Value x, Value y) -> Value {
    return b.create<OrPrimOp>(loc, x, y);
  };
  auto n = [&](Value x) -> Value { return b.create<NotPrimOp>(loc, x); };
  auto eq = [&](Value x, Value y) -> Value {
    return b.create<EQPrimOp>(loc, x, y);
  };
  auto mux = [&](Value s, Value x, Value y) -> Value {
    return b.create<MuxPrimOp>(loc, s, x, y);
  };
  auto trim = [&](Value x, unsigned w) -> Value {
    return b.create<BitsPrimOp>(loc, x, w - 1, 0);
  };
  auto reg = [&](StringRef name, unsigned w) -> Value {
    return b.create<RegResetOp>(loc, u(w), arg(0), arg(1), c(w, 0), name)
        .getResult();
  };
  auto increment = [&](Value v, unsigned width, unsigned slots) -> Value {
    return slots == 1 ? c(width, 0)
                      : trim(b.create<AddPrimOp>(loc, v, c(width, 1)), width);
  };
  auto countNext =
      [&](Value count, Value push, Value pop, unsigned width) -> Value {
    return mux(b.create<XorPrimOp>(loc, push, pop),
               mux(push,
                   trim(b.create<AddPrimOp>(loc, count, c(width, 1)), width),
                   trim(b.create<SubPrimOp>(loc, count, c(width, 1)), width)),
               count);
  };
  auto select = [&](ArrayRef<Value> xs, Value index, unsigned width) -> Value {
    Value value = xs.back();
    for (int i = int(xs.size()) - 2; i >= 0; --i)
      value = mux(eq(index, c(width, i)), xs[i], value);
    return value;
  };
  auto splitLast = [&](Value size) -> Value {
    if (!downsize)
      return c(laneBits, 0);
    Value split = trim(
        b.create<DShlPrimOp>(
            loc, c(1, 1), trim(b.create<SubPrimOp>(loc, size, c(3, 3)), 3)),
        laneBits + 1);
    return mux(
        b.create<GTPrimOp>(loc, size, c(3, 3)),
        trim(b.create<SubPrimOp>(loc, split, c(laneBits + 1, 1)), laneBits),
        c(laneBits, 0));
  };
  auto advanceOffset = [&](Value offset, Value size) -> Value {
    return trim(b.create<AddPrimOp>(
                    loc, offset, b.create<DShlPrimOp>(loc, c(1, 1), size)),
                offsetBits);
  };
  auto lane = [&](Value offset, Value segment) -> Value {
    if (!downsize)
      return b.create<BitsPrimOp>(loc, offset, 2, g.byteBits());
    Value first = b.create<BitsPrimOp>(loc, offset, offsetBits - 1, 3);
    return trim(b.create<AddPrimOp>(loc, first, segment), laneBits);
  };
  auto sourceAW = f(arg(2), "aw"), destAW = f(arg(3), "aw");
  auto sourceAR = f(arg(2), "ar"), destAR = f(arg(3), "ar");
  auto sourceW = f(arg(2), "w"), destW = f(arg(3), "w");
  auto sourceR = f(arg(2), "r"), destR = f(arg(3), "r");
  auto sourceB = f(arg(2), "b"), destB = f(arg(3), "b");
  Value writeCount = reg("writeCount", writeCountBits),
        writeEnq = reg("writeEnq", writePtrBits),
        writeDeq = reg("writeDeq", writePtrBits);
  Value writeSpace =
      b.create<LTPrimOp>(loc, writeCount, c(writeCountBits, writeSlots));
  Value writeActive = n(eq(writeCount, c(writeCountBits, 0)));
  SmallVector<Value> writeSizes, writeOffsets;
  for (unsigned i = 0; i < writeSlots; ++i) {
    writeSizes.push_back(reg("writeSize" + std::to_string(i), 3));
    writeOffsets.push_back(reg("writeOffset" + std::to_string(i), offsetBits));
  }
  Value writeSize = select(writeSizes, writeDeq, writePtrBits),
        writeOffset = select(writeOffsets, writeDeq, writePtrBits);
  Value writeSegment = reg("writeSegment", laneBits);
  Value finalWrite = eq(writeSegment, splitLast(writeSize));
  con(f(destW, "valid"), a(f(sourceW, "valid"), writeActive));
  con(f(sourceW, "ready"), a(a(f(destW, "ready"), writeActive), finalWrite));
  Value physicalWFire = a(f(destW, "valid"), f(destW, "ready"));
  Value targetWFire = a(physicalWFire, finalWrite);
  Value writePop = a(targetWFire, f(f(sourceW, "bits"), "last"));
  Value writePush = a(f(sourceAW, "valid"), f(sourceAW, "ready"));
  con(writeCount, countNext(writeCount, writePush, writePop, writeCountBits));
  con(writeEnq,
      mux(writePush, increment(writeEnq, writePtrBits, writeSlots), writeEnq));
  con(writeDeq,
      mux(writePop, increment(writeDeq, writePtrBits, writeSlots), writeDeq));
  con(writeSegment,
      mux(physicalWFire,
          mux(finalWrite,
              c(laneBits, 0),
              increment(writeSegment, laneBits, ratio)),
          writeSegment));
  Value writeLane = lane(writeOffset, writeSegment);
  for (auto [name, width] :
       {std::pair<StringRef, unsigned>{"data", 64}, {"strb", 8}}) {
    Value packed = f(f(sourceW, "bits"), name), piece;
    if (downsize) {
      for (unsigned i = 0; i < ratio; ++i) {
        Value bits =
            b.create<BitsPrimOp>(loc, packed, (i + 1) * width - 1, i * width);
        piece = piece ? mux(eq(writeLane, c(laneBits, i)), bits, piece) : bits;
      }
    } else {
      unsigned sourceWidth = width / ratio;
      piece = c(width, 0);
      for (unsigned i = 0; i < ratio; ++i) {
        Value shifted = trim(
            b.create<ShlPrimOp>(
                loc, b.create<PadPrimOp>(loc, packed, width), i * sourceWidth),
            width);
        piece = mux(eq(writeLane, c(laneBits, i)), shifted, piece);
      }
    }
    con(f(f(destW, "bits"), name), piece);
  }
  con(f(f(destW, "bits"), "last"),
      a(f(f(sourceW, "bits"), "last"), finalWrite));
  for (unsigned i = 0; i < writeSlots; ++i) {
    Value push = a(writePush, eq(writeEnq, c(writePtrBits, i)));
    Value advance = a(targetWFire, eq(writeDeq, c(writePtrBits, i)));
    con(writeSizes[i],
        mux(push, f(f(sourceAW, "bits"), "size"), writeSizes[i]));
    con(writeOffsets[i],
        mux(push,
            b.create<BitsPrimOp>(
                loc, f(f(sourceAW, "bits"), "addr"), offsetBits - 1, 0),
            mux(advance,
                advanceOffset(writeOffsets[i], writeSizes[i]),
                writeOffsets[i])));
  }
  // Read metadata is queued per AXI ID, allowing host-response interleaving.
  SmallVector<Value> readCount, readEnq, readDeq, readSegment, readData,
      readResp;
  SmallVector<SmallVector<Value>> readSizes(16), readOffsets(16);
  for (unsigned id = 0; id < 16; ++id) {
    std::string tag = std::to_string(id);
    readCount.push_back(reg("readCount" + tag, readCountBits));
    readEnq.push_back(reg("readEnq" + tag, readPtrBits));
    readDeq.push_back(reg("readDeq" + tag, readPtrBits));
    readSegment.push_back(reg("readSegment" + tag, laneBits));
    readData.push_back(reg("readData" + tag, g.dataBits));
    readResp.push_back(reg("readResp" + tag, 2));
    for (unsigned i = 0; i < readSlots; ++i) {
      readSizes[id].push_back(
          reg("readSize" + tag + "_" + std::to_string(i), 3));
      readOffsets[id].push_back(
          reg("readOffset" + tag + "_" + std::to_string(i), offsetBits));
    }
  }
  Value arId = f(f(sourceAR, "bits"), "id"), rId = f(f(destR, "bits"), "id");
  Value readSpace = b.create<LTPrimOp>(
      loc, select(readCount, arId, 4), c(readCountBits, readSlots));
  Value rSize, rOffset;
  SmallVector<Value> headSizes, headOffsets;
  for (unsigned id = 0; id < 16; ++id) {
    headSizes.push_back(select(readSizes[id], readDeq[id], readPtrBits));
    headOffsets.push_back(select(readOffsets[id], readDeq[id], readPtrBits));
  }
  rSize = select(headSizes, rId, 4);
  rOffset = select(headOffsets, rId, 4);
  Value segment = select(readSegment, rId, 4),
        finalRead = eq(segment, splitLast(rSize));
  Value rOutValid = reg("responseValid", 1),
        rOutData = reg("responseData", g.dataBits),
        rOutId = reg("responseId", 4), rOutLast = reg("responseLast", 1),
        rOutResp = reg("responseResp", 2);
  con(f(sourceR, "valid"), rOutValid);
  for (auto [name, value] : {std::pair<StringRef, Value>{"data", rOutData},
                             {"id", rOutId},
                             {"last", rOutLast},
                             {"resp", rOutResp}})
    con(f(f(sourceR, "bits"), name), value);
  con(f(destR, "ready"), o(n(rOutValid), f(sourceR, "ready")));
  Value rFire = a(f(destR, "valid"), f(destR, "ready")),
        rEmit = a(rFire, finalRead);
  Value rPop = a(rFire, f(f(destR, "bits"), "last"));
  con(rOutValid,
      mux(rEmit, c(1, 1), mux(f(sourceR, "ready"), c(1, 0), rOutValid)));
  Value oldData = mux(
      eq(segment, c(laneBits, 0)), c(g.dataBits, 0), select(readData, rId, 4));
  Value readLane = lane(rOffset, segment), merged;
  if (downsize) {
    for (int i = int(ratio) - 1; i >= 0; --i) {
      Value piece =
          mux(eq(readLane, c(laneBits, i)),
              f(f(destR, "bits"), "data"),
              b.create<BitsPrimOp>(loc, oldData, (i + 1) * 64 - 1, i * 64));
      merged = merged ? Value(b.create<CatPrimOp>(loc, merged, piece)) : piece;
    }
  } else {
    merged = c(g.dataBits, 0);
    for (unsigned i = 0; i < ratio; ++i)
      merged = mux(eq(readLane, c(laneBits, i)),
                   b.create<BitsPrimOp>(loc,
                                        f(f(destR, "bits"), "data"),
                                        (i + 1) * g.dataBits - 1,
                                        i * g.dataBits),
                   merged);
  }
  Value oldResp =
            mux(eq(segment, c(laneBits, 0)), c(2, 0), select(readResp, rId, 4)),
        newResp = f(f(destR, "bits"), "resp");
  Value mergedResp =
      mux(b.create<GTPrimOp>(loc, newResp, oldResp), newResp, oldResp);
  con(rOutData, mux(rEmit, merged, rOutData));
  con(rOutId, mux(rEmit, rId, rOutId));
  con(rOutLast, mux(rEmit, f(f(destR, "bits"), "last"), rOutLast));
  con(rOutResp, mux(rEmit, mergedResp, rOutResp));
  Value arPush = a(f(sourceAR, "valid"), f(sourceAR, "ready"));
  for (unsigned id = 0; id < 16; ++id) {
    Value push = a(arPush, eq(arId, c(4, id))),
          pop = a(rPop, eq(rId, c(4, id)));
    Value beat = a(rFire, eq(rId, c(4, id))),
          word = a(rEmit, eq(rId, c(4, id)));
    con(readCount[id], countNext(readCount[id], push, pop, readCountBits));
    con(readEnq[id],
        mux(push, increment(readEnq[id], readPtrBits, readSlots), readEnq[id]));
    con(readDeq[id],
        mux(pop, increment(readDeq[id], readPtrBits, readSlots), readDeq[id]));
    con(readSegment[id],
        mux(beat,
            mux(finalRead,
                c(laneBits, 0),
                increment(readSegment[id], laneBits, ratio)),
            readSegment[id]));
    con(readData[id], mux(beat, merged, readData[id]));
    con(readResp[id], mux(beat, mergedResp, readResp[id]));
    for (unsigned i = 0; i < readSlots; ++i) {
      Value enqueue = a(push, eq(readEnq[id], c(readPtrBits, i))),
            advance = a(word, eq(readDeq[id], c(readPtrBits, i)));
      con(readSizes[id][i],
          mux(enqueue, f(f(sourceAR, "bits"), "size"), readSizes[id][i]));
      con(readOffsets[id][i],
          mux(enqueue,
              b.create<BitsPrimOp>(
                  loc, f(f(sourceAR, "bits"), "addr"), offsetBits - 1, 0),
              mux(advance,
                  advanceOffset(readOffsets[id][i], readSizes[id][i]),
                  readOffsets[id][i])));
    }
  }
  for (auto [source, dest, space] :
       {std::tuple<Value, Value, Value>{sourceAW, destAW, writeSpace},
        {sourceAR, destAR, readSpace}}) {
    con(f(dest, "valid"), a(f(source, "valid"), space));
    con(f(source, "ready"), a(f(dest, "ready"), space));
    Value src = f(source, "bits"), dst = f(dest, "bits"), size = f(src, "size");
    Value wide = b.create<GTPrimOp>(loc, size, c(3, 3));
    for (auto field : cast<BundleType>(dst.getType()).getElements()) {
      Value value = f(src, field.name);
      if (field.name == "size")
        value = mux(wide, c(3, 3), size);
      if (field.name == "len") {
        Value length = b.create<AddPrimOp>(
            loc, b.create<PadPrimOp>(loc, value, 9), c(9, 1));
        Value shift = trim(b.create<SubPrimOp>(loc, size, c(3, 3)), 3);
        Value expanded =
            trim(b.create<SubPrimOp>(
                     loc, b.create<DShlPrimOp>(loc, length, shift), c(9, 1)),
                 8);
        value = mux(wide, expanded, value);
      }
      if (field.name == "burst")
        value = mux(wide, c(2, 1), value);
      con(f(dst, field.name), value);
    }
  }
  con(f(sourceB, "valid"), f(destB, "valid"));
  con(f(destB, "ready"), f(sourceB, "ready"));
  con(f(sourceB, "bits"), f(destB, "bits"));
  result = m;
  return success();
}
