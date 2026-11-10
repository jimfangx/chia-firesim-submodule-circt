// See LICENSE for license details.
// Two independently stalled normal NIC packet paths, with real CPU FIFOs.
#include "goldengate/NICPacketAdapters.h"
#include "goldengate/NICCPUStreamQueues.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok, StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
static std::string print(ModuleOp m) { std::string s; llvm::raw_string_ostream os(s); m.print(os); return s; }
static FModuleOp top(CircuitOp c) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == c.getName()) return m;
  throw std::runtime_error("missing top");
}
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  OpBuilder b(&ctx); auto loc = b.getUnknownLoc();
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGNICTokenQueuesWrapper" { firrtl.module @GGNICTokenQueuesWrapper() {} } })", &ctx);
  require(bool(root), "cannot parse fixture");
  auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  auto bit = UIntType::get(&ctx, 1);
  auto token = BundleType::get(&ctx, {
      {b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
      {b.getStringAttr("bits"), false, UIntType::get(&ctx, 66)}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  SmallVector<Attribute> annotations, bindings;
  for (unsigned i = 0; i < 2; ++i) {
    auto from = "nic" + std::to_string(i) + "_from_host_enq";
    auto to = "nic" + std::to_string(i) + "_to_host_deq";
    ports.push_back({b.getStringAttr(from), token, Direction::In});
    ports.push_back({b.getStringAttr(to), token, Direction::Out});
    auto target = "~GGNICTokenQueuesWrapper|GGNICTokenQueuesWrapper>" + from + ".bits";
    bindings.push_back(b.getDictionaryAttr({
        b.getNamedAttr("widget", b.getI64IntegerAttr(i)),
        b.getNamedAttr("fromHost", b.getStringAttr(from)),
        b.getNamedAttr("toHost", b.getStringAttr(to)),
        b.getNamedAttr("bridgeTarget", b.getStringAttr(target))}));
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
        b.getNamedAttr("widgetClass", b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule")),
        b.getNamedAttr("target", b.getStringAttr(target))}));
  }
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto m = b.create<FModuleOp>(loc, b.getStringAttr(c.getName()),
      ConventionAttr::get(&ctx, Convention::Internal), ports);
  m->setAttr("goldengate.nicTokenBindings", b.getArrayAttr(bindings));
  b.setInsertionPointToStart(m.getBodyBlock());
  for (unsigned i = 0; i < 2; ++i)
    b.create<ConnectOp>(loc, m.getArgument(3 + 2 * i), m.getArgument(2 + 2 * i));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return root;
}
static void corruptBinding(FModuleOp m, StringRef key, StringRef field, MLIRContext &ctx) {
  OpBuilder b(&ctx); auto bindings = m->getAttrOfType<ArrayAttr>(key);
  SmallVector<Attribute> records(bindings.begin(), bindings.end());
  NamedAttrList second(cast<DictionaryAttr>(records[1]));
  second.set(field, cast<DictionaryAttr>(records[0]).get(field));
  records[1] = second.getDictionary(&ctx); m->setAttr(key, b.getArrayAttr(records));
}
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    std::string error;
    auto bad = fixture(ctx); auto bc = *bad->getOps<CircuitOp>().begin();
    corruptBinding(top(bc), "goldengate.nicTokenBindings", "fromHost", ctx);
    auto before = print(*bad);
    require(failed(goldengate::addNICPacketAdapters(bc, error)) && print(*bad) == before,
            "late widget token binding rejection changed IR");
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICPacketAdapters(c, error)), error);
    require(succeeded(verify(*root)), "invalid two-widget packet IR");
    auto packets = top(c);
    unsigned decoders = 0, encoders = 0;
    for (auto inst : packets.getOps<InstanceOp>()) {
      decoders += inst.getModuleName() == "GGNICBigTokenDecoder";
      encoders += inst.getModuleName() == "GGNICBigTokenEncoder";
    }
    require(decoders == 2 && encoders == 2, "adapter state shared across widgets");
    auto invalid = fixture(ctx); auto ic = *invalid->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addNICPacketAdapters(ic, error)), error);
    corruptBinding(top(ic), "goldengate.nicPacketBindings", "toHostStream", ctx);
    before = print(*invalid);
    require(failed(goldengate::addNICCPUStreamQueues(ic, error)) && print(*invalid) == before,
            "late widget stream binding rejection changed IR");
    require(succeeded(goldengate::addNICCPUStreamQueues(c, error)), error);
    require(succeeded(verify(*root)), "invalid two-widget CPU queue IR");
    auto queues = top(c); require(queues.getNumPorts() == 10, "wrong duplex/count interface");
    std::set<std::string> names;
    for (auto inst : queues.getOps<InstanceOp>()) {
      if (inst.getName() == "sim") continue;
      auto attr = inst->getAttrOfType<DictionaryAttr>("goldengate.streamParameters");
      require(attr && !attr.get("index"), "missing geometry or premature AXI allocation");
      require(attr.getAs<IntegerAttr>("depth").getInt() == 3072 &&
              attr.getAs<IntegerAttr>("widthBytes").getInt() == 64, "wrong stream geometry");
      require(names.insert(attr.getAs<StringAttr>("name").getValue().str()).second, "duplicate queue identity");
    }
    require(names.size() == 4, "missing independent CPU stream queue");
    auto bindings = queues->getAttrOfType<ArrayAttr>("goldengate.nicCPUStreamBindings");
    auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(bindings && bindings.size() == 2, "missing widget bindings");
    for (unsigned i = 0; i < 2; ++i) {
      auto binding = cast<DictionaryAttr>(bindings[i]);
      require(binding.getAs<StringAttr>("bridgeTarget") ==
              cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target"), "stale bridge target");
      auto prefix = "nic" + std::to_string(i);
      require(binding.getAs<StringAttr>("fromHostCount") == prefix + "_from_host_stream_count" &&
              binding.getAs<StringAttr>("toHostCount") == prefix + "_to_host_stream_count", "count identity");
    }
    before = print(*root);
    require(failed(goldengate::addNICCPUStreamQueues(c, error)) && print(*root) == before,
            "repeated insertion changed IR");
    if (argc == 2) {
      std::error_code ec; llvm::raw_fd_ostream os(argv[1], ec); require(!ec, "cannot emit fixture");
      root->print(os); os << '\n';
    }
    llvm::outs() << "PASS two-widget NIC packet/CPU queue construction, metadata and atomic rejection\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
