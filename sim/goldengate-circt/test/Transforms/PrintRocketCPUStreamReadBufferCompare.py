#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print CPU AR attachment with SFC's AXI4Buffer_2.

Actual MemOps are exercised by CPUStreamReadBufferTest against a deque oracle.
This checks the immutable queue/engine contracts, all wrapper routes, retained
module bodies and recursive copied/consumed annotation targets.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring
from PrintRocketCPUStreamWriteBufferCompare import sfc_contract as queue_contract

OLD = 'GGPrintCPUStreamWriteBufferWrapper'
TOP = 'GGPrintCPUStreamReadBufferWrapper'
QUEUE = 'GGCPUStreamAWQueue2'
FIELDS = ['id', 'addr', 'len', 'size']
CONSUMED = ['cpu_stream_ar_' + n for n in ['ready', 'valid'] + ['bits_' + f for f in FIELDS]]


def sfc_contract(path):
    queue = queue_contract(path)['queues'][0]
    text = clean(path.read_text()); buffer = equations(module(text, 'AXI4Buffer_2')[1])
    q = 'nodeOut_ar_deq_q'
    expected = {'auto_in_ar_ready': q + '_io_enq_ready', q + '_io_enq_valid': 'auto_in_ar_valid',
                'auto_out_ar_valid': q + '_io_deq_valid', q + '_io_deq_ready': 'auto_out_ar_ready',
                q + '_clock': 'clock', q + '_reset': 'reset'}
    for f in FIELDS:
        expected[q + '_io_enq_bits_' + f] = 'auto_in_ar_bits_' + f
        expected['auto_out_ar_bits_' + f] = q + '_io_deq_bits_' + f
    for n, value in expected.items(): assert buffer[n] == value, n
    engine = equations(module(text, 'CPUManagedStreamEngine')[1])
    normalize = lambda s: re.sub(r'\s+', '', s)
    grant = 'TRACERVBRIDGEMODULE_0_to_cpu_stream_grant'
    last = 'TRACERVBRIDGEMODULE_0_to_cpu_stream_lastReadBeat'
    ready = '_TRACERVBRIDGEMODULE_0_to_cpu_stream_cpuManagedAXI4NodeOptIn_ar_ready_T_1'
    assert normalize(engine['auto_cpu_managed_axi4_in_ar_ready']) == normalize(grant + '&' + ready)
    assert normalize(engine[ready]) == normalize('auto_cpu_managed_axi4_in_r_ready & TRACERVBRIDGEMODULE_0_to_cpu_stream_ser_des_io_narrow_out_valid &' + last)
    assert engine['auto_cpu_managed_axi4_in_r_bits_id'] == 'auto_cpu_managed_axi4_in_ar_bits_id'
    return dict(queue=queue, wrapper_AR_routes=14, AR_dequeue='grant & R_ready & stream_valid & lastReadBeat',
                R_id_from_retained_AR=True)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}; assert len(result) == len(found)
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 292 and len(new) == 293 and set(new) - set(old) == {TOP}
    for name, body in old.items(): assert new[name] == body, 'changed prior body ' + name
    previous = native_ports(old[OLD]); assert len(previous) == 122
    assert native_ports(new[TOP]) == previous
    copied = [(d, n, t) for d, n, t in previous if n not in CONSUMED]
    expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    expected.update({'arQueue.clock': 'top.hostClock', 'arQueue.reset': 'top.hostReset',
                     'top.cpu_stream_ar_ready': 'arQueue.enq.ready', 'arQueue.enq.valid': 'top.cpu_stream_ar_valid',
                     'arQueue.deq.ready': 'sim.cpu_stream_ar_ready', 'sim.cpu_stream_ar_valid': 'arQueue.deq.valid'})
    for f in FIELDS:
        expected['arQueue.enq.bits.' + f] = 'top.cpu_stream_ar_bits_' + f
        expected['sim.cpu_stream_ar_bits_' + f] = 'arQueue.deq.bits.' + f
    assert wiring(new[TOP]) == expected and len(expected) == 130
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('arQueue', QUEUE)]
    assert new[QUEUE] == native_module(baseline, QUEUE), 'shared native AR queue differs'
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.CPUStreamReadBufferTargets", nested = \{targets = \[([^]]+)\]\}\}', line); assert probe
    targets = ['cpu_stream_ar_valid', 'cpu_stream_ar_bits_addr', 'hostClock', 'hostReset', 'mem_0.aw.bits.id', 'cpu_stream_aw_valid', 'cpu_stream_r_ready']
    assert re.findall(r'"([^"]+)"', probe[1]) == [f'~{TOP}|{OLD if n in CONSUMED else TOP}>' + n for n in targets]
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied: original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=292, public_ports=122,
                copied_ports=116, consumed_AR_ports=6, wrapper_connections=130,
                identical_shared_native_queue=True, recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['Queue_51', 'AXI4Buffer_2', 'CPUManagedStreamEngine'],
                  SFC_contract=sfc_contract(a.sfc), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print CPU AR queue; R/B buffering and production Print runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AR queue/reset/payload/last-beat contracts; 292 prior bodies, 122 ports, 130 routes and recursive targets per order')


if __name__ == '__main__': main()
