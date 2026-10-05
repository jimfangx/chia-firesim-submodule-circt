// See LICENSE for license details.
// FPGATop.HostPortIOConnectChannels2Port for PrintBridge's input wire channels.
#include "goldengate/PrintBridgePayload.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include <functional>
#include <set>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::bindPrintBridgeHosts(CircuitOp circuit,
    ArrayRef<FModuleOp> hosts, std::string &error) {
  if (hosts.empty()) return success();
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  constexpr StringLiteral wrapperName = "GGPrintBridgeHostWrapper";
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("PrintBridge binding requires retained annotations");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName) return reject("PrintBridge binding already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  if (!inner) return reject("PrintBridge binding requires an active FIRRTL top");
  bool used = false;
  circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("PrintBridge binding requires an uninstantiated top");
  auto *ctx = circuit.getContext(); OpBuilder b(ctx);
  auto bit = UIntType::get(ctx, 1), count = UIntType::get(ctx, 13);
  auto clock = ClockType::get(ctx);
  auto stream = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
      {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, UIntType::get(ctx, 512)}});
  std::optional<unsigned> hostClock, hostReset;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) {
    if (p.name == "hostClock" && p.type == clock && p.direction == Direction::In) hostClock = i;
    if (p.name == "hostReset" && p.type == bit && p.direction == Direction::In) hostReset = i;
  }
  if (!hostClock || !hostReset) return reject("PrintBridge binding requires hostClock and hostReset inputs");
  struct Leaf { std::string local; SmallVector<unsigned> fields; Type type; unsigned port, annotation; };
  struct Binding { FModuleOp host; unsigned annotation; SmallVector<Leaf> leaves; };
  SmallVector<Binding> bindings;
  std::set<unsigned> consumed;
  llvm::StringSet<> hostNames, globals, outputNames;
  for (auto p : inner.getPorts()) outputNames.insert(p.name.getValue());
  const char *names[] = {"hostClock", "hostReset", "hValid", "hBits", "hReady", "fromHostValid",
      "currentCycle", "enable", "streamReady", "streamValid", "streamData", "ctrl", "streamCount"};
  for (auto [index, item] : llvm::enumerate(hosts)) {
    FModuleOp host = item;
    if (!host || host->getParentOp() != circuit || host == inner || !hostNames.insert(host.getName()).second)
      return reject("PrintBridge binding requires distinct local queued hosts");
    auto info = host->getAttrOfType<DictionaryAttr>("goldengate.printHost");
    auto target = info ? info.getAs<StringAttr>("bridgeTarget") : StringAttr{};
    auto reset = info ? info.getAs<StringAttr>("resetPortName") : StringAttr{};
    auto depth = info ? info.getAs<IntegerAttr>("queueDepth") : IntegerAttr{};
    auto queueName = info ? info.getAs<StringAttr>("queueModule") : StringAttr{};
    if (!target || !reset || !queueName || !depth || depth.getInt() != 6144 || host.getNumPorts() != 13)
      return reject("PrintBridge binding requires queued host identity and capacity");
    FModuleOp queue; unsigned queueInstances = 0;
    for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == queueName) queue = m;
    for (auto i : host.getOps<InstanceOp>()) if (i.getModuleName() == queueName) ++queueInstances;
    if (!queue || queueInstances != 1 || queue.getNumPorts() != 5 ||
        queue.getPortType(0) != clock || queue.getPortDirection(0) != Direction::In ||
        queue.getPortType(1) != bit || queue.getPortDirection(1) != Direction::In ||
        queue.getPortType(2) != stream || queue.getPortDirection(2) != Direction::In ||
        queue.getPortType(3) != stream || queue.getPortDirection(3) != Direction::Out ||
        queue.getPortType(4) != count || queue.getPortDirection(4) != Direction::Out)
      return reject("PrintBridge binding requires the local queued stream and count");
    unsigned memories = 0;
    for (auto memory : queue.getOps<MemOp>()) {
      ++memories;
      if (memory.getDepth() != 6144 || memory.getDataType() != UIntType::get(ctx, 512) ||
          memory.getReadLatency() != 0 || memory.getWriteLatency() != 1)
        return reject("PrintBridge binding requires the 6144x512 CPU queue RAM");
    }
    if (memories != 1) return reject("PrintBridge binding requires one outgoing queue RAM");
    for (unsigned i = 0; i < 13; ++i) {
      auto p = host.getPorts()[i];
      Type expected = i == 0 ? Type(clock) : i == 6 ? Type(UIntType::get(ctx, 64)) :
          i == 10 ? Type(UIntType::get(ctx, 512)) : i == 12 ? Type(count) : Type(bit);
      auto direction = i < 4 || i == 8 || i == 11 ? Direction::In : Direction::Out;
      if (p.name != names[i] || p.direction != direction ||
          (i != 3 && i != 11 && p.type != expected) ||
          ((i == 3 || i == 11) && !isa<BundleType>(p.type)))
        return reject("PrintBridge binding queued host port contract mismatch");
    }
    bool hostUsed = false;
    circuit.walk([&](InstanceOp i) { hostUsed |= i.getModuleName() == host.getName(); });
    if (hostUsed) return reject("PrintBridge queued host already instantiated");
    Binding binding{host, 0, {}}; DictionaryAttr bridge;
    for (auto [i, attr] : llvm::enumerate(raw)) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto key = d ? d.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr{};
      if (d && d.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO &&
          d.getAs<StringAttr>("widgetClass") == AnnotationClasses::PrintBridgeModule &&
          d.getAs<StringAttr>("target") == target && key && key.getAs<StringAttr>("resetPortName") == reset) {
        if (bridge) return reject("ambiguous PrintBridge constructor identity");
        bridge = d; binding.annotation = i;
      }
    }
    auto key = bridge ? bridge.getAs<DictionaryAttr>("widgetConstructorKey") : DictionaryAttr{};
    auto mapping = bridge ? bridge.getAs<DictionaryAttr>("channelMapping") : DictionaryAttr{};
    if (!mapping || !key || key.getAs<StringAttr>("class") != AnnotationClasses::PrintBridgeParameters)
      return reject("PrintBridge binding requires matching constructor and channel mapping");
    auto payload = cast<FIRRTLBaseType>(host.getPortType(3));
    if (!payload.isPassive()) return reject("PrintBridge binding requires an input-only payload");
    auto bag = cast<BundleType>(payload);
    auto records = key.getAs<ArrayAttr>("printPorts");
    auto layouts = info.getAs<ArrayAttr>("records");
    if (!records || !layouts || records.empty() || records.size() != layouts.size() ||
        bag.getElements().size() != records.size() + 1 ||
        bag.getElements()[0].name != reset || bag.getElements()[0].type != bit)
      return reject("PrintBridge constructor does not match queued payload layout");
    for (auto [i, attr] : llvm::enumerate(records)) {
      auto record = dyn_cast<DictionaryAttr>(attr), layout = dyn_cast<DictionaryAttr>(layouts[i]);
      auto fields = record ? record.getAs<ArrayAttr>("ports") : ArrayAttr{};
      auto type = dyn_cast<BundleType>(bag.getElements()[i + 1].type);
      if (!record || !layout || !fields || !type || fields.size() != type.getElements().size() ||
          record.getAs<StringAttr>("name") != bag.getElements()[i + 1].name ||
          record.getAs<StringAttr>("name") != layout.getAs<StringAttr>("name") ||
          record.getAs<StringAttr>("format") != layout.getAs<StringAttr>("format"))
        return reject("PrintBridge record order or format does not match queued host");
      for (auto [j, fieldAttr] : llvm::enumerate(fields)) {
        auto fieldDict = dyn_cast<DictionaryAttr>(fieldAttr); auto element = type.getElements()[j];
        auto ground = dyn_cast<FIRRTLBaseType>(element.type);
        if (!fieldDict || fieldDict.size() != 1 || !ground || !isa<UIntType, SIntType>(ground))
          return reject("PrintBridge constructor requires ordered integer fields");
        std::string spelling = std::string(isa<UIntType>(ground) ? "UInt<" : "SInt<") +
            std::to_string(ground.getBitWidthOrSentinel()) + ">";
        if (fieldDict.getAs<StringAttr>(element.name) != spelling)
          return reject("PrintBridge constructor field does not match queued payload type");
      }
    }
    std::function<void(Type, std::string, SmallVector<unsigned>)> visit = [&](Type type, std::string local, SmallVector<unsigned> path) {
      if (auto bundle = dyn_cast<BundleType>(type)) {
        for (auto [i, field] : llvm::enumerate(bundle.getElements())) {
          auto next = path; next.push_back(i);
          visit(field.type, local.empty() ? field.name.getValue().str() : local + "_" + field.name.getValue().str(), next);
        }
      } else binding.leaves.push_back({local, path, type, 0, 0});
    };
    visit(payload, "", {});
    if (mapping.size() != binding.leaves.size()) return reject("PrintBridge mapping does not cover the payload leaves");
    Attribute channelClock; bool haveClock = false;
    for (auto &leaf : binding.leaves) {
      if (!isa<UIntType, SIntType>(leaf.type)) return reject("unsupported PrintBridge payload leaf type");
      auto global = mapping.getAs<StringAttr>(leaf.local);
      if (!global || !globals.insert(global.getValue()).second) return reject("missing or shared PrintBridge channel mapping");
      DictionaryAttr channel;
      for (auto [i, attr] : llvm::enumerate(raw)) {
        auto d = dyn_cast<DictionaryAttr>(attr);
        if (d && d.getAs<StringAttr>("class") == AnnotationClasses::ChannelConnection && d.getAs<StringAttr>("globalName") == global) {
          if (channel) return reject("ambiguous PrintBridge channel");
          channel = d; leaf.annotation = i;
        }
      }
      auto channelInfo = channel ? channel.getAs<DictionaryAttr>("channelInfo") : DictionaryAttr{};
      auto latency = channelInfo ? channelInfo.getAs<IntegerAttr>("latency") : IntegerAttr{};
      auto sources = channel ? channel.getAs<ArrayAttr>("sources") : ArrayAttr{};
      auto sinks = channel ? channel.getAs<ArrayAttr>("sinks") : ArrayAttr{};
      if (!channelInfo || channelInfo.getAs<StringAttr>("class") != AnnotationClasses::PipeChannel ||
          !latency || latency.getInt() != 0 || !sources || sources.size() != 1 || (sinks && !sinks.empty()))
        return reject("PrintBridge requires latency-zero output boundary pipe channels");
      if (haveClock && channel.get("clock") != channelClock)
        return reject("PrintBridge input channels must share a target clock domain");
      channelClock = channel.get("clock"); haveClock = true;
      auto spelling = dyn_cast<StringAttr>(sources[0]);
      auto resolved = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
      if (!resolved || resolved->module != inner || !resolved->port)
        return reject("PrintBridge source must resolve to an active top port");
      leaf.port = *resolved->port;
      auto token = dyn_cast<BundleType>(inner.getPortType(leaf.port));
      auto expected = BundleType::get(ctx, {{b.getStringAttr("ready"), true, bit},
          {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, cast<FIRRTLBaseType>(leaf.type)}});
      if (!token || token != expected || resolved->fieldID != token.getFieldID(*token.getElementIndex("bits")) ||
          inner.getPortDirection(leaf.port) != Direction::Out || !consumed.insert(leaf.port).second)
        return reject("PrintBridge source must be a distinct matching Decoupled payload output");
    }
    for (auto suffix : {"ctrl", "stream", "stream_count"})
      if (!outputNames.insert("print_" + std::to_string(index) + "_" + suffix).second)
        return reject("PrintBridge exposed port already exists");
    bindings.push_back(std::move(binding));
  }

  // The binding is a wrapper-only mutation. Verify a cloned circuit, then move
  // the new wrapper and its rewritten annotations into the live circuit.
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FModuleOp stagedInner;
  for (auto m : staged->getOps<FModuleOp>()) if (m.getName() == inner.getName()) stagedInner = m;
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i, p] : llvm::enumerate(inner.getPorts())) if (!consumed.count(i)) { copied.push_back(i); ports.push_back(p); }
  for (auto [i, binding] : llvm::enumerate(bindings)) {
    std::string prefix = "print_" + std::to_string(i) + "_";
    ports.push_back({b.getStringAttr(prefix + "ctrl"), binding.host.getPortType(11), Direction::In});
    ports.push_back({b.getStringAttr(prefix + "stream"), stream, Direction::Out});
    ports.push_back({b.getStringAttr(prefix + "stream_count"), count, Direction::Out});
  }
  auto loc = circuit.getLoc(); b.setInsertionPointToEnd(staged->getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(wrapperName), inner.getConventionAttr(), ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim = b.create<InstanceOp>(loc, stagedInner, "sim");
  auto connect = [&](Value dest, Value src) { b.create<ConnectOp>(loc, dest, src); };
  auto field = [&](Value value, StringRef name) -> Value { return b.create<SubfieldOp>(loc, value, name); };
  auto both = [&](Value a, Value c) -> Value { return b.create<AndPrimOp>(loc, a, c); };
  auto outer = [&](unsigned i) { return wrapper.getArgument(llvm::find(copied, i) - copied.begin()); };
  for (auto i : copied) connect(inner.getPortDirection(i) == Direction::In ? sim.getResult(i) : outer(i),
      inner.getPortDirection(i) == Direction::In ? outer(i) : sim.getResult(i));
  SmallVector<Attribute> summaries;
  for (auto [i, binding] : llvm::enumerate(bindings)) {
    FModuleOp host;
    for (auto m : staged->getOps<FModuleOp>()) if (m.getName() == binding.host.getName()) host = m;
    auto instance = b.create<InstanceOp>(loc, host, "PrintBridgeModule_" + std::to_string(i));
    connect(instance.getResult(0), outer(*hostClock)); connect(instance.getResult(1), outer(*hostReset));
    Value valid = b.create<ConstantOp>(loc, bit, APInt(1, 1));
    for (auto &leaf : binding.leaves) valid = both(valid, field(sim.getResult(leaf.port), "valid"));
    connect(instance.getResult(2), valid);
    for (auto [j, leaf] : llvm::enumerate(binding.leaves)) {
      Value token = sim.getResult(leaf.port), dest = instance.getResult(3);
      for (auto index : leaf.fields) dest = b.create<SubfieldOp>(loc, dest, index);
      connect(dest, field(token, "bits"));
      Value ready = instance.getResult(4);
      for (auto [k, other] : llvm::enumerate(binding.leaves)) if (j != k) ready = both(ready, field(sim.getResult(other.port), "valid"));
      connect(field(token, "ready"), ready);
    }
    unsigned start = copied.size() + 3 * i;
    connect(instance.getResult(11), wrapper.getArgument(start));
    Value streamPort = wrapper.getArgument(start + 1);
    connect(instance.getResult(8), field(streamPort, "ready"));
    connect(field(streamPort, "valid"), instance.getResult(9));
    connect(field(streamPort, "bits"), instance.getResult(10));
    connect(wrapper.getArgument(start + 2), instance.getResult(12));
    summaries.push_back(b.getDictionaryAttr({b.getNamedAttr("hostModule", b.getStringAttr(host.getName())),
        b.getNamedAttr("widgetName", b.getStringAttr("PrintBridgeModule_" + std::to_string(i))),
        b.getNamedAttr("controlPort", ports[start].name), b.getNamedAttr("streamPort", ports[start + 1].name),
        b.getNamedAttr("countPort", ports[start + 2].name)}));
  }
  wrapper->setAttr("goldengate.printHostBindings", b.getArrayAttr(summaries));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) { suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break; }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) { SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values); }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) { NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(ctx); }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto attr : raw) annotations.push_back(retarget(attr));
  for (auto &binding : bindings) {
    std::string prefix = newPrefix + "|" + binding.host.getName().str() + ">hBits";
    NamedAttrList bridge(cast<DictionaryAttr>(annotations[binding.annotation])); bridge.set("target", b.getStringAttr(prefix));
    annotations[binding.annotation] = bridge.getDictionary(ctx);
    for (auto &leaf : binding.leaves) {
      std::string suffix; auto type = cast<BundleType>(binding.host.getPortType(3));
      for (auto index : leaf.fields) { auto element = type.getElements()[index]; suffix += "." + element.name.getValue().str(); type = dyn_cast<BundleType>(element.type); }
      NamedAttrList channel(cast<DictionaryAttr>(annotations[leaf.annotation]));
      channel.set("sinks", b.getArrayAttr({b.getStringAttr(prefix + suffix)}));
      annotations[leaf.annotation] = channel.getDictionary(ctx);
    }
  }
  staged->setName(wrapperName); staged->getOperation()->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (failed(verify(*staged))) return reject("PrintBridge binding produced invalid staged FIRRTL");
  wrapper->moveBefore(circuit.getBodyBlock(), circuit.getBodyBlock()->end());
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
