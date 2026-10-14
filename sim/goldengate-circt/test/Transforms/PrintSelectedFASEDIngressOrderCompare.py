#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected transaction ordering with immutable SFC ingress/DualQueue.

SFC disables Print. This checks shared order semantics and selected connections;
selected host issue gates and the unused DualQueue.next lookahead remain pending.
"""
import argparse
import itertools
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintRocketFASEDIssueCompare import expressions, evaluate
from PrintSelectedFASEDIngressCompare import transfer

OLD = 'GGSelectedPrintFASEDIngressCreditsWrapper'
TOP = 'GGSelectedPrintFASEDIngressOrderWrapper'
HELPER = 'GGFASEDIngressOrder20'


def oracle(path):
    text = clean(path.read_text())
    ingress = equations(module(text, 'IngressModule')[1])
    for n, v in {
        'xaction_order_clock': 'clock', 'xaction_order_reset': 'reset',
        'xaction_order_io_enqA_valid': 'arQueue_io_enq_ready & arQueue_io_enq_valid',
        'xaction_order_io_enqB_valid': '_write_req_done_T_4 | _write_req_done_T_5',
        'xaction_order_io_deq_ready': '_xaction_order_io_deq_ready_T | _xaction_order_io_deq_ready_T_1',
        '_xaction_order_io_deq_ready_T': 'io_nastiOutputs_ar_ready & io_nastiOutputs_ar_valid',
        '_xaction_order_io_deq_ready_T_1': 'io_nastiOutputs_aw_ready & io_nastiOutputs_aw_valid',
    }.items(): assert ingress[n] == v, (n, ingress[n])
    dual_body = module(text, 'DualQueue')[1]
    dual = equations(dual_body)
    for n, v in {
        'io_enqA_ready': 'enqPointer ^ ~io_enqA_valid ? qB_io_enq_ready : qA_io_enq_ready',
        'io_enqB_ready': 'enqPointer ^ ~io_enqA_valid ? qA_io_enq_ready : qB_io_enq_ready',
        'io_deq_valid': 'deqPointer ? qB_io_deq_valid : qA_io_deq_valid',
        'io_deq_bits': 'deqPointer ? qB_io_deq_bits : qA_io_deq_bits',
        'qA_io_enq_valid': 'enqPointer ^ ~io_enqA_valid ? io_enqB_valid : io_enqA_valid',
        'qB_io_enq_valid': 'enqPointer ^ ~io_enqA_valid ? io_enqA_valid : io_enqB_valid',
        'qA_io_enq_bits': "enqPointer ^ ~io_enqA_valid ? 1'h0 : 1'h1",
        'qB_io_enq_bits': 'enqPointer ^ ~io_enqA_valid',
        'qA_io_deq_ready': "deqPointer ? 1'h0 : io_deq_ready",
        'qB_io_deq_ready': 'deqPointer & io_deq_ready',
    }.items(): assert dual[n] == v, (n, dual[n])
    for pointer, fire in [('enqPointer', '_T ^ _T_1'), ('deqPointer', '_T_5')]:
        assert f'else if ({fire})' in dual_body
        assert re.search(r'if \(reset\) begin\s+' + pointer + r" <= 1'h0;", dual_body)
        assert f'{pointer} <= ~{pointer};' in dual_body
    queue = module(text, 'Queue_10')[1]
    for bank in ('qA', 'qB'):
        assert re.search(r'\bQueue_10\s+' + bank + r'\s*\(', dual_body)
        assert dual[bank + '_clock'] == 'clock' and dual[bank + '_reset'] == 'reset'
    for n, v in {
        'ptr_match': 'enq_ptr_value == deq_ptr_value', 'empty': 'ptr_match & ~maybe_full',
        'full': 'ptr_match & maybe_full', 'do_enq': 'io_enq_ready & io_enq_valid',
        'do_deq': 'io_deq_ready & io_deq_valid', 'io_enq_ready': '~full', 'io_deq_valid': '~empty',
        'ram_MPORT_en': 'io_enq_ready & io_enq_valid', 'ram_MPORT_mask': "1'h1",
        'ram_MPORT_addr': 'enq_ptr_value', 'ram_MPORT_data': 'io_enq_bits',
        'ram_io_deq_bits_MPORT_en': "1'h1", 'ram_io_deq_bits_MPORT_addr': 'deq_ptr_value',
        'wrap': "enq_ptr_value == 4'h9", 'wrap_1': "deq_ptr_value == 4'h9",
    }.items(): assert equations(queue)[n] == v, (n, equations(queue)[n])
    assert re.search(r'reg\s+ram\s*\[0:9\]', queue)
    assert 'if (ram_MPORT_en & ram_MPORT_mask)' in queue
    assert 'else if (do_enq != do_deq)' in queue and 'maybe_full <= do_enq;' in queue
    # The write is outside the reset branches: reset flushes state, not RAM.
    assert queue.index('ram[ram_MPORT_addr] <= ram_MPORT_data;') < queue.index('if (reset)')


def compare(before, after, baseline, old_name=OLD, top=TOP, rtl_path=None):
    old_text, text = before.read_text(), after.read_text()
    def bodies(t):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', t, re.M | re.S)}
    previous, new = bodies(old_text), bodies(text)
    assert set(new) - set(previous) == {top, HELPER}
    for n, body in previous.items(): assert new[n] == body, 'prior module changed ' + n
    helper = new[HELPER]
    assert helper == native_module(baseline, HELPER), 'shared order operations changed'
    ports = native_ports(previous[old_name])
    assert native_ports(new[top]) == ports + [
        ('out', 'fased_ingress_order', '!firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>'),
        ('out', 'fased_ingress_order_ready', '!firrtl.bundle<read: uint<1>, write: uint<1>>')]
    expected = {'sim.' + n if d == 'in' else 'top.' + n:
                'top.' + n if d == 'in' else 'sim.' + n for d, n, _ in ports}
    expected.update({'xactionOrder.clock': 'top.hostClock',
        'xactionOrder.reset': 'sim.fased_ingress_reset',
        'xactionOrder.enqA_valid': 'sim.fased_ingress_ar_enq_fire',
        'xactionOrder.enqB_valid': 'sim.fased_ingress_credits.writeReqDone',
        'xactionOrder.deq_ready': 'top.fased_ingress_order.ready',
        'top.fased_ingress_order.valid': 'xactionOrder.deq_valid',
        'top.fased_ingress_order.bits': 'xactionOrder.deq_bits',
        'top.fased_ingress_order_ready.read': 'xactionOrder.enqA_ready',
        'top.fased_ingress_order_ready.write': 'xactionOrder.enqB_ready'})
    assert wiring(text, top, aggregate=True) == expected, 'copied/event/reset wiring differs'
    prior = json.loads(before.with_name(before.stem + '-all.json').read_text())
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    assert updated == transfer(prior, old_name, top, {n for _, n, _ in ports}), 'archive transfer differs'
    memories = [line for line in helper.splitlines() if ' = firrtl.mem ' in line]
    assert len(memories) == 2 and all('depth = 10 : i64' in l and 'data flip: uint<1>' in l and
        'readLatency = 0' in l and 'writeLatency = 1' in l for l in memories)
    assert helper.count(' = firrtl.regreset %clock, %reset, ') == 8
    nets, _ = expressions(text, HELPER)
    cases = 0
    # Exhaust every valid bank pointer pair, maybe_full, selector and handshake.
    # The other bank covers empty/full as well as the tested nonempty bank.
    for bank, ep, dp, flags in itertools.product(('qA', 'qB'), range(10), range(10), itertools.product((False, True), repeat=7)):
        mf, enq, deq, av, bv, ready, other_full = flags
        v = dict(enqPointer=enq, deqPointer=deq, enqA_valid=av, enqB_valid=bv, deq_ready=ready)
        for n in ('qA', 'qB'):
            v.update({n + '_enq_ptr_value': ep if n == bank else 0,
                      n + '_deq_ptr_value': dp if n == bank else 0,
                      n + '_maybe_full': mf if n == bank else other_full,
                      n + '_ram_read.data': n == 'qA'})
        swap = enq ^ (not av)
        er = {n: not (v[n + '_enq_ptr_value'] == v[n + '_deq_ptr_value'] and v[n + '_maybe_full']) for n in ('qA', 'qB')}
        dv = {n: not (v[n + '_enq_ptr_value'] == v[n + '_deq_ptr_value'] and not v[n + '_maybe_full']) for n in ('qA', 'qB')}
        checks = dict(enqA_ready=er['qB' if swap else 'qA'], enqB_ready=er['qA' if swap else 'qB'],
                      deq_valid=dv['qB' if deq else 'qA'], deq_bits=not deq)
        for n in ('qA', 'qB'):
            valid = (bv if swap else av) if n == 'qA' else (av if swap else bv)
            push, pop = er[n] and valid, dv[n] and ready and (deq == (n == 'qB'))
            en, dn = v[n + '_enq_ptr_value'], v[n + '_deq_ptr_value']
            checks.update({n + '_enq_ptr_value': (en + 1) % 10 if push else en,
                n + '_deq_ptr_value': (dn + 1) % 10 if pop else dn,
                n + '_maybe_full': push if push != pop else v[n + '_maybe_full'],
                n + '_ram_write.en': push, n + '_ram_write.data': (not swap) if n == 'qA' else swap,
                n + '_ram_write.mask': True, n + '_ram_write.addr': en,
                n + '_ram_read.en': True, n + '_ram_read.addr': dn})
        checks['enqPointer'] = not enq if (av and checks['enqA_ready']) != (bv and checks['enqB_ready']) else enq
        checks['deqPointer'] = not deq if ready and checks['deq_valid'] else deq
        for n, expected_value in checks.items(): assert evaluate(nets[n], v, nets) == expected_value, (n, v)
        cases += 1
    rtl = clean((rtl_path or after.with_suffix('.sv')).read_text())
    head, body = module(rtl, HELPER)
    assert 'enqA_ready' in head and 'deq_bits' in head
    for name in ('enqPointer', 'deqPointer', 'qA_maybe_full', 'qB_maybe_full'):
        assert re.search(r'if \(reset\).*?' + name + r" <= 1'h0;", body, re.S), name
    for bank in ('qA', 'qB'):
        instance = re.search(r'\b(\w+)\s+' + bank + r'_ram_ext\s*\(', body)
        assert instance, 'emitted bank RAM missing ' + bank
        ram_head, ram_body = module(rtl, instance[1])
        assert re.search(r'reg\s+Memory\[0:9\]', ram_body)
        assert re.search(r'input\s+\[3:0\] R0_addr', ram_head)
        assert re.search(r'input\s+\[3:0\] W0_addr', ram_head)
        assert 'always @(posedge W0_clk)' in ram_body and "if (W0_en & 1'h1)" in ram_body
        assert "assign R0_data = R0_en ? Memory[R0_addr] : 1'bx;" in ram_body
        assert 'reset' not in ram_head and 'reset' not in ram_body
    return dict(before=str(before), after=str(after), unchanged_modules=len(previous), wrapper_connections=len(expected),
                annotation_rows=len(updated), order_cases=cases, banks=2, bank_depth=10,
                helper_equal_to_shared_native=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    oracle(a.sfc)
    report = dict(golden=str(a.sfc.resolve()), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC transaction-order bank geometry/routing/reset, selected events/RTL, retained modules/annotations')


if __name__ == '__main__': main()
