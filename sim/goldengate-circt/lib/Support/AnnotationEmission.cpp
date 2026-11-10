// See LICENSE for license details.
#include "goldengate/AnnotationEmission.h"
#include "goldengate/AnnotationClasses.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>
#include <map>
#include <optional>

using namespace mlir;

namespace {
bool isFAMEAnnotation(llvm::StringRef name) {
  // EmitFAMEAnnotations in Scala selects the FAMEAnnotation trait.  CIRCT's
  // retained JSON has no trait information, so enumerate its concrete classes.
  static constexpr llvm::StringLiteral classes[] = {
      goldengate::AnnotationClasses::FAMETransform,
      goldengate::AnnotationClasses::ChannelConnection,
      goldengate::AnnotationClasses::ChannelPorts,
      goldengate::AnnotationClasses::ChannelFanout,
      goldengate::AnnotationClasses::HostClock,
      goldengate::AnnotationClasses::HostReset,
      goldengate::AnnotationClasses::BridgeIO,
      goldengate::AnnotationClasses::BridgeTopWiring,
      goldengate::AnnotationClasses::BridgeTopWiringOutput,
      "firesim.lib.bridgeutils.BridgeAnnotation",
      "midas.passes.fame.PromoteSubmoduleAnnotation",
      goldengate::AnnotationClasses::ModelReadPort,
      goldengate::AnnotationClasses::ModelWritePort,
      goldengate::AnnotationClasses::ModelReadWritePort,
      "midas.targetutils.FirrtlFAMEModelAnnotation",
      "midas.targetutils.FirrtlEnableModelMultiThreadingAnnotation",
      "midas.targetutils.AutoCounterCoverModuleFirrtlAnnotation",
      "midas.targetutils.PlusArgFirrtlAnnotation",
      goldengate::AnnotationClasses::TriggerSource,
      goldengate::AnnotationClasses::TriggerSink,
      "midas.targetutils.RoCCBusyFirrtlAnnotation",
      "midas.targetutils.FirrtlPartWrapperParentAnnotation",
      "midas.targetutils.FirrtlPortToNeighborRouterIdxAnno",
      "midas.targetutils.FirrtlCombLogicInsideModuleAnno",
      goldengate::AnnotationClasses::InternalTriggerSource,
      goldengate::AnnotationClasses::InternalTriggerSink,
  };
  for (llvm::StringRef fameClass : classes)
    if (name == fameClass)
      return true;
  return false;
}

std::optional<llvm::json::Value> toJSON(Attribute attr,
                                        std::string &error) {
  // CIRCT imports explicit JSON null (including Scala Option.None constructor
  // fields) as UnitAttr. Preserve it at every retained annotation boundary.
  if (isa<UnitAttr>(attr))
    return llvm::json::Value(nullptr);
  // Borrow immutable context-owned strings, including inline blackbox payloads.
  // The JSON tree is serialized and destroyed synchronously in emitAnnotations,
  // while the circuit/context still own every value and dictionary key.
  if (auto value = dyn_cast<StringAttr>(attr))
    return llvm::json::Value(value.getValue());
  if (auto value = dyn_cast<BoolAttr>(attr))
    return llvm::json::Value(value.getValue());
  if (auto value = dyn_cast<FloatAttr>(attr)) {
    // SFC DedupedResult annotations use fractional indices. Preserve their
    // retained JSON payload when emitting a normalized comparison boundary.
    double number = value.getValueAsDouble();
    if (!std::isfinite(number)) {
      error = "annotation float is not a finite JSON number";
      return std::nullopt;
    }
    return llvm::json::Value(number);
  }
  if (auto value = dyn_cast<IntegerAttr>(attr)) {
    if (value.getValue().getBitWidth() > 64) {
      error = "annotation integer exceeds JSON's 64-bit range";
      return std::nullopt;
    }
    return llvm::json::Value(value.getInt());
  }
  if (auto value = dyn_cast<ArrayAttr>(attr)) {
    llvm::json::Array array;
    for (Attribute element : value) {
      auto converted = toJSON(element, error);
      if (!converted)
        return std::nullopt;
      array.push_back(std::move(*converted));
    }
    return llvm::json::Value(std::move(array));
  }
  if (auto value = dyn_cast<DictionaryAttr>(attr)) {
    llvm::json::Object object;
    for (NamedAttribute member : value) {
      auto converted = toJSON(member.getValue(), error);
      if (!converted)
        return std::nullopt;
      object[member.getName().getValue()] = std::move(*converted);
    }
    return llvm::json::Value(std::move(object));
  }
  error = "unsupported attribute in FAME annotation";
  return std::nullopt;
}

// Validate before touching the destination, without allocating a second tree
// for the immutable MLIR attributes. Keep the same accepted values and errors
// as toJSON, including validation of every value in an invalid-key dictionary
// before JSON's normalized-key overwrite behavior can discard any member.
LogicalResult validateJSON(Attribute attr, std::string &error) {
  if (isa<UnitAttr, BoolAttr>(attr))
    return success();
  if (auto value = dyn_cast<StringAttr>(attr)) {
#ifndef NDEBUG
    // Keep LLVM's malformed-UTF-8 assertions before destination creation too.
    (void)llvm::json::Value(value.getValue());
#endif
    return success();
  }
  if (auto value = dyn_cast<FloatAttr>(attr)) {
    if (std::isfinite(value.getValueAsDouble()))
      return success();
    error = "annotation float is not a finite JSON number";
  } else if (auto value = dyn_cast<IntegerAttr>(attr)) {
    if (value.getValue().getBitWidth() <= 64)
      return success();
    error = "annotation integer exceeds JSON's 64-bit range";
  } else if (auto value = dyn_cast<ArrayAttr>(attr)) {
    for (Attribute element : value)
      if (failed(validateJSON(element, error)))
        return failure();
    return success();
  } else if (auto value = dyn_cast<DictionaryAttr>(attr)) {
    for (NamedAttribute member : value) {
      if (failed(validateJSON(member.getValue(), error)))
        return failure();
#ifndef NDEBUG
      (void)llvm::json::ObjectKey(member.getName().getValue());
#endif
    }
    return success();
  } else {
    error = "unsupported attribute in FAME annotation";
  }
  return failure();
}

void streamJSON(Attribute attr, llvm::json::OStream &out) {
  if (auto value = dyn_cast<ArrayAttr>(attr)) {
    out.array([&] {
      for (Attribute element : value)
        streamJSON(element, out);
    });
    return;
  }
  if (auto value = dyn_cast<DictionaryAttr>(attr)) {
    // DictionaryAttr already has the lexicographic key order used by LLVM's
    // JSON object writer. On malformed UTF-8, retain ObjectKey's normalization,
    // reordering and last-member-wins behavior through the original converter.
    // That exceptional path is also subject to LLVM's original debug assertion.
    bool invalidKey = llvm::any_of(value, [](NamedAttribute member) {
      return !llvm::json::isUTF8(member.getName().getValue());
    });
    if (!invalidKey) {
      out.object([&] {
        for (NamedAttribute member : value) {
          out.attributeBegin(member.getName().getValue());
          streamJSON(member.getValue(), out);
          out.attributeEnd();
        }
      });
      return;
    }
  }
  // LLVM retains responsibility for escaping, UTF-8 fallback and numeric
  // formatting. Ordinary scalars borrow strings and allocate no JSON containers.
  std::string unusedError;
  auto converted = toJSON(attr, unusedError);
  assert(converted && "annotation value was validated before streaming");
  out.value(*converted);
}
} // namespace

