// See LICENSE for license details.
#ifndef GOLDENGATE_TEST_HOST_MEMORY_PROFILE_CHECKS_H
#define GOLDENGATE_TEST_HOST_MEMORY_PROFILE_CHECKS_H

#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <limits>
#include <stdexcept>

// The host-memory stages retain fixed geometry and fixed buffer capacities.
// Compare the complete transformed circuit across target-total maxFlight and
// selected per-ID reuse 1/2. Only constructor attributes are normalized, so the
// interpreted helper traces apply to every accepted, hardware-identical profile.
template <typename Factory, typename Transform>
void checkHostMemoryProfiles(mlir::MLIRContext &ctx, Factory fixture,
                             Transform transform) {
  using namespace mlir;
  using namespace circt::firrtl;
  OpBuilder b(&ctx);
  auto check=[](bool yes, const char *message) {
    if (!yes) throw std::runtime_error(message);
  };
  auto print=[](Operation *op) {
    std::string result; llvm::raw_string_ostream stream(result);
    op->print(stream); return result;
  };
  auto set=[&](CircuitOp circuit, StringRef field, Attribute value) {
    for (auto m : circuit.getOps<FModuleOp>()) {
      auto key=m->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
      if (!key) continue;
      StringRef dict=(field=="addrBits" || field=="dataBits" || field=="idBits")
                         ? "axi4Widths" : "axi4Edge";
      NamedAttrList fields(key.getAs<DictionaryAttr>(dict));
      if (value) fields.set(field,value); else fields.erase(field);
      NamedAttrList updated(key); updated.set(dict,fields.getDictionary(&ctx));
      m->setAttr("goldengate.bridgeConstructor",updated.getDictionary(&ctx));
    }
  };
  std::string expected;
  for (int64_t reuse : {int64_t(1),int64_t(2)})
  for (int64_t capacity : {int64_t(1),int64_t(10),int64_t(16),int64_t(20),
                           int64_t(32),std::numeric_limits<int64_t>::max()}) {
    auto root=fixture(); auto circuit=*root->template getOps<CircuitOp>().begin();
    set(circuit,"maxFlight",b.getI64IntegerAttr(capacity));
    set(circuit,"idReuse",b.getI64IntegerAttr(reuse)); std::string error;
    check(succeeded(transform(circuit,error)),error.c_str());
    check(succeeded(verify(*root)),"capacity profile produced invalid IR");
    set(circuit,"maxFlight",b.getI64IntegerAttr(10));
    set(circuit,"idReuse",b.getI64IntegerAttr(1));
    auto actual=print(root->getOperation());
    if (expected.empty()) expected=actual;
    check(actual==expected,"constructor capacity/reuse changed independent host hardware or annotations");
  }
  unsigned rejects=0;
  for (StringRef field : {"addrBits","dataBits","idBits","maxReadTransfer",
                          "idReuse","maxFlight"}) {
    SmallVector<Attribute> bad{Attribute(), b.getStringAttr("10"),b.getUnitAttr(),
      b.getArrayAttr({}),b.getI64IntegerAttr(0),b.getI64IntegerAttr(-1),
      b.getIntegerAttr(b.getIntegerType(128),APInt(128,10)),
      b.getIntegerAttr(b.getIntegerType(128),APInt(128,1).shl(80))};
    if (field == "idReuse") {
      bad.push_back(b.getI64IntegerAttr(3));
      bad.push_back(b.getI64IntegerAttr(99));
      bad.push_back(b.getI64IntegerAttr(std::numeric_limits<int64_t>::max()));
    }
    for (auto value : bad) {
      auto root=fixture(); auto circuit=*root->template getOps<CircuitOp>().begin();
      set(circuit,field,value); auto before=print(root->getOperation());
      std::string error;
      check(failed(transform(circuit,error))&&!error.empty(),"malformed host profile accepted");
      check(before==print(root->getOperation()),"malformed host profile mutated IR");
      ++rejects;
    }
  }
  llvm::outs()<<"Twelve maxFlight/idReuse profiles have identical hardware/annotations; "
              <<rejects<<" malformed profile rejections are atomic\n";
}
#endif
