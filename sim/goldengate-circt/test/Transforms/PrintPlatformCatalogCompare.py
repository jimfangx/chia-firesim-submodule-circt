#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare native Rocket/Print allocation with immutable SFC MMIO routing.

The reference has no Print bridge. Baseline banks must match exactly; the
expanded catalog must preserve large banks and move the small tail by 64 bytes.
This compares pre-binding decoder IR, not a runnable Print-enabled platform.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module


def regions(path):
    text = path.read_text()
    return [(name, int(start), int(size), int(slave)) for name, size, slave, start in
            re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', text)]


evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
reference = clean(golden.read_text())
assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
baseline = regions(evidence / 'binding.mlir.platform-baseline.mlir')
assert len(baseline) == 11
assert baseline == regions(evidence / 'candidate' / 'post-fame-control-address-decode.mlir')
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
expanded = regions(evidence / 'binding.mlir.platform.mlir')
assert expanded == regions(evidence / 'binding.mlir.platform-reverse.mlir')
assert len(expanded) == 13
lookup = {name: (start, size, slave) for name, start, size, slave in expanded}
assert lookup['PrintBridgeModule_0'] == (544, 32, 8)
assert lookup['PrintBridgeModule_1'] == (576, 32, 9)
for name, start, size, slave in baseline:
    shift, slave_shift = (64, 2) if start >= 544 else (0, 0)
    assert lookup[name] == (start + shift, size, slave + slave_shift)
report = {'golden': str(golden), 'baseline_banks_matching_SFC': len(baseline),
          'baseline_regions': baseline, 'expanded_regions': expanded,
          'print_bases': [544, 576], 'count_base': 632, 'count_bank_streams': ['TracerV'],
          'print_stream_count_growth_pending': True, 'reference_has_print_host': False,
          'compiler_candidate': str(evidence / 'candidate' / 'post-fame-control-address-decode.mlir'),
          'boundary': 'materialized register catalog and native decoder, before platform Print transport binding'}
(evidence / 'platform-catalog-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS all 11 SFC AW/AR regions and slave bindings; two native Print banks at 544/576 shift small-bank tail by 64 bytes (pre-binding only)')
