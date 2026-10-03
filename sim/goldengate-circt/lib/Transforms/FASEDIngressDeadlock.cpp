// See LICENSE for license details.
// Oracle: IngressUnit.scala deadlock checks. Observe the actual enqueue valid
// and ready signals, never an accepted-enqueue pulse. Append a policy input
// through the unique ingress wrapper chain; existing ports and identities stay
// unchanged. Qualified ingress reset disables assertions on the host clock.
// All hierarchy, boundary and collision checks precede mutation.
#include "goldengate/FASEDIngressDeadlock.h"
#include "mlir/IR/Builders.h"
#include <tuple>
using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addFASEDIngressDeadlock(CircuitOp circuit,
                                               std::string &error) {
  auto reject = [&](llvm::StringRef message) { error = message.str(); return failure(); };
  constexpr llvm::StringLiteral helperName = "GGFASEDIngressDeadlock";
  constexpr llvm::StringLiteral contextName = "fased_ingress_deadlock_context";
  if (circuit.getName() != "GGFASEDIngressIssueWrapper" ||
      !circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    return reject("FASED deadlock checks require the active issue wrapper and annotations");
  const llvm::StringRef names[]{"GGFASEDIngressIssueWrapper", "GGFASEDIngressOrderWrapper",
      "GGFASEDIngressCreditsWrapper", "GGFASEDIngressARQueueWrapper",
      "GGFASEDIngressWQueueWrapper", "GGFASEDIngressAWWrapper", "GGFASEDIngressAW"};
  SmallVector<FModuleOp> modules;
  for (auto name : names) {
    FModuleOp found;
    for (auto m : circuit.getOps<FModuleLike>()) {
      if (m.getModuleName() == helperName)
        return reject("FASED deadlock helper already exists");
      if (m.getModuleName() == name) found = dyn_cast<FModuleOp>(m.getOperation());
    }
    if (!found) return reject("FASED deadlock checks require the recorded ingress hierarchy");
    for (auto p : found.getPorts()) if (p.name == contextName)
      return reject("FASED deadlock policy input already exists");
    modules.push_back(found);
  }
  SmallVector<InstanceOp> chain;
  for (unsigned j = 0; j < modules.size(); ++j) {
    SmallVector<InstanceOp> uses;
    circuit.walk([&](InstanceOp i) { if (i.getModuleName() == names[j]) uses.push_back(i); });
    if (j == 0 ? !uses.empty() : uses.size() != 1)
      return reject("FASED deadlock checks require unique ingress instances and an uninstantiated top");
    if (j && (uses[0]->getParentOfType<FModuleOp>() != modules[j-1] ||
        uses[0].getInstanceName() != (j == 6 ? "ingressAW" : "sim")))
      return reject("FASED deadlock checks require the recorded sim/ingressAW chain");
    if (j) chain.push_back(uses[0]);
  }
  auto *ctx = circuit.getContext(); OpBuilder b(ctx); auto loc = circuit.getLoc();
  auto bit = UIntType::get(ctx, 1, false);
  auto context = BundleType::get(ctx, {{b.getStringAttr("relaxed"),false,bit},
      {b.getStringAttr("awEmpty"),false,bit},{b.getStringAttr("wEmpty"),false,bit},
      {b.getStringAttr("orderValid"),false,bit}});
  auto port = [&](FModuleOp m, llvm::StringRef name, Type type, Direction dir) -> std::optional<unsigned> {
    for (auto [i,p] : llvm::enumerate(m.getPorts()))
      if (p.name == name && p.type == type && p.direction == dir) return i;
    return std::nullopt;
  };
  auto relaxed = port(modules[0], "fased_ingress_relaxed", bit, Direction::In);
  auto creditsType = BundleType::get(ctx, {{b.getStringAttr("awValue"),false,UIntType::get(ctx,4,false)},
      {b.getStringAttr("wValue"),false,UIntType::get(ctx,4,false)},
      {b.getStringAttr("awEmpty"),false,bit},{b.getStringAttr("wEmpty"),false,bit},
      {b.getStringAttr("writeReqDone"),false,bit}});
  auto credits = port(modules[0], "fased_ingress_credits", creditsType, Direction::Out);
  auto orderType = BundleType::get(ctx, {{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bit}});
  auto order = port(modules[1], "fased_ingress_order", orderType, Direction::Out);
  auto gates = modules.back();
  auto clock = port(gates,"clock",ClockType::get(ctx),Direction::In);
  auto reset = port(gates,"reset",bit,Direction::In);
  std::optional<unsigned> w;
  for (auto [i,p] : llvm::enumerate(gates.getPorts())) if (p.name == "w_enq" && p.direction == Direction::Out) {
    auto t = dyn_cast<BundleType>(p.type);
    if (t && t.getElements().size() == 3 && t.getElements()[0].name == "ready" &&
        t.getElements()[0].isFlip && t.getElements()[0].type == bit &&
        t.getElements()[1].name == "valid" && !t.getElements()[1].isFlip &&
        t.getElements()[1].type == bit) w = i;
  }
  InstanceOp awQueue;
  for (auto i : gates.getOps<InstanceOp>()) {
    if (i.getInstanceName() == "ingressDeadlock")
      return reject("FASED deadlock assertion instance already exists");
    if (i.getInstanceName() == "awQueue" && i.getModuleName() == "GGFASEDIngressAWQueue10") awQueue = i;
  }
  if (!relaxed || !credits || !order || !clock || !reset || !w || !awQueue ||
      awQueue.getNumResults() < 3 || awQueue.getPortNameStr(2) != "enq")
    return reject("FASED deadlock checks need exact policy, credits, order and enqueue boundaries");
  auto awType = dyn_cast<BundleType>(awQueue.getResult(2).getType());
  if (!awType || awType.getElements().size() != 3 || awType.getElements()[0].name != "ready" ||
      !awType.getElements()[0].isFlip || awType.getElements()[0].type != bit ||
      awType.getElements()[1].name != "valid" || awType.getElements()[1].isFlip ||
      awType.getElements()[1].type != bit)
    return reject("FASED deadlock checks need Decoupled AW enqueue");

  b.setInsertionPointToEnd(circuit.getBodyBlock());
  SmallVector<PortInfo> hp{{b.getStringAttr("clock"),ClockType::get(ctx),Direction::In}};
  for (auto n : {"reset","relaxed","awEmpty","wEmpty","orderValid",
                 "awValid","awReady","wValid","wReady"})
    hp.push_back({b.getStringAttr(n),bit,Direction::In});
  auto helper = b.create<FModuleOp>(loc,b.getStringAttr(helperName),
      ConventionAttr::get(ctx,Convention::Internal),hp);
  b.setInsertionPointToStart(helper.getBodyBlock());
  auto arg = [&](unsigned i) { return helper.getBodyBlock()->getArgument(i); };
  auto neg = [&](Value v)->Value { return b.create<NotPrimOp>(loc,v); };
  auto both = [&](Value a,Value z)->Value { return b.create<AndPrimOp>(loc,a,z); };
  Value enabled = neg(arg(1));
  for (auto [valid,ready,empty,message] : {
      std::tuple<unsigned,unsigned,unsigned,llvm::StringRef>{8,9,4,
        "DEADLOCK: Timing model requests w enqueue, but wQueue is full and cannot drain"},
      {6,7,3,"DEADLOCK: Timing model requests aw enqueue, but is awQueue is full and cannot drain"}}) {
    Value cannotDrain = b.create<MuxPrimOp>(loc,arg(2),arg(empty),neg(arg(5)));
    Value permitted = neg(both(both(arg(valid),neg(arg(ready))),cannotDrain));
    b.create<AssertOp>(loc,arg(0),permitted,enabled,message,ValueRange{},"");
  }
  auto field = [&](Value v,llvm::StringRef n)->Value { return b.create<SubfieldOp>(loc,v,n); };
  auto connect = [&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  // Append ports and clone uses from the bottom up. Existing values keep their
  // indices, annotations and all users, including the queue ready feedback.
  for (unsigned j = modules.size()-1; j > 0; --j) {
    unsigned index = modules[j].getNumPorts();
    SmallVector<std::pair<unsigned,PortInfo>> added{{index,
        PortInfo(b.getStringAttr(contextName),context,Direction::In)}};
    modules[j].insertPorts(added);
    InstanceOp replacement = chain[j-1].cloneAndInsertPorts(added);
    for (unsigned i = 0; i < chain[j-1].getNumResults(); ++i)
      chain[j-1].getResult(i).replaceAllUsesWith(replacement.getResult(i));
    chain[j-1].erase(); chain[j-1] = replacement;
  }
  for (unsigned j = 1; j+1 < modules.size(); ++j) {
    b.setInsertionPointToEnd(modules[j].getBodyBlock());
    connect(chain[j].getResult(chain[j].getNumResults()-1),
            modules[j].getBodyBlock()->getArgument(modules[j].getNumPorts()-1));
  }
  b.setInsertionPointToEnd(modules[0].getBodyBlock());
  Value outerContext = chain[0].getResult(chain[0].getNumResults()-1);
  connect(field(outerContext,"relaxed"),modules[0].getBodyBlock()->getArgument(*relaxed));
  Value creditStatus = modules[0].getBodyBlock()->getArgument(*credits);
  for (auto n : {"awEmpty","wEmpty"}) connect(field(outerContext,n),field(creditStatus,n));
  connect(field(outerContext,"orderValid"),field(chain[0].getResult(*order),"valid"));
  b.setInsertionPointToEnd(gates.getBodyBlock());
  auto check = b.create<InstanceOp>(loc,helper,"ingressDeadlock");
  connect(check.getResult(0),gates.getBodyBlock()->getArgument(*clock));
  connect(check.getResult(1),gates.getBodyBlock()->getArgument(*reset));
  Value policy = gates.getBodyBlock()->getArgument(gates.getNumPorts()-1);
  const llvm::StringRef policyFields[]{"relaxed","awEmpty","wEmpty","orderValid"};
  for (auto [j,n] : llvm::enumerate(policyFields))
    connect(check.getResult(j+2),field(policy,n));
  const llvm::StringRef enqueueFields[]{"valid","ready"};
  for (auto [j,n] : llvm::enumerate(enqueueFields)) {
    connect(check.getResult(j+6),field(awQueue.getResult(2),n));
    connect(check.getResult(j+8),field(gates.getBodyBlock()->getArgument(*w),n));
  }
  return success();
}
