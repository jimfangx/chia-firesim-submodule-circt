#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare allocated Print/count collateral with native IR and SFC transport.

The immutable platform disables Print. Absolute addresses therefore differ;
compare its shared count-word and DMA geometry, recording that difference.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module


tracerv = '--tracerv' in sys.argv[1:]
evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:] if arg != '--tracerv')
candidate = evidence / 'candidate'
header = (candidate / 'print-bridge-allocated.const.h').read_text()
ir = (candidate / 'post-print-control-master.mlir').read_text()
reference = clean(golden.read_text())
assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
regions = {name: (int(start), int(size), int(slave)) for name, size, slave, start in
           re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', ir)}
expected = {'PrintBridgeModule_0', 'CPUManagedStreamEngine_0'}
if tracerv:
    expected.add('TracerVBridgeModule_0')
    assert regions['TracerVBridgeModule_0'] == (0, 64, 0)
assert set(regions) == expected
fields = ['startCycleL', 'startCycleH', 'endCycleL', 'endCycleH', 'doneInit', 'flushNarrowPacket']
addresses = dict((name, int(value)) for name, value in re.findall(r'/\* (\w+) \*/ (\d+)ULL,', header))
base, size, slave = regions['PrintBridgeModule_0']
assert addresses == {name: base + 4 * i for i, name in enumerate(fields)}
assert (base, size, slave) == ((64, 32, 1) if tracerv else (0, 32, 0))
index = int(tracerv)
assert f'}}, 0U, args, {index}U).release()' in header
dma, count, depth, width = map(int, re.search(
    r'PRINTBRIDGEMODULE_0_to_cpu_stream"\), (\d+)ULL, (\d+)ULL, (\d+)U, (\d+)U\)', header).groups())
assert (dma, count, depth, width) == (index * 524288, regions['CPUManagedStreamEngine_0'][0] + 4 * index, 6144, 64)
assert regions['CPUManagedStreamEngine_0'] == ((96, 8, 2) if tracerv else (32, 4, 1))
if tracerv:
    assert 'TRACERVBRIDGEMODULE_0_to_cpu_stream"), 0ULL, 96ULL, 6144U, 64U)' in header
_, engine = module(reference, 'CPUManagedStreamEngine')
assert re.search(r'Queue_50\s+TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q\s*\(', engine)
assert re.search(r'reg\s*\[511:0\]\s+ram\s*\[0:6143\]', module(reference, 'Queue_50')[1])
eq = equations(engine)
assert eq['crFile_io_mcr_read_0_bits'].strip() == '{{19\'d0}, TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q_io_count}'
assert re.search(r'assert\s*\(\s*~crFile_io_mcr_write_0_valid\s*\)', engine)
assert re.fullmatch(r"auto_cpu_managed_axi4_in_ar_bits_addr\[63:19\]\s*==\s*45'h0", eq['TRACERVBRIDGEMODULE_0_to_cpu_stream_grant'].strip())
_, router = module(reference, 'NastiRouter')
top = equations(module(reference, 'FPGATop')[1])
if tracerv:
    _, trace = module(reference, 'TracerVBridgeModule')
    assert set(map(int, re.findall(r'crFile_io_mcr_write_(\d+)_valid', trace))) == set(range(15))
    for channel in ('aw', 'ar'):
        assert re.search(r"25'h100 <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h140", router)
        assert top['TracerVBridgeModule_0_io_ctrl_' + channel + '_valid'].strip() == 'ctrlInterconnect_io_slaves_2_' + channel + '_valid'
for channel in ('aw', 'ar'):
    assert re.search(r"25'h238 <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h23c", router)
    assert top['CPUManagedStreamEngine_0_io_ctrl_' + channel + '_valid'].strip() == 'ctrlInterconnect_io_slaves_10_' + channel + '_valid'
    assert re.search(channel + r'_route = \{_' + channel + r'_route_T_32,', router)
report = {'golden': str(golden), 'candidate': str(candidate / 'print-bridge-allocated.const.h'),
          'print_register_addresses': addresses, 'stream_index': index, 'dma_base': dma,
          'queue_depth': depth, 'beat_bytes': width, 'dma_window_bytes': 524288,
          'count_register_bytes': 4, 'count_register_read_only': True,
          'candidate_count_address': count, 'golden_count_address': 568,
          'absolute_address_match': count == 568, 'reference_has_print_host': False}
if tracerv:
    report.update(candidate_tracerv_address=0, golden_tracerv_address=256,
                  tracerv_register_words=15, tracerv_region_bytes=64)
(evidence / 'allocated-header-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS allocated six-word Print ABI and SFC CPU queue/count/DMA geometry; Print stream {index}, count address {count} versus 568 reflects different widget catalogs')
