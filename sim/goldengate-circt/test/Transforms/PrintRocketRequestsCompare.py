#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare Rocket request identities with SFC and the expanded native router.

Print is disabled in the immutable SFC fixture. Compare all seven early
baseline AW/W/AR slave identities, then check nine actual native bindings
against the expanded allocation in both Print constructor orders. Native C++
checks the 180 scalar AW/W connections, including shared Nasti metadata.
"""
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module


def bindings(path, attribute):
    text = path.read_text()
    rows = re.findall(r'goldengate\.' + attribute + r' = \[(.*?)\]', text)
    assert len(rows) == 1, f'expected one {attribute} in {path}'
    return [(name, port, int(slave)) for name, port, slave in re.findall(
        r'\{name = "([^"]+)", port = "([^"]+)", slave = (\d+) : i32\}', rows[0])]


evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
reference = clean(golden.read_text())
top = equations(module(reference, 'FPGATop')[1])
baseline = bindings(evidence / 'candidate/post-fame-control-widget-writes.mlir', 'controlWriteBindings')
assert len(baseline) == 7
assert baseline == bindings(evidence / 'candidate/post-fame-control-read-dispatch.mlir', 'controlReadBindings')
for name, port, slave in baseline:
    for channel in ('aw', 'w', 'ar'):
        assert top[f'{name}_io_ctrl_{channel}_valid'].strip() == f'ctrlInterconnect_io_slaves_{slave}_{channel}_valid'

expanded = []
for suffix in ('rocket-requests', 'rocket-requests-reverse'):
    path = evidence / f'binding.mlir.{suffix}.mlir'
    writes = bindings(path, 'controlWriteBindings')
    assert writes == bindings(path, 'controlReadBindings') and len(writes) == 9
    text = path.read_text()
    regions = {name: int(slave) for name, slave in re.findall(
        r'\{name = "([^"]+)", size = \d+ : i64, slave = (\d+) : i32, start = \d+ : i64\}', text)}
    assert len(regions) == 13
    assert writes[:5] == baseline[:5]
    assert writes[5:7] == [('ResetPulseBridgeModule_0', 'resetBridge_ctrl', 12),
                         ('CPUManagedStreamEngine_0', 'cpuStream_ctrl', 11)]
    assert [(name, slave) for name, port, slave in writes[7:]] == [('PrintBridgeModule_0', 8), ('PrintBridgeModule_1', 9)]
    assert all(regions[name] == slave for name, port, slave in writes)
    assert set(regions) - {name for name, port, slave in writes} == {
        'SimulationMaster_0', 'FASEDMemoryTimingModel_0', 'TSIBridgeModule_0', 'BlockDevBridgeModule_0'}
    expanded.append(writes)
assert expanded[0] == expanded[1]
assert not re.search(r'\bmodule\s+PrintBridgeModule(?:_\d+)?\s*\(', reference)
report = {'golden': str(golden), 'baseline_AW_W_AR_banks_matching_SFC': baseline,
          'expanded_native_bindings': expanded[0], 'native_AW_W_scalar_connections_per_order': 180,
          'native_regions': 13, 'constructor_orders': 2, 'reference_has_print_host': False,
          'boundary': 'nine early request banks; LoadMem register side and four later request/response banks exposed'}
(evidence / 'rocket-requests-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS seven SFC AW/W/AR bank identities; nine native Rocket/Print request bindings across 13 regions in both constructor orders')
