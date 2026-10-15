#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print output queues with immutable SFC AXI4Buffer contracts.

The C++ interpreter checks emitted queue SSA, including reset-edge RAM writes,
all occupancy states and independently stalled AW/W/B/AR/R transactions.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring

TOP = 'GGHostMemoryReadResponseWrapper'
HELPER = 'GGHostMemoryOutputBuffer'
QUEUES = ['GGHostMemoryAddressQueue2', 'GGHostMemoryWriteQueue2',
          'GGHostMemoryAckQueue2', 'GGHostMemoryReadQueue2']
FIELDS = [dict(id=5, addr=34, len=8, size=3, burst=2, lock=1, cache=4, prot=3, qos=4),
          dict(data=64, strb=8, last=1), dict(id=5, resp=2),
          dict(id=5, data=64, resp=2, last=1)]


def sfc_contract(path):
    text = clean(path.read_text()); reports = []
    normalize = lambda x: re.sub(r'\s+', '', x)
    header, buffer = module(text, 'AXI4Buffer')
    instances = re.findall(r'(Queue_\d+)\s+(\w+)\s*\(', buffer)
    assert [name for _, name in instances] == [
        'nodeOut_aw_deq_q', 'nodeOut_w_deq_q', 'nodeIn_b_deq_q',
        'nodeOut_ar_deq_q', 'nodeIn_r_deq_q']
    assert instances[0][0] == instances[3][0], 'AW/AR queue types differ'
    queue_modules = [instances[i][0] for i in (0, 1, 2, 4)]
    assert len(set(queue_modules)) == 4, 'payload queue types unexpectedly shared'
    # Verify the five channels really traverse those queues, including response
    # direction, all payload leaves, and the shared host clock/reset.
    expected_ports = dict(clock=['input', 1], reset=['input', 1])
    routes = {}
    for (queue, inst), channel, fields in zip(instances, ('aw', 'w', 'b', 'ar', 'r'),
                                             (FIELDS[0], FIELDS[1], FIELDS[2], FIELDS[0], FIELDS[3])):
        producer, consumer = ('out', 'in') if channel in ('b', 'r') else ('in', 'out')
        leaves = dict(valid=1, **{'bits_' + f: w for f, w in fields.items()})
        routes[inst + '_clock'] = 'clock'
        routes[inst + '_reset'] = 'reset'
        routes[inst + '_io_deq_ready'] = f'auto_{consumer}_{channel}_ready'
        routes[f'auto_{producer}_{channel}_ready'] = inst + '_io_enq_ready'
        for side in ('in', 'out'):
            direction = 'input' if side == producer else 'output'
            expected_ports[f'auto_{side}_{channel}_ready'] = ['output' if direction == 'input' else 'input', 1]
            for leaf, width in leaves.items():
                expected_ports[f'auto_{side}_{channel}_{leaf}'] = [direction, width]
        for leaf in leaves:
            routes[inst + '_io_enq_' + leaf] = f'auto_{producer}_{channel}_{leaf}'
            routes[f'auto_{consumer}_{channel}_{leaf}'] = inst + '_io_deq_' + leaf
        for port in ['clock', 'reset', 'io_enq_ready', 'io_deq_ready'] + [
                f'io_{side}_{leaf}' for side in ('enq', 'deq') for leaf in leaves]:
            assert f'.{port}({inst}_{port})' in normalize(buffer), (queue, inst, port)
    assert rtl_ports(header) == expected_ports, 'AXI4Buffer port geometry/direction differs'
    nets = equations(buffer)
    for name, value in routes.items():
        assert normalize(nets[name]) == value, ('AXI4Buffer', name, nets[name], value)
    for name, fields in zip(queue_modules, FIELDS):
        header, body = module(text, name); nets = equations(body); ports = rtl_ports(header)
        expected = dict(ptr_match='enq_ptr_value == deq_ptr_value',
                        empty='ptr_match & ~maybe_full', full='ptr_match & maybe_full',
                        do_enq='io_enq_ready & io_enq_valid', do_deq='io_deq_ready & io_deq_valid',
                        io_enq_ready='~full', io_deq_valid='~empty')
        for f, width in fields.items():
            for side, direction in [('enq', 'input'), ('deq', 'output')]:
                assert ports[f'io_{side}_bits_{f}'] == [direction, width]
            expected.update({f'ram_{f}_MPORT_data': f'io_enq_bits_{f}',
                             f'ram_{f}_MPORT_addr': 'enq_ptr_value',
                             f'ram_{f}_MPORT_mask': "1'h1", f'ram_{f}_MPORT_en': 'io_enq_ready & io_enq_valid',
                             f'ram_{f}_io_deq_bits_MPORT_addr': 'deq_ptr_value',
                             f'io_deq_bits_{f}': f'ram_{f}_io_deq_bits_MPORT_data'})
            assert re.search(r'reg\s+(?:\[\d+:0\]\s+)?ram_' + f + r'\s+\[0:1\];', body)
            # RAM writes have no reset guard: an accepted enqueue writes on reset.
            assignment = f'if (ram_{f}_MPORT_en & ram_{f}_MPORT_mask) begin ram_{f}[ram_{f}_MPORT_addr] <= ram_{f}_MPORT_data; end'
            assert normalize(assignment) in normalize(body)
        for n, value in expected.items():
            assert normalize(nets[n]) == normalize(value), (name, n, nets[n], value)
        for pointer, fire in [('enq_ptr_value', 'do_enq'), ('deq_ptr_value', 'do_deq')]:
            state = f"if (reset) begin {pointer} <= 1'h0; end else if ({fire}) begin {pointer} <= {pointer} + 1'h1; end"
            assert normalize(state) in normalize(body)
        assert normalize("if (reset) begin maybe_full <= 1'h0; end else if (do_enq != do_deq) begin maybe_full <= do_enq; end") in normalize(body)
        reports.append(dict(module=name, payload_bits=sum(fields.values()), depth=2,
                            equations=len(expected), flow=False, pipe=False, reset_clears_occupancy=True,
                            accepted_reset_enqueue_writes_RAM=True))
    return dict(queues=reports, independent_channels=5, buffer_ports=len(expected_ports),
                buffer_equations=len(routes))


