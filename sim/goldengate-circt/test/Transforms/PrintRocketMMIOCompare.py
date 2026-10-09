#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the complete native MMIO allocation after Print count-bank growth.

The immutable SFC platform has Print disabled. Compare its baseline regions
and bindings, then the expanded native decoder against Widget.scala's size
sorting and contiguous allocation, retaining actual bank word counts.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module


def regions(path):
    return [(name, int(start), int(size), int(slave)) for name, size, slave, start in
            re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', path.read_text())]


evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
reference = clean(golden.read_text())
baseline = regions(evidence / 'candidate' / 'post-fame-control-address-decode.mlir')
assert len(baseline) == 11
router = module(reference, 'NastiRouter')[1]
top = equations(module(reference, 'FPGATop')[1])
for channel in ('aw', 'ar'):
    ranges = [(0, 128)] + [(int(lo, 16), int(hi, 16)) for lo, hi in re.findall(
        r"25'h([0-9a-f]+) <= io_master_" + channel + r"_bits_addr & io_master_" + channel + r"_bits_addr < 25'h([0-9a-f]+)", router)]
    assert len(ranges) == 11
    assert re.search(r'io_master_' + channel + r"_bits_addr < 25'h80", router)
    for name, start, size, slave in baseline:
        assert ranges[slave] == (start, start + size)
        assert top[name + '_io_ctrl_' + channel + '_valid'].strip() == f'ctrlInterconnect_io_slaves_{slave}_{channel}_valid'

expanded = regions(evidence / 'binding.mlir.rocket-mmio.mlir')
assert expanded == regions(evidence / 'binding.mlir.rocket-mmio-reverse.mlir')
assert len(expanded) == 13
# The baseline's small-bank order changes: CPU grows from one word to three
# and sorts alongside SimulationMaster before the two-word ResetPulse bank.
expected = baseline[:8] + [('PrintBridgeModule_0', 544, 32, 8), ('PrintBridgeModule_1', 576, 32, 9),
                         ('SimulationMaster_0', 608, 16, 10), ('CPUManagedStreamEngine_0', 624, 16, 11),
                         ('ResetPulseBridgeModule_0', 640, 8, 12)]
assert expanded == expected
assert sum(row[2] for row in expanded) == 648
for path in ('rocket-streams', 'rocket-streams-reverse'):
    text = (evidence / f'binding.mlir.{path}.mlir').read_text()
    bank = re.search(r'firrtl.module @GGCPUStreamCountBank\(.*?(?=\n\s+firrtl.module|\Z)', text, re.S).group()
    for i, name in enumerate(('TRACERVBRIDGEMODULE_0', 'PRINTBRIDGEMODULE_0', 'PRINTBRIDGEMODULE_1')):
        assert f'name = "{name}_to_cpu_stream_count", offset = {4 * i} : i32, readable = true, writeable = false' in bank
    assert 'read: vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 3>' in bank

assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
report = {'golden': str(golden), 'baseline_banks_matching_SFC': 11,
          'baseline_regions': baseline, 'expanded_regions': expanded,
          'count_words': 3, 'count_offsets': [0, 4, 8], 'count_region_bytes': 16,
          'print_bases': [544, 576], 'cpu_count_base': 624, 'reset_base': 640,
          'reference_has_print_host': False,
          'boundary': 'actual Rocket register banks plus bound Print DMA/count, native full-platform decoder; request/response binding pending'}
(evidence / 'rocket-mmio-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS 11 baseline SFC AW/AR regions/slaves; expanded 13-bank decoder has Print 544/576, CPU 624 (16 bytes), ResetPulse 640 in both orders')