LogicalResult goldengate::emitJSONAttribute(Attribute value,
                                           llvm::StringRef path,
                                           std::string &error) {
  if (!value || failed(validateJSON(value, error)))
    return failure();
  std::error_code writeError;
  llvm::raw_fd_ostream out(path, writeError);
  if (writeError) {
    error = "cannot write compiler metadata: " + writeError.message();
    return failure();
  }
  {
    llvm::json::OStream json(out, 2);
    streamJSON(value, json);
  }
  out << '\n';
  out.close();
  if (out.has_error()) {
    error = "cannot finish compiler metadata: " + out.error().message();
    out.clear_error();
    return failure();
  }
  return success();
}

static LogicalResult emitAnnotations(circt::firrtl::CircuitOp circuit,
                                     llvm::StringRef path, bool fameOnly,
                                     std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "circuit has no retained annotations";
    return failure();
  }
  llvm::SmallVector<Attribute> annotations;
  for (Attribute attr : raw) {
    auto dict = dyn_cast<DictionaryAttr>(attr);
    if (!dict) {
      error = "FAME annotation is not a dictionary";
      return failure();
    }
    auto className = dict.getAs<StringAttr>("class");
    if (!className) {
      error = "annotation has no class name";
      return failure();
    }
    if (fameOnly && !isFAMEAnnotation(className.getValue()))
      continue;
    if (failed(validateJSON(attr, error)))
      return failure();
    annotations.push_back(attr);
  }
  std::error_code writeError;
  llvm::raw_fd_ostream out(path, writeError);
  if (writeError) {
    error = "cannot write FAME annotations: " + writeError.message();
    return failure();
  }
  {
    llvm::json::OStream json(out, 2);
    json.array([&] {
      for (Attribute attr : annotations)
        streamJSON(attr, json);
    });
  }
  out << '\n';
  out.close();
  if (out.has_error()) {
    error = "cannot finish FAME annotations: " + out.error().message();
    out.clear_error();
    return failure();
  }
  return success();
}

