#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print CPU writes/queues with the immutable SFC boundary.

Native operation interpreters cover every live AW ID and queue ordering/reset.
The recorded SFC disables Print; this checks shared transport semantics only.
"""
import argparse
import json
import re
from pathlib import Path
from PrintRocketEmptyCPUStreamWriteCompare import FIELDS, sfc_contract as empty_contract
from PrintRocketCPUStreamWriteBufferCompare import CONSUMED, sfc_contract as queue_contract
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring

OLD = 'GGControlMasterWrapper'
WRITE = 'GGSelectedPrintCPUStreamWriteWrapper'
TOP = 'GGSelectedPrintCPUStreamWriteBufferWrapper'
HELPER = 'GGEmptyCPUStreamWrite'
QUEUES = ['GGCPUStreamAWQueue2', 'GGCPUStreamWQueue2']


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()

    def modules(text):
        entries = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in entries}
        assert len(result) == len(entries)
        return result

    old, new = modules(before), modules(after)
    assert set(new) - set(old) == {WRITE, TOP, HELPER, *QUEUES}
    for name, body in old.items():
        assert new[name] == body, 'changed prior module ' + name
    previous = native_ports(old[OLD])
    assert not any(n.startswith(('cpu_stream_aw_', 'cpu_stream_w_', 'cpu_stream_b_')) for _, n, _ in previous)
    write_ports = previous + [(d, 'cpu_stream_' + n, f'!firrtl.uint<{w}>') for d, n, w in FIELDS]
    assert native_ports(new[WRITE]) == write_ports
    assert native_ports(new[TOP]) == write_ports + [
        ('in', 'cpu_stream_aw_bits_addr', '!firrtl.uint<64>'),
        ('in', 'cpu_stream_aw_bits_len', '!firrtl.uint<8>')]
    routes = {('sim.' + n if d == 'in' else 'top.' + n):
              ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in previous}
    routes.update({'cpuStreamWrite.clock': 'top.hostClock', 'cpuStreamWrite.reset': 'top.hostReset'})
    for d, n, _ in FIELDS:
        outer, inner = 'top.cpu_stream_' + n, 'cpuStreamWrite.' + n
        routes[inner if d == 'in' else outer] = outer if d == 'in' else inner
    assert wiring(new[WRITE]) == routes
    copied = [(d, n, t) for d, n, t in write_ports if n not in CONSUMED]
    buffered = {('sim.' + n if d == 'in' else 'top.' + n):
                ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    for q, ch, fields in [('awQueue', 'aw', ['id', 'size']), ('wQueue', 'w', ['strb'])]:
        buffered.update({q + '.clock': 'top.hostClock', q + '.reset': 'top.hostReset',
                         'top.cpu_stream_' + ch + '_ready': q + '.enq.ready',
                         q + '.enq.valid': 'top.cpu_stream_' + ch + '_valid',
                         q + '.deq.ready': 'sim.cpu_stream_' + ch + '_ready',
                         'sim.cpu_stream_' + ch + '_valid': q + '.deq.valid'})
        for f in fields:
            buffered[q + '.enq.bits.' + f] = 'top.cpu_stream_' + ch + '_bits_' + f
            buffered['sim.cpu_stream_' + ch + '_bits_' + f] = q + '.deq.bits.' + f
    for f in ('addr', 'len'):
        buffered['awQueue.enq.bits.' + f] = 'top.cpu_stream_aw_bits_' + f
    assert wiring(new[TOP]) == buffered
    for name, instances in [(WRITE, [('sim', OLD), ('cpuStreamWrite', HELPER)]),
                            (TOP, [('sim', WRITE), ('awQueue', QUEUES[0]), ('wQueue', QUEUES[1])])]:
        assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[name]) == instances
    for name in [HELPER, *QUEUES]:
        assert new[name] == native_module(baseline, name), 'native semantic helper differs ' + name
    assert wiring(new[HELPER]) == {'top.aw_ready': '0', 'top.w_ready': '0', 'top.b_valid': '0',
                                  'top.b_bits_id': 'top.aw_bits_id', 'top.b_bits_resp': '0'}

    # Check the complete circuit archive, including nested lists/dictionaries,
    # and retained analysis/stream/control metadata, through both wrappers.
    original = before.splitlines()[1]
    for source, dest, ports in [(OLD, WRITE, previous), (WRITE, TOP, copied)]:
        original = original.replace('firrtl.circuit "' + source + '"', 'firrtl.circuit "' + dest + '"')
        original = original.replace('~' + source + '|', '~' + dest + '|').replace('"~' + source + '"', '"~' + dest + '"')
        for _, n, _ in ports:
            original = re.sub(r'\|' + source + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + dest + '>' + n, original)
    assert after.splitlines()[1] == original, 'archive or analysis metadata changed'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(old),
                public_ports=len(write_ports)+2, added_write_ports=13,
                empty_writer_routes=len(routes), buffered_routes=len(buffered),
                identical_native_helpers=3, copied_consumed_targets_preserved=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'):
        p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'):
        p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args()
    assert len(a.before) == len(a.after)
    baseline = a.baseline.read_text()
    report = dict(golden=str(a.sfc.resolve()), empty_writer=empty_contract(a.sfc),
                  queues=queue_contract(a.sfc),
                  candidates=[compare(x, y, baseline) for x, y in zip(a.before, a.after)],
                  limitation='Shared zero-sink/AW/W semantics; AR/R/B buffers, host memory and enabled-Print runtime remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC empty CPU writes and depth-two AW/W queues; preserved modules, targets and allocations')


if __name__ == '__main__':
    main()
