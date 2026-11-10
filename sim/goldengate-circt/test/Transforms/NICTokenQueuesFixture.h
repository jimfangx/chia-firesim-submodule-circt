#ifndef GOLDENGATE_NIC_TOKEN_QUEUES_FIXTURE_H
#define GOLDENGATE_NIC_TOKEN_QUEUES_FIXTURE_H
// See LICENSE for license details.
#include "goldengate/NICTokenQueues.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <stdexcept>
#include <map>
#include <random>
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
static void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, bool nic = true, int latency = 1, unsigned widgets = 1, bool expose = false) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGCPUStreamPlatformWrapper" { firrtl.module @GGCPUStreamPlatformWrapper() {} } })", &ctx);
  require(bool(root), "parse fixture"); auto circuit = *root->getOps<CircuitOp>().begin();
  (*circuit.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(&ctx); auto bit = UIntType::get(&ctx, 1, false);
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  const StringRef names[]{"nic_out_bits_data", "nic_out_bits_keep", "nic_out_bits_last", "nic_out_valid",
      "nic_in_bits_data", "nic_in_bits_keep", "nic_in_bits_last", "nic_in_valid", "nic_macAddr",
      "nic_rlimit_inc", "nic_rlimit_period", "nic_rlimit_size", "nic_pauser_threshold", "nic_pauser_quanta", "nic_pauser_refresh"};
  unsigned widths[]{64,8,1,1,64,8,1,1,48,8,8,8,16,16,16};
  SmallVector<Attribute> annotations;
  for (unsigned widget = 0; widget < widgets; ++widget) {
  NamedAttrList mapping;
  std::string endpoint = "ep_" + std::to_string(widget + 3);
  for (unsigned j = 0; j < 15; ++j) {
    auto name = "FireSim_" + endpoint + "_" + names[j].str() + (j < 4 ? "_source" : "_sink");
    auto t = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
        {b.getStringAttr("bits"), false, UIntType::get(&ctx, widths[j], false)}});
    ports.push_back({b.getStringAttr(name), t, j < 4 ? Direction::Out : Direction::In});
    mapping.set(names[j], b.getStringAttr(endpoint + "_" + names[j].str()));
    NamedAttrList channel; channel.set("class", b.getStringAttr("midas.passes.fame.FAMEChannelConnectionAnnotation"));
    channel.set("globalName", b.getStringAttr(endpoint + "_" + names[j].str()));
    channel.set("clock", b.getStringAttr("~GGCPUStreamPlatformWrapper|GGCPUStreamPlatformWrapper>hostClock"));
    channel.set("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("midas.passes.fame.PipeChannel")),
        b.getNamedAttr("latency", b.getI64IntegerAttr(latency))}));
    channel.set(j < 4 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr("~GGCPUStreamPlatformWrapper|GGCPUStreamPlatformWrapper>" + name + ".bits")}));
    annotations.push_back(channel.getDictionary(&ctx));
  }
  if (nic) annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridgeutils.BridgeIOAnnotation")),
      b.getNamedAttr("widgetClass", b.getStringAttr("firechip.goldengateimplementations.SimpleNICBridgeModule")),
      b.getNamedAttr("target", b.getStringAttr("~FireSim|FireSim>" + endpoint)), b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))}));
  }
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  unsigned scalarBase = ports.size();
  if (expose) for (unsigned w=0;w<widgets;++w) for(unsigned j=0;j<15;++j) {
    auto prefix="w"+std::to_string(w)+"_p"+std::to_string(j)+"_";
    ports.push_back({b.getStringAttr(prefix+"bits"), UIntType::get(&ctx,widths[j],false),j<4?Direction::In:Direction::Out});
    ports.push_back({b.getStringAttr(prefix+"valid"),bit,j<4?Direction::In:Direction::Out});
    ports.push_back({b.getStringAttr(prefix+"ready"),bit,j<4?Direction::Out:Direction::In});
  }
  auto inner=b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("GGCPUStreamPlatformWrapper"), ConventionAttr::get(&ctx, Convention::Internal), ports);
  b.setInsertionPointToStart(inner.getBodyBlock());
  if(expose)for(unsigned w=0;w<widgets;++w)for(unsigned j=0;j<15;++j) {
    Value t=inner.getBodyBlock()->getArgument(2+w*15+j);
    for(unsigned f=0;f<3;++f) {
      Value leaf=b.create<SubfieldOp>(circuit.getLoc(),t,f==0?"bits":f==1?"valid":"ready");
      Value scalar=inner.getBodyBlock()->getArgument(scalarBase+3*(w*15+j)+f);
      bool drive=(j<4)==(f<2);
      b.create<StrictConnectOp>(circuit.getLoc(),drive?leaf:scalar,drive?scalar:leaf);
    }
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return root;
}

#endif
