#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare Rocket/Print DMA and count allocation with immutable SFC transport.

The expanded native boundary includes two Print hosts; the SFC reference has
Print disabled. Compare baseline transport semantics and the preserved TracerV
allocation, then verify Print appends and the resulting count-bank growth.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module, terms


evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
reference = clean(golden.read_text())
candidate = clean((evidence / 'candidate' / 'FireSim-generated.sv').read_text())
native = equations(module(candidate, 'GGCPUStreamRead')[1])
oracle = equations(module(reference, 'CPUManagedStreamEngine')[1])
prefix = 'TRACERVBRIDGEMODULE_0_to_cpu_stream_'
grant = next(name for name, expr in native.items()
             if re.fullmatch(r"ar_bits_addr\[63:19\]\s*==\s*45'h0", expr.strip()))
last = next(name for name, expr in native.items()
            if re.fullmatch(r"readBeatCounter\s*==\s*\{1'h0,\s*ar_bits_len\}", expr.strip()))
assert re.fullmatch(r"auto_cpu_managed_axi4_in_ar_bits_addr\[63:19\]\s*==\s*45'h0",
                    oracle[prefix + 'grant'].strip())
names = {'ar_valid': 'ar', 'r_ready': 'ready', 'stream_valid': 'valid', grant: 'grant', last: 'last'}
oracle_names = {'auto_cpu_managed_axi4_in_ar_valid': 'ar', 'auto_cpu_managed_axi4_in_r_ready': 'ready',
                prefix + 'ser_des_io_narrow_out_valid': 'valid', prefix + 'grant': 'grant', prefix + 'lastReadBeat': 'last'}
handshakes = {}
for out, expected in {'r_valid': 'auto_cpu_managed_axi4_in_r_valid', 'ar_ready': 'auto_cpu_managed_axi4_in_ar_ready',
                      'stream_ready': prefix + 'ser_des_io_narrow_out_ready'}.items():
    actual = terms(native[out], native, names)
    assert actual == terms(oracle[expected], oracle, oracle_names)
    handshakes[out] = sorted(actual)
assert native['r_bits_data'].strip() == 'stream_bits'
assert native['r_bits_id'].strip() == 'ar_bits_id'
assert equations(module(candidate, 'GGCPUStreamCountBank')[1])['mcr_read_0_bits'].strip() == "{19'h0, count}"
assert oracle['crFile_io_mcr_read_0_bits'].strip() == "{{19'd0}, " + prefix + 'outgoingQueueIO_q_io_count}'
assert re.search(r'reg\s*\[511:0\]\s+ram\s*\[0:6143\]', module(reference, 'Queue_50')[1])
assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)


def allocations(text):
    # Historical bank dependencies may retain their own namespaced reader.
    # Compare the active shared transport, not those isolated definitions.
    text = re.search(r'firrtl.module @GGCPUStreamRead\(.*?(?=\n\s+firrtl.module|\Z)', text, re.S).group()
    rows = re.findall(r'\{bufferBaseAddress = (\d+) : i64, depth = (\d+) : i64, index = (\d+) : i64, name = "([^"]+)", port = "([^"]+)", widthBytes = (\d+) : i64\}', text)
    return [(name, int(index), int(base), int(depth), int(width)) for base, depth, index, name, port, width in rows]


baseline_ir = (evidence / 'candidate' / 'post-fame-cpu-stream-count.mlir').read_text()
baseline = [('TRACERVBRIDGEMODULE_0_to_cpu_stream', 0, 0, 6144, 64)]
assert allocations(baseline_ir) == baseline
expanded = baseline + [(f'PRINTBRIDGEMODULE_{i}_to_cpu_stream', i + 1, (i + 1) * 524288, 6144, 64) for i in range(2)]
for suffix in ('rocket-streams', 'rocket-streams-reverse'):
    text = (evidence / f'binding.mlir.{suffix}.mlir').read_text()
    assert allocations(text) == expanded
    bank = re.search(r'firrtl.module @GGCPUStreamCountBank\(.*?(?=\n\s+firrtl.module|\Z)', text, re.S).group()
    for i, (name, _, _, _, _) in enumerate(expanded):
        assert f'name = "{name}_count", offset = {4 * i} : i32, readable = true, writeable = false' in bank
    assert 'read: vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 3>' in bank
report = {'golden': str(golden), 'baseline_handshakes_matching_SFC': handshakes,
          'baseline_allocation': baseline, 'expanded_allocations': expanded,
          'count_offsets': [0, 4, 8], 'count_region_bytes': 16,
          'reference_has_print_host': False, 'boundary': 'native Rocket queue plus bound Print CPU DMA/count, before full platform MMIO assembly'}
(evidence / 'rocket-streams-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS SFC TracerV CPU handshake/storage/count geometry; native Print appends at indices 1/2 and count offsets 4/8 in both host orders')
