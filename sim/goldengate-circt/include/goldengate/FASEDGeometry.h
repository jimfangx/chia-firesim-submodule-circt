// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/Support/MathExtras.h"
#include <limits>
#include <algorithm>
#include <string>
namespace goldengate {
struct FASEDGeometry {
  unsigned dataBits = 64, readBeats = 8, writeBeats = 8;
  unsigned idReuse = 1;
  uint64_t maxFlight = 10;
  unsigned byteBits() const { return llvm::Log2_64(dataBits / 8); }
  unsigned pendingBits(unsigned capacity) const {
    return llvm::Log2_64_Ceil(capacity + 1);
  }
  unsigned readDepth() const {
    return llvm::PowerOf2Ceil(uint64_t(readBeats) * idReuse);
  }
};
inline FASEDGeometry getFASEDGeometry(circt::firrtl::CircuitOp circuit) {
  using namespace mlir;
  DictionaryAttr key;
  for (auto module : circuit.getOps<circt::firrtl::FModuleOp>())
    if (module.getName() == "GGFASEDTokenEngine")
      key =
          module->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
  if (!key)
    if (auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
      for (auto a : raw)
        if (auto row = dyn_cast<DictionaryAttr>(a))
          if (auto widget = row.getAs<StringAttr>("widgetClass"))
            if (widget.getValue() == "midas.models.FASEDMemoryTimingModel")
              key = row.getAs<DictionaryAttr>("widgetConstructorKey");
  FASEDGeometry result;
  auto get = [](DictionaryAttr d, llvm::StringRef n, unsigned fallback) {
    auto a = d ? d.getAs<IntegerAttr>(n) : IntegerAttr();
    if (!a)
      return d && d.get(n) ? 0u : fallback;
    if (a.getValue().getBitWidth() > 64 || a.getInt() <= 0 ||
        uint64_t(a.getInt()) > std::numeric_limits<unsigned>::max())
      return 0u;
    return unsigned(a.getInt());
  };
  auto widths =
      key ? key.getAs<DictionaryAttr>("axi4Widths") : DictionaryAttr();
  auto edge = key ? key.getAs<DictionaryAttr>("axi4Edge") : DictionaryAttr();
  result.dataBits = get(widths, "dataBits", widths ? 0 : 64);
  result.readBeats = get(edge, "maxReadTransfer", 8);
  result.writeBeats = get(edge, "maxWriteTransfer", 8);
  result.idReuse = get(edge, "idReuse", 1);
  if (auto flight = edge ? edge.getAs<mlir::IntegerAttr>("maxFlight")
                         : mlir::IntegerAttr())
    result.maxFlight =
        flight.getValue().getBitWidth() <= 64 && flight.getInt() > 0
            ? uint64_t(flight.getInt())
            : 0;
  else
    result.maxFlight = edge && edge.get("maxFlight") ? 0 : 10;
  return result;
}
inline bool validFASEDGeometry(const FASEDGeometry &g) {
  return g.dataBits >= 8 && g.dataBits <= 1024 &&
         llvm::isPowerOf2_32(g.dataBits) && g.readBeats && g.readBeats <= 256 &&
         g.writeBeats && g.writeBeats <= 256 && g.idReuse &&
         g.idReuse <= 65536 && g.maxFlight;
}
inline unsigned getFASEDEffectiveCapacity(circt::firrtl::CircuitOp circuit) {
  auto base =
      circuit->getAttrOfType<mlir::IntegerAttr>("goldengate.fasedBaseCapacity");
  return std::min<uint64_t>(getFASEDGeometry(circuit).maxFlight,
                            unsigned(base ? base.getInt() : 16));
}
inline bool needsFASEDReadTranslation(const FASEDGeometry &g,
                                      unsigned maxRequests) {
  uint64_t virtualState = uint64_t(g.idReuse) * 16 * g.readBeats * g.dataBits;
  uint64_t physicalState = uint64_t(maxRequests) * g.readBeats * g.dataBits;
  return virtualState > 0x20000 && virtualState > physicalState + 0x10000;
}
} // namespace goldengate
