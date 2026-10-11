#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the composed Print/Rocket AXI buffer with the immutable SFC boundary.

The C++ interpreter separately executes the actual composed SSA queues. SFC
eliminates the always-ready B dequeue input; native preserves that backpressure.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDAddressTranslationCompare import routes
from PrintRocketFASEDHostMemoryCompare import memory_type
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintRocketMasterCompare import native_module

TOP = 'GGFASEDAddressTranslationWrapper'
HELPER = 'GGFASEDHostMemoryBuffer'
QUEUES = ['GGFASEDMemoryAddressQueue2', 'GGFASEDMemoryWriteQueue2',
          'GGFASEDMemoryAckQueue2', 'GGFASEDMemoryReadQueue2']
SFC_QUEUES = ['Queue_29', 'Queue_23', 'Queue_31', 'Queue_33']
FIELDS = [dict(id=4, qos=4, prot=3, cache=4, lock=1, burst=2, size=3, len=8, addr=34),
          dict(strb=8, last=1, data=64), dict(id=4, resp=2), dict(id=4, last=1, data=64, resp=2)]
CHANNELS = [('aw', 0), ('w', 1), ('b', 2), ('ar', 0), ('r', 3)]


def sfc_contract(path):
    text = clean(path.read_text())
    header, body = module(text, 'AXI4Buffer_1')
    eq = {k: ' '.join(v.split()) for k, v in equations(body).items()}
    expected = {'clock': ['input', 1], 'reset': ['input', 1]}
    for key, (outward, width) in routes()[1].items():
        for side in ('in', 'out'):
            if side == 'in' and key == 'b.ready':
                continue
            expected['auto_' + side + '_' + key.replace('.', '_')] = [
                'output' if outward == (side == 'out') else 'input', 34 if key.endswith('.addr') else width]
    assert rtl_ports(header) == expected
    instances, compared = [], 0
    for ch, index in CHANNELS:
        request = ch not in ('b', 'r')
        producer, consumer = ('in', 'out') if request else ('out', 'in')
        inst = ('nodeOut_' if request else 'nodeIn_') + ch + '_deq_q'
        instances.append((SFC_QUEUES[index], inst))
        assert eq[inst + '_clock'] == 'clock' and eq[inst + '_reset'] == 'reset'
        assert eq[inst + '_io_enq_valid'] == f'auto_{producer}_{ch}_valid'
        assert eq[f'auto_{producer}_{ch}_ready'] == inst + '_io_enq_ready'
        assert eq[f'auto_{consumer}_{ch}_valid'] == inst + '_io_deq_valid'
        if ch != 'b':
            assert eq[inst + '_io_deq_ready'] == f'auto_{consumer}_{ch}_ready'
        for field in FIELDS[index]:
            assert eq[inst + '_io_enq_bits_' + field] == f'auto_{producer}_{ch}_bits_{field}'
            assert eq[f'auto_{consumer}_{ch}_bits_{field}'] == inst + '_io_deq_bits_' + field
            compared += 2
    assert re.findall(r'\b(Queue_\d+) (\w+) \(', body) == instances
    for name, fields in zip(SFC_QUEUES, FIELDS):
        _, queue = module(text, name)
        qe = {k: ' '.join(v.split()) for k, v in equations(queue).items()}
        assert qe['ptr_match'] == 'enq_ptr_value == deq_ptr_value'
        assert qe['empty'] == 'ptr_match & ~maybe_full' and qe['full'] == 'ptr_match & maybe_full'
        assert qe['io_enq_ready'] == '~full' and qe['io_deq_valid'] == '~empty'
        assert qe['do_enq'] == 'io_enq_ready & io_enq_valid'
        pop = 'io_deq_valid' if name == 'Queue_31' else 'do_deq'
        if pop == 'do_deq':
            assert qe[pop] == 'io_deq_ready & io_deq_valid'
        assert f'end else if (do_enq != {pop}) begin' in queue and 'maybe_full <= do_enq;' in queue
        for ptr, fire in [('enq_ptr_value', 'do_enq'), ('deq_ptr_value', pop)]:
            assert re.search(r"if \(reset\) begin\s+" + ptr + r" <= 1'h0;", queue)
            assert f'end else if ({fire}) begin' in queue and f"{ptr} <= {ptr} + 1'h1;" in queue
        for field, width in fields.items():
            decl = r'reg\s+' + (r'\[' + str(width - 1) + r':0\]\s+' if width > 1 else '')
            assert re.search(decl + 'ram_' + field + r' \[0:1\];', queue)
            assert qe['ram_' + field + '_MPORT_en'] == 'io_enq_ready & io_enq_valid'
            assert qe['ram_' + field + '_MPORT_addr'] == 'enq_ptr_value'
            assert qe['ram_' + field + '_MPORT_data'] == 'io_enq_bits_' + field
            assert qe['ram_' + field + '_io_deq_bits_MPORT_addr'] == 'deq_ptr_value'
            assert qe['ram_' + field + '_io_deq_bits_MPORT_data'] == f'ram_{field}[ram_{field}_io_deq_bits_MPORT_addr]'
            assert qe['io_deq_bits_' + field] == 'ram_' + field + '_io_deq_bits_MPORT_data'
            assert queue.index(f'ram_{field}[ram_{field}_MPORT_addr] <=') < queue.index('if (reset) begin')
    return dict(ports=len(expected), channels=5, payload_routes=compared, depth=2,
                flow=False, pipe=False, payload_widths=[sum(f.values()) for f in FIELDS],
                asynchronous_reads=True, reset_independent_RAM_writes=True)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
    prior, result = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, after))
    assert len(prior) == 265 and set(result) - set(prior) == {HELPER, *QUEUES}
    assert all(result[n] == body for n, body in prior.items() if n != TOP)
    old, new = native_module(before, TOP), native_module(after, TOP)
    assert native_ports(old) == native_ports(new) and len(native_ports(new)) == 110
    nets = wiring(old)
    assert nets.pop('top.fased_host_mem') == 'translation.out'
    nets.update({'memory_buffer.in': 'translation.out', 'top.fased_host_mem': 'memory_buffer.out',
                 'memory_buffer.clock': 'top.hostClock', 'memory_buffer.reset': 'top.hostReset'})
    assert wiring(new) == nets and len(nets) == 119
    assert before.splitlines()[1] == after.splitlines()[1], 'annotation archive changed'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new) == [
        ('sim', 'GGFASEDHostMemoryWrapper'), ('translation', 'GGFASEDAddressTranslation'),
        ('deinterleaver', 'GGFASEDReadDeinterleaver'), ('memory_buffer', HELPER)]
    helper = native_module(after, HELPER)
    master = memory_type().replace('uint<35>', 'uint<34>')
    assert native_ports(helper) == [('in', 'clock', '!firrtl.clock'), ('in', 'reset', '!firrtl.uint<1>'),
                                    ('in', 'in', master), ('out', 'out', master)]
    helper_nets = {}
    for ch, index in CHANNELS:
        producer, consumer = ('top.in', 'top.out') if ch not in ('b', 'r') else ('top.out', 'top.in')
        q = ch + '_queue'
        helper_nets.update({q + '.clock': 'top.clock', q + '.reset': 'top.reset',
                           q + '.enq.valid': producer + '.' + ch + '.valid',
                           producer + '.' + ch + '.ready': q + '.enq.ready',
                           consumer + '.' + ch + '.valid': q + '.deq.valid',
                           q + '.deq.ready': consumer + '.' + ch + '.ready'})
        for field in FIELDS[index]:
            helper_nets[q + '.enq.bits.' + field] = producer + '.' + ch + '.bits.' + field
            helper_nets[consumer + '.' + ch + '.bits.' + field] = q + '.deq.bits.' + field
    assert wiring(helper) == helper_nets and len(helper_nets) == 84
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', helper) == [
        (ch + '_queue', QUEUES[index]) for ch, index in CHANNELS]
    for name in [HELPER, *QUEUES]:
        assert native_module(after, name) == native_module(baseline, name), 'shared semantics changed: ' + name
    for name, fields in zip(QUEUES, FIELDS):
        queue = native_module(after, name)
        memories = [line for line in queue.splitlines() if ' = firrtl.mem ' in line]
        assert len(memories) == 1 and all(x in memories[0] for x in [
            'depth = 2 : i64', 'readLatency = 0 : i32', 'writeLatency = 1 : i32',
            f'data: uint<{sum(fields.values())}>'])
        assert queue.count('firrtl.regreset ') == 3
    assert len([n for n in prior if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=264,
                preserved_ports=110, wrapper_connections=119, helper_connections=84,
                annotation_archive_unchanged=True, shared_native_queue_and_helper_match=True,
                preserved_Print_modules=21)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('before', 'after'):
        p.add_argument('--' + key, type=Path, action='append', required=True)
    for key in ('sfc', 'baseline', 'report-json'):
        p.add_argument('--' + key, type=Path, required=True)
    a = p.parse_args()
    assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), baseline=str(a.baseline.resolve()),
                  SFC_modules=['AXI4Buffer_1', *SFC_QUEUES], SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  optimized_SFC_ports=['auto_in_b_ready', 'Queue_31.io_deq_ready'],
                  limitation='Print-enabled production runtime parity remains pending; native retains B.ready stalls removed by SFC.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC five depth-two AXI queues, ports, payload/clock/reset routes and RAM semantics; '
          '264 prior bodies, 110 ports, archive and 119 wrapper connections preserved per order')


if __name__ == '__main__':
    main()
