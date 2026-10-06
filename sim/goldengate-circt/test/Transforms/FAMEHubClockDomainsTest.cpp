// See LICENSE for license details.
// Oracle: FAMEUtils.portsByInputChannel and FAMETransform.clockMetadata.
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEClockGate.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/XDCEmission.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <iterator>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void print(const goldengate::FAMEHubClockDomain &d) {
  llvm::outs() << "DOMAIN " << d.modelClockName << ' ' << d.payloadField
               << ' ' << d.clockInfo.name << ' ' << d.clockInfo.multiplier
               << ' ' << d.clockInfo.divisor << ' ' << d.clockInfo.mfmr << '\n';
}
void fixture(MLIRContext &ctx, bool reversed) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Top" {
    firrtl.module @Top(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, in %bridge_clocks_0: !firrtl.clock,
        in %bridge_clocks_1: !firrtl.clock) {}
    firrtl.module @Model(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, in %bridge_clocks_1: !firrtl.clock,
        in %bridge_clocks_0: !firrtl.clock) {
      %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      %state0 = firrtl.reg %bridge_clocks_0 : !firrtl.clock, !firrtl.uint<1>
      %state1 = firrtl.reg %bridge_clocks_1 : !firrtl.clock, !firrtl.uint<1>
    }
  } })mlir", &ctx);
  require(bool(root), "fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto model = *std::next(circuit.getOps<FModuleOp>().begin());
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", b.getArrayAttr({}));
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  // Declaration, connect statement, and annotation orders are independent.
  for (unsigned i : {2u, 3u, 1u, 0u})
    b.create<StrictConnectOp>(top.getLoc(), instance.getResult(i),
                             top.getArgument(i >= 2 ? 5 - i : i));
  std::string error;
  auto hierarchy = goldengate::analyzeTopHierarchy(circuit, error);
  require(bool(hierarchy), error);
  goldengate::ModelPortGroup group{"bridge_clocks", model, Direction::In,
                                   std::nullopt, {3, 2}};
  goldengate::GGChannelConnection channel;
  channel.name = "bridge_clocks";
  channel.kind = goldengate::ChannelKind::TargetClock;
  channel.sinks = {{circuit, top, 2, 0, "bridge_clocks_0"},
                   {circuit, top, 3, 0, "bridge_clocks_1"}};
  channel.targetClocks = {{"domain0", 1, 2, 2}, {"domain1", 1, 3, 3}};
  if (reversed) {
    std::reverse(group.ports.begin(), group.ports.end());
    std::reverse(channel.sinks.begin(), channel.sinks.end());
  }
  auto bindings = goldengate::bindChannelToModels(
      channel, *hierarchy, llvm::ArrayRef<goldengate::ModelPortGroup>(&group, 1), error);
  require(bindings && bindings->size() == 1, error);
  require(bindings->front().instancePorts == SmallVector<unsigned>({2, 3}),
          "fixture did not expose the sorted-binding hazard");
  auto domains = goldengate::analyzeFAMEHubClockDomains(
      channel, *hierarchy, bindings->front(), error);
  require(domains && domains->size() == 2, error);
  for (unsigned i = 0; i < 2; ++i) {
    const auto &d = (*domains)[i];
    unsigned clock = reversed ? 1 - i : i;
    require(d.modelPort == 3 - clock && d.topPort == 2 + clock &&
                d.modelClockName == "bridge_clocks_" + std::to_string(clock) &&
                d.payloadField == "_" + std::to_string(clock) &&
                d.clockInfo.name == "domain" + std::to_string(i) &&
                d.clockInfo.mfmr == i + 2, "ordered clock association differs");
    print(d);
  }
  // Reject malformed analysis inputs before any mutation. Exercise both
  // stale identities and metadata/order mismatches, with otherwise valid IR.
  for (unsigned bad = 0; bad < 13; ++bad) {
    auto savedChannel = channel;
    auto savedGroup = group;
    auto savedHierarchy = *hierarchy;
    auto savedBinding = bindings->front();
    switch (bad) {
    case 0: channel.targetClocks.pop_back(); break;
    case 1: group.ports[1] = group.ports[0]; break;
    case 2: std::reverse(group.ports.begin(), group.ports.end()); break;
    case 3: channel.sinks[1] = channel.sinks[0]; break;
    case 4: group.clockPort = 0; break;
    case 5: group.direction = Direction::Out; break;
    case 6: channel.targetClocks[1].divisor = 0; break;
    case 7: channel.targetClocks[0].name.clear(); break;
    case 8: channel.sources.push_back(channel.sinks[0]); break;
    case 9: group.ports[0] = 1; break;
    case 10: hierarchy->connections.clear(); break;
    case 11: bindings->front().instancePorts[0] = 1; break;
    case 12: channel.sinks[0].fieldID = 1; break;
    }
    auto before = dump(*root);
    error.clear();
    require(!goldengate::analyzeFAMEHubClockDomains(
                channel, *hierarchy, bindings->front(), error) &&
                !error.empty() && dump(*root) == before,
            "invalid hub clock plan accepted or changed IR");
    channel = savedChannel;
    group = savedGroup;
    *hierarchy = savedHierarchy;
    bindings->front() = savedBinding;
  }
  // Repeated exact RationalClock keys use the last MFMR, matching Scala toMap.
  auto savedInfo = channel.targetClocks[1];
  channel.targetClocks[1] = channel.targetClocks[0];
  channel.targetClocks[1].mfmr = 7;
  error.clear();
  auto repeated = goldengate::analyzeFAMEHubClockDomains(
      channel, *hierarchy, bindings->front(), error);
  require(repeated && (*repeated)[0].clockInfo.mfmr == 7 &&
              (*repeated)[1].clockInfo.mfmr == 7, "Scala clock MFMR map differs");
  channel.targetClocks[1] = savedInfo;
  auto plan = goldengate::analyzeFAMEPorts(*hierarchy, *bindings, {channel},
                                         {model}, error);
  require(plan && plan->sinks.size() == 1, error);
  require(succeeded(goldengate::rewriteFAMEInputChannel(
              *hierarchy, plan->sinks.front(), error)), error);
  b.setInsertionPointToStart(model.getBodyBlock());
  unsigned tokenPort = 0;
  while (tokenPort < model.getNumPorts() &&
         model.getPortName(tokenPort) != "bridge_clocks_sink") ++tokenPort;
  require(tokenPort < model.getNumPorts(), "channelized clock input missing");
  auto bits = b.create<SubfieldOp>(model.getLoc(), model.getArgument(tokenPort), "bits");
  // Consume the captured records after scalar port erasure. The original
  // numeric indices are deliberately never consulted at this boundary.
  for (const auto &d : *domains) {
    auto token = b.create<SubfieldOp>(model.getLoc(), bits.getResult(), d.payloadField);
    auto flag = b.create<AsUIntPrimOp>(model.getLoc(), token.getResult());
    require(succeeded(goldengate::addFAMEClockEnable(
                model, d.modelClockName, flag.getResult(), error)), error);
    require(succeeded(goldengate::addFAMEClockGate(
                circuit, model, d.modelClockName, error, token.getResult())), error);
    require(succeeded(goldengate::addFAMEClockConstraint(
                circuit, model, d.modelClockName, d.clockInfo, error)), error);
  }
  goldengate::FAMEClockGateIndex gates;
  require(succeeded(gates.collect(model, error)), error);
  for (const auto &d : *domains) {
    auto gate = gates.lookup(d.modelClockName, false);
    require(bool(gate), "ordered clock gate missing");
    auto metadata = gate->getAttrOfType<DictionaryAttr>("goldengate.generatedClockConstraint");
    require(metadata && metadata.getAs<StringAttr>("name").getValue() == d.clockInfo.name &&
                metadata.getAs<IntegerAttr>("mfmr").getUInt() == d.clockInfo.mfmr,
            "gate constraint was assigned to the wrong ordered clock");
  }
  for (auto state : model.getOps<RegOp>()) {
    auto clockName = state.getName() == "state0" ? "bridge_clocks_0" : "bridge_clocks_1";
    auto gate = gates.lookup(clockName, false);
    require(gate && state.getClockVal() == gate.getResult(2),
            "ordered plan gated the wrong domain");
  }
  require(succeeded(verify(*root)), "ordered hub construction invalid");
}

