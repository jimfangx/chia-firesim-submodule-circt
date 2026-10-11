#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print driver addresses with live IR and SFC Rocket RTL.

The immutable oracle disables Print. Check its unchanged large-bank prefix,
queue/count/DMA ABI and empty writer, and account for the new small-bank tail.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketEmptyCPUStreamWriteCompare import sfc_contract


ir_path, header_path, golden, report_path = map(Path, sys.argv[1:])
ir, header, reference = ir_path.read_text(), header_path.read_text(), clean(golden.read_text())
# Imported catalog modules also carry historical maps. Only the active decoder
# supplies addresses to the driver; do not flatten all metadata into one map.
active = re.search(r'firrtl.module @GGControlAddressDecode\(.*?attributes \{goldengate.controlRegions = \[(.*?)\]\}', ir, re.S).group(1)
regions = {name: (int(start), int(size), int(slave)) for name, size, slave, start in
           re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', active)}
assert len(regions) == 13
fields = ['startCycleL', 'startCycleH', 'endCycleL', 'endCycleH', 'doneInit', 'flushNarrowPacket']
factories = re.findall(r'make_print_bridge_(\d+)\(.*?PRINTBRIDGEMODULE_struct\{(.*?)\}, (\d+)U, args, (\d+)U\)', header, re.S)
assert len(factories) == 2
for slot, registers, widget, stream in factories:
    slot = int(slot)
    start, size, slave = regions[f'PrintBridgeModule_{slot}']
    assert (start, size, slave) == (544 + 32 * slot, 32, 8 + slot)
    assert (int(widget), int(stream)) == (slot, slot + 1)
    addresses = {name: int(value) for name, value in re.findall(r'/\* (\w+) \*/ (\d+)ULL,', registers)}
    assert addresses == {name: start + 4 * i for i, name in enumerate(fields)}
streams = re.findall(r'CPUManagedStreams::StreamParameters\(std::string\("([^"]+)"\), (\d+)ULL, (\d+)ULL, (\d+)U, (\d+)U\)', header)
assert len(streams) == 3 and regions['CPUManagedStreamEngine_0'] == (624, 16, 11)
for index, (name, base, count, depth, width) in enumerate(streams):
    assert name == ('TRACERVBRIDGEMODULE_0_to_cpu_stream' if not index else f'PRINTBRIDGEMODULE_{index - 1}_to_cpu_stream')
    assert tuple(map(int, (base, count, depth, width))) == (index * 524288, 624 + 4 * index, 6144, 64)
assert 'std::vector<CPUManagedStreams::StreamParameters>{},' in header
_, engine = module(reference, 'CPUManagedStreamEngine')
eq = equations(engine)
assert re.search(r'reg\s*\[511:0\]\s+ram\s*\[0:6143\]', module(reference, 'Queue_50')[1])
assert eq['crFile_io_mcr_read_0_bits'].strip() == "{{19'd0}, TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q_io_count}"
assert re.search(r'assert\s*\(\s*~crFile_io_mcr_write_0_valid\s*\)', engine)
assert re.fullmatch(r"auto_cpu_managed_axi4_in_ar_bits_addr\[63:19\]\s*==\s*45'h0", eq['TRACERVBRIDGEMODULE_0_to_cpu_stream_grant'].strip())
empty_writer = sfc_contract(golden)
assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
router = module(reference, 'NastiRouter')[1]
for name, start, end in [('BlockDevBridgeModule_0', 0, 128), ('FASEDMemoryTimingModel_0', 128, 256),
                         ('TracerVBridgeModule_0', 256, 320), ('TSIBridgeModule_0', 320, 384),
                         ('LoadMemWidget_0', 384, 448), ('PeekPokeBridgeModule_0', 448, 480),
                         ('UARTBridgeModule_0', 480, 512), ('ClockBridgeModule_0', 512, 544)]:
    assert regions[name][:2] == (start, end - start)
    low = r"25'h" + format(start, 'x') + r" <= io_master_ar_bits_addr & " if start else ''
    assert re.search(low + r"io_master_ar_bits_addr < 25'h" + format(end, 'x'), router)
assert re.search(r"25'h238 <= io_master_ar_bits_addr & io_master_ar_bits_addr < 25'h23c", router)
report_path.write_text(json.dumps({'golden': str(golden.resolve()), 'candidate': str(header_path.resolve()),
    'unchanged_native_bank_prefix': 8, 'print_bases': [544, 576], 'stream_indices': [1, 2],
    'candidate_count_addresses': [624, 628, 632], 'golden_count_address': 568,
    'queue_depth': 6144, 'beat_bytes': 64, 'dma_window_bytes': 524288,
    'count_read_only': True, 'empty_incoming_transport': empty_writer,
    'reference_has_print_host': False, 'absolute_count_address_match': False}, indent=2) + '\n')
print('PASS expanded Print constructors, eight unchanged native regions and SFC CPU queue/count/DMA/empty-writer ABI; expected count base 624 versus Print-disabled 568')
