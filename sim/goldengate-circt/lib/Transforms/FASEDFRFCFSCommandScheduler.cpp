// See LICENSE for license details.
// Oracle: midas/models/dram/FRFCFSModel.scala FirstReadyFCFSModel, 81-275.
#include "goldengate/FASEDFRFCFSCommandScheduler.h"
#include "goldengate/FASEDCollapsingBuffer.h"
#include "goldengate/FASEDRankStateTracker.h"
#include "goldengate/FASEDRefreshUnit.h"
#include "mlir/IR/Builders.h"
#include <array>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::materializeFASEDFRFCFSCommandScheduler(
    CircuitOp circuit, FModuleOp &result, std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDFRFCFSCommandScheduler";
  // The rank materializer also creates the bank helper. Check every dependency
  // before invoking any materializer, including helpers created later below.
  for (auto module : circuit.getOps<FModuleLike>())
    for (auto symbol : {name, llvm::StringLiteral("GGFASEDRankStateTracker"),
                        llvm::StringLiteral("GGFASEDBankStateTracker"),
                        llvm::StringLiteral("GGFASEDRefreshUnit"),
                        llvm::StringLiteral("GGFASEDCollapsingBuffer")})
      if (module.getModuleName() == symbol) {
        error = "FASED FRFCFS scheduler requires free core, rank, bank, refresh and buffer symbols";
        return failure();
      }
  FModuleOp rank, refresh, buffer;
  if (failed(materializeFASEDRankStateTracker(circuit, rank, error)) ||
      failed(materializeFASEDRefreshUnit(circuit, refresh, error)) ||
      failed(materializeFASEDCollapsingBuffer(circuit, buffer, error)))
    return failure();

  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bitType = u(1);
  constexpr std::array<const char *, 8> metadataNames{
      "id", "len", "isWrite", "rowAddr", "bankAddrOH", "bankAddr", "rankAddrOH", "rankAddr"};
  constexpr std::array<unsigned, 8> metadataWidths{4, 8, 1, 26, 8, 3, 4, 2};
  SmallVector<BundleType::BundleElement> metadataFields, statusFields;
  for (unsigned i = 0; i < metadataNames.size(); ++i)
    metadataFields.push_back({b.getStringAttr(metadataNames[i]), false, u(metadataWidths[i])});
  auto metadataType = BundleType::get(ctx, metadataFields);
  for (auto n : {"canCASW", "canCASR", "canPRE", "canACT", "canREF", "wantREF", "state"})
    statusFields.push_back({b.getStringAttr(n), false, bitType});
  statusFields.push_back({b.getStringAttr("banks"), false,
                         cast<FIRRTLBaseType>(rank.getPortType(17))});
  auto statusType = BundleType::get(ctx, statusFields);
  SmallVector<PortInfo> ports;
  auto port = [&](StringRef n, Type t, Direction d) { ports.push_back({b.getStringAttr(n), t, d}); };
  port("clock", ClockType::get(ctx), Direction::In);                         // 0
  for (auto n : {"reset", "targetFire"}) port(n, bitType, Direction::In);  // 1-2
  port("tCycle", u(7), Direction::In);                                    // 3
  port("nextValid", bitType, Direction::In);                              // 4
  port("nextBits", metadataType, Direction::In);                          // 5
  for (auto n : {"backendReadReady", "backendWriteReady"})
    port(n, bitType, Direction::In);                                      // 6-7
  port("timings", rank.getPortType(8), Direction::In);                // 8
  port("schedulerWindowSize", u(3), Direction::In);                       // 9
  port("openPagePolicy", bitType, Direction::In);                         // 10
  port("ranksInUse", u(4), Direction::In);                                // 11
  port("backendLatency", u(12), Direction::In);                           // 12
  port("nextReady", bitType, Direction::Out);                            // 13
  port("newReadValid", bitType, Direction::Out);                          // 14
  port("newReadId", u(4), Direction::Out);                                // 15
  port("newReadLen", u(8), Direction::Out);                               // 16
  port("newWriteValid", bitType, Direction::Out);                         // 17
  port("newWriteId", u(4), Direction::Out);                               // 18
  port("readLatency", u(12), Direction::Out);                             // 19
  port("writeLatency", u(12), Direction::Out);                            // 20
  port("selectedCmd", u(3), Direction::Out);                              // 21
  port("cmdRank", u(2), Direction::Out);                                  // 22
  port("cmdBank", u(3), Direction::Out);                                  // 23
  port("cmdRow", u(26), Direction::Out);                                  // 24
  port("autoPRE", bitType, Direction::Out);                               // 25
  port("entries", buffer.getPortType(6), Direction::Out);             // 26
  port("rankStati", FVectorType::get(statusType, 4), Direction::Out);      // 27
  port("bankHasReadyEntries", u(32), Direction::Out);                      // 28
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module = b.create<FModuleOp>(loc, b.getStringAttr(name),
      ConventionAttr::get(ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg = [&](unsigned i) { return module.getBodyBlock()->getArgument(i); };
  auto constant = [&](unsigned w, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  auto connect = [&](Value d, Value s) { b.create<StrictConnectOp>(loc, d, s); };
  auto field = [&](Value v, StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto index = [&](Value v, unsigned i) -> Value { return b.create<SubindexOp>(loc, v, i); };
  auto bit = [&](Value v, unsigned i) -> Value { return b.create<BitsPrimOp>(loc, v, i, i); };
  auto and_ = [&](Value a, Value z) -> Value { return b.create<AndPrimOp>(loc, a, z); };
  auto or_ = [&](Value a, Value z) -> Value { return b.create<OrPrimOp>(loc, a, z); };
  auto not_ = [&](Value v) -> Value { return b.create<NotPrimOp>(loc, v); };
  auto mux = [&](Value c, Value a, Value z) -> Value { return b.create<MuxPrimOp>(loc, c, a, z); };
  auto eq = [&](Value a, Value z) -> Value { return b.create<EQPrimOp>(loc, a, z); };
  auto cat = [&](Value a, Value z) -> Value { return b.create<CatPrimOp>(loc, a, z); };
  auto trim = [&](Value v, unsigned w) -> Value { return b.create<BitsPrimOp>(loc, v, w - 1, 0); };
  auto pack = [&](ArrayRef<Value> values) -> Value {
    Value packed = values.back();
    for (int i = int(values.size()) - 2; i >= 0; --i) packed = cat(packed, values[i]);
    return packed;
  };
  Value zero = constant(1, 0), one = constant(1, 1);
  Value enabledReset = and_(arg(1), arg(2));
  SmallVector<InstanceOp> states;
  for (unsigned i = 0; i < 4; ++i)
    states.push_back(b.create<InstanceOp>(loc, rank, "rank" + std::to_string(i)));
  auto refreshInstance = b.create<InstanceOp>(loc, refresh, "refresh");
  auto bufferInstance = b.create<InstanceOp>(loc, buffer, "referenceBuffer");
  auto status = [&](unsigned r, StringRef n) -> Value {
    for (unsigned p = 10; p < rank.getNumPorts(); ++p)
      if (rank.getPortName(p) == n) return states[r].getResult(p);
    llvm_unreachable("unknown rank status field");
  };
  auto bankStatus = [&](unsigned r, unsigned k, StringRef n) -> Value {
    return field(index(states[r].getResult(17), k), n);
  };
  std::array<Value, 4> wantsRefresh, refreshable, prechargeable;
  for (unsigned r = 0; r < 4; ++r) {
    wantsRefresh[r] = status(r, "wantREF");
    refreshable[r] = status(r, "canREF");
    prechargeable[r] = status(r, "canPRE");
    std::array<Value, 8> bankCanPRE;
    for (unsigned k = 0; k < 8; ++k) bankCanPRE[k] = bankStatus(r, k, "canPRE");
    connect(refreshInstance.getResult(3 + r), pack(bankCanPRE));
    Value out = index(arg(27), r);
    for (auto n : {"canCASW", "canCASR", "canPRE", "canACT", "canREF", "wantREF", "state", "banks"})
      connect(field(out, n), status(r, n));
  }
  connect(refreshInstance.getResult(0), pack(wantsRefresh));
  connect(refreshInstance.getResult(1), pack(refreshable));
  connect(refreshInstance.getResult(2), pack(prechargeable));
  connect(refreshInstance.getResult(7), arg(11));
  connect(bufferInstance.getResult(0), arg(0));
  connect(bufferInstance.getResult(1), arg(1));
  connect(bufferInstance.getResult(2), arg(2));
  connect(bufferInstance.getResult(3), arg(4));
  // The model's three-bit setting is widened at the CollapsingBuffer boundary.
  connect(bufferInstance.getResult(8), b.create<PadPrimOp>(loc, arg(9), 4));
  connect(arg(13), bufferInstance.getResult(4));
  connect(arg(26), bufferInstance.getResult(6));

  std::array<Value, 32> bankHasReady;
  for (unsigned i = 0; i < 32; ++i)
    bankHasReady[i] = b.create<RegResetOp>(loc, bitType, arg(0), enabledReset, zero,
                                        "bankHasReady" + std::to_string(i)).getResult();
  connect(arg(28), pack(bankHasReady));
  std::array<Value, 8> entries, entryBits, valid, casCandidates, preCandidates, actCandidates;
  // Preserve the original one-hot legality reduction separately at rank and
  // bank level, including its behavior for inconsistent decoded one-hot inputs.
  auto legal = [&](Value bits, StringRef command) -> Value {
    Value rankLegal = zero, bankLegal = zero;
    Value rankOH = field(bits, "rankAddrOH"), bankOH = field(bits, "bankAddrOH");
    for (unsigned r = 0; r < 4; ++r) {
      rankLegal = or_(rankLegal, and_(bit(rankOH, r), status(r, command)));
      Value inRank = zero;
      for (unsigned k = 0; k < 8; ++k)
        inRank = or_(inRank, and_(bit(bankOH, k), bankStatus(r, k, command)));
      bankLegal = or_(bankLegal, and_(bit(rankOH, r), inRank));
    }
    return and_(rankLegal, bankLegal);
  };
  auto rankWantsRefresh = [&](Value bits) -> Value {
    Value wanting = zero;
    for (unsigned r = 0; r < 4; ++r)
      wanting = or_(wanting, and_(bit(field(bits, "rankAddrOH"), r), wantsRefresh[r]));
    return wanting;
  };
  for (unsigned i = 0; i < 8; ++i) {
    entries[i] = index(bufferInstance.getResult(6), i);
    entryBits[i] = field(entries[i], "bits");
    valid[i] = field(entries[i], "valid");
    Value notRefreshing = not_(rankWantsRefresh(entryBits[i]));
    Value readLegal = and_(legal(entryBits[i], "canCASR"), arg(6));
    Value writeLegal = and_(legal(entryBits[i], "canCASW"), arg(7));
    casCandidates[i] = and_(and_(valid[i], field(entryBits[i], "isReady")),
        and_(notRefreshing, mux(field(entryBits[i], "isWrite"), writeLegal, readLegal)));
    preCandidates[i] = and_(and_(valid[i], not_(field(entryBits[i], "isReady"))),
        and_(field(entryBits[i], "mayPRE"), legal(entryBits[i], "canPRE")));
    actCandidates[i] = and_(and_(valid[i], not_(field(entryBits[i], "isReady"))),
        and_(notRefreshing, legal(entryBits[i], "canACT")));
  }
  // All Chisel PriorityMux/Arbiter payloads fall back to the final input even
  // with no grant. Keep these invalid payloads for exact command observability.
  auto priorityBits = [&](ArrayRef<Value> candidates) -> Value {
    Value selected = entryBits[7];
    for (int i = 6; i >= 0; --i) selected = mux(candidates[i], entryBits[i], selected);
    return selected;
  };
  Value casBits = priorityBits(casCandidates), preBits = priorityBits(preCandidates);
  Value actBits = priorityBits(actCandidates), casValid = zero, suggestPre = zero;
  for (unsigned i = 0; i < 8; ++i) {
    casValid = or_(casValid, casCandidates[i]);
    suggestPre = or_(suggestPre, preCandidates[i]);
  }
  Value suggestAct = zero;
  for (int i = 7; i >= 0; --i)
    suggestAct = mux(actCandidates[i], one, and_(not_(preCandidates[i]), suggestAct));
  Value suggestREF = refreshInstance.getResult(8), refreshPRE = refreshInstance.getResult(10);
  Value cmdRow = field(actBits, "rowAddr");
  Value cmd = mux(suggestREF, constant(3, 5), mux(refreshPRE, constant(3, 2),
      mux(casValid, mux(field(casBits, "isWrite"), constant(3, 3), constant(3, 4)),
      mux(suggestAct, constant(3, 1), mux(suggestPre, constant(3, 2), constant(3, 0))))));
  Value cmdRank = mux(suggestREF, refreshInstance.getResult(9),
      mux(refreshPRE, refreshInstance.getResult(11),
      mux(casValid, field(casBits, "rankAddr"),
      mux(suggestAct, field(actBits, "rankAddr"),
      mux(suggestPre, field(preBits, "rankAddr"), field(casBits, "rankAddr"))))));
  Value cmdBank = mux(suggestREF, field(preBits, "bankAddr"),
      mux(refreshPRE, refreshInstance.getResult(12),
      mux(casValid, field(casBits, "bankAddr"),
      mux(suggestAct, field(actBits, "bankAddr"), field(preBits, "bankAddr")))));
  Value memReqDone = or_(eq(cmd, constant(3, 3)), eq(cmd, constant(3, 4)));
  Value isACT = eq(cmd, constant(3, 1)), isPRE = eq(cmd, constant(3, 2));
  auto bankMatch = [&](Value bits) -> Value {
    return and_(eq(cmdRank, field(bits, "rankAddr")), eq(cmdBank, field(bits, "bankAddr")));
  };
  std::array<Value, 8> killed;
  Value earlierCAS = zero, otherReady = zero;
  for (unsigned i = 0; i < 8; ++i) {
    killed[i] = and_(memReqDone, and_(casCandidates[i], not_(earlierCAS)));
    earlierCAS = or_(earlierCAS, casCandidates[i]);
    // In the CAS case, update.isReady remains entry.isReady. Other command
    // cases cannot use otherReady in an observable state/output expression.
    Value stillReady = and_(and_(not_(killed[i]), valid[i]), field(entryBits[i], "isReady"));
    otherReady = or_(otherReady, and_(stillReady, bankMatch(entryBits[i])));
  }
  Value autoPRE = and_(not_(arg(10)), and_(memReqDone, not_(otherReady)));
  for (unsigned i = 0; i < 8; ++i) {
    Value updated = index(bufferInstance.getResult(7), i), bits = field(updated, "bits");
    connect(field(updated, "valid"), and_(valid[i], not_(killed[i])));
    for (auto n : metadataNames) connect(field(bits, n), field(entryBits[i], n));
    Value matching = bankMatch(entryBits[i]);
    Value ready = mux(and_(matching, isACT), eq(field(entryBits[i], "rowAddr"), cmdRow),
        mux(and_(matching, isPRE), zero, field(entryBits[i], "isReady")));
    Value mayPRE = mux(and_(matching, or_(isACT, isPRE)), zero,
        mux(and_(matching, and_(memReqDone, not_(otherReady))), one, field(entryBits[i], "mayPRE")));
    connect(field(bits, "isReady"), ready);
    connect(field(bits, "mayPRE"), mayPRE);
  }
  Value newBits = arg(5), enqueueBits = bufferInstance.getResult(5);
  for (auto n : metadataNames) connect(field(enqueueBits, n), field(newBits, n));
  Value newBankMatch = bankMatch(newBits), rowHit = zero;
  for (unsigned r = 0; r < 4; ++r) {
    Value hitsInRank = zero;
    for (unsigned k = 0; k < 8; ++k) {
      Value hit = and_(eq(field(newBits, "rowAddr"), bankStatus(r, k, "openRow")), bankStatus(r, k, "state"));
      hitsInRank = or_(hitsInRank, and_(bit(field(newBits, "bankAddrOH"), k), hit));
    }
    rowHit = or_(rowHit, and_(eq(field(newBits, "rankAddr"), constant(2, r)), hitsInRank));
  }
  Value newReady = or_(and_(and_(isACT, newBankMatch), eq(field(newBits, "rowAddr"), cmdRow)),
      and_(rowHit, and_(not_(and_(and_(memReqDone, autoPRE), newBankMatch)),
                       not_(and_(isPRE, newBankMatch)))));
  Value newBankIndex = cat(field(newBits, "rankAddr"), field(newBits, "bankAddr"));
  Value selectedBankIndex = cat(cmdRank, cmdBank), oldBankHasReady = zero;
  for (unsigned i = 0; i < 32; ++i)
    oldBankHasReady = or_(oldBankHasReady, and_(eq(newBankIndex, constant(5, i)), bankHasReady[i]));
  Value newMayPRE = and_(arg(10),
      or_(and_(newBankMatch, and_(memReqDone, not_(otherReady))),
          and_(not_(oldBankHasReady), not_(and_(isPRE, newBankMatch)))));
  connect(field(enqueueBits, "isReady"), newReady);
  connect(field(enqueueBits, "mayPRE"), newMayPRE);
  Value enqueued = and_(arg(4), bufferInstance.getResult(4));
  for (unsigned i = 0; i < 32; ++i) {
    Value cmdUpdates = and_(or_(memReqDone, isACT), eq(selectedBankIndex, constant(5, i)));
    Value next = mux(cmdUpdates, or_(and_(memReqDone, otherReady), isACT), bankHasReady[i]);
    // The later incoming-ready assignment wins when both updates hit a bank.
    next = mux(and_(and_(newReady, enqueued), eq(newBankIndex, constant(5, i))), one, next);
    connect(bankHasReady[i], mux(arg(2), next, bankHasReady[i]));
  }
  Value cmdBankOH = constant(8, 0);
  for (unsigned k = 0; k < 8; ++k)
    cmdBankOH = or_(cmdBankOH, mux(eq(cmdBank, constant(3, k)), constant(8, 1u << k), constant(8, 0)));
  for (unsigned r = 0; r < 4; ++r) {
    connect(states[r].getResult(0), arg(0));
    connect(states[r].getResult(1), arg(1));
    connect(states[r].getResult(2), arg(2));
    connect(states[r].getResult(3), cmd);
    connect(states[r].getResult(4), eq(cmdRank, constant(2, r)));
    connect(states[r].getResult(5), cmdBankOH);
    connect(states[r].getResult(6), autoPRE);
    connect(states[r].getResult(7), cmdRow);
    connect(states[r].getResult(8), arg(8));
    connect(states[r].getResult(9), arg(3));
  }
  connect(arg(14), and_(memReqDone, not_(field(casBits, "isWrite"))));
  connect(arg(15), field(casBits, "id"));
  connect(arg(16), field(casBits, "len"));
  connect(arg(17), and_(memReqDone, field(casBits, "isWrite")));
  connect(arg(18), field(casBits, "id"));
  Value tCAS = field(arg(8), "tCAS"), tAL = field(arg(8), "tAL");
  // Chisel '+' truncates the first sum to seven bits before widening to the
  // backend's 12-bit input. Preserve this even for extreme runtime timings.
  Value casLatency = trim(b.create<AddPrimOp>(loc, tCAS, tAL), 7);
  Value readLatency = trim(b.create<AddPrimOp>(loc,
      b.create<PadPrimOp>(loc, casLatency, 12), arg(12)), 12);
  connect(arg(19), readLatency);
  connect(arg(20), constant(12, 1));
  connect(arg(21), cmd);
  connect(arg(22), cmdRank);
  connect(arg(23), cmdBank);
  connect(arg(24), cmdRow);
  connect(arg(25), autoPRE);
  // The source creates cmdBusBusy but never reads it: tCMD must not gate
  // command arbitration. Instrumentation/response queues are outside this core.
  result = module;
  return success();
}
