#!/usr/bin/env python3
# See LICENSE for license details.
"""Check expanded native composition and the immutable SFC deinterleaver contract.

The separate C++ test executes these actual composed queues and helper against
an independent burst model. SFC has constant R.ready and eliminates B.ready;
native preserves both forms of backpressure for the expanded interface.
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
HELPER = 'GGFASEDReadDeinterleaver'
QUEUE = 'GGFASEDDeinterleaveQueue8'


def sfc_contract(path):
    text = clean(path.read_text())
    header, body = module(text, 'AXI4Deinterleaver')
    eq = {k: ' '.join(v.split()) for k, v in equations(body).items()}
    expected = {'clock': ['input', 1], 'reset': ['input', 1]}
    passthrough = 0
    for key, (outward, width) in routes()[1].items():
        if key == 'b.ready':
            continue
        for side in ('in', 'out'):
            if side == 'in' and key == 'r.ready':
                continue
            expected['auto_anon_' + side + '_' + key.replace('.', '_')] = [
                'output' if outward == (side == 'out') else 'input', width]
        if not key.startswith('r.'):
            dest, src = ('out', 'in') if outward else ('in', 'out')
            assert eq['auto_anon_' + dest + '_' + key.replace('.', '_')] == 'auto_anon_' + src + '_' + key.replace('.', '_')
            passthrough += 1
    assert rtl_ports(header) == expected and len(expected) == 73
    assert passthrough == 30
    assert re.findall(r'Queue_34 qs_queue_(\d+) \(', body) == [str(i) for i in range(16)]
    assert eq['auto_anon_in_r_valid'] == 'locked'
    assert eq['enq_OH'] == "16'h1 << auto_anon_out_r_bits_id"
    assert eq['deq_OH'] == "16'h1 << deq_id"
    assert eq['_pending_inc_T_1'] == 'anonOut_r_ready & auto_anon_out_r_valid'
    # Follow lowered vector reads back to every queue payload, rather than
    # relying on SFC's generated temporary names for the response muxes.
    for field in ('id', 'data', 'resp', 'last'):
        expr = eq['auto_anon_in_r_bits_' + field]
        for i in range(15, 0, -1):
            while expr in eq:
                expr = eq[expr]
            match = re.fullmatch(r"4'h([0-9a-f]+) == deq_id \? (\w+) : (\w+)", expr)
            assert match and int(match[1], 16) == i and match[2] == f'deq_bits_{i}_{field}'
            expr = match[3]
        assert expr == 'deq_bits_0_' + field
    for i in range(16):
        suffix = '_' + str(i) if i else ''
        q = 'qs_queue_' + str(i) + '_'
        assert eq[q + 'clock'] == 'clock' and eq[q + 'reset'] == 'reset'
        assert eq[q + 'io_enq_valid'] == f'enq_OH[{i}] & auto_anon_out_r_valid'
        assert eq[q + 'io_deq_ready'] == f'deq_OH[{i}] & locked'
        for field in ('id', 'data', 'resp', 'last'):
            assert eq[q + 'io_enq_bits_' + field] == 'auto_anon_out_r_bits_' + field
        assert eq['pending_inc' + suffix] == f'enq_OH[{i}] & _pending_inc_T_1 & auto_anon_out_r_bits_last'
        assert eq['pending_dec' + suffix] == f'deq_OH[{i}] & locked & anonIn_r_bits_last'
        inc, dec = '_GEN_' + str(82 + i * 2), '_GEN_' + str(83 + i * 2)
        temp = '_pending_next_T_' + str(1 + i * 4)
        assert eq[inc] == "{{3'd0}, pending_inc" + suffix + '}'
        assert eq[dec] == "{{3'd0}, pending_dec" + suffix + '}'
        assert eq[temp] == 'pending_count' + suffix + ' + ' + inc
        assert eq['pending_next' + suffix] == temp + ' - ' + dec
        assert 'assert(~pending_dec' + suffix + " | pending_count" + suffix + " != 4'h0);" in body
        assert 'assert(~pending_inc' + suffix + " | pending_count" + suffix + " != 4'h8);" in body
        assert 'pending_count' + suffix + ' <= pending_next' + suffix + ';' in body
    assert len(re.findall(r'assert\(', body)) == 32
    assert "locked <= 1'h0;" in body and 'locked <= |pending;' in body
    assert eq['_GEN_1'] == "~locked | locked & anonIn_r_bits_last ? _deq_id_T_12 : {{1'd0}, deq_id}"
    # Prefix OR and inverted shifted mask select the lowest pending ID.
    prior = 'pending'
    for shift, a, b in [(1, '', '_2'), (2, '_3', '_5'), (4, '_6', '_8'), (8, '_9', '_11')]:
        assert eq['_winner_T' + a] == '{' + prior + f", {shift}'h0" + '}'
        assert eq['_winner_T' + b] == prior + ' | _winner_T' + a + '[15:0]'
        prior = '_winner_T' + b
    assert eq['_winner_T_13'] == "{_winner_T_11, 1'h0}"
    assert eq['_winner_T_14'] == '~_winner_T_13' and eq['winner'] == '_GEN_114 & _winner_T_14'
    _, queue = module(text, 'Queue_34')
    qe = {k: ' '.join(v.split()) for k, v in equations(queue).items()}
    for field, width in [('id', 4), ('data', 64), ('resp', 2), ('last', 1)]:
        decl = r'reg\s+' + (r'\[' + str(width - 1) + r':0\]\s+' if width > 1 else '') + 'ram_' + field + r' \[0:7\];'
        assert re.search(decl, queue)
        assert qe['ram_' + field + '_MPORT_en'] == 'io_enq_ready & io_enq_valid'
        assert qe['ram_' + field + '_io_deq_bits_MPORT_addr'] == 'deq_ptr_value'
    assert qe['ptr_match'] == 'enq_ptr_value == deq_ptr_value'
    assert qe['empty'] == 'ptr_match & ~maybe_full' and qe['full'] == 'ptr_match & maybe_full'
    assert qe['io_enq_ready'] == '~full' and qe['io_deq_valid'] == '~empty'
    assert 'end else if (do_enq != do_deq) begin' in queue and 'maybe_full <= do_enq;' in queue
    for ptr in ('enq_ptr_value', 'deq_ptr_value'):
        assert re.search(r"if \(reset\) begin\s+" + ptr + r" <= 3'h0;", queue)
    return dict(ports=73, passthrough_routes=30, ID_queues=16, queue_depth=8,
                queue_payload_bits=71, pending_counter_bits=4, bounds_assertions=32,
                next_count_lowest_ID_arbitration=True, accepted_last_lock=True)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
    prior, result = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, after))
    assert len(prior) == 263 and set(result) - set(prior) == {HELPER, QUEUE}
    assert all(result[n] == body for n, body in prior.items() if n != TOP)
    old, new = native_module(before, TOP), native_module(after, TOP)
    assert native_ports(old) == native_ports(new) and len(native_ports(new)) == 110
    nets = wiring(old)
    assert nets.pop('translation.in') == 'sim.fased_host_mem'
    nets.update({'deinterleaver.in': 'sim.fased_host_mem', 'translation.in': 'deinterleaver.out',
                 'deinterleaver.clock': 'top.hostClock', 'deinterleaver.reset': 'top.hostReset'})
    assert wiring(new) == nets and len(nets) == 116
    assert before.splitlines()[1] == after.splitlines()[1], 'annotation archive changed'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new) == [
        ('sim', 'GGFASEDHostMemoryWrapper'), ('translation', 'GGFASEDAddressTranslation'),
        ('deinterleaver', HELPER)]
    helper = native_module(after, HELPER)
    assert native_ports(helper) == [('in', 'clock', '!firrtl.clock'), ('in', 'reset', '!firrtl.uint<1>'),
                                    ('in', 'in', memory_type()), ('out', 'out', memory_type())]
    for name in (HELPER, QUEUE):
        assert native_module(after, name) == native_module(baseline, name), 'shared semantics changed: ' + name
    assert re.findall(r' = firrtl.instance qs_queue_(\d+) @' + QUEUE, helper) == [str(i) for i in range(16)]
    assert helper.count('firrtl.assert ') == 32
    assert len([n for n in prior if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=262,
                preserved_ports=110, wrapper_connections=116, annotation_archive_unchanged=True,
                shared_native_queue_and_helper_match=True, preserved_Print_modules=21)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('before', 'after'):
        p.add_argument('--' + key, type=Path, action='append', required=True)
    for key in ('sfc', 'baseline', 'report-json'):
        p.add_argument('--' + key, type=Path, required=True)
    a = p.parse_args()
    assert len(a.before) == len(a.after)
    contract = sfc_contract(a.sfc)
    baseline = a.baseline.read_text()
    report = dict(golden=str(a.sfc.resolve()), baseline=str(a.baseline.resolve()),
                  SFC_modules=['AXI4Deinterleaver', 'Queue_34'],
                  SFC_contract=contract, orders=[compare(x, y, baseline) for x, y in zip(a.before, a.after)],
                  optimized_SFC_ports=['auto_anon_in_b_ready', 'auto_anon_out_b_ready', 'auto_anon_in_r_ready'],
                  limitation='Print-enabled production runtime parity remains pending; native retains R.ready stalls removed by SFC.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC 73 ports, 30 passthrough routes, 16 eight-beat queues, pending counts, lock and lowest-ID arbitration; '
          '262 prior bodies, 110 ports, archive and 116 wrapper connections preserved per order')


if __name__ == '__main__':
    main()
