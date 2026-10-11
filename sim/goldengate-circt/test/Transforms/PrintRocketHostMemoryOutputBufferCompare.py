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
SFC = ['Queue_22', 'Queue_23', 'Queue_24', 'Queue_26']
FIELDS = [dict(id=5, addr=34, len=8, size=3, burst=2, lock=1, cache=4, prot=3, qos=4),
          dict(data=64, strb=8, last=1), dict(id=5, resp=2),
          dict(id=5, data=64, resp=2, last=1)]


def sfc_contract(path):
    text = clean(path.read_text()); reports = []
    normalize = lambda x: re.sub(r'\s+', '', x)
    for name, fields in zip(SFC, FIELDS):
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
    _, buffer = module(text, 'AXI4Buffer')
    assert re.findall(r'(Queue_\d+)\s+(\w+)\s*\(', buffer) == [
        ('Queue_22', 'nodeOut_aw_deq_q'), ('Queue_23', 'nodeOut_w_deq_q'),
        ('Queue_24', 'nodeIn_b_deq_q'), ('Queue_22', 'nodeOut_ar_deq_q'), ('Queue_26', 'nodeIn_r_deq_q')]
    return dict(queues=reports, independent_channels=5)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}
        assert len(result) == len(found), 'duplicate modules'
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 281 and len(new) == 286
    for name, body in old.items():
        if name != TOP: assert new[name] == body, 'changed prior body ' + name
    assert set(new) - set(old) == set(QUEUES + [HELPER])
    assert native_ports(old[TOP]) == native_ports(new[TOP]) and len(native_ports(new[TOP])) == 109
    expected = wiring(old[TOP])
    expected = {d.replace('top.host_mem.', 'host_memory_buffer.in.'):
                s.replace('top.host_mem.', 'host_memory_buffer.in.') for d, s in expected.items()}
    expected.update({'host_memory_buffer.clock': 'top.hostClock',
                     'host_memory_buffer.reset': 'top.hostReset',
                     'top.host_mem': 'host_memory_buffer.out'})
    assert wiring(new[TOP]) == expected, 'buffer or copied connections differ'
    assert len(expected) == 120
    instances = re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP])
    assert instances == [('host_memory_buffer', HELPER), ('sim', 'GGHostMemoryReadWrapper'),
                         ('read_responses', 'GGHostMemoryReadResponseRouter')]
    for name in QUEUES + [HELPER]:
        assert new[name] == native_module(baseline, name), 'shared queue/helper differs ' + name
    assert before.splitlines()[1] == after.splitlines()[1], 'recursive archive changed'
    assert len([n for n in old if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=280,
                public_ports=109, wrapper_connections=120, identical_native_helpers=5,
                preserved_Print_modules=21, recursive_archive_unchanged=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=SFC + ['AXI4Buffer'],
                  SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print buffer composition; production Print integration and runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC five-channel depth-two queue/reset/payload contracts; 280 prior bodies, 109 ports, 120 routes and nested targets preserved per order')


if __name__ == '__main__': main()
