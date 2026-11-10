// See LICENSE for license details.
#pragma once
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/SmallVector.h"
#include <string>

namespace goldengate {
// TracerVBridge.scala packs up to seven retiring instructions into one beat.
// The four selected target families fit one arm. Reject wider/multi-arm
// constructors until the corresponding counter scheduling is implemented.
struct TracerVConfig {
  unsigned retireWidth = 0, wdataWidth = 0;
  llvm::SmallVector<std::string> names;
  llvm::SmallVector<unsigned> widths;
  unsigned inputs() const { return names.size() - 2; }
  static std::string lane(unsigned index, llvm::StringRef field) {
    return "tiletrace_trace_retiredinsns_" + std::to_string(index) + "_" + field.str();
  }
  static std::string observation(unsigned index, llvm::StringRef field) {
    return field.str() + (index ? "_" + std::to_string(index) : "");
  }
  bool parse(mlir::DictionaryAttr key) {
    retireWidth=0;wdataWidth=0;names.clear();widths.clear();
    if (!key || key.getAs<mlir::StringAttr>("class") != "firechip.bridgeinterfaces.TraceBundleWidths") return false;
    auto width = [&](llvm::StringRef n, int64_t expected) {
      auto v = key.getAs<mlir::IntegerAttr>(n); return v && v.getValue().getBitWidth()<=64 && v.getInt() == expected;
    };
    auto retire = key.getAs<mlir::IntegerAttr>("retireWidth");
    if (!retire || retire.getValue().getBitWidth()>64 || retire.getInt() < 1 || retire.getInt() > 7 ||
        !width("iaddrWidth",40) || !width("insnWidth",32) ||
        !width("causeWidth",64) || !width("tvalWidth",40)) return false;
    retireWidth = retire.getInt();
    // CIRCT's JSON importer represents an absent Scala Option (null) as UnitAttr.
    if (auto raw = key.get("wdataWidth"); raw && !mlir::isa<mlir::UnitAttr>(raw)) {
      auto data=mlir::dyn_cast<mlir::IntegerAttr>(raw);
      if (!data || data.getValue().getBitWidth()>64 || data.getInt() < 1 || data.getInt() > 64) return false;
      wdataWidth = data.getInt();
    }
    auto add = [&](std::string n, unsigned w) { names.push_back(std::move(n)); widths.push_back(w); };
    add("tiletrace_reset",1);
    for (unsigned i=0;i<retireWidth;++i) {
      for (auto field : {std::pair<llvm::StringRef,unsigned>{"valid",1},{"iaddr",40},{"insn",32},
                        {"priv",3},{"exception",1},{"interrupt",1},{"cause",64},{"tval",40}})
      {
        add(lane(i,field.first),field.second);
        if(field.first=="insn" && wdataWidth) add(lane(i,"wdata"),wdataWidth);
      }
    }
    add("tiletrace_trace_time",64);add("triggerCredit",1);add("triggerDebit",1);
    return true;
  }
};
} // namespace goldengate
