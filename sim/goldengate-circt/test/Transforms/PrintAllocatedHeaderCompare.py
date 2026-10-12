#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare allocated Print/count collateral with native IR and SFC transport.

The immutable platform disables Print. Absolute addresses therefore differ;
compare its shared count-word and DMA geometry, recording that difference.
"""
import json
import re
import argparse
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('evidence', type=Path)
parser.add_argument('golden', type=Path)
parser.add_argument('--tracerv', action='store_true')
parser.add_argument('--tracerv-annotations', type=Path)
options = parser.parse_args()
tracerv = options.tracerv
evidence, golden = options.evidence.resolve(), options.golden.resolve()
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
    # Widget.genConstructor uses the bank's byte offset order. Recover it
    # from the immutable executable register assignments, including truncation.
    trace_equations = equations(trace)
    def expand_mux(text):
        return re.sub(r'\b_GEN_\d+\b', lambda m: '(' + expand_mux(trace_equations[m[0]]) + ')', text)
    trace_words = []
    for name, rhs in re.findall(r'(\w+)\s*<=\s*([^;]+);', trace):
        words = set(re.findall(r'crFile_io_mcr_write_(\d+)_bits', expand_mux(rhs)))
        if words:
            assert len(words) == 1
            trace_words.append((name, words.pop()))
    assert len(trace_words) == 15
    trace_addresses = [(name, int(value)) for name, value in
                       re.findall(r'^\s*\.(\w+) = (\d+),$', header, re.M)]
    expected_trace = sorted(((name, 4 * int(word)) for name, word in trace_words), key=lambda r: r[1])
    assert trace_addresses == expected_trace
    assert header.count('registry.add_widget(new tracerv_t(') == 1
    constructor = re.search(r'TRACERVBRIDGEMODULE_struct\{.*?\},\s*(\d+),\s*args,\s*(\d+),\s*(\d+),\s*(\d+),\s*ClockInfo\{"([^"]+)", (\d+)U, (\d+)U\}', header, re.S)
    assert constructor
    widget, stream, trace_depth, retire, clock, mult, div = constructor.groups()
    assert tuple(map(int, (widget, stream, trace_depth, retire, mult, div))) == (0, 0, 6144, 1, 1, 1)
    assert clock == 'uart_clock,clock_1000.0MHz,harnessbinder_clock,reference'
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
                  tracerv_register_words=15, tracerv_region_bytes=64,
                  tracerv_constructor=dict(register_addresses=trace_addresses, widget_index=int(widget),
                      stream_index=int(stream), depth=int(trace_depth), retire_width=int(retire),
                      clock=dict(name=clock, multiplier=int(mult), divisor=int(div))))
if options.tracerv_annotations:
    assert tracerv
    annotations = options.tracerv_annotations.resolve()
    bridges = [r for r in json.loads(annotations.read_text()) if
               r.get('widgetClass') == 'firechip.goldengateimplementations.TracerVBridgeModule']
    assert len(bridges) == 1
    key = bridges[0]['widgetConstructorKey']
    assert key['retireWidth'] == int(retire)
    assert (key['iaddrWidth'], key['insnWidth'], key['causeWidth'], key['tvalWidth']) == (40, 32, 64, 40)
    frequency = annotations.with_name(annotations.name.removesuffix('.anno.json') + '.firesimRationalClockBridge.freq-summary')
    frequencies = re.findall(r'Output clock ([^,]+), requested: ([\d.]+) MHz, actual: ([\d.]+) MHz \(division of (\d+)\)', frequency.read_text())
    assert len(frequencies) == 3
    assert ','.join(row[0] for row in frequencies) + ',reference' == clock
    assert all(requested == actual and int(division) == int(div) == int(mult) for _, requested, actual, division in frequencies)
    report.update(golden_annotations=str(annotations), golden_frequency_summary=str(frequency),
                  tracerv_constructor_payload_match=True, tracerv_clock_ratio_match=True)
(evidence / 'allocated-header-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
print(f'PASS allocated six-word Print ABI and SFC CPU queue/count/DMA geometry; Print stream {index}, count address {count} versus 568 reflects different widget catalogs')
if tracerv:
    print('PASS one allocated TracerV constructor, fifteen SFC register offsets, widget/stream 0, depth 6144 and retirement width 1')
if options.tracerv_annotations:
    print('PASS immutable TraceBundleWidths constructor payload and rational-clock frequency summary')
