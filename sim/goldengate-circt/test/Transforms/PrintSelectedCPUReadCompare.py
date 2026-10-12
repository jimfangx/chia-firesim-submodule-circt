#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print AR buffering to immutable SFC transport contracts.

The recorded fixture disables Print. This checks shared AR storage/handshakes;
CPUStreamReadBufferTest interprets the candidate queue against a deque oracle.
"""
import argparse
import json
import re
from pathlib import Path
from PrintRocketCPUStreamReadBufferCompare import FIELDS, CONSUMED, sfc_contract
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring

OLD = 'GGSelectedPrintCPUStreamWriteBufferWrapper'
TOP = 'GGSelectedPrintCPUStreamReadBufferWrapper'
QUEUE = 'GGCPUStreamAWQueue2'


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()

    def modules(text):
        entries = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in entries}
        assert len(result) == len(entries)
        return result

    old, new = modules(before), modules(after)
    assert set(new) - set(old) == {TOP}
    for name, body in old.items():
        assert new[name] == body, 'changed prior module ' + name
    previous = native_ports(old[OLD])
    assert native_ports(new[TOP]) == previous
    copied = [(d, n, t) for d, n, t in previous if n not in CONSUMED]
    routes = {('sim.' + n if d == 'in' else 'top.' + n):
              ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    routes.update({'arQueue.clock': 'top.hostClock', 'arQueue.reset': 'top.hostReset',
                   'top.cpu_stream_ar_ready': 'arQueue.enq.ready',
                   'arQueue.enq.valid': 'top.cpu_stream_ar_valid',
                   'arQueue.deq.ready': 'sim.cpu_stream_ar_ready',
                   'sim.cpu_stream_ar_valid': 'arQueue.deq.valid'})
    for f in FIELDS:
        routes['arQueue.enq.bits.' + f] = 'top.cpu_stream_ar_bits_' + f
        routes['sim.cpu_stream_ar_bits_' + f] = 'arQueue.deq.bits.' + f
    assert wiring(new[TOP]) == routes
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('arQueue', QUEUE)]
    assert new[QUEUE] == native_module(baseline, QUEUE), 'shared native AR queue differs'
    # Two occurrences of the helper have their own registers and memory:
    # the new AR instance does not consume the AW instance's occupancy.
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', old[OLD]) == [
        ('sim', 'GGSelectedPrintCPUStreamWriteWrapper'),
        ('awQueue', QUEUE), ('wQueue', 'GGCPUStreamWQueue2')]

    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied:
        original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert after.splitlines()[1] == original, 'archive or analysis metadata changed'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(old),
                public_ports=len(previous), copied_ports=len(copied), consumed_AR_ports=6,
                wrapper_connections=len(routes), identical_shared_native_queue=True,
                independent_AR_and_AW_instances=True, recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'):
        p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'):
        p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args()
    assert len(a.before) == len(a.after)
    baseline = a.baseline.read_text()
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['Queue_51', 'AXI4Buffer_2', 'CPUManagedStreamEngine'],
                  SFC_contract=sfc_contract(a.sfc),
                  candidates=[compare(x, y, baseline) for x, y in zip(a.before, a.after)],
                  limitation='Shared AR transport only; R/B buffering, host memory and enabled-Print runtime remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AR storage/reset/payload/last-beat contracts; retained modules, ports, routes and target identities')


if __name__ == '__main__':
    main()
