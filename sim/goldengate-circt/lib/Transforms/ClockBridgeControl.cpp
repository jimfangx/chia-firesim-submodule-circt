// See LICENSE for license details.
// Input: an uninstantiated bridge wrapper with a decoded MCR bank.
// Port Lib.scala MCRFile through FIRRTL operations: independent AW/W capture,
// one outstanding read/write, selected register handshakes, and held responses.
// Annotation classes/constructors are retained; copied top ports transfer to the
// new top. Internal model/clock/channel targets retain their module identities.
// Reset clears transaction flags, never the captured IDs/data/indices. AW/AR
// len assertions are enabled only on handshake outside reset. W strobes, last,
// and metadata are deliberately ignored as in the executable Scala oracle.
// Output: local Nasti slave; global widget allocation/crossbar remain pending.
#include "goldengate/ClockBridgeControl.h"
#include "mlir/IR/Builders.h"
#include "llvm/Support/MathExtras.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;

namespace {
// These bridges use the same Lib.scala MCRFile. Keep transport behavior shared;
// the bank size determines the captured index width and local address aliasing.
// LoadMem incrementally builds four sparse decoded banks; route each word
// to this same transport without changing the register/FIFO implementations.
struct DecodedBankGroup { llvm::StringRef name; ArrayRef<unsigned> words; };
LogicalResult mapBridgeControl(CircuitOp circuit, unsigned addressBits,
    unsigned idBits, unsigned bankWords, llvm::StringRef expectedTop,
    llvm::StringRef wrapperName, llvm::StringRef adapterName,
    llvm::StringRef bankPortName, llvm::StringRef controlPortName,
    std::string &error, ArrayRef<DecodedBankGroup> groups = {}) {
  unsigned indexBits = llvm::Log2_64_Ceil(bankWords);
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (addressBits < indexBits + 2 || idBits == 0)
    return reject("MCRFile control needs enough address bits for word selection and a nonzero ID width");
  if (circuit.getName() != expectedTop)
    return reject("MCRFile control needs the active decoded bridge wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getName() == wrapperName || m.getName() == adapterName)
      return reject("MCRFile control module already exists");
    if (m.getName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("MCRFile control requires a top and retained annotations");
  bool instantiated = false;
  circuit.walk([&](InstanceOp i) { instantiated |= i.getModuleName() == inner.getName(); });
  if (instantiated) return reject("MCRFile control requires an uninstantiated top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(ctx, w, false); };
  auto bit = uint(1);
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> fields) {
    return BundleType::get(ctx, fields);
  };
  auto token = [&](FIRRTLBaseType payload) {
    return bundle({{b.getStringAttr("ready"), true, bit},
                   {b.getStringAttr("valid"), false, bit},
                   {b.getStringAttr("bits"), false, payload}});
  };
  auto words = FVectorType::get(token(uint(32)), bankWords);
  auto mcrType = bundle({{b.getStringAttr("read"), false, words},
                        {b.getStringAttr("write"), true, words},
                        {b.getStringAttr("wstrb"), true, uint(4)}});
  std::optional<unsigned> mcrPort, clockPort, resetPort;
  SmallVector<unsigned> bankPorts;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name.getValue() == controlPortName) return reject("MCRFile control port already exists");
    if (p.name.getValue() == bankPortName && p.type == mcrType && p.direction == Direction::Out) mcrPort = i;
    if (p.name.getValue() == "hostClock" && isa<ClockType>(p.type) && p.direction == Direction::In) clockPort = i;
    if (p.name.getValue() == "hostReset" && p.type == bit && p.direction == Direction::In) resetPort = i;
  }
  if (!groups.empty()) {
    SmallVector<bool> covered(bankWords, false);
    for (auto group : groups) {
      std::optional<unsigned> found;
      for (auto [i,p] : llvm::enumerate(inner.getPorts())) if (p.name == group.name) {
        auto type = dyn_cast<BundleType>(p.type);
        if (!type || p.direction != Direction::Out || type.getNumElements() != 2*group.words.size()+1)
          return reject("LoadMem decoded MCR bank has incompatible fields");
        for (auto word : group.words) {
          if (word >= bankWords || covered[word]) return reject("LoadMem MCR words overlap or exceed bank size");
          covered[word] = true;
          for (auto name : {"read", "write"}) {
            auto element = type.getElement(""+std::string(name)+"_"+std::to_string(word));
            if (!element || element->type != token(uint(32)) || element->isFlip != (StringRef(name)=="write"))
              return reject("LoadMem decoded MCR token has incompatible type or direction");
          }
        }
        auto strobe = type.getElement("wstrb");
        if (!strobe || !strobe->isFlip || strobe->type != uint(4)) return reject("LoadMem MCR strobe type differs");
        found = i;
      }
      if (!found) return reject("LoadMem decoded MCR bank is missing");
      bankPorts.push_back(*found);
    }
    if (!llvm::all_of(covered, [](bool v) { return v; })) return reject("LoadMem MCR bank has missing words");
    mcrPort = bankPorts.front();
  } else if (mcrPort) bankPorts.push_back(*mcrPort);
  if (!mcrPort || !clockPort || !resetPort)
    return reject("MCRFile control needs the expected decoded bank and host clock/reset ports");
  // This boundary is freshly created and has no target annotations. Reject any
  // unexpected reference rather than silently rebinding a decoded field to AXI.
  bool referenced = false;
  SmallVector<std::string> bankTargets;
  for (auto port : bankPorts) bankTargets.push_back("~"+circuit.getName().str()+"|"+inner.getName().str()+">"+inner.getPortName(port).str());
  std::function<void(Attribute)> check = [&](Attribute a) {
    if (auto s = dyn_cast<StringAttr>(a)) {
      auto v = s.getValue();
      for (auto &bankTarget : bankTargets)
        referenced |= v == bankTarget || v.starts_with(bankTarget + ".") || v.starts_with(bankTarget + "[");
    } else if (auto arr = dyn_cast<ArrayAttr>(a)) for (auto v : arr) check(v);
    else if (auto d = dyn_cast<DictionaryAttr>(a)) for (auto v : d) check(v.getValue());
  };
  check(raw);
  if (referenced) return reject("Bridge decoded MCR target cannot transfer to a Nasti field");

  // Nasti.scala field widths and flips, including unused request metadata.
  auto address = bundle({{b.getStringAttr("addr"), false, uint(addressBits)},
      {b.getStringAttr("len"), false, uint(8)}, {b.getStringAttr("size"), false, uint(3)},
      {b.getStringAttr("burst"), false, uint(2)}, {b.getStringAttr("lock"), false, bit},
      {b.getStringAttr("cache"), false, uint(4)}, {b.getStringAttr("prot"), false, uint(3)},
      {b.getStringAttr("qos"), false, uint(4)}, {b.getStringAttr("region"), false, uint(4)},
      {b.getStringAttr("id"), false, uint(idBits)}, {b.getStringAttr("user"), false, bit}});
  auto writeData = bundle({{b.getStringAttr("data"), false, uint(32)},
      {b.getStringAttr("last"), false, bit}, {b.getStringAttr("id"), false, uint(idBits)},
      {b.getStringAttr("strb"), false, uint(4)}, {b.getStringAttr("user"), false, bit}});
  auto response = bundle({{b.getStringAttr("resp"), false, uint(2)},
      {b.getStringAttr("id"), false, uint(idBits)}, {b.getStringAttr("user"), false, bit}});
  auto readData = bundle({{b.getStringAttr("resp"), false, uint(2)},
      {b.getStringAttr("data"), false, uint(32)}, {b.getStringAttr("last"), false, bit},
      {b.getStringAttr("id"), false, uint(idBits)}, {b.getStringAttr("user"), false, bit}});
  auto nastiType = bundle({{b.getStringAttr("aw"), false, token(address)},
      {b.getStringAttr("w"), false, token(writeData)}, {b.getStringAttr("b"), true, token(response)},
      {b.getStringAttr("ar"), false, token(address)}, {b.getStringAttr("r"), true, token(readData)}});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> adapterPorts{
      {b.getStringAttr("clock"), ClockType::get(ctx), Direction::In},
      {b.getStringAttr("reset"), bit, Direction::In},
      {b.getStringAttr("nasti"), nastiType, Direction::In},
      {b.getStringAttr("mcr"), mcrType, Direction::In}};
  auto adapter = b.create<FModuleOp>(loc, b.getStringAttr(adapterName),
      ConventionAttr::get(ctx, Convention::Internal), adapterPorts);
  b.setInsertionPointToStart(adapter.getBodyBlock());
  auto arg = [&](unsigned i) { return adapter.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc, v, n); };
  auto constant = [&](unsigned w, uint64_t n) -> Value { return b.create<ConstantOp>(loc, uint(w), APInt(w, n)); };
  auto and2 = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto not1 = [&](Value a) -> Value { return b.create<NotPrimOp>(loc, a); };
  auto mux = [&](Value s, Value a, Value c) -> Value { return b.create<MuxPrimOp>(loc, s, a, c); };
  auto connect = [&](Value dest, Value src) { b.create<StrictConnectOp>(loc, dest, src); };
  Value zero = constant(1, 0), one = constant(1, 1);
  auto flag = [&](llvm::StringRef name) -> Value { return b.create<RegResetOp>(loc, bit, arg(0), arg(1), zero, name).getResult(); };
  Value arFired = flag("arFired"), awFired = flag("awFired"), wFired = flag("wFired"), committed = flag("wCommited");
  auto reg = [&](unsigned w, llvm::StringRef n) -> Value { return b.create<RegOp>(loc, uint(w), arg(0), n).getResult(); };
  Value bId = reg(idBits, "bId"), rId = reg(idBits, "rId"), data = reg(32, "wData");
  // A singleton Vec has no address decode. Avoid UInt<0> registers and
  // backwards slices; every address aliases its sole word as in MCRFile_10.
  Value wIndex, rIndex;
  if (indexBits) {
    wIndex = reg(indexBits, "wIndex"); rIndex = reg(indexBits, "rIndex");
  }
  Value aw = field(arg(2), "aw"), w = field(arg(2), "w"), ar = field(arg(2), "ar"),
        responseB = field(arg(2), "b"), responseR = field(arg(2), "r");
  connect(field(aw, "ready"), not1(awFired));
  connect(field(w, "ready"), not1(wFired));
  connect(field(ar, "ready"), not1(arFired));
  auto fire = [&](Value v) { return and2(field(v, "ready"), field(v, "valid")); };
  Value awFire = fire(aw), wFire = fire(w), arFire = fire(ar);
  auto capture = [&](Value r, Value enable, Value next) { connect(r, mux(enable, next, r)); };
  auto payload = [&](Value v, llvm::StringRef n) { return field(field(v, "bits"), n); };
  capture(bId, awFire, payload(aw, "id")); capture(rId, arFire, payload(ar, "id"));
  capture(data, wFire, payload(w, "data"));
  // Local register decode deliberately aliases high address bits, as MCRFile.
  if (indexBits) {
    capture(wIndex, awFire, b.create<BitsPrimOp>(loc, payload(aw, "addr"), indexBits + 1, 2));
    capture(rIndex, arFire, b.create<BitsPrimOp>(loc, payload(ar, "addr"), indexBits + 1, 2));
  }
  for (Value request : {aw, ar}) {
    Value equalZero = b.create<EQPrimOp>(loc, payload(request, "len"), constant(8, 0));
    b.create<AssertOp>(loc, arg(0), equalZero, and2(fire(request), not1(arg(1))),
        "MCRFile only supports single beat transactions", ValueRange{}, "");
  }
  Value writeValid = and2(and2(awFired, wFired), not1(committed));
  Value readBits, readValid, writeFire = zero;
  auto slot = [&](llvm::StringRef group, unsigned i) -> Value { return b.create<SubindexOp>(loc, field(arg(3), group), i); };
  for (unsigned i = 0; i < bankWords; ++i) {
    Value selectedW = one, selectedR = one;
    if (indexBits) {
      selectedW = b.create<EQPrimOp>(loc, wIndex, constant(indexBits, i));
      selectedR = b.create<EQPrimOp>(loc, rIndex, constant(indexBits, i));
    }
    Value wr = slot("write", i), rd = slot("read", i);
    Value valid = and2(selectedW, writeValid);
    connect(field(wr, "valid"), valid); connect(field(wr, "bits"), data);
    writeFire = b.create<OrPrimOp>(loc, writeFire, and2(valid, field(wr, "ready")));
    connect(field(rd, "ready"), and2(and2(selectedR, arFired), field(responseR, "ready")));
    // SFC dynamic Vec read lowering defaults to lane zero for out-of-range
    // indices in non-power-of-two banks (e.g. six-word ClockBridge).
    readBits = i ? mux(selectedR, field(rd, "bits"), readBits) : field(rd, "bits");
    readValid = i ? mux(selectedR, field(rd, "valid"), readValid) : field(rd, "valid");
  }
  connect(field(arg(3), "wstrb"), constant(4, 0));
  connect(field(responseR, "valid"), and2(arFired, readValid));
  connect(payload(responseR, "data"), readBits); connect(payload(responseR, "id"), rId);
  connect(payload(responseR, "resp"), constant(2, 0)); connect(payload(responseR, "last"), one);
  connect(payload(responseR, "user"), zero);
  connect(field(responseB, "valid"), and2(and2(awFired, wFired), committed));
  connect(payload(responseB, "id"), bId); connect(payload(responseB, "resp"), constant(2, 0));
  connect(payload(responseB, "user"), zero);
  Value bFire = fire(responseB), rFire = fire(responseR);
  auto setThenClear = [&](Value state, Value set, Value clear) { connect(state, mux(clear, zero, mux(set, one, state))); };
  setThenClear(arFired, arFire, rFire); setThenClear(awFired, awFire, bFire);
  setThenClear(wFired, wFire, bFire);
  // Scala connects commit after clearing on B.fire: preserve that priority.
  connect(committed, mux(writeFire, one, mux(bFire, zero, committed)));

  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  unsigned controlIndex = 0;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) {
    if (i == *mcrPort) { controlIndex=ports.size(); ports.push_back({b.getStringAttr(controlPortName),nastiType,Direction::In}); }
    else if (!llvm::is_contained(bankPorts,i)) ports.push_back(p);
    if (!llvm::is_contained(bankPorts,i)) copied.push_back(i);
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, inner, "sim");
  auto crFile = b.create<InstanceOp>(loc, adapter, "crFile");
  unsigned outerIndex=0;
  for (auto [i,p] : llvm::enumerate(inner.getPorts())) {
    if (i == *mcrPort) { ++outerIndex; continue; }
    if (llvm::is_contained(bankPorts,i)) continue;
    Value external = wrapper.getBodyBlock()->getArgument(outerIndex++);
    b.create<ConnectOp>(loc, p.direction == Direction::In ? sim.getResult(i) : external,
        p.direction == Direction::In ? external : sim.getResult(i));
    if (i == *clockPort) connect(crFile.getResult(0),external);
    if (i == *resetPort) connect(crFile.getResult(1),external);
  }
  b.create<ConnectOp>(loc, crFile.getResult(2), wrapper.getBodyBlock()->getArgument(controlIndex));
  if (groups.empty()) b.create<ConnectOp>(loc, crFile.getResult(3), sim.getResult(*mcrPort));
  else for (auto [j,group] : llvm::enumerate(groups)) {
    Value bank=sim.getResult(bankPorts[j]);
    for (auto word : group.words) {
      Value rd=b.create<SubindexOp>(loc,field(crFile.getResult(3),"read"),word);
      Value wr=b.create<SubindexOp>(loc,field(crFile.getResult(3),"write"),word);
      b.create<ConnectOp>(loc,rd,field(bank,"read_"+std::to_string(word)));
      b.create<ConnectOp>(loc,field(bank,"write_"+std::to_string(word)),wr);
    }
    connect(field(bank,"wstrb"),field(crFile.getResult(3),"wstrb"));
  }
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
  circuit->setAttr("rawAnnotations", retarget(raw));
  circuit.setNameAttr(b.getStringAttr(wrapperName));
  return success();
}

} // namespace