void rocket(MLIRContext &ctx, const char *boundary, const char *golden) {
  auto root = parseSourceFile<ModuleOp>(boundary, &ctx);
  require(bool(root), "Rocket host-control boundary parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  std::string error;
  auto hierarchy = goldengate::analyzeTopHierarchy(circuit, error);
  require(bool(hierarchy), error);
  std::optional<goldengate::GGChannelConnection> channel;
  SmallVector<goldengate::ModelPortGroup> groups;
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    Annotation anno(attr);
    if (anno.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
      auto candidate = goldengate::analyzeChannelConnection(circuit, anno, error);
      require(bool(candidate), error);
      if (candidate->kind == goldengate::ChannelKind::TargetClock) channel = *candidate;
    } else if (anno.isClass(goldengate::AnnotationClasses::ChannelPorts)) {
      auto group = goldengate::analyzeModelPortGroup(circuit, anno, error);
      require(bool(group), error);
      groups.push_back(*group);
    }
  }
  require(bool(channel), "Rocket clock channel missing");
  auto bindings = goldengate::bindChannelToModels(*channel, *hierarchy, groups, error);
  require(bindings && bindings->size() == 1, error);
  auto domains = goldengate::analyzeFAMEHubClockDomains(
      *channel, *hierarchy, bindings->front(), error);
  require(domains && domains->size() == 1, error);
  const auto &d = domains->front();
  require(d.modelClockName == "clockBridge_clocks_0" &&
              d.topClockName == "clockBridge_clocks_0" && d.payloadField.empty(),
          "Rocket scalar domain identity differs");
  auto file = llvm::MemoryBuffer::getFile(golden);
  require(bool(file), "golden annotation file missing");
  auto json = llvm::json::parse((*file)->getBuffer());
  require(bool(json) && json->getAsArray(), "golden annotation JSON invalid");
  unsigned matched = 0;
  for (const auto &value : *json->getAsArray()) {
    const auto *anno = value.getAsObject();
    auto *channels = anno ? anno->getArray("bridgeChannels") : nullptr;
    if (!channels) continue;
    for (const auto &entry : *channels) {
      const auto *ch = entry.getAsObject();
      if (!ch || ch->getString("class") != "firesim.lib.bridgeutils.ClockBridgeChannel")
        continue;
      auto *clocks = ch->getArray("clocks");
      auto *mfmrs = ch->getArray("clockMFMRs");
      auto *sinks = ch->getArray("sinks");
      require(clocks && clocks->size() == 1 && mfmrs && mfmrs->size() == 1 &&
                  sinks && sinks->size() == 1, "golden clock arity differs");
      const auto *info = (*clocks)[0].getAsObject();
      require(info && info->getString("name") == d.clockInfo.name &&
                  info->getInteger("multiplier") == d.clockInfo.multiplier &&
                  info->getInteger("divisor") == d.clockInfo.divisor &&
                  (*mfmrs)[0].getAsInteger() == d.clockInfo.mfmr,
              "Rocket ordered clock metadata differs from immutable SFC annotation");
      ++matched;
    }
  }
  require(matched == 1, "golden clock bridge not uniquely matched");
  print(d);
  llvm::outs() << "Rocket scalar clock identity, ratio and MFMR match immutable SFC annotation\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    if (argc == 2 && StringRef(argv[1]) == "--reversed") fixture(ctx, true);
    else if (argc == 3) rocket(ctx, argv[1], argv[2]);
    else fixture(ctx, false);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
