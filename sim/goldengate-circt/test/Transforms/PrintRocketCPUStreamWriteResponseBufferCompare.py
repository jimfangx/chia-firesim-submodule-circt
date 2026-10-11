#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print CPU B attachment with SFC's AXI4Buffer_2.

Actual MemOps are exercised by CPUStreamWriteResponseBufferTest against a deque oracle.
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

OLD = 'GGPrintCPUStreamResponseBufferWrapper'
TOP = 'GGPrintCPUStreamWriteResponseBufferWrapper'
QUEUE = 'GGCPUStreamBQueue2'
FIELDS = ['id', 'resp']
CONSUMED = ['cpu_stream_b_' + n for n in ['ready', 'valid'] + ['bits_' + f for f in FIELDS]]


def sfc_contract(path):
    text = clean(path.read_text()); normalize = lambda x: re.sub(r'\s+', '', x)
    header, body = module(text, 'Queue_53'); nets = equations(body); ports = rtl_ports(header)
    fields = dict(id=16, resp=2)
    # Rocket has no incoming streams: SFC specializes enqueue valid to zero,
    # eliminating enqueue/full state but preserving uninitialized RAM reads.
    expected = dict(ptr_match='~deq_ptr_value', do_deq='io_deq_ready & io_deq_valid',
                    io_deq_valid='~ptr_match')
    assert ports['io_enq_bits_id'] == ['input', 16]
    assert 'io_enq_valid' not in ports and 'io_enq_ready' not in ports
    assert 'io_enq_bits_resp' not in ports
    for f, width in fields.items():
        assert ports['io_deq_bits_' + f] == ['output', width]
        expected.update({f'ram_{f}_MPORT_data': "2'h0" if f == 'resp' else 'io_enq_bits_id',
                         f'ram_{f}_MPORT_addr': "1'h0", f'ram_{f}_MPORT_mask': "1'h1",
                         f'ram_{f}_MPORT_en': "1'h0",
                         f'ram_{f}_io_deq_bits_MPORT_addr': 'deq_ptr_value',
                         f'ram_{f}_io_deq_bits_MPORT_en': "1'h1",
                         f'io_deq_bits_{f}': f'ram_{f}_io_deq_bits_MPORT_data'})
        assert re.search(r'reg\s+(?:\[\d+:0\]\s+)?ram_' + f + r'\s+\[0:1\];', body)
        assert normalize(f'if (ram_{f}_MPORT_en & ram_{f}_MPORT_mask) begin ram_{f}[ram_{f}_MPORT_addr] <= ram_{f}_MPORT_data; end') in normalize(body)
    for n, value in expected.items(): assert normalize(nets[n]) == normalize(value), n
    assert normalize("if (reset) begin deq_ptr_value <= 1'h0; end else if (do_deq) begin deq_ptr_value <= deq_ptr_value + 1'h1; end") in normalize(body)
    buffer = equations(module(text, 'AXI4Buffer_2')[1]); q = 'nodeIn_b_deq_q'
    routes = {q + '_io_deq_ready': 'auto_in_b_ready', 'auto_in_b_valid': q + '_io_deq_valid',
              q + '_clock': 'clock', q + '_reset': 'reset',
              q + '_io_enq_bits_id': 'auto_out_b_bits_id'}
    for f in FIELDS: routes['auto_in_b_bits_' + f] = q + '_io_deq_bits_' + f
    for n, value in routes.items(): assert buffer[n] == value, n
    engine_header, engine_body = module(text, 'CPUManagedStreamEngine')
    engine_ports = rtl_ports(engine_header)
    assert 'auto_cpu_managed_axi4_in_b_valid' not in engine_ports
    assert 'auto_cpu_managed_axi4_in_b_bits_resp' not in engine_ports
    assert equations(engine_body)['auto_cpu_managed_axi4_in_b_bits_id'] == 'auto_cpu_managed_axi4_in_aw_bits_id'
    return dict(queue='Queue_53', payload=fields, depth=2,
                zero_sink_enqueue_specialized=True, response_Okay_write_data=True,
                invalid_payload_reads_RAM=True, reset_flushes_control_only=True,
                specialized_wrapper_B_routes=7)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}; assert len(result) == len(found)
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 295 and len(new) == 297 and set(new) - set(old) == {TOP, QUEUE}
    for name, body in old.items(): assert new[name] == body, 'changed prior body ' + name
    engine = old['CatalogRocket_GGEmptyCPUStreamWrite']
    for n, width in [('b_valid', 1), ('b_bits_resp', 2)]:
        zero = re.search(r'(%\w+) = firrtl.constant 0 : !firrtl.uint<' + str(width) + '>', engine); assert zero
        assert f'firrtl.strictconnect %{n}, {zero[1]} : !firrtl.uint<{width}>' in engine
    assert 'firrtl.strictconnect %b_bits_id, %aw_bits_id : !firrtl.uint<16>' in engine
    previous = native_ports(old[OLD]); assert len(previous) == 122
    assert native_ports(new[TOP]) == previous
    copied = [(d, n, t) for d, n, t in previous if n not in CONSUMED]
    expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    expected.update({'bQueue.clock': 'top.hostClock', 'bQueue.reset': 'top.hostReset',
                     'sim.cpu_stream_b_ready': 'bQueue.enq.ready', 'bQueue.enq.valid': 'sim.cpu_stream_b_valid',
                     'bQueue.deq.ready': 'top.cpu_stream_b_ready', 'top.cpu_stream_b_valid': 'bQueue.deq.valid'})
    for f in FIELDS:
        expected['bQueue.enq.bits.' + f] = 'sim.cpu_stream_b_bits_' + f
        expected['top.cpu_stream_b_bits_' + f] = 'bQueue.deq.bits.' + f
    assert wiring(new[TOP]) == expected and len(expected) == 128
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('bQueue', QUEUE)]
    assert new[QUEUE] == native_module(baseline, QUEUE), 'shared native B queue differs'
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.CPUStreamWriteResponseBufferTargets", nested = \{targets = \[([^]]+)\]\}\}', line); assert probe
    targets = ['cpu_stream_b_valid', 'cpu_stream_b_bits_id', 'hostClock', 'hostReset', 'mem_0.aw.bits.id', 'cpu_stream_aw_valid', 'cpu_stream_ar_bits_addr']
    assert re.findall(r'"([^"]+)"', probe[1]) == [f'~{TOP}|{OLD if n in CONSUMED else TOP}>' + n for n in targets]
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied: original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=295, public_ports=122,
                copied_ports=118, consumed_B_ports=4, wrapper_connections=128,
                identical_shared_native_queue=True, recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['Queue_53', 'AXI4Buffer_2', 'CPUManagedStreamEngine'],
                  SFC_contract=sfc_contract(a.sfc), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print CPU B queue matches the zero-sink specialization structurally; generic queue traffic is independently tested. Platform aggregate and production Print runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC zero-sink B queue/RAM/Okay contracts; 295 prior bodies, 122 ports, 128 routes and recursive targets per order')


if __name__ == '__main__': main()
