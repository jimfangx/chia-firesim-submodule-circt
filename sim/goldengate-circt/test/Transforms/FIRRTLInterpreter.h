// See LICENSE for license details.
// Test-only emitted FIRRTL graph evaluator. Include inside the fixture namespace
// after require(bool, string) and field(builder, location, value, name).
struct Interpreter {
  SmallVector<FModuleOp> scopes;
  SmallVector<FModuleOp> instanceCopies;
  llvm::DenseMap<Value, Value> aliases;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  std::set<std::string> visiting;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>())
      return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto i = v.getDefiningOp<SubindexOp>())
      return key(i.getInput()) + "[" + std::to_string(i.getIndex()) + "]";
    if (aliases.count(v)) return key(aliases.lookup(v));
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(CircuitOp circuit, FModuleOp top) {
    // Elaborate repeated definitions into private interpreter copies so each
    // queue instance owns independent registers. The exported compiler IR
    // retains the shared definition and the actual instance connections.
    std::function<void(FModuleOp)> visit = [&](FModuleOp scope) {
      require(!llvm::is_contained(scopes, scope), "multiply instantiated fixture module");
      scopes.push_back(scope);
      for (auto instance : scope.getOps<InstanceOp>()) {
        FModuleOp child;
        for (auto module : circuit.getOps<FModuleOp>())
          if (module.getName() == instance.getModuleName()) child = module;
        if (!child) continue; // AbstractClockGate extmodule: observe its CE.
        if (llvm::is_contained(scopes, child)) {
          child = cast<FModuleOp>(child->clone());
          instanceCopies.push_back(child);
        }
        for (unsigned i = 0; i < child.getNumPorts(); ++i)
          aliases[child.getArgument(i)] = instance.getResult(i);
        visit(child);
      }
    };
    visit(top);
    // Channelization emits aggregate connects. Project their actual fields,
    // including ready's reversed flow, without rebuilding the wiring by hand.
    OpBuilder builder(top.getContext());
    std::function<void(Value, Value, Location)> connect =
        [&](Value dest, Value src, Location loc) {
      if (auto bundle = dyn_cast<BundleType>(dest.getType())) {
        for (const auto &element : bundle.getElements()) {
          auto d = field(builder, loc, dest, element.name.getValue());
          auto s = field(builder, loc, src, element.name.getValue());
          connect(element.isFlip ? s : d, element.isFlip ? d : s, loc);
        }
      } else if (auto vector = dyn_cast<FVectorType>(dest.getType())) {
        for (unsigned i = 0; i < vector.getNumElements(); ++i)
          connect(builder.create<SubindexOp>(loc, dest, i),
                  builder.create<SubindexOp>(loc, src, i), loc);
      } else {
        require(drivers.emplace(key(dest), src).second, "multiple drivers");
      }
    };
    for (auto scope : scopes)
      for (auto &operation : llvm::make_early_inc_range(scope.getBodyBlock()->getOperations())) {
        builder.setInsertionPoint(&operation);
        if (auto c = dyn_cast<StrictConnectOp>(operation))
          connect(c.getDest(), c.getSrc(), c.getLoc());
        else if (auto c = dyn_cast<ConnectOp>(operation))
          connect(c.getDest(), c.getSrc(), c.getLoc());
      }
  }
  ~Interpreter() {
    for (auto copy : instanceCopies) copy->destroy();
  }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    require(visiting.insert(k).second, "combinational loop");
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AsUIntPrimOp, AsClockPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (auto p = dyn_cast_or_null<MuxPrimOp>(op)) n = eval(eval(p.getSel()) ? p.getHigh() : p.getLow());
    else if (auto p = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(p.getInput()) >> p.getLo();
    else throw std::runtime_error("unsupported operation or missing driver: " + k);
    if (auto type = dyn_cast<UIntType>(v.getType()); type && type.getWidth() && *type.getWidth() < 64)
      n &= (uint64_t(1) << *type.getWidth()) - 1;
    visiting.erase(k); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto scope : scopes) {
      for (auto r : scope.getOps<RegResetOp>())
        next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
      for (auto r : scope.getOps<RegOp>()) {
        // Target registers are clocked by the actual AbstractClockGate.O.
        auto clock = dyn_cast<OpResult>(r.getClockVal());
        auto gate = clock ? dyn_cast<InstanceOp>(clock.getOwner()) : InstanceOp();
        if (gate) {
          require(gate.getModuleName() == "AbstractClockGate" && clock.getResultNumber() == 2,
                  "target state lacks gated clock");
          next[r.getResult()] = eval(gate.getResult(1)) ? eval(drivers.at(key(r.getResult()))) : state.lookup(r.getResult());
        } else {
          // ClockBridge counter snapshot registers advance on hostClock.
          next[r.getResult()] = eval(drivers.at(key(r.getResult())));
        }
      }
    }
    state = std::move(next);
  }
};