def compare_binding(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}
        assert len(result) == len(found), 'duplicate modules'
        return result
    old, new = modules(before), modules(after)
    assert len(new) == len(old) + 5
    for name, body in old.items():
        if name != TOP: assert new[name] == body, 'changed prior body ' + name
    assert set(new) - set(old) == set(QUEUES + [HELPER])
    assert native_ports(old[TOP]) == native_ports(new[TOP])
    expected = wiring(old[TOP])
    expected = {d.replace('top.host_mem.', 'host_memory_buffer.in.'):
                s.replace('top.host_mem.', 'host_memory_buffer.in.') for d, s in expected.items()}
    expected.update({'host_memory_buffer.clock': 'top.hostClock',
                     'host_memory_buffer.reset': 'top.hostReset',
                     'top.host_mem': 'host_memory_buffer.out'})
    assert wiring(new[TOP]) == expected, 'buffer or copied connections differ'
    instances = re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP])
    assert instances == [('host_memory_buffer', HELPER), ('sim', 'GGHostMemoryReadWrapper'),
                         ('read_responses', 'GGHostMemoryReadResponseRouter')]
    for name in QUEUES + [HELPER]:
        assert new[name] == native_module(baseline, name), 'shared queue/helper differs ' + name
    assert before.splitlines()[1] == after.splitlines()[1], 'recursive archive changed'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(old)-1,
                public_ports=len(native_ports(new[TOP])), wrapper_connections=len(expected), identical_native_helpers=5,
                preserved_Print_modules=len([n for n in old if n.startswith('GGPrintBridge')]), recursive_archive_unchanged=True)


def compare(before_path, after_path, baseline):
    report = compare_binding(before_path, after_path, baseline)
    assert (report['unchanged_module_bodies'], report['public_ports'],
            report['wrapper_connections'], report['preserved_Print_modules']) == (280, 109, 120, 21)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    contract = sfc_contract(a.sfc)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=[q['module'] for q in contract['queues']] + ['AXI4Buffer'],
                  SFC_contract=contract,
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print buffer composition; production Print integration and runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC five-channel depth-two queue/reset/payload contracts; 280 prior bodies, 109 ports, 120 routes and nested targets preserved per order')


if __name__ == '__main__': main()
