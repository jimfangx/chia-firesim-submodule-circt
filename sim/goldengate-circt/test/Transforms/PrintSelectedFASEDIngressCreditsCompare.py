#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected AW/W credits with immutable SFC IngressModule/counters.

The SFC fixture disables Print. This checks shared completion, retirement and
qualified reset semantics; transaction-order and issue gates remain external.
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

OLD = 'GGSelectedPrintFASEDIngressARQueueWrapper'
TOP = 'GGSelectedPrintFASEDIngressCreditsWrapper'
HELPER = 'GGFASEDIngressCredits'


def oracle(path):
    text = clean(path.read_text())
    ingress = equations(module(text, 'IngressModule')[1])
    expected = {
        'SatUpDownCounter_io_inc': 'awQueue_io_enq_ready & awQueue_io_enq_valid',
        'SatUpDownCounter_1_io_inc': '_T_3 & wQueue_io_enq_bits_last',
        '_T_3': 'wQueue_io_enq_ready & wQueue_io_enq_valid',
        '_T_1': 'wQueue_io_deq_ready & wQueue_io_deq_valid',
        '_T_5': 'awQueue_io_deq_ready & awQueue_io_deq_valid',
        'SatUpDownCounter_io_dec': '~io_relaxed ? write_req_done : _T_1 & wQueue_io_deq_bits_last',
        'SatUpDownCounter_1_io_dec': '~io_relaxed ? write_req_done : _T_5',
        '_write_req_done_T_3': 'SatUpDownCounter_1_io_value < SatUpDownCounter_io_value & SatUpDownCounter_1_io_inc',
        '_write_req_done_T_4': 'SatUpDownCounter_1_io_value > SatUpDownCounter_io_value & SatUpDownCounter_io_inc | _write_req_done_T_3',
        '_write_req_done_T_5': 'SatUpDownCounter_1_io_inc & SatUpDownCounter_io_inc',
        'write_req_done': '_write_req_done_T_4 | _write_req_done_T_5',
    }
    for n, v in expected.items(): assert ' '.join(ingress[n].split()) == v, (n, ingress[n])
    for n in ('SatUpDownCounter', 'SatUpDownCounter_1'):
        assert ingress[n + '_io_max'] == "4'ha"
        assert ingress[n + '_clock'] == 'clock' and ingress[n + '_reset'] == 'reset'
    body = module(text, 'IngressModule')[1]
    for n in ('SatUpDownCounter', 'SatUpDownCounter_1'):
        assert re.search(r'\bSatUpDownCounter_1\s+' + n + r'\s*\(', body)
    counter = module(text, 'SatUpDownCounter_1')[1]
    for expr in ('reg [3:0] value;', 'value + 4\'h1', 'value - 4\'h1', 'value >= io_max',
                 "value == 4'h0", 'if (reset)', "value <= 4'h0;",
                 'else if (io_inc & ~io_dec & ~io_full)', 'else if (~io_inc & io_dec & ~io_empty)'):
        assert expr in counter, expr
    model = equations(module(text, 'FASEDMemoryTimingModel')[1])
    assert model['ingress_reset'] == 'reset | hPort_hBits_reset & _ingress_io_nastiInputs_hValid_T_3'


def compare(before, after, baseline):
    old_text, text = before.read_text(), after.read_text()
    def bodies(t):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', t, re.M | re.S)}
    old, new = bodies(old_text), bodies(text)
    assert set(new) - set(old) == {TOP, HELPER}
    for n, body in old.items(): assert new[n] == body, 'prior module changed ' + n
    assert new[HELPER] == native_module(baseline, HELPER), 'shared credit operations changed'
    ports = native_ports(old[OLD])
    helper_ports = native_ports(new[HELPER])
    assert native_ports(new[TOP]) == ports + [('in', 'fased_ingress_relaxed', '!firrtl.uint<1>'),
                                            ('out', 'fased_ingress_credits', helper_ports[-1][2])]
    expected = {'sim.' + n if d == 'in' else 'top.' + n:
                'top.' + n if d == 'in' else 'sim.' + n for d, n, _ in ports}
    expected.update({
        'ingressCredits.clock': 'top.hostClock',
        'ingressCredits.reset': 'sim.fased_ingress_reset',
        'ingressCredits.relaxed': 'top.fased_ingress_relaxed',
        'ingressCredits.awEnqFire': 'sim.fased_ingress_aw_enq_fire',
        'ingressCredits.wLastEnqFire': 'sim.fased_ingress_w_last_fire',
        'ingressCredits.awDeqFire': frozenset({'top.fased_ingress_aw_deq.valid', 'top.fased_ingress_aw_deq.ready'}),
        'ingressCredits.wLastDeqFire': frozenset({'top.fased_ingress_w_deq.valid', 'top.fased_ingress_w_deq.ready', 'top.fased_ingress_w_deq.bits.last'}),
        'top.fased_ingress_credits': 'ingressCredits.status'})
    assert wiring(text, TOP, aggregate=True) == expected, 'copied/event/reset wiring differs'
    prior = json.loads(before.with_name(before.stem + '-all.json').read_text())
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    assert updated == transfer(prior, OLD, TOP, {n for _, n, _ in ports}), 'archive transfer differs'
    nets, _ = expressions(text, HELPER)
    cases = 0
    for aw, w, bits in itertools.product(range(16), range(16), itertools.product((False, True), repeat=5)):
        v = dict(zip(('relaxed', 'awEnqFire', 'wLastEnqFire', 'awDeqFire', 'wLastDeqFire'), bits))
        v.update(awCredits=aw, wCredits=w)
        done = aw > w and v['awEnqFire'] or aw < w and v['wLastEnqFire'] or v['awEnqFire'] and v['wLastEnqFire']
        assert evaluate(nets['status.writeReqDone'], v, nets) == done
        for name, count, inc, dec in [('awCredits', aw, v['wLastEnqFire'], v['awDeqFire']),
                                       ('wCredits', w, v['awEnqFire'], v['wLastDeqFire'])]:
            dec = dec if v['relaxed'] else done
            expected_count = count + 1 if inc and not dec and count < 10 else count - 1 if dec and not inc and count else count
            assert evaluate(nets[name], v, nets) == expected_count
        assert evaluate(nets['status.awEmpty'], v, nets) == (aw == 0)
        assert evaluate(nets['status.wEmpty'], v, nets) == (w == 0)
        cases += 1
    # Actual reset operands, not a counter-level behavioral assumption.
    helper = new[HELPER]
    for name in ('awCredits', 'wCredits'):
        assert re.search(r'%' + name + r' = firrtl.regreset %clock, %reset, %\w+ : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<4>, !firrtl.uint<4>', helper)
    rtl = clean(after.with_suffix('.sv').read_text())
    head, body = module(rtl, HELPER)
    for name in ('awCredits', 'wCredits'):
        assert re.search(r'reg\s+\[3:0\]\s+' + name + r'\b', body), name
        assert re.search(r'if \(reset\).*?' + name + r" <= 4'h0;", body, re.S)
    for field in ('awValue', 'wValue', 'awEmpty', 'wEmpty', 'writeReqDone'):
        assert 'status_' + field in head
    return dict(before=str(before), after=str(after), unchanged_modules=len(old),
                wrapper_connections=len(expected), annotation_rows=len(updated), credit_cases=cases,
                helper_equal_to_shared_native=True, emitted_counter_width=4, maximum=10)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    oracle(a.sfc)
    report = dict(golden=str(a.sfc.resolve()), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AW/W credits/completion/reset, native selected event wiring/RTL, retained modules/annotations')


if __name__ == '__main__': main()
