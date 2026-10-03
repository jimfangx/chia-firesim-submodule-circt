// See LICENSE for license details.
// Oracle: LoadMem.scala.genHeader and Widget.genConstructor.
// Requires a unique live bank and matching control read/write allocation.
// Validate the nine MMIO slots and live packing/unpacking geometry before
// appending loadmem_t to .const.h. Only rawAnnotations changes.
#include "goldengate/LoadMemHeader.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <cstdint>

using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::prepareLoadMemHeader(
    CircuitOp circuit, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("LoadMem header needs retained annotations");
  DictionaryAttr output;
  unsigned outputIndex = 0;
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto cls = d ? d.getAs<StringAttr>("class") : StringAttr();
    if (!cls) return reject("malformed LoadMem header annotation");
    auto suffix = d.getAs<StringAttr>("fileSuffix");
    if (cls.getValue() == AnnotationClasses::OutputFile && suffix && suffix == ".const.h") {
      if (output) return reject("ambiguous driver header output");
      output = d; outputIndex = i;
    }
  }
  auto previous = output ? output.getAs<StringAttr>("body") : StringAttr();
  if (!previous || output.get("goldengate.loadMemHeader"))
    return reject("LoadMem needs one driver header without a prior LoadMem constructor");

  circt::firrtl::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses()) return reject("invalid LoadMem header top");
  auto liveModule = [&](StringRef name) -> FModuleOp {
    auto *target = graph.lookup(StringAttr::get(circuit.getContext(), name));
    if (!target) return {};
    // Count instance paths, saturating at two, without expanding shared target
    // hierarchy. A module definition alone cannot authorize a constructor.
    llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
    llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
    bool recursive = false;
    std::function<unsigned(circt::igraph::InstanceGraphNode *)> count = [&](auto *node) {
      if (active.count(node)) { recursive = true; return 0U; }
      auto found = counts.find(node);
      if (found != counts.end()) return found->second;
      active.insert(node);
      unsigned n = node == target;
      for (auto *record : *node) n = std::min(2U, n + count(record->getTarget()));
      active.erase(node); counts[node] = n; return n;
    };
    if (count(top) != 1 || recursive) return {};
    return dyn_cast<FModuleOp>(target->getModule().getOperation());
  };
  auto bank = liveModule("GGLoadMemWriteMMIOBank");
  auto decoder = liveModule("GGControlAddressDecode");
  auto writes = liveModule("GGControlWidgetWriteWrapper");
  auto reads = liveModule("GGControlReadDispatchWrapper");
  if (!bank || !decoder || !writes || !reads)
    return reject("LoadMem bank, decoder and read/write bindings need unique live instance paths");
  auto findBinding = [&](FModuleOp module, StringRef attribute) -> DictionaryAttr {
    auto bindings = module->getAttrOfType<ArrayAttr>(attribute);
    DictionaryAttr found;
    if (!bindings) return {};
    for (auto attr : bindings) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto port = d ? d.getAs<StringAttr>("port") : StringAttr();
      if (!port || port != "loadmem_ctrl") continue;
      if (found) return {};
      found = d;
    }
    return found;
  };
  auto binding = findBinding(writes, "goldengate.controlWriteBindings");
  if (!binding || binding != findBinding(reads, "goldengate.controlReadBindings"))
    return reject("LoadMem read and write control identities differ");
  auto slave = binding.getAs<IntegerAttr>("slave");
  auto regions = decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  if (!slave || slave.getInt() < 0 || !regions || uint64_t(slave.getInt()) >= regions.size())
    return reject("missing LoadMem control allocation");
  auto row = dyn_cast<DictionaryAttr>(regions[slave.getInt()]);
  auto name = row ? row.getAs<StringAttr>("name") : StringAttr();
  auto regionSlave = row ? row.getAs<IntegerAttr>("slave") : IntegerAttr();
  auto start = row ? row.getAs<IntegerAttr>("start") : IntegerAttr();
  auto size = row ? row.getAs<IntegerAttr>("size") : IntegerAttr();
  unsigned widgetIndex = 0;
  StringRef identity = name ? name.getValue() : StringRef();
  if (name != binding.getAs<StringAttr>("name") || !identity.consume_front("LoadMemWidget_") || identity.empty() ||
      identity.getAsInteger(10, widgetIndex) || !regionSlave || regionSlave.getInt() != slave.getInt() ||
      !start || start.getInt() < 0 || start.getInt() % 4 || !size || size.getInt() < 36)
    return reject("invalid LoadMem widget identity or MMIO region");
  auto address = decoder.getNumPorts() ? dyn_cast<UIntType>(decoder.getPorts()[0].type) : UIntType();
  if (!address || !address.getWidth() || *address.getWidth() < 1 || *address.getWidth() > 63 ||
      uint64_t(start.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
      uint64_t(size.getInt()) > (uint64_t(1) << *address.getWidth()) - start.getInt())
    return reject("LoadMem MMIO region exceeds control address width");
  for (auto [i, attr] : llvm::enumerate(regions)) {
    if (i == uint64_t(slave.getInt())) continue;
    auto d = dyn_cast<DictionaryAttr>(attr);
    auto otherName = d ? d.getAs<StringAttr>("name") : StringAttr();
    auto otherStart = d ? d.getAs<IntegerAttr>("start") : IntegerAttr();
    auto otherSize = d ? d.getAs<IntegerAttr>("size") : IntegerAttr();
    if (!otherName || otherName == name || !otherStart || otherStart.getInt() < 0 ||
        !otherSize || otherSize.getInt() <= 0 ||
        uint64_t(otherStart.getInt()) >= (uint64_t(1) << *address.getWidth()) ||
        uint64_t(otherSize.getInt()) > (uint64_t(1) << *address.getWidth()) - otherStart.getInt() ||
        (uint64_t(otherStart.getInt()) < uint64_t(start.getInt()) + size.getInt() &&
         uint64_t(start.getInt()) < uint64_t(otherStart.getInt()) + otherSize.getInt()))
      return reject("ambiguous LoadMem MMIO allocation");
  }
  auto writeData = liveModule("GGLoadMemWriteDataWrapper");
  auto readRequests = liveModule("GGLoadMemReadRequestWrapper");
  auto readData = liveModule("GGLoadMemReadDataWrapper");
  auto pack = liveModule("GGLoadMemWriteDataFIFO");
  auto unpack = liveModule("GGLoadMemReadDataFIFO");
  auto requests = liveModule("GGLoadMemReadRequests");
  auto writer = liveModule("GGLoadMemWriter");
  if (!writeData || !readRequests || !readData || !pack || !unpack || !requests || !writer)
    return reject("LoadMem MMIO owners, writer and FIFOs need unique live instance paths");
  auto uintWidth = [](Type type) -> unsigned {
    auto u = dyn_cast<UIntType>(type); return u && u.getWidth() ? *u.getWidth() : 0;
  };
  auto portWidth = [&](FModuleOp m, unsigned i) {
    return i < m.getNumPorts() ? uintWidth(m.getPortType(i)) : 0U;
  };
  if (bank.getNumPorts() != 10 || requests.getNumPorts() != 16)
    return reject("LoadMem address bank geometry differs");
  unsigned controlBits = portWidth(pack, 4), memoryBits = portWidth(pack, 7);
  if (controlBits != 32 || !memoryBits || memoryBits % controlBits ||
      portWidth(unpack, 4) != memoryBits || portWidth(unpack, 7) != controlBits ||
      portWidth(writer, 16) != memoryBits || portWidth(writer, 12) != portWidth(requests, 15) ||
      portWidth(writer, 12) != 34)
    return reject("LoadMem driver chunk count needs matching typed memory and control beats");
  // The constructor passes conf_target.mem, which is emitted from FPGATop.
  // Its physical memory payload must agree with the LoadMem FIFO chunk count.
  auto platform = liveModule("FPGATop");
  Type memory;
  if (platform) for (auto port : platform.getPorts())
    if (port.name == "mem_0" && port.direction == Direction::Out) memory=port.type;
  auto memoryWidth = [&](std::initializer_list<StringRef> path) {
    Type type=memory;
    for (auto name : path) {
      auto bundle=type ? dyn_cast<BundleType>(type) : BundleType();
      auto field=bundle ? bundle.getElement(name) : std::nullopt;
      if (!field) return 0U; type=field->type;
    }
    return uintWidth(type);
  };
  if (!platform || memoryWidth({"w","bits","data"}) != memoryBits ||
      memoryWidth({"r","bits","data"}) != memoryBits ||
      memoryWidth({"aw","bits","addr"}) != portWidth(writer,12) ||
      memoryWidth({"ar","bits","addr"}) != portWidth(requests,15))
    return reject("LoadMem constructor memory config differs from the live platform interface");
  for (auto pair : {std::make_pair(pack, false), std::make_pair(unpack, true)}) {
    auto input = pair.first->getAttrOfType<IntegerAttr>("goldengate.inputWidth");
    auto output = pair.first->getAttrOfType<IntegerAttr>("goldengate.outputWidth");
    if (!input || !output || input.getInt() != portWidth(pair.first, 4) ||
        output.getInt() != portWidth(pair.first, 7))
      return reject("LoadMem FIFO metadata differs from live packing/unpacking ports");
  }
  const char *fields[]{"W_ADDRESS_H", "W_ADDRESS_L", "W_LENGTH", "ZERO_OUT_DRAM", "W_DATA",
                       "ZERO_FINISHED", "R_ADDRESS_H", "R_ADDRESS_L", "R_DATA"};
  const bool readable[]{true,true,false,false,false,true,true,false,true};
  const bool writeable[]{true,true,true,true,true,false,true,true,false};
  FModuleOp owners[]{bank,bank,bank,bank,writeData,bank,readRequests,readRequests,readData};
  const char *controls[]{"mcr","mcr","mcr","mcr","loadmemData_mcr","mcr",
                        "loadmemRead_mcr","loadmemRead_mcr","loadmemReadData_mcr"};
  Value control[9];
  unsigned seen[9]{};
  for (auto owner : {bank,writeData,readRequests,readData}) {
    auto registers = owner->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    if (!registers) return reject("LoadMem MMIO owner is missing register identities");
    for (auto attr : registers) {
      auto d = dyn_cast<DictionaryAttr>(attr);
      auto field = d ? d.getAs<StringAttr>("name") : StringAttr();
      auto offset = d ? d.getAs<IntegerAttr>("offset") : IntegerAttr();
      auto rd = d ? d.getAs<BoolAttr>("readable") : BoolAttr();
      auto wr = d ? d.getAs<BoolAttr>("writeable") : BoolAttr();
      unsigned word = 9;
      for (unsigned i=0;i<9;++i) if (field && field == fields[i]) word=i;
      if (word == 9 || owners[word] != owner || ++seen[word] != 1 || !offset ||
          offset.getInt() != word*4 || !rd || rd.getValue() != readable[word] ||
          !wr || wr.getValue() != writeable[word])
        return reject("LoadMem register identity/owner/offset/permissions differ from driver ABI");
    }
  }
  for (unsigned i=0;i<9;++i) {
    if (seen[i] != 1) return reject("LoadMem driver requires all nine MMIO words");
    for (unsigned p=0;p<owners[i].getNumPorts();++p)
      if (owners[i].getPortName(p) == controls[i] && owners[i].getPortDirection(p) == Direction::Out)
        control[i] = owners[i].getBodyBlock()->getArgument(p);
    auto mcr = control[i] ? dyn_cast<BundleType>(control[i].getType()) : BundleType();
    for (StringRef group : {"read", "write"}) {
      auto slot = mcr ? mcr.getElement((group+"_"+Twine(i)).str()) : std::nullopt;
      auto token = slot ? dyn_cast<BundleType>(slot->type) : BundleType();
      auto bits = token ? token.getElement("bits") : std::nullopt;
      auto valid = token ? token.getElement("valid") : std::nullopt;
      auto ready = token ? token.getElement("ready") : std::nullopt;
      if (!slot || slot->isFlip != (group == "write") || !bits || uintWidth(bits->type) != controlBits ||
          !valid || uintWidth(valid->type) != 1 || !ready || uintWidth(ready->type) != 1)
        return reject("LoadMem driver addresses require 32-bit decoded read/write slots");
    }
  }
  auto slot = [&](Value v, unsigned i, StringRef group, StringRef member) {
    auto field = v ? v.getDefiningOp<SubfieldOp>() : SubfieldOp();
    auto token = field ? field.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
    return field && field.getFieldName() == member && token &&
           token.getFieldName() == (group+"_"+Twine(i)).str() && token.getInput() == control[i];
  };
  auto slotValue = [&](unsigned i, StringRef group, StringRef member) -> Value {
    Value found;
    for (auto f : owners[i].getOps<SubfieldOp>()) if (slot(f.getResult(),i,group,member)) {
      if (found) return {}; found = f.getResult();
    }
    return found;
  };
  auto source = [](FModuleOp m, Value dest) -> Value {
    Value found;
    for (auto c : m.getOps<StrictConnectOp>()) if (c.getDest() == dest) {
      if (found) return {}; found = c.getSrc();
    }
    return found;
  };
  auto constant = [](Value v, unsigned n) {
    auto c = v ? v.getDefiningOp<ConstantOp>() : ConstantOp();
    return c && c.getValue().getZExtValue() == n;
  };
  auto instance = [](FModuleOp m, FModuleOp helper) -> InstanceOp {
    InstanceOp found;
    for (auto i : m.getOps<InstanceOp>()) if (i.getModuleName() == helper.getName()) {
      if (found) return {}; found=i;
    }
    return found;
  };
  auto packInstance = instance(writeData,pack), unpackInstance = instance(readData,unpack);
  auto requestInstance = instance(readRequests,requests);
  if (!packInstance || !unpackInstance || !requestInstance)
    return reject("LoadMem MMIO owner does not instantiate its advertised helper");
  // Bind the advertised data registers to actual packing/unpacking handshakes.
  if (source(writeData,slotValue(4,"write","ready")) != packInstance.getResult(2) ||
      !slot(source(writeData,packInstance.getResult(3)),4,"write","valid") ||
      !slot(source(writeData,packInstance.getResult(4)),4,"write","bits") ||
      !constant(source(writeData,slotValue(4,"read","valid")),0) ||
      !constant(source(writeData,slotValue(4,"read","bits")),0) ||
      !slot(source(readData,unpackInstance.getResult(5)),8,"read","ready") ||
      source(readData,slotValue(8,"read","valid")) != unpackInstance.getResult(6) ||
      source(readData,slotValue(8,"read","bits")) != unpackInstance.getResult(7) ||
      !constant(source(readData,slotValue(8,"write","ready")),0))
    return reject("LoadMem W_DATA/R_DATA slots differ from the typed FIFO handshakes");
  for (unsigned i=6;i<=7;++i) {
    unsigned first = i == 6 ? 2 : 7;
    if (source(readRequests,slotValue(i,"write","ready")) != requestInstance.getResult(first) ||
        !slot(source(readRequests,requestInstance.getResult(first+1)),i,"write","valid") ||
        !slot(source(readRequests,requestInstance.getResult(first+2)),i,"write","bits") ||
        source(readRequests,slotValue(i,"read","valid")) != requestInstance.getResult(first+3) ||
        source(readRequests,slotValue(i,"read","bits")) != requestInstance.getResult(first+4))
      return reject("LoadMem read-address MMIO slots differ from the request helper");
  }
  auto addressState = [&](FModuleOp m, Value read, StringRef name, unsigned width,
                          Value valid, Value bits) -> Value {
    auto pad = read ? read.getDefiningOp<PadPrimOp>() : PadPrimOp();
    auto reg = pad ? pad.getInput().getDefiningOp<RegOp>() : RegOp();
    if (!reg || reg.getName() != name || uintWidth(reg.getResult().getType()) != width ||
        reg->getOperand(0) != m.getBodyBlock()->getArgument(0)) return {};
    auto next = source(m,reg.getResult());
    auto mux = next ? next.getDefiningOp<MuxPrimOp>() : MuxPrimOp();
    if (!mux || mux.getSel() != valid || mux.getLow() != reg.getResult()) return {};
    Value write = mux.getHigh();
    if (width != controlBits) {
      auto slice = write.getDefiningOp<BitsPrimOp>();
      if (!slice || slice.getHi() != width-1 || slice.getLo() != 0) return {};
      write=slice.getInput();
    }
    return write == bits ? reg.getResult() : Value();
  };
  Value high = addressState(bank,source(bank,slotValue(0,"read","bits")),fields[0],2,
                            slotValue(0,"write","valid"),slotValue(0,"write","bits"));
  Value low = addressState(bank,source(bank,slotValue(1,"read","bits")),fields[1],32,
                           slotValue(1,"write","valid"),slotValue(1,"write","bits"));
  Value readHigh = addressState(requests,source(requests,requests.getBodyBlock()->getArgument(6)),fields[6],2,
                                requests.getBodyBlock()->getArgument(3),requests.getBodyBlock()->getArgument(4));
  if (!high || !low || !readHigh) return reject("LoadMem address readback/update state differs from MMIO identities");
  auto a = [&](unsigned i) { return bank.getBodyBlock()->getArgument(i); };
  if (bank.getNumPorts() != 10) return reject("LoadMem write register bank geometry differs");
  Value addrValue = source(bank,a(4)), lengthValue = source(bank,a(5));
  Value finishedValue = source(bank,slotValue(5,"read","bits"));
  auto addr = addrValue ? addrValue.getDefiningOp<CatPrimOp>() : CatPrimOp();
  auto length = lengthValue ? lengthValue.getDefiningOp<PadPrimOp>() : PadPrimOp();
  auto finished = finishedValue ? finishedValue.getDefiningOp<PadPrimOp>() : PadPrimOp();
  if (!addr || addr.getLhs() != high || addr.getRhs() != low || !length ||
      !slot(length.getInput(),2,"write","bits") || !finished || finished.getInput() != a(8) ||
      !slot(source(bank,a(3)),2,"write","valid") || !slot(source(bank,a(7)),3,"write","valid") ||
      source(bank,slotValue(2,"write","ready")) != a(2) || source(bank,slotValue(3,"write","ready")) != a(6))
    return reject("LoadMem write/zero request and completion slots differ from advertised MMIO");
  for (unsigned i : {0U,1U,2U,3U,5U})
    if (!constant(source(bank,slotValue(i,"read","valid")),readable[i]))
      return reject("LoadMem register read permissions differ from emitted handshakes");
  std::string snippet; llvm::raw_string_ostream out(snippet);
  out << "\n#ifdef GET_INCLUDES\n#include \"bridges/loadmem.h\"\n#endif // GET_INCLUDES\n"
         "#ifdef GET_SUBSTRUCT_CHECKS\n";
  for (unsigned i=0;i<9;++i)
    out << "static_assert(offsetof(LOADMEMWIDGET_struct, " << fields[i] << ") == "
        << i << " * sizeof(uint64_t), \"invalid " << fields[i] << "\");\n";
  out << "static_assert(sizeof(LOADMEMWIDGET_struct) == 9 * sizeof(uint64_t), \"invalid structure\");\n"
         "#endif // LOADMEMWIDGET_checks\n#ifdef GET_CORE_CONSTRUCTOR\n"
         "registry.add_widget(new loadmem_t(\n  simif,\nLOADMEMWIDGET_struct{\n";
  for (unsigned i=0;i<9;++i) out << "    ." << fields[i] << " = " << start.getInt()+i*4 << ",\n";
  out << "},\n  " << widgetIndex << ",\n  args,\n  conf_target.mem,\n  " << memoryBits/controlBits
      << "U\n));\n#endif // GET_CORE_CONSTRUCTOR\n";
  out.flush(); OpBuilder b(circuit.getContext()); NamedAttrList updated(output);
  updated.set("body",b.getStringAttr(previous.getValue().str()+snippet));
  updated.set("goldengate.loadMemHeader",b.getBoolAttr(true));
  SmallVector<Attribute> annotations(raw.begin(),raw.end()); annotations[outputIndex]=updated.getDictionary(circuit.getContext());
  circuit->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return success();
}
