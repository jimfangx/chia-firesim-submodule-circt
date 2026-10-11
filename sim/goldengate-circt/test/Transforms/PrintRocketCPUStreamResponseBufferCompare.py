#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print CPU R attachment with SFC's AXI4Buffer_2.

Actual MemOps are exercised by CPUStreamResponseBufferTest against a deque oracle.
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
from PrintControlMasterCompare import ports as rtl_ports

OLD = 'GGPrintCPUStreamReadBufferWrapper'
TOP = 'GGPrintCPUStreamResponseBufferWrapper'
QUEUE = 'GGCPUStreamRQueue2'
FIELDS = ['id', 'data', 'resp', 'last']
CONSUMED = ['cpu_stream_r_' + n for n in ['ready', 'valid'] + ['bits_' + f for f in FIELDS]]


def sfc_contract(path):
    text = clean(path.read_text()); normalize = lambda x: re.sub(r'\s+', '', x)
    header, body = module(text, 'Queue_55'); nets = equations(body); ports = rtl_ports(header)
    fields = dict(id=16, data=512, resp=2, last=1)
    expected = dict(ptr_match='enq_ptr_value == deq_ptr_value', empty='ptr_match & ~maybe_full',
                    full='ptr_match & maybe_full', do_enq='io_enq_ready & io_enq_valid',
                    do_deq='io_deq_ready & io_deq_valid', io_enq_ready='~full', io_deq_valid='~empty')
    for f, width in fields.items():
        assert ports['io_deq_bits_' + f] == ['output', width]
        if f != 'resp': assert ports['io_enq_bits_' + f] == ['input', width]
        expected.update({f'ram_{f}_MPORT_data': "2'h0" if f == 'resp' else f'io_enq_bits_{f}',
                         f'ram_{f}_MPORT_addr': 'enq_ptr_value', f'ram_{f}_MPORT_mask': "1'h1",
                         f'ram_{f}_MPORT_en': 'io_enq_ready & io_enq_valid',
                         f'ram_{f}_io_deq_bits_MPORT_addr': 'deq_ptr_value',
                         f'ram_{f}_io_deq_bits_MPORT_en': "1'h1",
                         f'io_deq_bits_{f}': f'ram_{f}_io_deq_bits_MPORT_data'})
        assert re.search(r'reg\s+(?:\[\d+:0\]\s+)?ram_' + f + r'\s+\[0:1\];', body)
        assert normalize(f'if (ram_{f}_MPORT_en & ram_{f}_MPORT_mask) begin ram_{f}[ram_{f}_MPORT_addr] <= ram_{f}_MPORT_data; end') in normalize(body)
    for n, value in expected.items(): assert normalize(nets[n]) == normalize(value), n
    for pointer, fire in [('enq_ptr_value', 'do_enq'), ('deq_ptr_value', 'do_deq')]:
        assert normalize(f"if (reset) begin {pointer} <= 1'h0; end else if ({fire}) begin {pointer} <= {pointer} + 1'h1; end") in normalize(body)
    assert normalize("if (reset) begin maybe_full <= 1'h0; end else if (do_enq != do_deq) begin maybe_full <= do_enq; end") in normalize(body)
    buffer = equations(module(text, 'AXI4Buffer_2')[1]); q = 'nodeIn_r_deq_q'
    routes = {'auto_out_r_ready': q + '_io_enq_ready', q + '_io_enq_valid': 'auto_out_r_valid',
              'auto_in_r_valid': q + '_io_deq_valid', q + '_io_deq_ready': 'auto_in_r_ready',
              q + '_clock': 'clock', q + '_reset': 'reset'}
    for f in FIELDS:
        if f != 'resp': routes[q + '_io_enq_bits_' + f] = 'auto_out_r_bits_' + f
        routes['auto_in_r_bits_' + f] = q + '_io_deq_bits_' + f
    for n, value in routes.items(): assert buffer[n] == value, n
    engine_header, _ = module(text, 'CPUManagedStreamEngine')
    assert 'auto_cpu_managed_axi4_in_r_bits_resp' not in rtl_ports(engine_header)
    # SFC sinks the engine's constant Okay response into Queue_55's RAM write.
    return dict(queue='Queue_55', payload=fields, depth=2, flow=False, pipe=False,
                response_Okay_stored=True, reset_flushes_control_only=True,
                accepted_reset_enqueue_writes_RAM=True, specialized_wrapper_R_routes=13)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}; assert len(result) == len(found)
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 293 and len(new) == 295 and set(new) - set(old) == {TOP, QUEUE}
    for name, body in old.items(): assert new[name] == body, 'changed prior body ' + name
    engine = old['GGCPUStreamRead']
    zero = re.search(r'(%\w+) = firrtl.constant 0 : !firrtl.uint<2>', engine); assert zero
    assert f'firrtl.strictconnect %r_bits_resp, {zero[1]} : !firrtl.uint<2>' in engine
    previous = native_ports(old[OLD]); assert len(previous) == 122
    assert native_ports(new[TOP]) == previous
    copied = [(d, n, t) for d, n, t in previous if n not in CONSUMED]
    expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    expected.update({'rQueue.clock': 'top.hostClock', 'rQueue.reset': 'top.hostReset',
                     'sim.cpu_stream_r_ready': 'rQueue.enq.ready', 'rQueue.enq.valid': 'sim.cpu_stream_r_valid',
                     'rQueue.deq.ready': 'top.cpu_stream_r_ready', 'top.cpu_stream_r_valid': 'rQueue.deq.valid'})
    for f in FIELDS:
        expected['rQueue.enq.bits.' + f] = 'sim.cpu_stream_r_bits_' + f
        expected['top.cpu_stream_r_bits_' + f] = 'rQueue.deq.bits.' + f
    assert wiring(new[TOP]) == expected and len(expected) == 130
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('rQueue', QUEUE)]
    assert new[QUEUE] == native_module(baseline, QUEUE), 'shared native R queue differs'
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.CPUStreamResponseBufferTargets", nested = \{targets = \[([^]]+)\]\}\}', line); assert probe
    targets = ['cpu_stream_r_valid', 'cpu_stream_r_bits_data', 'hostClock', 'hostReset', 'mem_0.aw.bits.id', 'cpu_stream_aw_valid', 'cpu_stream_ar_bits_addr']
    assert re.findall(r'"([^"]+)"', probe[1]) == [f'~{TOP}|{OLD if n in CONSUMED else TOP}>' + n for n in targets]
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied: original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=293, public_ports=122,
                copied_ports=116, consumed_R_ports=6, wrapper_connections=130,
                identical_shared_native_queue=True, recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['Queue_55', 'AXI4Buffer_2', 'CPUManagedStreamEngine'],
                  SFC_contract=sfc_contract(a.sfc), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print CPU R queue; B buffering and production Print runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC R queue/reset/payload/Okay contracts; 293 prior bodies, 122 ports, 130 routes and recursive targets per order')


if __name__ == '__main__': main()
