// See LICENSE for license details.
// Oracle: CPUManagedStreamEngine.genHeader and Widget.genConstructor.
// Required input: unique live CPU read/write transport, stream queue/count
// bank and allocated read/write control bindings. Outgoing order is the native
// transport allocation order; count MMIO allocates outgoing before incoming.
// Annotations read: sole .const.h OutputFile; none consumed. Produces one
// managed-stream constructor and a peer-to-peer address-map OutputFile.
// Analyses required: InstanceGraph plus SSA/instance forwarding of count/data.
// Hardware, hierarchy, clocks and existing unrelated annotations are preserved. Driver
// descriptors must match typed queue storage, address grant and count read.
// All rejection paths precede mutation; duplicate construction is an error.
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/CPUStreamRead.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <cstdint>

using namespace mlir;
using namespace circt::firrtl;
namespace {
// Validate live SSA, not a marker attribute: the constructor may only describe
// a byte-assembling writer whose state, guards and physical responses are intact.
LogicalResult validateIncomingAssembly(FModuleOp module, unsigned count,
                                      unsigned windowBits, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  using S = std::string;
  auto op = [](StringRef name, std::initializer_list<S> args) {
    S out = name.str() + "("; bool first = true;
    for (auto &arg : args) { if (!first) out += ","; first = false; out += arg; }
    return out + ")";
  };
  auto c = [&](unsigned width, uint64_t value) { return "c" + std::to_string(width) + ":" + std::to_string(value); };
  auto p = [](unsigned index) { return "p" + std::to_string(index); };
  auto bits = [&](S value, unsigned hi, unsigned lo) { return op("bits", {value, std::to_string(hi), std::to_string(lo)}); };
  auto eq = [&](S lhs, S rhs) { return op("eq", {lhs, rhs}); };
  auto And = [&](S lhs, S rhs) { return op("and", {lhs, rhs}); };
  auto Or = [&](S lhs, S rhs) { return op("or", {lhs, rhs}); };
  auto Not = [&](S value) { return op("not", {value}); };
  auto mux = [&](S cond, S yes, S no) { return op("mux", {cond, yes, no}); };
  auto pad = [&](S value, unsigned width) { return op("pad", {value, std::to_string(width)}); };
  llvm::DenseMap<Value, S> memo;
  std::function<S(Value)> expression = [&](Value value) -> S {
    auto old = memo.find(value); if (old != memo.end()) return old->second;
    S out; auto *def = value.getDefiningOp();
    if (auto arg = dyn_cast<BlockArgument>(value)) {
      if (arg.getOwner() != module.getBodyBlock()) return "foreign-argument";
      out = p(arg.getArgNumber());
    } else if (auto reg = dyn_cast_or_null<RegResetOp>(def)) out = "r:" + reg.getName().str();
    else if (auto reg = dyn_cast_or_null<RegOp>(def)) out = "r:" + reg.getName().str();
    else if (auto constant = dyn_cast_or_null<ConstantOp>(def)) {
      auto v = constant.getValue(); if (v.getBitWidth() > 64) return "oversized-constant";
      out = c(v.getBitWidth(), v.getZExtValue());
    } else if (auto field = dyn_cast_or_null<SubfieldOp>(def)) out = op("field", {expression(field.getInput()), field.getFieldName().str()});
    else if (auto b = dyn_cast_or_null<BitsPrimOp>(def)) out = bits(expression(b.getInput()), b.getHi(), b.getLo());
    else if (auto shift = dyn_cast_or_null<ShlPrimOp>(def)) out = op("shl", {expression(shift.getInput()), std::to_string(shift.getAmount())});
    else if (auto padding = dyn_cast_or_null<PadPrimOp>(def)) out = pad(expression(padding.getInput()), padding.getAmount());
    else if (isa_and_nonnull<AndPrimOp>(def)) out = And(expression(def->getOperand(0)), expression(def->getOperand(1)));
    else if (isa_and_nonnull<OrPrimOp>(def)) out = Or(expression(def->getOperand(0)), expression(def->getOperand(1)));
    else if (isa_and_nonnull<NotPrimOp>(def)) out = Not(expression(def->getOperand(0)));
    else if (isa_and_nonnull<EQPrimOp>(def)) out = eq(expression(def->getOperand(0)), expression(def->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(def)) out = op("add", {expression(def->getOperand(0)), expression(def->getOperand(1))});
    else if (isa_and_nonnull<CatPrimOp>(def)) out = op("cat", {expression(def->getOperand(0)), expression(def->getOperand(1))});
    else if (isa_and_nonnull<MuxPrimOp>(def)) out = mux(expression(def->getOperand(0)), expression(def->getOperand(1)), expression(def->getOperand(2)));
    else out = "unsupported-expression";
    memo[value] = out; return out;
  };
  std::map<S, Value> regs;
  llvm::DenseMap<Value, Value> drivers;
  for (auto &operation : *module.getBodyBlock()) {
    if (auto reg = dyn_cast<RegResetOp>(operation)) {
      if (!regs.emplace(reg.getName().str(), reg.getResult()).second) return reject("CPU incoming assembly register names alias");
      if (reg.getClockVal() != module.getArgument(0) || reg.getResetSignal() != module.getArgument(1) ||
          expression(reg.getResetValue()) != c(*cast<UIntType>(reg.getResult().getType()).getWidth(), 0))
        return reject("CPU incoming assembly reset or clock differs");
    } else if (auto reg = dyn_cast<RegOp>(operation)) {
      if (!regs.emplace(reg.getName().str(), reg.getResult()).second || reg.getClockVal() != module.getArgument(0))
        return reject("CPU incoming assembly payload clock or identity differs");
    } else if (auto connect = dyn_cast<StrictConnectOp>(operation)) {
      if (!drivers.try_emplace(connect.getDest(), connect.getSrc()).second)
        return reject("CPU incoming assembly has multiple drivers");
    }
  }
  if (regs.size() != 5 * count) return reject("CPU incoming assembly state count differs");
  unsigned axi = count + 2;
  auto a = [&](unsigned offset) { return p(axi + offset); };
  auto driven = [&](Value dest, const S &expected) {
    auto it = drivers.find(dest); return it != drivers.end() && expression(it->second) == expected;
  };
  auto fieldDriven = [&](unsigned port, StringRef field, const S &expected) {
    unsigned matches = 0;
    for (auto connect : module.getOps<StrictConnectOp>()) {
      auto sub = connect.getDest().getDefiningOp<SubfieldOp>();
      if (sub && sub.getInput() == module.getArgument(port) && sub.getFieldName() == field) {
        if (expression(connect.getSrc()) != expected) return false; ++matches;
      }
    }
    return matches == 1;
  };
  SmallVector<AssertOp> assertions(module.getOps<AssertOp>());
  if (assertions.size() != count) return reject("CPU incoming assembly malformed-write guards missing");
  S awReady = c(1,0), wReady = c(1,0), bValid = c(1,0), responseError = c(1,0);
  for (unsigned index = 0; index < count; ++index) {
    auto name = [&](StringRef base) { return base.str() + (count == 1 ? "" : "_" + std::to_string(index)); };
    auto reg = [&](StringRef base) { return "r:" + name(base); };
    const std::pair<StringRef,unsigned> state[]{{"writeBeatCounter",9},{"partialWriteMask",64},{"partialWriteData",512},{"partialWriteLine",58},{"writeBurstError",1}};
    for (auto [base, width] : state) {
      auto found = regs.find(name(base));
      bool reset = base != "partialWriteData" && base != "partialWriteLine";
      if (found == regs.end() || found->second.getType() != UIntType::get(module.getContext(),width,false) ||
          bool(found->second.getDefiningOp<RegResetOp>()) != reset)
        return reject("CPU incoming assembly register geometry differs");
    }
    S beats=reg("writeBeatCounter"), mask=reg("partialWriteMask"), buffered=reg("partialWriteData"), line=reg("partialWriteLine"), burstError=reg("writeBurstError");
    S grant=eq(bits(a(3),63,windowBits),c(64-windowBits,index));
    S last=eq(beats,pad(a(4),9)), offset=pad(beats,64);
    for(unsigned size=1;size<=6;++size)offset=mux(eq(a(5),c(3,size)),pad(op("shl",{beats,std::to_string(size)}),64),offset);
    S address=bits(op("add",{a(3),offset}),63,0), currentLine=bits(address,63,6);
    S wrongWindow=Not(eq(bits(address,63,windowBits),c(64-windowBits,index)));
    S pending=Not(eq(mask,c(64,0))), overlap=Not(eq(And(mask,a(9)),c(64,0)));
    S wrongLine=And(pending,Not(eq(line,currentLine)));
    S malformed=Or(overlap,Or(wrongWindow,Or(wrongLine,Or(eq(a(5),c(3,7)),eq(a(9),c(64,0))))));
    S errorNow=Or(burstError,malformed), mergedMask=Or(mask,a(9)), complete=eq(mergedMask,c(64,~uint64_t(0)));
    S merged=mux(bits(a(9),0,0),bits(a(8),7,0),bits(buffered,7,0));
    for(unsigned lane=1;lane<64;++lane)merged=op("cat",{mux(bits(a(9),lane,lane),bits(a(8),8*lane+7,8*lane),bits(buffered,8*lane+7,8*lane)),merged});
    S ready=op("field",{p(index+2),"ready"}), canAccept=Or(errorNow,Or(Not(complete),ready));
    S selectedWReady=And(grant,And(a(1),And(a(10),canAccept)));
    awReady=Or(awReady,And(grant,And(a(7),And(a(10),And(canAccept,last)))));
    wReady=Or(wReady,selectedWReady);
    bValid=Or(bValid,And(grant,And(a(1),And(a(7),And(canAccept,last)))));
    responseError=Or(responseError,And(grant,errorNow));
    S physicalFire=And(selectedWReady,a(7)), goodFire=And(physicalFire,Not(errorNow));
    if(!fieldDriven(index+2,"bits",merged) || !fieldDriven(index+2,"valid",And(grant,And(a(1),And(a(7),And(a(10),And(complete,Not(errorNow))))))))
      return reject("CPU incoming byte assembly or complete-word enqueue differs");
    auto update=[&](StringRef base,S expected){return driven(regs.at(name(base)),expected);};
    if(!update("writeBeatCounter",mux(physicalFire,mux(last,c(9,0),bits(op("add",{beats,c(9,1)}),8,0)),beats)) ||
       !update("partialWriteMask",mux(goodFire,mux(complete,c(64,0),mergedMask),mask)) ||
       !update("partialWriteData",mux(goodFire,merged,buffered)) ||
       !update("partialWriteLine",mux(goodFire,currentLine,line)) ||
       !update("writeBurstError",mux(physicalFire,mux(last,c(1,0),errorNow),burstError)))
      return reject("CPU incoming assembly ownership or physical burst state differs");
    if(expression(assertions[index].getPredicate())!=Not(And(grant,And(a(1),And(a(7),malformed)))) ||
       expression(assertions[index].getEnable())!=Not(p(1)) || assertions[index].getClock()!=module.getArgument(0))
      return reject("CPU incoming malformed-write assertion differs");
  }
  if(!driven(module.getArgument(axi),awReady) || !driven(module.getArgument(axi+6),wReady) ||
     !driven(module.getArgument(axi+11),bValid) || !driven(module.getArgument(axi+12),a(2)) ||
     !driven(module.getArgument(axi+13),mux(responseError,c(2,2),c(2,0))))
    return reject("CPU incoming physical AW/W/B completion or error response differs");
  return success();
}
} // namespace

LogicalResult goldengate::prepareCPUManagedStreamHeader(
    CircuitOp circuit, std::string &error, CPUStreamHeaderBoundary boundary) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  bool selected = boundary == CPUStreamHeaderBoundary::SelectedOutgoing;
  bool bufferedSelected = selected &&
      (circuit.getName() == "GGSelectedPrintCPUStreamWriteBufferWrapper" ||
       circuit.getName() == "GGSelectedPrintCPUStreamReadBufferWrapper" ||
       circuit.getName() == "GGSelectedPrintCPUStreamResponseBufferWrapper" ||
       circuit.getName() == "GGSelectedPrintCPUStreamWriteResponseBufferWrapper" ||
       circuit.getName() == "GGSelectedPrintFASEDTokenWrapper" ||
       circuit.getName() == "GGSelectedPrintFASEDHostOutstandingWrapper" ||
       circuit.getName() == "GGSelectedPrintFASEDIngressARQueueWrapper" ||
       circuit.getName() == "GGSelectedPrintFASEDIngressCreditsWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDIngressOrderWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDIngressIssueWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDReadBufferWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDReadSchedulerWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDWriteEgressWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDResponseReleaserWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDTimingCycleWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDReadLatencyWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDWriteLatencyWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDTimingAWQueueWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDWritePairingWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDWriteRetirementWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDWriteAdmissionWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDReadAdmissionWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDRequestLimitsWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDLatencyRegistersWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDFunctionalModelRegisterWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDResponseErrorsWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDStatisticsWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDHistogramsWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDMMIOWrapper" ||
      circuit.getName() == "GGSelectedPrintFASEDBridgeControlWrapper" ||
       circuit.getName() == "GGSelectedPrintCPUStreamPlatformWrapper");
  if (selected && circuit.getName() != "GGControlMasterWrapper" &&
      circuit.getName() != "GGSelectedPrintCPUStreamWriteBufferWrapper" &&
      circuit.getName() != "GGSelectedPrintCPUStreamReadBufferWrapper" &&
      circuit.getName() != "GGSelectedPrintCPUStreamResponseBufferWrapper" &&
      circuit.getName() != "GGSelectedPrintCPUStreamWriteResponseBufferWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDTokenWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDHostOutstandingWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDIngressARQueueWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDIngressCreditsWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDIngressOrderWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDIngressIssueWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDReadBufferWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDReadSchedulerWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDWriteEgressWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDResponseReleaserWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDTimingCycleWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDReadLatencyWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDWriteLatencyWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDTimingAWQueueWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDWritePairingWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDWriteRetirementWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDWriteAdmissionWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDReadAdmissionWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDRequestLimitsWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDLatencyRegistersWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDFunctionalModelRegisterWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDResponseErrorsWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDStatisticsWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDHistogramsWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDMMIOWrapper" &&
      circuit.getName() != "GGSelectedPrintFASEDBridgeControlWrapper" &&
      circuit.getName() != "GGSelectedPrintCPUStreamPlatformWrapper")
    return reject("selected outgoing CPU header requires the assembled selected control master");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("CPUManagedStreamEngine header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed CPUManagedStreamEngine header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix &&
        suffix == ".peer2peer.const.yaml")
      return reject("CPU stream peer-to-peer address map already exists");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.cpuManagedStreamHeader"))
    return reject("CPUManagedStreamEngine needs one driver header without a prior CPU stream constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid CPUManagedStreamEngine header top");
  auto countPaths = [&](circt::igraph::InstanceGraphNode *target, unsigned limit) {
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
    llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
    bool recursive = false;
    std::function<unsigned(circt::igraph::InstanceGraphNode *)> count = [&](auto *node) {
      if (active.count(node)) { recursive = true; return 0U; }
      auto found = counts.find(node);
      if (found != counts.end()) return found->second;
      active.insert(node);
      unsigned n = node == target;
      for (auto *record : *node) n = std::min(limit, n + count(record->getTarget()));
      active.erase(node); counts[node] = n; return n;
    };
    unsigned n = count(top);
    return recursive ? limit : n;
  };
  auto liveModule = [&](StringRef name) -> FModuleOp {
    auto *target = graph.lookup(StringAttr::get(circuit.getContext(), name));
    if (!target || countPaths(target, 2) != 1) return {};
    return dyn_cast<FModuleOp>(target->getModule().getOperation());
  };
  auto bank = liveModule("GGCPUStreamCountBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("CPUManagedStreamEngine bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "cpuStream_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("CPUManagedStreamEngine read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing CPUManagedStreamEngine control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("CPUManagedStreamEngine_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 4)
    return reject("invalid CPUManagedStreamEngine widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("CPUManagedStreamEngine MMIO region exceeds control address width");
  for (auto [i, attr] : llvm::enumerate(regions)) {
    if (i == uint64_t(slave.getInt())) continue;
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto otherName = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto otherStart = d ? d.getAs<IntegerAttr>("start") : IntegerAttr();
    auto otherSize = d ? d.getAs<IntegerAttr>("size") : IntegerAttr();
    if (!otherName || otherName == name || !otherStart || otherStart.getInt() < 0 ||
        !otherSize || otherSize.getInt() <= 0 ||
        uint64_t(otherStart.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
        uint64_t(otherSize.getInt()) > (uint64_t(1) << *address.getWidth()) - otherStart.getInt() ||
        (uint64_t(otherStart.getInt()) < uint64_t(start.getInt()) + size.getInt() &&
         uint64_t(start.getInt()) < uint64_t(otherStart.getInt()) + otherSize.getInt()))
      return reject("ambiguous CPUManagedStreamEngine MMIO allocation");
  }
  auto uint = [&](unsigned w) { return UIntType::get(circuit.getContext(), w, false); };
  auto fieldType = [](Type t, StringRef name) -> Type {
    auto bundle = dyn_cast_or_null<BundleType>(t);
    auto field = bundle ? bundle.getElement(name) : std::nullopt;
    return field ? field->type : Type();
  };
  auto transport = liveModule("GGCPUStreamRead");
  FModuleOp incoming;
  // Catalog imports retain their own symbol prefix. Identify the writer by
  // its typed contract and unique live path, never by a particular spelling.
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (!module->hasAttr("goldengate.fromHostCPUStreamCount")) continue;
    auto *node = graph.lookup(module.getNameAttr());
    unsigned paths = node ? countPaths(node, 2) : 0;
    if (!paths) continue;
    if (paths != 1 || incoming) return reject("CPU stream header requires one live incoming engine");
    incoming = module;
  }
  if (selected) {
    // Buffered selected boundaries own a real empty writer. Require its live
    // zero-sink contract below; earlier boundaries must still omit all writes.
    if (!bufferedSelected && llvm::any_of(circuit.getOps<FModuleLike>(), [](FModuleLike module) {
          return module.getModuleName() == "GGEmptyCPUStreamWrite" ||
              module->hasAttr("goldengate.fromHostCPUStreamCount");
        }))
      return reject("selected outgoing CPU header cannot omit an existing incoming engine");
    auto module = dyn_cast<FModuleOp>(top->getModule().getOperation());
    if (!module) return reject("selected outgoing CPU header requires a FIRRTL top");
    for (auto p : module.getPorts()) {
      auto name = p.name.getValue();
      if (name.contains("from_cpu_stream") || (!bufferedSelected &&
          (name.starts_with("cpu_stream_aw_") || name.starts_with("cpu_stream_w_") ||
           name.starts_with("cpu_stream_b_"))))
        return reject("selected outgoing CPU header cannot omit incoming CPU stream ports");
    }
  }
  auto sources = transport ? transport->getAttrOfType<ArrayAttr>("goldengate.sourceStreams") : ArrayAttr();
  auto space = transport ? transport->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits") : IntegerAttr();
  auto sinks = incoming ? incoming->getAttrOfType<IntegerAttr>("goldengate.fromHostCPUStreamCount") : IntegerAttr();
  auto sinkAllocations = incoming ? incoming->getAttrOfType<ArrayAttr>("goldengate.sinkStreams") : ArrayAttr();
  auto sinkSpace = incoming ? incoming->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits") : IntegerAttr();
  bool needsIncoming = !selected || bufferedSelected;
  if (!transport || (needsIncoming && !incoming) || !sources || sources.empty() || sources.size() > INT32_MAX / 4 ||
      !space || space.getInt() < 6 || space.getInt() > 30 || (needsIncoming && (!sinks || sinks.getInt() < 0 || sinks.getInt() > INT32_MAX / 4)))
    return reject("CPU stream descriptors need ordered outgoing allocations and an incoming engine");
  bool f2Ack = transport->hasAttr("goldengate.f2CPUWordReads");
  if (f2Ack && failed(validateF2CPUStreamWordReads(transport, error))) return failure();
  unsigned n = sources.size(), incomingCount = needsIncoming ? sinks.getInt() : 0;
  if (incomingCount && (selected || !sinkAllocations || sinkAllocations.size() != incomingCount ||
      !sinkSpace || sinkSpace.getInt() < 6 || sinkSpace.getInt() > 30 ||
      incoming.getNumPorts() != incomingCount + 16))
    return reject("CPU incoming descriptors need ordered write allocations and typed ports");
  if (incomingCount && failed(validateIncomingAssembly(incoming, incomingCount, sinkSpace.getInt(), error))) return failure();
  unsigned total = n + incomingCount, mcrPort = total + 2, addressPort = n + 5;
  if (transport.getNumPorts() != n + 14 + unsigned(f2Ack) || bank.getNumPorts() != total + 3 + unsigned(f2Ack) ||
      transport.getPortName(addressPort) != "ar_bits_addr" ||
      transport.getPortType(addressPort) != uint(64) || transport.getPortDirection(addressPort) != Direction::In ||
      transport.getPortType(n + 11) != uint(512) || bank.getPortName(mcrPort) != "mcr" ||
      bank.getPortType(0) != ClockType::get(circuit.getContext()) || bank.getPortType(1) != uint(1) ||
      uint64_t(size.getInt()) < 4ULL * total)
    return reject("CPU stream transport/count ports or MMIO region differ from allocation");
  for (unsigned i = 0; i < total + 3; ++i)
    if (bank.getPortDirection(i) != (i == mcrPort ? Direction::Out : Direction::In))
      return reject("CPU stream count bank directions differ");
  for (StringRef group : {"read", "write"}) {
    auto slots = dyn_cast_or_null<FVectorType>(fieldType(bank.getPortType(mcrPort), group));
    if (!slots || slots.getNumElements() != total || fieldType(slots.getElementType(), "bits") != uint(32))
      return reject("CPU stream counts require one 32-bit MCR slot per allocation");
  }
  auto arg = [&](FModuleOp m, unsigned i) { return m.getBodyBlock()->getArgument(i); };
  // Carry the full instance path while following aggregate connects. Shared
  // queue definitions must not collapse two independent storage instances.
  using Path = SmallVector<InstanceOp>;
  auto uniquePath = [&](FModuleOp module) {
    Path result;
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, bool> contains;
    std::function<bool(circt::igraph::InstanceGraphNode *)> hasTarget = [&](auto *node) {
      auto found = contains.find(node);
      if (found != contains.end()) return found->second;
      bool yes = node->getModule().getOperation() == module.getOperation();
      for (auto *record : *node) yes |= hasTarget(record->getTarget());
      contains[node] = yes; return yes;
    };
    std::function<bool(circt::igraph::InstanceGraphNode *)> find = [&](auto *node) {
      if (node->getModule().getOperation() == module.getOperation()) return true;
      for (auto *record : *node) {
        if (!hasTarget(record->getTarget())) continue;
        // Only descend into branches containing the requested definition.
        auto inst = dyn_cast<InstanceOp>(record->getInstance().getOperation());
        if (!inst) continue;
        result.push_back(inst);
        if (find(record->getTarget())) return true;
        result.pop_back();
      }
      return false;
    };
    find(top); return result;
  };
  struct Endpoint { FModuleOp queue; Path path; unsigned port = 0; bool namedPort = false; };
  auto trace = [&](Value value, Path path, StringRef sourcePort = {}, StringRef member = {}) -> Endpoint {
    bool namedPort = sourcePort.empty();
    SmallVector<std::pair<Value, Path>> seen;
    while (value && seen.size() < 4096) {
      for (auto &old : seen) if (old.first == value && old.second == path) return {};
      seen.emplace_back(value, path);
      auto *block = value.getParentBlock(); Value driver; unsigned drivers = 0;
      // Print hosts expose flattened valid/bits outputs. Follow the payload
      // member through those scalar connects as well as whole-bundle connects.
      if (!member.empty()) {
        for (auto &op : *block) {
          auto field = dyn_cast<SubfieldOp>(op);
          if (!field || field.getInput() != value || field.getFieldName() != member) continue;
          for (auto &connect : *block) {
            if (auto c = dyn_cast<ConnectOp>(connect)) if (c.getDest() == field.getResult()) { driver = c.getSrc(); ++drivers; }
            if (auto c = dyn_cast<StrictConnectOp>(connect)) if (c.getDest() == field.getResult()) { driver = c.getSrc(); ++drivers; }
          }
        }
        if (drivers) {
          if (drivers != 1) return {};
          value = driver; member = {}; continue;
        }
      }
      for (auto &op : *block) {
        if (auto c = dyn_cast<ConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
        if (auto c = dyn_cast<StrictConnectOp>(op)) if (c.getDest() == value) { driver = c.getSrc(); ++drivers; }
      }
      if (drivers) { if (drivers != 1) return {}; value = driver; continue; }
      if (auto field = value.getDefiningOp<SubfieldOp>()) {
        if (!member.empty()) return {};
        member = field.getFieldName(); value = field.getInput(); continue;
      }
      if (auto a = dyn_cast<BlockArgument>(value)) {
        auto module = dyn_cast<FModuleOp>(a.getOwner()->getParentOp());
        if (!module || module.getPortDirection(a.getArgNumber()) != Direction::In || path.empty()) return {};
        namedPort |= module.getPortName(a.getArgNumber()) == sourcePort;
        auto inst = path.pop_back_val();
        if (inst.getModuleName() != module.getName()) return {};
        value = inst.getResult(a.getArgNumber()); continue;
      }
      auto inst = value.getDefiningOp<InstanceOp>();
      auto *node = inst ? graph.lookup(inst.getModuleNameAttr().getAttr()) : nullptr;
      auto module = node ? dyn_cast<FModuleOp>(node->getModule().getOperation()) : FModuleOp();
      if (!module) return {};
      unsigned port = cast<OpResult>(value).getResultNumber();
      if (module.getPortDirection(port) != Direction::Out) return {};
      namedPort |= module.getPortName(port) == sourcePort;
      path.push_back(inst);
      if (module == incoming && (member == "bits" || member == "valid")) return {module, path, port, namedPort};
      if (module.getNumPorts() == 5 && !module.getOps<MemOp>().empty()) {
        if (!sourcePort.empty() && member != "bits") return {};
        return {module, path, port, namedPort};
      }
      value = arg(module, port);
    }
    return {};
  };
  auto bankPath = uniquePath(bank), transportPath = uniquePath(transport);
  auto isSlot = [&](Value v, StringRef groupName, StringRef member, unsigned index) {
    auto f = v ? v.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto s = f ? f.getInput().getDefiningOp<SubindexOp>() : SubindexOp();
    auto g = s ? s.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return f && f.getFieldName() == member && s && s.getIndex() == index &&
        g && g.getFieldName() == groupName && g.getInput() == arg(bank, mcrPort);
  };
  auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  if (!registers || registers.size() != total) return reject("CPU stream count allocation size differs");
  llvm::StringSet<> names, ports;
  SmallVector<Endpoint> queues;
  SmallVector<DictionaryAttr> allocations;
  uint64_t maxDepth = 0, maxIncomingDepth = 0;
  SmallVector<Attribute> ordered(sources.begin(), sources.end());
  if (incomingCount) ordered.append(sinkAllocations.begin(), sinkAllocations.end());
  for (auto [i, attr] : llvm::enumerate(ordered)) {
    bool fromHost = i >= n;
    unsigned localIndex = fromHost ? i - n : i, directionCount = fromHost ? incomingCount : n;
    auto engine = fromHost ? incoming : transport;
    unsigned windowBits = fromHost ? sinkSpace.getInt() : space.getInt();
    uint64_t windowBytes = uint64_t(1) << windowBits;
    auto source = dyn_cast<DictionaryAttr>(attr);
    auto name = source ? source.getAs<StringAttr>("name") : StringAttr();
    auto port = source ? source.getAs<StringAttr>("port") : StringAttr();
    auto index = source ? source.getAs<IntegerAttr>("index") : IntegerAttr();
    auto depth = source ? source.getAs<IntegerAttr>("depth") : IntegerAttr();
    auto bytes = source ? source.getAs<IntegerAttr>("widthBytes") : IntegerAttr();
    auto base = source ? source.getAs<IntegerAttr>("bufferBaseAddress") : IntegerAttr();
    if (!name || name.getValue().empty() || name.getValue().contains('\0') || !names.insert(name.getValue()).second ||
        !port || port.getValue().empty() || port.getValue().contains('\0') || !ports.insert(port.getValue()).second ||
        !index || index.getInt() != int64_t(localIndex) || !depth || depth.getInt() <= 0 || depth.getInt() > INT32_MAX / 64 ||
        !bytes || bytes.getInt() != 64 || !base || base.getInt() != int64_t(localIndex * windowBytes) || base.getInt() > INT32_MAX ||
        engine.getPortName(localIndex + 2) != (directionCount == 1 ? "stream" : "stream_" + std::to_string(localIndex)) ||
        engine.getPortDirection(localIndex + 2) != (fromHost ? Direction::Out : Direction::In) ||
        fieldType(engine.getPortType(localIndex + 2), "bits") != uint(512))
      return reject("CPU stream identity, order, capacity or DMA base differs from native allocation");
    auto &maximum = fromHost ? maxIncomingDepth : maxDepth;
    maximum = std::max(maximum, uint64_t(depth.getInt()));
    auto count = trace(arg(bank, i + 2), bankPath);
    Endpoint data;
    if (fromHost) {
      if (count.queue) data = trace(arg(count.queue, 2), count.path, port.getValue(), "bits");
    } else data = trace(arg(transport, localIndex + 2), transportPath, port.getValue(), "bits");
    bool sameData = fromHost ? data.queue == incoming && data.port == localIndex + 2 &&
        data.path == uniquePath(incoming) : data.port == 3 && data.path == count.path;
    if (!data.queue || !count.queue || !data.namedPort || count.port != 4 || !sameData)
      return reject("CPU stream count and transport must forward the same live queue instance");
    for (auto &old : queues) if (old.path == count.path)
      return reject("CPU stream allocations cannot share one queue instance");
    auto queue = count.queue;
    unsigned countBits = llvm::Log2_64_Ceil(uint64_t(depth.getInt()) + 1);
    if (queue.getPortType(0) != ClockType::get(circuit.getContext()) || queue.getPortType(1) != uint(1) ||
        fieldType(queue.getPortType(2), "bits") != uint(512) || fieldType(queue.getPortType(3), "bits") != uint(512) ||
        queue.getPortType(4) != uint(countBits) || bank.getPortType(i + 2) != uint(countBits) ||
        bank.getPortName(i + 2) != (total == 1 ? "count" : "count_" + std::to_string(i)))
      return reject("CPU stream descriptor width differs from native queue/count ports");
    for (unsigned p = 0; p < 5; ++p)
      if (queue.getPortDirection(p) != (p < 3 ? Direction::In : Direction::Out))
        return reject("CPU queue directions differ");
    auto legacy = queue->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
    if (queue->hasAttr("goldengate.streamParameters") && !legacy)
      return reject("malformed CPU queue identity");
    if (legacy) {
      if (legacy.getAs<StringAttr>("name") != name)
        return reject("CPU queue identity differs from transport allocation");
      for (StringRef key : {"depth", "widthBytes"}) {
        auto actual = legacy.getAs<IntegerAttr>(key);
        if (!actual || actual.getInt() != source.getAs<IntegerAttr>(key).getInt())
          return reject("CPU queue identity differs from transport allocation");
      }
    }
    // Queue creation precedes the shared stream catalog. Its legacy index is
    // local to that creator (TracerV starts at zero even when NIC is first).
    // Driver order comes from the transport allocation and live DMA selector.
    if (legacy && legacy.get("index")) {
      auto local = legacy.getAs<IntegerAttr>("index");
      if (!local || local.getInt() < 0) return reject("malformed legacy queue index");
    }
    unsigned memories = 0;
    for (auto mem : queue.getOps<MemOp>()) {
      if (mem.getDepth() != uint64_t(depth.getInt()) || mem.getDataType() != uint(512) ||
          mem.getReadLatency() != 0 || mem.getWriteLatency() != 1)
        return reject("CPU stream descriptor capacity differs from native queue storage");
      ++memories;
    }
    if (memories != 1) return reject("CPU stream needs one native queue memory");
    if (fromHost) {
      // The instance catalog identifies the bridge-facing queue, while SSA
      // tracing above proves its count and payload refer to the same storage.
      auto instanceInfo = count.path.back()->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
      if (!instanceInfo || instanceInfo.getAs<StringAttr>("name") != name ||
          instanceInfo.getAs<StringAttr>("direction") != "fromHostCPU" ||
          instanceInfo.getAs<IntegerAttr>("depth") != depth || instanceInfo.getAs<IntegerAttr>("widthBytes") != bytes)
        return reject("CPU incoming queue identity differs from write allocation");
      auto valid = trace(arg(queue, 2), count.path, port.getValue(), "valid");
      if (valid.queue != incoming || valid.path != data.path || valid.port != data.port || !valid.namedPort)
        return reject("CPU incoming queue valid differs from its write transport");
      unsigned axi = incomingCount + 2;
      for (auto [offset, width] : {std::make_pair(3U,64U), {8U,512U}})
        if (incoming.getPortType(axi + offset) != uint(width) ||
            incoming.getPortDirection(axi + offset) != Direction::In)
          return reject("CPU incoming AXI address or payload type differs");
      Value grant; unsigned grants = 0;
      for (auto eq : incoming.getOps<EQPrimOp>()) {
        auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
        if (!bits || bits.getInput() != arg(incoming, axi + 3)) continue;
        auto selector = eq.getRhs().getDefiningOp<ConstantOp>();
        if (bits.getHi() != 63 || bits.getLo() != windowBits || !selector ||
            selector.getValue().getLimitedValue() >= incomingCount)
          return reject("CPU incoming DMA address grant differs from ordered windows");
        if (selector.getValue().getLimitedValue() == localIndex) { grant = eq; ++grants; }
      }
      if (grants != 1) return reject("CPU incoming allocation needs one DMA selector");
      // Full byte assembly, per-stream state and physical AW/W/B are checked
      // above from live SSA before any descriptor/header mutation.
    } else {
      bool f2Words = transport->hasAttr("goldengate.f2CPUWordReads");
      if (f2Words && failed(validateF2CPUStreamWordReads(transport, error))) return failure();
      unsigned grants = 0; Value grant;
      for (auto eq : transport.getOps<EQPrimOp>()) {
        auto bits = eq.getLhs().getDefiningOp<BitsPrimOp>();
        if (!bits || bits.getInput() != arg(transport, addressPort)) continue;
        auto selector = eq.getRhs().getDefiningOp<ConstantOp>();
        if (f2Words && selector && bits.getLo() == 0 &&
            bits.getHi() >= 1 && bits.getHi() <= 4 && selector.getValue() == 0)
          continue; // Canonical low-address tests belong to the validated F2 profile.
        if (bits.getHi() != 63 || bits.getLo() != space.getInt() || !selector || selector.getValue().getLimitedValue() >= n)
          return reject("CPU stream DMA address grant differs from ordered windows");
        if (selector.getValue().getLimitedValue() == i) { ++grants; grant = eq.getResult(); }
      }
      if (grants != 1) return reject("CPU stream needs one DMA address grant per allocation");
      // Check that this selector actually controls this queue's dequeue handshake.
      // An unused equality or swapped selector cannot authorize its descriptor.
      unsigned readyDrivers = 0;
      for (auto c : transport.getOps<StrictConnectOp>()) {
        auto field = c.getDest().getDefiningOp<SubfieldOp>();
        if (!field || field.getFieldName() != "ready" || field.getInput() != arg(transport, i + 2)) continue;
        Value ready = c.getSrc();
        if (f2Words) ready = ready.getDefiningOp<OrPrimOp>().getLhs().getDefiningOp<AndPrimOp>().getLhs();
        auto outer = ready.getDefiningOp<AndPrimOp>();
        auto inner = outer ? outer.getRhs().getDefiningOp<AndPrimOp>() : AndPrimOp();
        if (!outer || outer.getLhs() != grant || !inner || inner.getLhs() != arg(transport, n + 3) ||
            inner.getRhs() != arg(transport, n + 8))
          return reject("CPU stream dequeue handshake differs from its DMA selector");
        ++readyDrivers;
      }
      if (readyDrivers != 1) return reject("CPU stream dequeue ready has missing or ambiguous drivers");
      unsigned payloads = 0;
      auto isPayload = [&](Value value) {
        auto field = value.getDefiningOp<SubfieldOp>();
        return field && field.getFieldName() == "bits" && field.getInput() == arg(transport, i + 2);
      };
      for (auto c : transport.getOps<StrictConnectOp>()) if (c.getDest() == arg(transport, n + 11)) {
        if (n == 1) { if (isPayload(c.getSrc())) ++payloads; }
        else {
          Value value = c.getSrc();
          while (auto mux = value.getDefiningOp<MuxPrimOp>()) {
            if (mux.getSel() == grant && isPayload(mux.getHigh())) ++payloads;
            value = mux.getLow();
          }
        }
      }
      if (payloads != 1) return reject("CPU stream read payload differs from its DMA selector");
    }
    auto word = dyn_cast<DictionaryAttr>(registers[i]);
    auto offset = word ? word.getAs<IntegerAttr>("offset") : IntegerAttr();
    auto readable = word ? word.getAs<BoolAttr>("readable") : BoolAttr();
    auto writeable = word ? word.getAs<BoolAttr>("writeable") : BoolAttr();
    if (!word || word.getAs<StringAttr>("name") != name.getValue().str() + "_count" || !offset || offset.getInt() != int64_t(4 * i) ||
        !readable || !readable.getValue() || !writeable || writeable.getValue() != (f2Ack && i < n))
      return reject("CPU stream driver count word differs from ordered read-only MMIO allocation");
    unsigned readsCount = 0, valids = 0, readies = 0, assertions = 0;
    for (auto connect : bank.getOps<StrictConnectOp>()) {
      if (isSlot(connect.getDest(), "read", "bits", i)) {
        auto pad = connect.getSrc().getDefiningOp<PadPrimOp>();
        if (!pad || pad.getInput() != arg(bank, i + 2) || pad.getAmount() != 32)
          return reject("CPU stream count read does not expose the allocated live queue count");
        ++readsCount;
      }
      if (isSlot(connect.getDest(), "read", "valid", i) || isSlot(connect.getDest(), "write", "ready", i)) {
        auto one = connect.getSrc().getDefiningOp<ConstantOp>();
        if (!one || !one.getValue().isOne()) return reject("CPU stream count MCR slot must always be available");
        if (isSlot(connect.getDest(), "read", "valid", i)) ++valids; else ++readies;
      }
    }
    for (auto a : bank.getOps<AssertOp>()) {
      auto pred = a.getPredicate().getDefiningOp<NotPrimOp>();
      auto enable = a.getEnable().getDefiningOp<NotPrimOp>();
      if (!pred || !isSlot(pred.getInput(), "write", "valid", i)) continue;
      if (a.getClock() != arg(bank, 0) || !enable || enable.getInput() != arg(bank, 1))
        return reject("CPU stream count must reject writes outside host reset");
      ++assertions;
    }
    if (readsCount != 1 || valids != 1 || readies != 1 || assertions != unsigned(!(f2Ack && i < n)))
      return reject("CPU stream count has missing or ambiguous MMIO drivers");
    queues.push_back(count); allocations.push_back(source);
  }
  if (llvm::Log2_64_Ceil(64 * maxDepth) != unsigned(space.getInt()))
    return reject("CPU stream DMA window differs from maximum outgoing capacity");
  if (incomingCount && llvm::Log2_64_Ceil(64 * maxIncomingDepth) != unsigned(sinkSpace.getInt()))
    return reject("CPU incoming DMA window differs from maximum incoming capacity");
  for (auto &endpoint : queues) {
    unsigned allocated = llvm::count_if(queues, [&](const auto &q) { return q.queue == endpoint.queue; });
    auto *node = graph.lookup(endpoint.queue.getNameAttr());
    if (countPaths(node, allocated + 1) != allocated)
      return reject("CPU queue has live instances outside the CPU stream allocation");
  }
  // Empty incoming vectors are meaningful ABI state; check the actual transport.
  if (needsIncoming && !incomingCount) for (StringRef port : {"aw_ready", "w_ready", "b_valid"}) {
    std::optional<unsigned> n;
    for (auto [i, p] : llvm::enumerate(incoming.getPorts())) if (p.name == port) n = i;
    if (!n || incoming.getPortType(*n) != uint(1) || incoming.getPortDirection(*n) != Direction::Out)
      return reject("CPU incoming stream boundary differs");
    unsigned matches = 0;
    for (auto c : incoming.getOps<StrictConnectOp>()) if (c.getDest() == arg(incoming, *n)) {
      auto zero = c.getSrc().getDefiningOp<ConstantOp>();
      if (!zero || !zero.getValue().isZero()) return reject("CPU incoming stream vector is not empty");
      ++matches;
    }
    if (matches != 1) return reject("CPU incoming stream boundary has ambiguous drivers");
  }
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/cpu_managed_stream.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_MANAGED_STREAM_CONSTRUCTOR\nregistry.add_widget(new CPUManagedStreamWidget(\n"
         "  simif,\n  " << widgetIndex << ",\n  args,\n"
         "  std::vector<CPUManagedStreams::StreamParameters>{";
  auto emit = [&](unsigned begin, unsigned end) {
    if (begin == end) { out << "}"; return; }
    out << '\n';
    for (unsigned i = begin; i < end; ++i) {
      auto source = allocations[i];
      out << "    CPUManagedStreams::StreamParameters(std::string(\"";
      for (unsigned char ch : source.getAs<StringAttr>("name").getValue()) {
        if (ch == '"' || ch == '\\') out << '\\' << char(ch);
        else if (ch >= 32 && ch < 127) out << char(ch);
        else out << '\\' << char('0' + ((ch >> 6) & 7)) << char('0' + ((ch >> 3) & 7)) << char('0' + (ch & 7));
      }
      out << "\"), " << source.getAs<IntegerAttr>("bufferBaseAddress").getInt() << "ULL, "
          << start.getInt() + 4 * i << "ULL, " << source.getAs<IntegerAttr>("depth").getInt()
          << "U, " << source.getAs<IntegerAttr>("widthBytes").getInt() << "U"
          << ((f2Ack && i < n) ? ", true)" : ")") << (i + 1 == end ? "\n" : ",\n");
    }
    out << "  }";
  };
  emit(n, total);
  out << ",\n  std::vector<CPUManagedStreams::StreamParameters>{";
  emit(0, n);
  out << "\n));\n#endif // GET_MANAGED_STREAM_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body", b.getStringAttr(previous.getValue().str() + snippet));
  updated.set("goldengate.cpuManagedStreamHeader", b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(), raw.end()); annotations[outputIndex] = updated.getDictionary(circuit.getContext());
  // SimulationMapping/CPUManagedStreamEngine emit source entries before sink
  // entries. Reuse the same validated allocations as the driver constructor;
  // no target-specific names, capacities or addresses belong in this map.
  std::string peerMap;
  llvm::raw_string_ostream peer(peerMap);
  for (auto allocation : allocations) {
    // JSON strings are valid YAML quoted keys and preserve arbitrary driver
    // names (including quotes, backslashes and embedded newlines).
    peer << llvm::json::Value(allocation.getAs<StringAttr>("name").getValue()) << ":\n"
         << "  bufferBaseAddress: " << allocation.getAs<IntegerAttr>("bufferBaseAddress").getInt() << '\n'
         << "  bufferCapacity:    " << allocation.getAs<IntegerAttr>("depth").getInt() << '\n'
         << "  bufferWidthBytes:  " << allocation.getAs<IntegerAttr>("widthBytes").getInt() << '\n';
  }
  if (f2Ack) peer << "# F2 source-count write1 ACK validated against live source-only wiring\n";
  peer.flush();
  annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix", b.getStringAttr(".peer2peer.const.yaml")),
      b.getNamedAttr("body", b.getStringAttr(peerMap))}));
  if (f2Ack) {
    llvm::json::Array sourceACKs;
    for (unsigned i=0; i<n; ++i) sourceACKs.push_back(llvm::json::Object{
      {"name", allocations[i].getAs<StringAttr>("name").getValue()},
      {"index", int64_t(i)}, {"countAddress", start.getInt()+4*i},
      {"ackWriteValue", int64_t(1)}, {"beatBytes", int64_t(64)}});
    llvm::json::Value protocol(llvm::json::Object{
      {"version", int64_t(2)}, {"platform", "f2"},
      {"protocol", "source-count-write1-ack"},
      {"liveHierarchyAndPredicatesValidated", true},
      {"controlWriteStrobesValidated", true},
      {"pciReadsNonDestructive", true}, {"sources", std::move(sourceACKs)}});
    std::string text; llvm::raw_string_ostream stream(text); stream << protocol; stream.flush();
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::OutputFile)),
      b.getNamedAttr("fileSuffix", b.getStringAttr(".f2-cpu-stream-protocol.json")),
      b.getNamedAttr("body", b.getStringAttr(text))}));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return success();
}