LogicalResult goldengate::mapClockBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 6,
      "GGClockBridgeWrapper", "GGClockBridgeControlWrapper",
      "GGClockBridgeMCRFile", "clockBridge_mcr", "clockBridge_ctrl", error);
}

LogicalResult goldengate::mapResetPulseBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 2,
      "GGResetPulseBridgeWrapper", "GGResetPulseBridgeControlWrapper",
      "GGResetPulseBridgeMCRFile", "resetBridge_mcr", "resetBridge_ctrl", error);
}

LogicalResult goldengate::mapUARTBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 6,
      "GGUARTMMIOWrapper", "GGUARTBridgeControlWrapper",
      "GGUARTMCRFile", "uartBridge_mcr", "uartBridge_ctrl", error);
}

LogicalResult goldengate::mapPeekPokeBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 7,
      "GGPeekPokeMMIOWrapper", "GGPeekPokeBridgeControlWrapper",
      "GGPeekPokeMCRFile", "peekPokeBridge_mcr", "peekPokeBridge_ctrl", error);
}

LogicalResult goldengate::mapTracerVBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 15,
      "GGTracerVTriggerWrapper", "GGTracerVBridgeControlWrapper",
      "GGTracerVMCRFile", "tracerv_mcr", "tracerv_ctrl", error);
}

