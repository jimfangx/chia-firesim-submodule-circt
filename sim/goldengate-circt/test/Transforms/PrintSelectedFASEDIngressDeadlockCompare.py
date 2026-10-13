#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print ingress assertions against immutable SFC IngressModule.

The reference disables Print. Compare shared AW/W deadlock semantics, actual
queue signals, qualified reset/clock and policy forwarding; full selected timing,
response scheduling and simulator composition remain pending.
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
from PrintSelectedFASEDIngressIssueCompare import issue_cases

CHAIN = ['GGSelectedPrintFASEDIngress' + n + 'Wrapper' for n in
         ('Issue', 'Order', 'Credits', 'ARQueue', 'WQueue', 'AW') ] + ['GGFASEDIngressAW']
HELPER = 'GGFASEDIngressDeadlock'
CONTEXT = 'fased_ingress_deadlock_context'
POLICY = ('relaxed', 'awEmpty', 'wEmpty', 'orderValid')
MESSAGES = ['DEADLOCK: Timing model requests w enqueue, but wQueue is full and cannot drain',
            'DEADLOCK: Timing model requests aw enqueue, but is awQueue is full and cannot drain']


def oracle(path):
    body = module(clean(path.read_text()), 'IngressModule')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(body).items()}
    expected = {'_T_12': '~xaction_order_io_deq_valid',
                '_T_13': 'io_relaxed ? SatUpDownCounter_io_empty : ~xaction_order_io_deq_valid',
                '_T_15': '~(wQueue_io_enq_valid & ~wQueue_io_enq_ready & _T_13)',
                '_T_22': 'io_relaxed ? SatUpDownCounter_1_io_empty : _T_12',
                '_T_24': '~(awQueue_io_enq_valid & ~awQueue_io_enq_ready & _T_22)',
                '_T_17': '~reset'}
    for n, v in expected.items(): assert eq[n] == v, (n, eq[n])
    for message in MESSAGES: assert message in body
    assert 'if (~reset & ~_T_15)' in body and 'if (_T_17 & ~_T_24)' in body
    assert re.search(r'if \(~reset\) begin\s+assert\(_T_15\);', body)
    assert re.search(r'if \(_T_17\) begin\s+assert\(_T_24\);', body)


def predicates(text):
    nets, assertions = expressions(text, HELPER)
    assert len(assertions) == 2
    assert [a[2] for a in assertions] == MESSAGES
    assert native_module(text, HELPER).count('firrtl.assert %clock, ') == 2
    names = ['reset', 'relaxed', 'awEmpty', 'wEmpty', 'orderValid',
             'awValid', 'awReady', 'wValid', 'wReady']
    failures = [0, 0]
    for bits in itertools.product((False, True), repeat=9):
        v = dict(zip(names, bits))
        for i, ch in enumerate(('w', 'aw')):
            expected = not v['reset'] and v[ch + 'Valid'] and not v[ch + 'Ready'] and (
                v[ch + 'Empty'] if v['relaxed'] else not v['orderValid'])
            predicate, enabled, _ = assertions[i]
            observed = evaluate(enabled, v, nets) and not evaluate(predicate, v, nets)
            assert observed == expected, (ch, v)
            failures[i] += observed
    assert failures == [32, 32]
    return 512


def compare(before, after, baseline):
    original, text = before.read_text(), after.read_text()
    def bodies(t):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', t, re.M | re.S)}
    old, new = bodies(original), bodies(text)
    assert set(new) - set(old) == {HELPER}
    assert new[HELPER] == native_module(baseline, HELPER), 'shared assertions changed'
    unchanged = 0
    for n, body in old.items():
        if n not in CHAIN:
            assert new[n] == body, 'unrelated module changed ' + n
            unchanged += 1
    appended = ('in', CONTEXT, '!firrtl.bundle<relaxed: uint<1>, awEmpty: uint<1>, wEmpty: uint<1>, orderValid: uint<1>>')
    connections = 0
    for j, n in enumerate(CHAIN):
        assert native_ports(new[n]) == native_ports(old[n]) + ([appended] if j else [])
        expected = wiring(original, n, aggregate=True)
        if j == 0:
            expected.update({'sim.' + CONTEXT + '.' + leaf: src for leaf, src in zip(POLICY,
                ('top.fased_ingress_relaxed', 'top.fased_ingress_credits.awEmpty',
                 'top.fased_ingress_credits.wEmpty', 'sim.fased_ingress_order.valid'))})
        elif j < 6:
            child = 'ingressAW' if j == 5 else 'sim'
            expected[child + '.' + CONTEXT] = 'top.' + CONTEXT
        else:
            expected.update({'ingressDeadlock.' + leaf: 'top.' + CONTEXT + '.' + leaf for leaf in POLICY})
            expected.update({'ingressDeadlock.clock': 'top.clock', 'ingressDeadlock.reset': 'top.reset',
                'ingressDeadlock.awValid': 'awQueue.enq.valid', 'ingressDeadlock.awReady': 'awQueue.enq.ready',
                'ingressDeadlock.wValid': 'top.w_enq.valid', 'ingressDeadlock.wReady': 'top.w_enq.ready'})
        assert wiring(text, n, aggregate=True) == expected, 'policy or prior wiring differs ' + n
        connections += len(expected)
    gates = wiring(text, CHAIN[5], aggregate=True)
    assert gates['ingressAW.clock'] == 'top.hostClock'
    assert gates['ingressAW.reset'] == 'sim.fased_ingress_reset', 'assertions need qualified ingress reset'
    prior = json.loads(before.with_name(before.stem + '-all.json').read_text())
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    assert prior == updated, 'annotation archive changed'
    cases = predicates(text)
    issue_count, collisions = issue_cases(text)
    rtl = clean(after.with_suffix('.sv').read_text())
    header, body = module(rtl, HELPER)
    assert 'always @(posedge clock)' in body
    for message in MESSAGES: assert message in body
    guarded = re.search(r'if \(~reset\) begin(.*?)^    end', body, re.M | re.S)
    assert guarded, 'emitted assertions lost their reset guard'
    for ch in ('w', 'aw'):
        policy = re.search(r'automatic logic (\w+) = relaxed \? ' + ch + r'Empty : ~orderValid;', guarded[1])
        assert policy, 'emitted strict/relaxed policy differs'
        assert f'assert(~({ch}Valid & ~{ch}Ready & {policy[1]}))' in guarded[1], 'emitted enqueue predicate differs'
    return dict(before=str(before), after=str(after), unchanged_modules=unchanged,
                propagated_internal_ports=6, chain_connections=connections, annotation_rows=len(updated),
                deadlock_cases=cases, failures_per_channel=32, retained_issue_cases=issue_count,
                aw_final_w_collisions=collisions, shared_helper_equal=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    oracle(a.sfc)
    report = dict(golden=str(a.sfc.resolve()), candidates=[compare(x, y, a.baseline.read_text())
                  for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AW/W predicates, qualified host reset/clock, actual enqueue and six policy inputs; retained issue/annotations')


if __name__ == '__main__': main()
