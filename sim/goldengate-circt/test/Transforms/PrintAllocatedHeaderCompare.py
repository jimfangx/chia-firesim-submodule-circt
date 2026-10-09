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


evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
candidate = evidence / 'candidate'
header = (candidate / 'print-bridge-allocated.const.h').read_text()
ir = (candidate / 'post-print-control-master.mlir').read_text()
reference = clean(golden.read_text())
assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
regions = {name: (int(start), int(size), int(slave)) for name, size, slave, start in
           re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', ir)}
assert set(regions) == {'PrintBridgeModule_0', 'CPUManagedStreamEngine_0'}
fields = ['startCycleL', 'startCycleH', 'endCycleL', 'endCycleH', 'doneInit', 'flushNarrowPacket']
addresses = dict((name, int(value)) for name, value in re.findall(r'/\* (\w+) \*/ (\d+)ULL,', header))
base, size, slave = regions['PrintBridgeModule_0']
assert addresses == {name: base + 4 * i for i, name in enumerate(fields)}
assert size == 32 and slave == 0
assert '}, 0U, args, 0U).release()' in header
dma, count, depth, width = map(int, re.search(
    r'PRINTBRIDGEMODULE_0_to_cpu_stream"\), (\d+)ULL, (\d+)ULL, (\d+)U, (\d+)U\)', header).groups())
assert (dma, count, depth, width) == (0, regions['CPUManagedStreamEngine_0'][0], 6144, 64)
assert regions['CPUManagedStreamEngine_0'][1:] == (4, 1)
_, engine = module(reference, 'CPUManagedStreamEngine')
assert re.search(r'Queue_50\s+TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q\s*\(', engine)
assert re.search(r'reg\s*\[511:0\]\s+ram\s*\[0:6143\]', module(reference, 'Queue_50')[1])
eq = equations(engine)
assert eq['crFile_io_mcr_read_0_bits'].strip() == '{{19\'d0}, TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q_io_count}'
assert re.search(r'assert\s*\(\s*~crFile_io_mcr_write_0_valid\s*\)', engine)
assert re.fullmatch(r"auto_cpu_managed_axi4_in_ar_bits_addr\[63:19\]\s*==\s*45'h0", eq['TRACERVBRIDGEMODULE_0_to_cpu_stream_grant'].strip())
_, router = module(reference, 'NastiRouter')
top = equations(module(reference, 'FPGATop')[1])
for channel in ('aw', 'ar'):
    assert re.search(r"25'h238 <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h23c", router)
    assert top['CPUManagedStreamEngine_0_io_ctrl_' + channel + '_valid'].strip() == 'ctrlInterconnect_io_slaves_10_' + channel + '_valid'
    assert re.search(channel + r'_route = \{_' + channel + r'_route_T_32,', router)
report = {'golden': str(golden), 'candidate': str(candidate / 'print-bridge-allocated.const.h'),
          'print_register_addresses': addresses, 'stream_index': 0, 'dma_base': dma,
          'queue_depth': depth, 'beat_bytes': width, 'dma_window_bytes': 524288,
          'count_register_bytes': 4, 'count_register_read_only': True,
          'candidate_count_address': count, 'golden_count_address': 568,
          'absolute_address_match': count == 568, 'reference_has_print_host': False}
(evidence / 'allocated-header-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS allocated six-word Print ABI and SFC CPU queue/count/DMA geometry; count base 32 versus 568 reflects different widget catalogs')
