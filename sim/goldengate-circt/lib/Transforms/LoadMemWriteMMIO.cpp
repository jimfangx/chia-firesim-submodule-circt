// See LICENSE for license details.
// Required input invariants: uninstantiated GGLoadMemRequestWrapper, retained
// rawAnnotations, exact U250 host clock/reset and seven request boundary ports,
// and fresh bank/wrapper/control names. This decoded bank covers words 0,1,2,3,5;
// W_DATA (4) and read-memory words (6,7,8) require their FIFO/read paths later.
// Scala genWOReg calls attach with its default ReadWrite permission: address
// words are readable, unreset, and written regardless of host reset or wstrb.
// Annotations consumed/produced: none. Transfer circuit/copied-port targets;
// consumed request targets stay on the inner wrapper. Emit MMIO name/offset/
// permission attributes for the five implemented words.
// IR mutations: add FIRRTL register/decoded MCR operations and outer wrapper.
// Analyses required: none. Preserved: inner operations and channel endpoints.
// Hierarchy analyses must be rebuilt. Output invariants: verified FIRRTL;
// pre-edge address concatenation, UInt32 length zero-extension, live zero status.
// No MCRFile AXI transport, write-data FIFO, or read-memory path is introduced.
#include "goldengate/LoadMemWriter.h"
#include "mlir/IR/Builders.h"
#include <functional>
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::addLoadMemWriteMMIO(CircuitOp circuit, std::string &error) {
  constexpr llvm::StringLiteral wrapperName = "GGLoadMemWriteMMIOWrapper";
  constexpr llvm::StringLiteral bankName = "GGLoadMemWriteMMIOBank";
  constexpr llvm::StringLiteral controlName = "loadmemWrite_mcr";
  auto reject = [&](llvm::StringRef s) { error = s.str(); return failure(); };
  if (circuit.getName() != "GGLoadMemRequestWrapper")
    return reject("LoadMem write MMIO requires the active request wrapper");
  FModuleOp inner;
  for (auto m : circuit.getOps<FModuleLike>()) {
    if (m.getModuleName() == wrapperName || m.getModuleName() == bankName)
      return reject("LoadMem write MMIO helper or wrapper already exists");
    if (m.getModuleName() == circuit.getName()) inner = dyn_cast<FModuleOp>(m.getOperation());
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!inner || !raw) return reject("LoadMem write MMIO needs a top and retained annotations");
  auto *context = circuit.getContext(); OpBuilder b(context); auto loc = circuit.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(context, w, false); }; auto bit = uint(1);
  const llvm::StringRef names[]{"hostClock", "hostReset", "loadmem_write_ready", "loadmem_write_valid",
      "loadmem_write_bits_addr", "loadmem_write_bits_len", "loadmem_zero_ready", "loadmem_zero_valid", "loadmem_zero_finished"};
  const unsigned widths[]{0,1,1,1,34,34,1,1,1};
  unsigned indices[9];
  for (unsigned j = 0; j < 9; ++j) {
    std::optional<unsigned> found;
    for (auto [i, p] : llvm::enumerate(inner.getPorts()))
      if (p.name == names[j] && p.direction == (j == 2 || j == 6 || j == 8 ? Direction::Out : Direction::In) &&
          p.type == (j == 0 ? Type(ClockType::get(context)) : Type(uint(widths[j])))) found = i;
    if (!found) return reject("LoadMem write MMIO needs exact U250 clock/reset and request geometry");
    indices[j] = *found;
  }
  for (auto p : inner.getPorts()) {
    auto n = p.name.getValue();
    if ((n.starts_with("loadmem_write_") || n.starts_with("loadmem_zero_")) &&
        !llvm::is_contained(ArrayRef<llvm::StringRef>(names), n)) return reject("unsupported LoadMem request field");
    if (n == controlName) return reject("LoadMem write MCR boundary already exists");
  }
  bool used = false; circuit.walk([&](InstanceOp i) { used |= i.getModuleName() == inner.getName(); });
  if (used) return reject("LoadMem write MMIO needs an uninstantiated top");
  auto token = BundleType::get(context, {{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit}, {b.getStringAttr("bits"),false,uint(32)}});
  const unsigned slots[]{0,1,2,3,5};
  SmallVector<BundleType::BundleElement> fields;
  for (auto i : slots) {
    fields.push_back({b.getStringAttr("read_" + std::to_string(i)),false,token});
    fields.push_back({b.getStringAttr("write_" + std::to_string(i)),true,token});
  }
  fields.push_back({b.getStringAttr("wstrb"),true,uint(4)});
  auto mcr = BundleType::get(context,fields);
  SmallVector<PortInfo> bankPorts{{b.getStringAttr("clock"),ClockType::get(context),Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In}};
  for (unsigned i = 2; i < 9; ++i) bankPorts.push_back({b.getStringAttr(names[i].drop_front(8)),uint(widths[i]),
      i == 2 || i == 6 || i == 8 ? Direction::In : Direction::Out});
  bankPorts.push_back({b.getStringAttr("mcr"),mcr,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto bank = b.create<FModuleOp>(loc,b.getStringAttr(bankName),ConventionAttr::get(context,Convention::Internal),bankPorts);
  const llvm::StringRef registerNames[]{"W_ADDRESS_H","W_ADDRESS_L","W_LENGTH","ZERO_OUT_DRAM","ZERO_FINISHED"};
  SmallVector<Attribute> metadata;
  for (unsigned i = 0; i < 5; ++i) metadata.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr(registerNames[i])),b.getNamedAttr("offset",b.getI32IntegerAttr(slots[i]*4)),
      b.getNamedAttr("readable",b.getBoolAttr(i < 2 || i == 4)),b.getNamedAttr("writeable",b.getBoolAttr(i < 4))}));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(metadata));
  b.setInsertionPointToStart(bank.getBodyBlock());
  auto arg = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(loc,v,n); };
  auto slot = [&](llvm::StringRef group,unsigned i) -> Value { return field(arg(9), group.str()+"_"+std::to_string(i)); };
  auto connect = [&](Value d,Value s) { b.create<StrictConnectOp>(loc,d,s); };
  auto constant = [&](unsigned w,uint64_t n) -> Value { return b.create<ConstantOp>(loc,uint(w),APInt(w,n)); };
  Value one = constant(1,1), zero = constant(1,0), resetInactive = b.create<NotPrimOp>(loc,arg(1));
  Value high = b.create<RegOp>(loc,uint(2),arg(0),"W_ADDRESS_H").getResult(), low = b.create<RegOp>(loc,uint(32),arg(0),"W_ADDRESS_L").getResult();
  connect(high,b.create<MuxPrimOp>(loc,field(slot("write",0),"valid"),
      b.create<BitsPrimOp>(loc,field(slot("write",0),"bits"),1,0),high));
  connect(low,b.create<MuxPrimOp>(loc,field(slot("write",1),"valid"),field(slot("write",1),"bits"),low));
  connect(arg(3),field(slot("write",2),"valid")); connect(arg(4),b.create<CatPrimOp>(loc,high,low));
  connect(arg(5),b.create<PadPrimOp>(loc,field(slot("write",2),"bits"),34));
  connect(field(slot("write",2),"ready"),arg(2));
  connect(arg(7),field(slot("write",3),"valid")); connect(field(slot("write",3),"ready"),arg(6));
  for (auto i : slots) {
    Value rd=slot("read",i), wr=slot("write",i);
    Value data = i == 0 ? high : i == 1 ? low : i == 5 ? arg(8) : constant(32,0);
    connect(field(rd,"bits"),b.create<PadPrimOp>(loc,data,32));
    connect(field(rd,"valid"),i == 2 || i == 3 ? zero : one);
    if (i != 2 && i != 3) connect(field(wr,"ready"),one);
    if (i == 2 || i == 3) b.create<AssertOp>(loc,arg(0),b.create<NotPrimOp>(loc,field(rd,"ready")),
        resetInactive,"Can only write to this decoupled sink",ValueRange{},"");
    if (i == 5) b.create<AssertOp>(loc,arg(0),b.create<NotPrimOp>(loc,field(wr,"valid")),
        resetInactive,"Register ZERO_FINISHED is read only",ValueRange{},"");
  }
  SmallVector<PortInfo> ports; SmallVector<unsigned> copied;
  for (auto [i,p] : llvm::enumerate(inner.getPorts()))
    if (!llvm::is_contained(ArrayRef<unsigned>(indices).drop_front(2),i)) { copied.push_back(i); ports.push_back(p); }
  ports.push_back({b.getStringAttr(controlName),mcr,Direction::Out});
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto wrapper = b.create<FModuleOp>(loc,b.getStringAttr(wrapperName),inner.getConventionAttr(),ports);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(loc,inner,"sim"), mmio=b.create<InstanceOp>(loc,bank,"loadmemRegisters");
  for (auto [j,i] : llvm::enumerate(copied)) {
    auto p=inner.getPorts()[i]; auto external=wrapper.getBodyBlock()->getArgument(j);
    b.create<ConnectOp>(loc,p.direction==Direction::In ? sim.getResult(i) : external,
        p.direction==Direction::In ? external : sim.getResult(i));
    if (i == indices[0] || i == indices[1]) connect(mmio.getResult(i == indices[0] ? 0 : 1),external);
  }
  for (unsigned j=2;j<9;++j) {
    bool toBank=j==2 || j==6 || j==8;
    connect(toBank ? mmio.getResult(j) : sim.getResult(indices[j]),toBank ? sim.getResult(indices[j]) : mmio.getResult(j));
  }
  b.create<ConnectOp>(loc,wrapper.getBodyBlock()->getArguments().back(),mmio.getResult(9));
  std::string oldPrefix = "~" + circuit.getName().str(), newPrefix = "~" + wrapperName.str();
  std::string modulePrefix = "|" + inner.getName().str() + ">";
  std::function<Attribute(Attribute)> retarget = [&](Attribute attr) -> Attribute {
    if (auto s = dyn_cast<StringAttr>(attr)) {
      auto value = s.getValue(); if (value == oldPrefix) return b.getStringAttr(newPrefix);
      if (!value.consume_front(oldPrefix + "|")) return attr;
      std::string suffix = "|" + value.str(); llvm::StringRef ref(suffix);
      if (ref.consume_front(modulePrefix)) {
        auto name = ref.take_front(ref.find_first_of(".["));
        for (auto i : copied) if (name == inner.getPortName(i)) {
          suffix.replace(0, modulePrefix.size(), "|" + wrapperName.str() + ">"); break;
        }
      }
      return b.getStringAttr(newPrefix + suffix);
    }
    if (auto a = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values; for (auto v : a) values.push_back(retarget(v)); return b.getArrayAttr(values);
    }
    if (auto d = dyn_cast<DictionaryAttr>(attr)) {
      NamedAttrList values; for (auto v : d) values.set(v.getName(), retarget(v.getValue())); return values.getDictionary(context);
    }
    return attr;
  };
  SmallVector<Attribute> annotations; for (auto a : raw) annotations.push_back(retarget(a));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations)); circuit.setName(wrapperName);
  return success();
}
