// See LICENSE for license details.
// Oracle: Widget.attachIO/genAndAttachReg, MCRIO.bindReg, FRFCFSModel,
// DRAMProgrammableTimings, LLCProgrammableSettings and FASEDMemoryTimingModel.
// Exact selected .const.h register order; no LBP programmable request limits.
#include "goldengate/FASEDFRFCFSMMIO.h"
#include "mlir/IR/Builders.h"
#include "goldengate/FASEDDRAMProfile.h"
#include <vector>
using namespace mlir;
using namespace circt::firrtl;

llvm::ArrayRef<goldengate::FASEDFRFCFSMMIORegister>
goldengate::getFASEDFRFCFSMMIORegisters() {
  static const FASEDFRFCFSMMIORegister rows[]{
      {"transactionQueueDepth", "transactionQueueDepth", "", -1, 32, 8, true},
      {"schedulerWindowSize", "schedulerWindowSize", "", -1, 3, 8, true},
      {"rankPower_0_numACT", "rankPower", "numACT", 0, 32, 0, false},
      {"rankPower_0_numCASW", "rankPower", "numCASW", 0, 32, 0, false},
      {"rankPower_0_numCASR", "rankPower", "numCASR", 0, 32, 0, false},
      {"rankPower_0_allPreCycles",
       "rankPower",
       "allPreCycles",
       0,
       32,
       0,
       false},
      {"rankPower_1_numACT", "rankPower", "numACT", 1, 32, 0, false},
      {"rankPower_1_numCASW", "rankPower", "numCASW", 1, 32, 0, false},
      {"rankPower_1_numCASR", "rankPower", "numCASR", 1, 32, 0, false},
      {"rankPower_1_allPreCycles",
       "rankPower",
       "allPreCycles",
       1,
       32,
       0,
       false},
      {"rankPower_2_numACT", "rankPower", "numACT", 2, 32, 0, false},
      {"rankPower_2_numCASW", "rankPower", "numCASW", 2, 32, 0, false},
      {"rankPower_2_numCASR", "rankPower", "numCASR", 2, 32, 0, false},
      {"rankPower_2_allPreCycles",
       "rankPower",
       "allPreCycles",
       2,
       32,
       0,
       false},
      {"rankPower_3_numACT", "rankPower", "numACT", 3, 32, 0, false},
      {"rankPower_3_numCASW", "rankPower", "numCASW", 3, 32, 0, false},
      {"rankPower_3_numCASR", "rankPower", "numCASR", 3, 32, 0, false},
      {"rankPower_3_allPreCycles",
       "rankPower",
       "allPreCycles",
       3,
       32,
       0,
       false},
      {"dramTimings_tWTR", "timings", "tWTR", -1, 7, 8, true},
      {"dramTimings_tWR", "timings", "tWR", -1, 7, 15, true},
      {"dramTimings_tRTRS", "timings", "tRTRS", -1, 7, 2, true},
      {"dramTimings_tRTP", "timings", "tRTP", -1, 7, 8, true},
      {"dramTimings_tRP", "timings", "tRP", -1, 7, 14, true},
      {"dramTimings_tRRD", "timings", "tRRD", -1, 7, 8, true},
      {"dramTimings_tRFC", "timings", "tRFC", -1, 10, 160, true},
      {"dramTimings_tRCD", "timings", "tRCD", -1, 7, 14, true},
      {"dramTimings_tRC", "timings", "tRC", -1, 7, 47, true},
      {"dramTimings_tREFI", "timings", "tREFI", -1, 14, 7800, true},
      {"dramTimings_tRAS", "timings", "tRAS", -1, 7, 33, true},
      {"dramTimings_tFAW", "timings", "tFAW", -1, 7, 25, true},
      {"dramTimings_tCCD", "timings", "tCCD", -1, 7, 4, true},
      {"dramTimings_tCWD", "timings", "tCWD", -1, 7, 10, true},
      {"dramTimings_tCMD", "timings", "tCMD", -1, 7, 1, true},
      {"dramTimings_tCAS", "timings", "tCAS", -1, 7, 14, true},
      {"dramTimings_tAL", "timings", "tAL", -1, 7, 0, true},
      {"backendLatency", "backendLatency", "", -1, 12, 2, true},
      {"openPagePolicy", "openPagePolicy", "", -1, 1, 1, true},
      {"rowAddr_mask", "rowAddrMask", "", -1, 26, 65535, true},
      {"rowAddr_offset", "rowAddrOffset", "", -1, 32, 18, true},
      {"rankAddr_mask", "rankAddrMask", "", -1, 2, 3, true},
      {"rankAddr_offset", "rankAddrOffset", "", -1, 32, 16, true},
      {"bankAddr_mask", "bankAddrMask", "", -1, 3, 7, true},
      {"bankAddr_offset", "bankAddrOffset", "", -1, 32, 13, true},
      {"writeOutstandingHistogram_0",
       "writeOutstandingHistogram",
       "",
       0,
       32,
       0,
       false},
      {"writeOutstandingHistogram_1",
       "writeOutstandingHistogram",
       "",
       1,
       32,
       0,
       false},
      {"writeOutstandingHistogram_2",
       "writeOutstandingHistogram",
       "",
       2,
       32,
       0,
       false},
      {"writeOutstandingHistogram_3",
       "writeOutstandingHistogram",
       "",
       3,
       32,
       0,
       false},
      {"writeOutstandingHistogram_4",
       "writeOutstandingHistogram",
       "",
       4,
       32,
       0,
       false},
      {"readOutstandingHistogram_0",
       "readOutstandingHistogram",
       "",
       0,
       32,
       0,
       false},
      {"readOutstandingHistogram_1",
       "readOutstandingHistogram",
       "",
       1,
       32,
       0,
       false},
      {"readOutstandingHistogram_2",
       "readOutstandingHistogram",
       "",
       2,
       32,
       0,
       false},
      {"readOutstandingHistogram_3",
       "readOutstandingHistogram",
       "",
       3,
       32,
       0,
       false},
      {"readOutstandingHistogram_4",
       "readOutstandingHistogram",
       "",
       4,
       32,
       0,
       false},
      {"llc_peakMSHRsUsed", "llc_peakMSHRsUsed", "", -1, 4, 0, false},
      {"llc_refills", "llc_refills", "", -1, 32, 0, false},
      {"llc_writebacks", "llc_writebacks", "", -1, 32, 0, false},
      {"llc_misses", "llc_misses", "", -1, 32, 0, false},
      {"llc_activeMSHRs", "activeMSHRs", "", -1, 4, 8, true},
      {"llc_blockBits", "blockBits", "", -1, 3, 7, true},
      {"llc_setBits", "setBits", "", -1, 4, 12, true},
      {"llc_wayBits", "wayBits", "", -1, 2, 3, true},
      {"totalWriteBeats", "totalWriteBeats", "", -1, 32, 0, false},
      {"totalReadBeats", "totalReadBeats", "", -1, 32, 0, false},
      {"totalWrites", "totalWrites", "", -1, 32, 0, false},
      {"totalReads", "totalReads", "", -1, 32, 0, false},
      {"relaxFunctionalModel", "relaxFunctionalModel", "", -1, 1, 0, true},
      {"rrespError", "rrespError", "", -1, 2, 0, false},
      {"brespError", "brespError", "", -1, 2, 0, false},
  };
  return rows;
}

