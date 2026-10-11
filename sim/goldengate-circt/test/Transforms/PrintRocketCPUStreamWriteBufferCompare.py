#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded CPU AW/W buffers with the immutable SFC AXI4Buffer_2.

The C++ queue interpreter exercises the emitted MemOps against a deque oracle.
This comparison checks SFC geometry, reset/handshake equations, attachment and
recursive targets; W's eliminated dequeue is the constant-ready-zero profile.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintControlMasterCompare import ports as rtl_ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring

OLD = 'GGPrintCPUStreamWriteWrapper'
TOP = 'GGPrintCPUStreamWriteBufferWrapper'
QUEUES = ['GGCPUStreamAWQueue2', 'GGCPUStreamWQueue2']
CONSUMED = ['cpu_stream_' + x for x in ('aw_ready', 'aw_valid', 'aw_bits_id', 'aw_bits_size',
                                      'w_ready', 'w_valid', 'w_bits_strb')]


def sfc_contract(path):
    text = clean(path.read_text()); normalize = lambda x: re.sub(r'\s+', '', x)
    reports = []
    for name, fields in [('Queue_51', dict(id=16, addr=64, len=8, size=3)),
                         ('Queue_52', dict(strb=64))]:
        header, body = module(text, name); nets = equations(body); ports = rtl_ports(header)
        read_addr = 'deq_ptr_value' if name == 'Queue_51' else "1'h0"
        expected = dict(ptr_match='enq_ptr_value == deq_ptr_value' if name == 'Queue_51' else '~enq_ptr_value',
                        empty='ptr_match & ~maybe_full', full='ptr_match & maybe_full',
                        do_enq='io_enq_ready & io_enq_valid', io_enq_ready='~full', io_deq_valid='~empty')
        for f, width in fields.items():
            for side, direction in [('enq', 'input'), ('deq', 'output')]:
                assert ports[f'io_{side}_bits_{f}'] == [direction, width]
            expected.update({f'ram_{f}_MPORT_data': f'io_enq_bits_{f}', f'ram_{f}_MPORT_addr': 'enq_ptr_value',
                             f'ram_{f}_MPORT_mask': "1'h1", f'ram_{f}_MPORT_en': 'io_enq_ready & io_enq_valid',
                             f'ram_{f}_io_deq_bits_MPORT_addr': read_addr,
                             f'ram_{f}_io_deq_bits_MPORT_en': "1'h1",
                             f'io_deq_bits_{f}': f'ram_{f}_io_deq_bits_MPORT_data'})
            assert re.search(r'reg\s+(?:\[\d+:0\]\s+)?ram_' + f + r'\s+\[0:1\];', body)
            assert normalize(f'if (ram_{f}_MPORT_en & ram_{f}_MPORT_mask) begin ram_{f}[ram_{f}_MPORT_addr] <= ram_{f}_MPORT_data; end') in normalize(body)
        if name == 'Queue_51': expected['do_deq'] = 'io_deq_ready & io_deq_valid'
        else: assert 'io_deq_ready' not in ports
        for n, value in expected.items(): assert normalize(nets[n]) == normalize(value), (name, n)
        for pointer, fire in [('enq_ptr_value', 'do_enq')] + ([('deq_ptr_value', 'do_deq')] if name == 'Queue_51' else []):
            assert normalize(f"if (reset) begin {pointer} <= 1'h0; end else if ({fire}) begin {pointer} <= {pointer} + 1'h1; end") in normalize(body)
        condition = 'do_enq != do_deq' if name == 'Queue_51' else 'do_enq'
        assert normalize(f"if (reset) begin maybe_full <= 1'h0; end else if ({condition}) begin maybe_full <= do_enq; end") in normalize(body)
        reports.append(dict(module=name, payload=fields, depth=2, flow=False, pipe=False,
                            reset_flushes_control_only=True, accepted_reset_enqueue_writes_RAM=True))
    buffer = equations(module(text, 'AXI4Buffer_2')[1])
    assert buffer['nodeOut_aw_deq_q_io_deq_ready'] == "1'h0"
    for channel, fields in [('aw', ['id', 'addr', 'len', 'size']), ('w', ['strb'])]:
        q = f'nodeOut_{channel}_deq_q'
        assert buffer[f'auto_in_{channel}_ready'] == q + '_io_enq_ready'
        assert buffer[q + '_io_enq_valid'] == f'auto_in_{channel}_valid'
        assert buffer[f'auto_out_{channel}_valid'] == q + '_io_deq_valid'
        for f in fields: assert buffer[q + '_io_enq_bits_' + f] == f'auto_in_{channel}_bits_{f}'
        assert buffer[q + '_clock'] == 'clock' and buffer[q + '_reset'] == 'reset'
    return dict(queues=reports, empty_sink_AW_W_ready=0, independent_channels=2)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}; assert len(result) == len(found)
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 289 and len(new) == 292 and set(new) - set(old) == set(QUEUES + [TOP])
    for name, body in old.items(): assert new[name] == body, 'changed prior body ' + name
    previous = native_ports(old[OLD]); assert len(previous) == 120
    assert native_ports(new[TOP]) == previous + [('in', 'cpu_stream_aw_bits_addr', '!firrtl.uint<64>'),
                                               ('in', 'cpu_stream_aw_bits_len', '!firrtl.uint<8>')]
    copied = [(d, n, t) for d, n, t in previous if n not in CONSUMED]
    expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    for q, channel, fields in [('awQueue', 'aw', ['id', 'size']), ('wQueue', 'w', ['strb'])]:
        expected.update({q + '.clock': 'top.hostClock', q + '.reset': 'top.hostReset',
                         'top.cpu_stream_' + channel + '_ready': q + '.enq.ready',
                         q + '.enq.valid': 'top.cpu_stream_' + channel + '_valid',
                         q + '.deq.ready': 'sim.cpu_stream_' + channel + '_ready',
                         'sim.cpu_stream_' + channel + '_valid': q + '.deq.valid'})
        for f in fields:
            expected[q + '.enq.bits.' + f] = 'top.cpu_stream_' + channel + '_bits_' + f
            expected['sim.cpu_stream_' + channel + '_bits_' + f] = q + '.deq.bits.' + f
    for f in ('addr', 'len'): expected['awQueue.enq.bits.' + f] = 'top.cpu_stream_aw_bits_' + f
    assert wiring(new[TOP]) == expected and len(expected) == 133
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('awQueue', QUEUES[0]), ('wQueue', QUEUES[1])]
    for name in QUEUES: assert new[name] == native_module(baseline, name), 'shared queue differs ' + name
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.CPUStreamWriteBufferTargets", nested = \{targets = \[([^]]+)\]\}\}', line); assert probe
    targets = ['cpu_stream_aw_valid', 'cpu_stream_w_bits_strb', 'hostClock', 'hostReset', 'mem_0.aw.bits.id', 'cpu_stream_b_bits_id', 'cpu_stream_ar_bits_addr']
    assert re.findall(r'"([^"]+)"', probe[1]) == [f'~{TOP}|{OLD if n in CONSUMED else TOP}>' + n for n in targets]
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied: original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=289, public_ports=122,
                copied_ports=113, consumed_AW_W_ports=7, wrapper_connections=133,
                identical_native_queues=2, recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['Queue_51', 'Queue_52', 'AXI4Buffer_2'],
                  SFC_contract=sfc_contract(a.sfc), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print CPU AW/W queues; AR/R/B buffering and production Print runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC CPU AW/W queue/reset/payload contracts; 289 prior bodies, 122 ports, 133 routes and recursive targets per order')


if __name__ == '__main__': main()