LogicalResult goldengate::mapCPUStreamControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 1,
      "GGCPUStreamCountWrapper", "GGCPUStreamControlWrapper",
      "GGCPUStreamMCRFile", "cpuStream_mcr", "cpuStream_ctrl", error);
}

// All preflight checks precede IR creation. Reject references to consumed
// decoded fields rather than attempting an ambiguous bank-to-AXI rename.
LogicalResult goldengate::mapLoadMemControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  const unsigned write[]{0,1,2,3,5}, data[]{4}, read[]{6,7}, readData[]{8};
  const DecodedBankGroup groups[]{{"loadmemWrite_mcr",write},{"loadmemData_mcr",data},
      {"loadmemRead_mcr",read},{"loadmemReadData_mcr",readData}};
  return mapBridgeControl(circuit,addressBits,idBits,9,
      "GGLoadMemReadDataWrapper","GGLoadMemControlWrapper","GGLoadMemMCRFile",
      "","loadmem_ctrl",error,groups);
}

LogicalResult goldengate::mapSimulationMasterControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 3,
      "GGSimulationMasterWrapper", "GGSimulationMasterControlWrapper",
      "GGSimulationMasterMCRFile", "simulationMaster_mcr", "simulationMaster_ctrl", error);
}

// Requires: uninstantiated GGTSIMMIOWrapper, nine decoded UInt32 lanes,
// host clock/reset, and retained annotations. No additional analyses required.
// Consumes: decoded tsiBridge_mcr port, with references rejected before mutation.
// Produces: no annotation classes; copied targets transfer, inner targets stay.
// Mutates: adds a FIRRTL MCRFile and wrapper, replacing decoded lanes with Nasti.
// Preserves: inner modules, TSI register state, channel/clock/constructor metadata.
// Output: independent AW/W capture, held B/R responses and four-bit local decode.
// The platform control slave binding remains a separate transformation.
LogicalResult goldengate::mapTSIBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 9,
      "GGTSIMMIOWrapper", "GGTSIBridgeControlWrapper",
      "GGTSIMCRFile", "tsiBridge_mcr", "tsiBridge_ctrl", error);
}

