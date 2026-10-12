// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROL_CATALOG_H
#define GOLDENGATE_CONTROL_CATALOG_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Import only register-bank roots and their FIRRTL instance dependencies.
// Existing roots must have identical definitions and dependency closures.
// New private dependencies use the CatalogRocket_ namespace. The source,
// active top, annotations and existing operation identities remain intact;
// a rejected import leaves the destination intact as well.
mlir::LogicalResult importControlCatalog(circt::firrtl::CircuitOp destination,
    circt::firrtl::CircuitOp source, llvm::ArrayRef<llvm::StringRef> roots,
    std::string &error);
mlir::LogicalResult importRocketControlCatalog(circt::firrtl::CircuitOp destination,
    circt::firrtl::CircuitOp source, std::string &error);
}
#endif