llvm::ArrayRef<goldengate::FASEDFRFCFSMMIORegister>
goldengate::getFASEDFRFCFSMMIORegisters(CircuitOp circuit) {
  static const auto noLLC = [] {
    std::vector<FASEDFRFCFSMMIORegister> result;
    for (const auto &r : getFASEDFRFCFSMMIORegisters())
      if (!StringRef(r.name).starts_with("llc_"))
        result.push_back(r);
    return result;
  }();
  static const auto fcfs = [] {
    std::vector<FASEDFRFCFSMMIORegister> result;
    for (const auto &r : noLLC)
      if (r.name != "schedulerWindowSize" && r.name != "transactionQueueDepth")
        result.push_back(r);
    return result;
  }();
  if (isFASEDFCFS(circuit))
    return fcfs;
  return hasFASEDLLC(circuit) ? getFASEDFRFCFSMMIORegisters()
                              : llvm::ArrayRef<FASEDFRFCFSMMIORegister>(noLLC);
}

LogicalResult goldengate::materializeFASEDFRFCFSMMIO(CircuitOp circuit,
                                                     FModuleOp &result,
                                                     std::string &error) {
  constexpr llvm::StringLiteral name = "GGFASEDFRFCFSMMIO";
  for (auto m : circuit.getOps<FModuleLike>())
    if (m.getModuleName() == name) {
      error = "FRFCFS MMIO requires a free bank symbol";
      return failure();
    }
  auto *ctx = circuit.getContext();
  OpBuilder b(ctx);
  auto loc = circuit.getLoc();
  auto u = [&](unsigned width) { return UIntType::get(ctx, width, false); };
  auto bit = u(1);
  SmallVector<BundleType::BundleElement> timingFields, powerFields;
  for (auto n : {"tAL",
                 "tCAS",
                 "tCMD",
                 "tCWD",
                 "tCCD",
                 "tFAW",
                 "tRAS",
                 "tREFI",
                 "tRC",
                 "tRCD",
                 "tRFC",
                 "tRRD",
                 "tRP",
                 "tRTP",
                 "tRTRS",
                 "tWR",
                 "tWTR"})
    timingFields.push_back({b.getStringAttr(n),
                            false,
                            u(StringRef(n) == "tREFI"  ? 14
                              : StringRef(n) == "tRFC" ? 10
                                                       : 7)});
  for (auto n : {"allPreCycles", "numCASR", "numCASW", "numACT"})
    powerFields.push_back({b.getStringAttr(n), false, u(32)});
  auto token = BundleType::get(ctx,
                               {{b.getStringAttr("ready"), true, bit},
                                {b.getStringAttr("valid"), false, bit},
                                {b.getStringAttr("bits"), false, u(32)}});
  auto words =
      FVectorType::get(token, getFASEDFRFCFSMMIORegisters(circuit).size());
  auto mcr = BundleType::get(ctx,
                             {{b.getStringAttr("read"), false, words},
                              {b.getStringAttr("write"), true, words},
                              {b.getStringAttr("wstrb"), true, u(4)}});
  SmallVector<PortInfo> ports{
      {b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In}};
  auto add = [&](StringRef n, Type t, Direction d) {
    ports.push_back({b.getStringAttr(n), t, d});
  };
  for (auto n : {"rValid", "rReady", "bValid", "bReady"})
    add(n, bit, Direction::In);
  for (auto n : {"rResp", "bResp"})
    add(n, u(2), Direction::In);
  for (auto &r : getFASEDFRFCFSMMIORegisters(circuit)) {
    if (r.port == "rrespError" || r.port == "brespError" ||
        llvm::any_of(ports, [&](auto p) { return p.getName() == r.port; }))
      continue;
    Type t = r.port == "timings" ? Type(BundleType::get(ctx, timingFields))
             : r.port == "rankPower"
                 ? Type(FVectorType::get(BundleType::get(ctx, powerFields), 4))
             : r.index >= 0 ? Type(FVectorType::get(u(r.width), 5))
                            : Type(u(r.width));
    add(r.port, t, r.writeable ? Direction::Out : Direction::In);
  }
  add("mcr", mcr, Direction::Out);
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank =
      b.create<FModuleOp>(loc,
                          b.getStringAttr(name),
                          ConventionAttr::get(ctx, Convention::Internal),
                          ports);
  SmallVector<Attribute> registry;
  for (auto [i, r] : llvm::enumerate(getFASEDFRFCFSMMIORegisters(circuit)))
    registry.push_back(b.getDictionaryAttr(
        {b.getNamedAttr("name", b.getStringAttr(r.name)),
         b.getNamedAttr("offset", b.getI32IntegerAttr(4 * i)),
         b.getNamedAttr("readable", b.getBoolAttr(true)),
         b.getNamedAttr("writeable", b.getBoolAttr(r.writeable))}));
  bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(registry));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](StringRef n) -> Value {
    for (auto [i, p] : llvm::enumerate(ports))
      if (p.getName() == n)
        return bank.getArgument(i);
    llvm_unreachable("unknown FRFCFS MMIO port");
  };
  auto field = [&](Value v, StringRef n) -> Value {
    return b.create<SubfieldOp>(loc, v, n);
  };
  auto index = [&](Value v, unsigned i) -> Value {
    return b.create<SubindexOp>(loc, v, i);
  };
  auto con = [&](Value dst, Value src) {
    b.create<StrictConnectOp>(loc, dst, src);
  };
  auto c = [&](unsigned w, uint64_t n) -> Value {
    return b.create<ConstantOp>(loc, u(w), APInt(w, n));
  };
  Value one = c(1, 1), zero = c(2, 0),
        enabled = b.create<NotPrimOp>(loc, arg("reset"));
  for (auto [i, r] : llvm::enumerate(getFASEDFRFCFSMMIORegisters(circuit))) {
    Value read = index(field(arg("mcr"), "read"), i);
    Value write = index(field(arg("mcr"), "write"), i);
    con(field(read, "valid"), one);
    con(field(write, "ready"), one);
    Value value;
    bool responseError = r.name == "rrespError" || r.name == "brespError";
    if (r.writeable || responseError) {
      unsigned width = responseError ? 2 : 32;
      value = b.create<RegResetOp>(loc,
                                   u(width),
                                   arg("clock"),
                                   arg("reset"),
                                   c(width, r.initial),
                                   r.name)
                  .getResult();
      Value next;
      if (r.writeable)
        next = b.create<MuxPrimOp>(
            loc, field(write, "valid"), field(write, "bits"), value);
      else {
        StringRef prefix = r.name == "rrespError" ? "r" : "b";
        Value fire = b.create<AndPrimOp>(
            loc, arg((prefix + "Valid").str()), arg((prefix + "Ready").str()));
        Value bad =
            b.create<NEQPrimOp>(loc, arg((prefix + "Resp").str()), zero);
        Value capture = b.create<AndPrimOp>(loc, fire, bad);
        // Preserve SFC's B-error latch sourcing R response bits.
        next = b.create<MuxPrimOp>(loc, capture, arg("rResp"), value);
      }
      con(value, next);
      if (r.writeable) {
        Value output = arg(r.port);
        if (r.index >= 0)
          output = index(output, r.index);
        if (!r.field.empty())
          output = field(output, r.field);
        Value truncated =
            r.width == 32
                ? value
                : Value(b.create<BitsPrimOp>(loc, value, r.width - 1, 0));
        con(output, truncated);
      }
    } else {
      value = arg(r.port);
      if (r.index >= 0)
        value = index(value, r.index);
      if (!r.field.empty())
        value = field(value, r.field);
    }
    con(field(read, "bits"),
        r.writeable || r.width == 32
            ? value
            : Value(b.create<PadPrimOp>(loc, value, 32)));
    if (!r.writeable) {
      Value illegal = b.create<AndPrimOp>(loc, enabled, field(write, "valid"));
      b.create<PrintFOp>(loc,
                         arg("clock"),
                         illegal,
                         "Assertion failed: Register " + r.name +
                             " is read only\n",
                         ValueRange{},
                         "");
    }
  }
  result = bank;
  return success();
}
