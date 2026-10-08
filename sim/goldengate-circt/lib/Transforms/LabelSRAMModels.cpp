// See LICENSE for license details.
#include "goldengate/LabelSRAMModels.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/STLExtras.h"
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::labelSRAMModels(CircuitOp circuit,
                                        unsigned &extracted,
                                        std::string &error) {
  extracted = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "LabelSRAMModels needs retained annotations";
    return failure();
  }
  auto *context = circuit.getContext();
  OpBuilder b(context);
  std::set<std::string> selected, names;
  SmallVector<Attribute> retained;
  for (Attribute attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::MemModel)) {
      retained.push_back(attr);
      continue;
    }
    auto target = anno.getMember<StringAttr>("target");
    if (!target) {
      error = "FirrtlMemModelAnnotation has no string target";
      return failure();
    }
    selected.insert(target.getValue().str());
  }
  for (auto module : circuit.getOps<FModuleLike>())
    names.insert(module.getModuleName().str());

  struct Plan {
    MemOp memory;
    FModuleOp parent;
    std::string name;
    SmallVector<unsigned> order;
    SmallVector<PortInfo> ports;
  };
  SmallVector<Plan> plans;
  bool invalid = false;
  for (auto parent : circuit.getOps<FModuleOp>()) {
    parent.walk([&](MemOp memory) {
      auto identity = "~" + circuit.getName().str() + "|" +
                      parent.getName().str() + ">" + memory.getName().str();
      if (!selected.count(identity)) return;
      // Nonlocal targets and memory inner symbols need a rename-map boundary
      // before moving the declaration. Never silently leave a dangling path.
      if (memory.getInnerSymAttr()) {
        error = "selected SRAM has an inner symbol: " + identity;
        invalid = true;
        return;
      }
      Plan plan{memory, parent, memory.getName().str(), {}, {}};
      for (unsigned suffix = 0; !names.insert(plan.name).second; ++suffix)
        plan.name = memory.getName().str() + "_" + std::to_string(suffix);
      plan.ports.emplace_back(b.getStringAttr("clk"), ClockType::get(context),
                              Direction::In, StringAttr(), memory.getLoc());
      for (auto kind : {MemOp::PortKind::Read, MemOp::PortKind::Write,
                        MemOp::PortKind::ReadWrite}) {
        for (unsigned i = 0; i < memory.getNumResults(); ++i) {
          if (memory.getPortKind(i) != kind) continue;
          auto type = dyn_cast<BundleType>(memory.getResult(i).getType());
          if (!type || memory.getPortNameStr(i) == "clk") {
            error = "SRAM port must be a bundle with a distinct name: " + identity;
            invalid = true;
            return;
          }
          SmallVector<BundleType::BundleElement> fields;
          unsigned clocks = 0;
          for (auto field : type.getElements()) {
            if (isa<ClockType>(field.type)) {
              if (field.name.getValue() != "clk" || field.isFlip) {
                error = "unsupported SRAM clock field: " + identity;
                invalid = true;
                return;
              }
              ++clocks;
            } else fields.push_back(field);
          }
          if (clocks != 1) {
            error = "SRAM port needs exactly one clk field: " + identity;
            invalid = true;
            return;
          }
          for (Operation *user : memory.getResult(i).getUsers()) {
            if (!isa<SubfieldOp>(user)) {
              error = "SRAM port has an aggregate use requiring expansion: " + identity;
              invalid = true;
              return;
            }
          }
          plan.order.push_back(i);
          plan.ports.emplace_back(memory.getPortName(i),
              BundleType::get(context, fields), Direction::In, StringAttr(), memory.getLoc());
        }
      }
      if (plan.order.size() != memory.getNumResults() || plan.order.empty()) {
        error = "SRAM extraction does not support debug or empty ports: " + identity;
        invalid = true;
        return;
      }
      plans.push_back(std::move(plan));
    });
  }
  if (invalid) return failure();

  auto annotation = [&](StringRef klass, StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(klass)),
                                b.getNamedAttr("target", b.getStringAttr(target))});
  };
  Operation *firstOriginal = &circuit.getBodyBlock()->front();
  for (auto &plan : plans) {
    auto memory = plan.memory;
    auto loc = memory.getLoc();
    b.setInsertionPoint(firstOriginal);
    auto wrapper = b.create<FModuleOp>(loc, b.getStringAttr(plan.name),
        ConventionAttr::get(context, Convention::Internal), plan.ports);
    b.setInsertionPointToStart(wrapper.getBodyBlock());
    auto clone = cast<MemOp>(b.clone(*memory));
    for (auto [port, oldIndex] : llvm::enumerate(plan.order)) {
      auto type = cast<BundleType>(clone.getResult(oldIndex).getType());
      for (auto field : type.getElements()) {
        auto memField = b.create<SubfieldOp>(loc, clone.getResult(oldIndex),
                                            field.name.getValue());
        if (isa<ClockType>(field.type)) {
          b.create<ConnectOp>(loc, memField, wrapper.getArgument(0));
        } else {
          auto external = b.create<SubfieldOp>(loc, wrapper.getArgument(port + 1),
                                              field.name.getValue());
          if (field.isFlip) b.create<ConnectOp>(loc, external, memField);
          else b.create<ConnectOp>(loc, memField, external);
        }
      }
    }
    b.setInsertionPoint(memory);
    auto instance = b.create<InstanceOp>(loc, wrapper, memory.getName());
    for (auto [port, oldIndex] : llvm::enumerate(plan.order)) {
      SmallVector<Operation *> users(memory.getResult(oldIndex).getUsers());
      for (Operation *user : users) {
        auto field = cast<SubfieldOp>(user);
        b.setInsertionPoint(field);
        Value replacement = instance.getResult(0);
        if (field.getFieldName() != "clk")
          replacement = b.create<SubfieldOp>(field.getLoc(),
              instance.getResult(port + 1), field.getFieldName());
        field.getResult().replaceAllUsesWith(replacement);
        field.erase();
      }
    }
    std::string moduleTarget = "~" + circuit.getName().str() + "|" + plan.name;
    retained.push_back(annotation(AnnotationClasses::FAMEModel,
        "~" + circuit.getName().str() + "|" + plan.parent.getName().str() +
        "/" + memory.getName().str() + ":" + plan.name));
    retained.push_back(annotation("firrtl.transforms.NoDedupAnnotation", moduleTarget));
    for (unsigned index : plan.order) {
      auto kind = memory.getPortKind(index);
      StringRef klass = kind == MemOp::PortKind::Read ? AnnotationClasses::ModelReadPort :
          kind == MemOp::PortKind::Write ? AnnotationClasses::ModelWritePort :
                                          AnnotationClasses::ModelReadWritePort;
      std::string target = moduleTarget + ">" + memory.getPortNameStr(index).str();
      SmallVector<NamedAttribute> members{b.getNamedAttr("class", b.getStringAttr(klass))};
      SmallVector<StringRef> memberNames;
      if (kind == MemOp::PortKind::Read) memberNames = {"data", "addr", "en"};
      else if (kind == MemOp::PortKind::Write) memberNames = {"data", "mask", "addr", "en"};
      else memberNames = {"wmode", "rdata", "wdata", "wmask", "addr", "en"};
      for (StringRef member : memberNames)
        members.push_back(b.getNamedAttr(member, b.getStringAttr(target + "." + member.str())));
      retained.push_back(b.getDictionaryAttr(members));
    }
    memory.erase();
    ++extracted;
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(retained));
  return success();
}