// Requires: uninstantiated GGBlockDevMMIOWrapper, 26 decoded UInt32 lanes,
// host clock/reset and retained annotations. No additional analyses required.
// Consumes: blockdevBridge_mcr; references to its fields are rejected atomically.
// Produces: no annotation classes; copied port targets transfer to the wrapper.
// Mutates: adds FIRRTL MCRFile hardware and replaces the decoded bank with Nasti.
// Preserves: inner modules, bank state and channel/clock/constructor metadata.
// Output: independent AW/W capture, held response flags/IDs, live selected R
// data and five-bit decode. Indices 26..31 read lane zero but cannot write.
// Platform slave binding and BlockDev timing remain separate transformations.
LogicalResult goldengate::mapBlockDevBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 26,
      "GGBlockDevMMIOWrapper", "GGBlockDevBridgeControlWrapper",
      "GGBlockDevMCRFile", "blockdevBridge_mcr", "blockdevBridge_ctrl", error);
}

// Requires: active uninstantiated GGFASEDMMIOWrapper, exact 21-word UInt32
// MCR bundle, host clock/reset and retained annotations. No analyses required.
// Consumes: fasedBridge_mcr; unexpected decoded references reject atomically.
// Produces: no annotation classes. Copied port targets transfer to the new top;
// internal clock/model/channel and constructor identities remain on inner ops.
// Mutates: adds ordinary FIRRTL MCRFile state and a Nasti-to-bank wrapper.
// Preserves: FASED register permissions, timing state, histograms and MMIO order.
// Output: independent AW/W capture, held B/R flags/IDs, live R data and five-bit
// local decode. Indices 21..31 read lane zero and cannot commit writes. Host
// reset clears flags only; the legacy zero MCR strobe and burst checks remain.
// Platform control slave binding and simulator emission are separate steps.
LogicalResult goldengate::mapFASEDBridgeControl(CircuitOp circuit,
    unsigned addressBits, unsigned idBits, std::string &error) {
  return mapBridgeControl(circuit, addressBits, idBits, 21,
      "GGFASEDMMIOWrapper", "GGFASEDBridgeControlWrapper",
      "GGFASEDMCRFile", "fasedBridge_mcr", "fasedBridge_ctrl", error);
}