LogicalResult goldengate::emitFAMEAnnotations(
    circt::firrtl::CircuitOp circuit, llvm::StringRef path,
    std::string &error) {
  return emitAnnotations(circuit, path, true, error);
}

LogicalResult goldengate::emitAllAnnotations(
    circt::firrtl::CircuitOp circuit, llvm::StringRef path,
    std::string &error) {
  return emitAnnotations(circuit, path, false, error);
}

LogicalResult goldengate::emitOutputFiles(
    circt::firrtl::CircuitOp circuit, llvm::StringRef directory,
    llvm::StringRef baseFilename, std::string &error) {
  auto reject = [&](llvm::StringRef reason) {
    error = reason.str();
    return failure();
  };
  auto filename = [](llvm::StringRef value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find_first_of("/\\") == llvm::StringRef::npos &&
           value.find('\0') == llvm::StringRef::npos;
  };
  if (!filename(baseFilename))
    return reject("output base must be a filename");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw)
    return reject("circuit has no retained annotations");
  std::map<std::string, llvm::StringRef> files;
  for (Attribute attr : raw) {
    auto dict = dyn_cast<DictionaryAttr>(attr);
    auto cls = dict ? dict.getAs<StringAttr>("class") : StringAttr();
    if (!cls)
      return reject("malformed retained annotation");
    bool xdc = cls.getValue() == AnnotationClasses::XDCOutput;
    if (cls.getValue() != AnnotationClasses::OutputFile && !xdc)
      continue;
    auto body = dict.getAs<StringAttr>(xdc ? "fileBody" : "body");
    auto suffix = dict.getAs<StringAttr>(xdc ? "suffix" : "fileSuffix");
    if (!body || !suffix || dict.get("target"))
      return reject("output file annotation requires body/fileSuffix and no target");
    if (!filename(suffix.getValue()) || !suffix.getValue().starts_with("."))
      return reject("output file suffix must be a local extension");
    std::string name = (baseFilename + suffix.getValue()).str();
    if (!files.emplace(name, body.getValue()).second)
      return reject("multiple output file annotations select the same filename");
  }
  // No destination is touched until the entire annotation set is validated.
  for (auto &[name, body] : files) {
    llvm::SmallString<256> path(directory);
    llvm::sys::path::append(path, name);
    std::error_code ec;
    llvm::raw_fd_ostream out(path, ec);
    if (ec) {
      error = "cannot write Golden Gate output file: " + ec.message();
      return failure();
    }
    out << body;
    out.close();
    if (out.has_error()) {
      error = "cannot finish Golden Gate output file: " + out.error().message();
      out.clear_error();
      return failure();
    }
  }
  return success();
}
