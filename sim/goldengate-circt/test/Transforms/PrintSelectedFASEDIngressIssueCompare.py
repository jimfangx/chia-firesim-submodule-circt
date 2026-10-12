#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print host issue with immutable SFC IngressModule.

The reference disables Print. Check shared strict/relaxed issue semantics,
actual selected feedback, payload identity and collateral retention; selected
assertions, timing, responses and full simulator composition remain pending.
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

OLD = 'GGSelectedPrintFASEDIngressOrderWrapper'
TOP = 'GGSelectedPrintFASEDIngressIssueWrapper'
HELPER = 'GGFASEDIngressIssue'
RESPONSE = ('rReady', 'rValid', 'rLast', 'bReady', 'bValid')
REQUEST = {'arReady': 'ar.ready', 'arValid': 'ar.valid',
           'awReady': 'aw.ready', 'awValid': 'aw.valid'}
CONSUMED = {'fased_ingress_order', 'fased_ingress_aw_deq', 'fased_ingress_w_deq',
            'fased_ingress_ar_deq', 'fased_host_transactions'}


def oracle(path):
    body = module(clean(path.read_text()), 'IngressModule')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(body).items()}
    expected = {
        'do_hread': 'io_relaxed | _do_hread_T_2',
        '_do_hread_T_2': '(io_host_mem_idle | io_host_read_inflight) & xaction_order_io_deq_valid & xaction_order_io_deq_bits',
        'do_hwrite': 'io_relaxed ? ~SatUpDownCounter_1_io_empty : io_host_mem_idle & xaction_order_io_deq_valid & ~ xaction_order_io_deq_bits',
        'do_hwrite_data': 'io_relaxed ? ~SatUpDownCounter_io_empty : do_hwrite_data_reg',
        '_GEN_2': "_T_8 & io_nastiOutputs_w_bits_last ? 1'h0 : do_hwrite_data_reg",
        '_GEN_3': '_xaction_order_io_deq_ready_T_1 | _GEN_2',
        '_T_8': 'io_nastiOutputs_w_ready & io_nastiOutputs_w_valid',
        '_xaction_order_io_deq_ready_T': 'io_nastiOutputs_ar_ready & io_nastiOutputs_ar_valid',
        '_xaction_order_io_deq_ready_T_1': 'io_nastiOutputs_aw_ready & io_nastiOutputs_aw_valid',
        'xaction_order_io_deq_ready': '_xaction_order_io_deq_ready_T | _xaction_order_io_deq_ready_T_1',
    }
    for ch, gate in [('ar', 'do_hread'), ('aw', 'do_hwrite'), ('w', 'do_hwrite_data')]:
        expected['io_nastiOutputs_' + ch + '_valid'] = gate + ' & ' + ch + 'Queue_io_deq_valid'
        expected[ch + 'Queue_io_deq_ready'] = gate + ' & io_nastiOutputs_' + ch + '_ready'
    for n, v in expected.items(): assert eq[n] == v, (n, eq[n])
    assert re.search(r"if \(reset\) begin\s+do_hwrite_data_reg <= 1'h0;\s+end else begin\s+do_hwrite_data_reg <= _GEN_3;", body)


def issue_cases(text):
    nets, assertions = expressions(text, HELPER)
    assert not assertions
    helper = native_module(text, HELPER)
    assert helper.count(' = firrtl.regreset %clock, %reset, ') == 1
    names = ['relaxed', 'hostMemIdle', 'hostReadInflight', 'awEmpty', 'wEmpty',
             'order.valid', 'order.bits', 'aw.valid', 'requests.aw.ready',
             'w.valid', 'requests.w.ready', 'w.bits.last', 'ar.valid',
             'requests.ar.ready', 'do_hwrite_data_reg']
    cases, collisions = 0, 0
    for bits in itertools.product((False, True), repeat=len(names)):
        v = dict(zip(names, bits))
        gates = {'ar': v['relaxed'] or (v['hostMemIdle'] or v['hostReadInflight']) and v['order.valid'] and v['order.bits'],
                 'aw': not v['awEmpty'] if v['relaxed'] else v['hostMemIdle'] and v['order.valid'] and not v['order.bits'],
                 'w': not v['wEmpty'] if v['relaxed'] else v['do_hwrite_data_reg']}
        fire = {}
        for ch, gate in gates.items():
            assert evaluate(nets[f'requests.{ch}.valid'], v, nets) == (gate and v[ch + '.valid'])
            assert evaluate(nets[ch + '.ready'], v, nets) == (gate and v[f'requests.{ch}.ready'])
            assert nets[f'requests.{ch}.bits'] == ('leaf', ch + '.bits')
            fire[ch] = gate and v[ch + '.valid'] and v[f'requests.{ch}.ready']
        assert evaluate(nets['order.ready'], v, nets) == (fire['aw'] or fire['ar'])
        nxt = True if fire['aw'] else False if fire['w'] and v['w.bits.last'] else v['do_hwrite_data_reg']
        assert evaluate(nets['do_hwrite_data_reg'], v, nets) == nxt
        collisions += fire['aw'] and fire['w'] and v['w.bits.last']
        cases += 1
    assert collisions
    return cases, collisions


def transaction_transfer(value):
    if isinstance(value, str):
        prefix = '~' + TOP + '|' + OLD + '>fased_host_transactions.'
        if value.startswith(prefix):
            leaf = value[len(prefix):]
            if leaf in RESPONSE: return '~' + TOP + '|' + TOP + '>fased_host_responses.' + leaf
            if leaf in REQUEST: return '~' + TOP + '|' + TOP + '>fased_host_requests.' + REQUEST[leaf]
        return value
    if isinstance(value, list): return [transaction_transfer(x) for x in value]
    if isinstance(value, dict): return {k: transaction_transfer(x) for k, x in value.items()}
    return value


def compare(before, after, baseline):
    original, text = before.read_text(), after.read_text()
    def bodies(t):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', t, re.M | re.S)}
    old, new = bodies(original), bodies(text)
    assert set(new) - set(old) == {TOP, HELPER}
    for n, body in old.items(): assert new[n] == body, 'prior module changed ' + n
    assert new[HELPER] == native_module(baseline, HELPER), 'shared issue operations changed'
    ports = native_ports(old[OLD]); copied = [p for p in ports if p[1] not in CONSUMED]
    helper_ports = {n: t for _, n, t in native_ports(new[HELPER])}
    assert native_ports(new[TOP]) == copied + [
        ('out', 'fased_host_requests', helper_ports['requests']),
        ('in', 'fased_host_responses', '!firrtl.bundle<rReady: uint<1>, rValid: uint<1>, rLast: uint<1>, bReady: uint<1>, bValid: uint<1>>')]
    expected = {'sim.' + n if d == 'in' else 'top.' + n:
                'top.' + n if d == 'in' else 'sim.' + n for d, n, _ in copied}
    expected.update({'ingressIssue.clock': 'top.hostClock', 'ingressIssue.reset': 'sim.fased_ingress_reset',
        'ingressIssue.relaxed': 'top.fased_ingress_relaxed', 'ingressIssue.hostMemIdle': 'sim.fased_host_mem_idle',
        'ingressIssue.hostReadInflight': 'sim.fased_host_read_inflight',
        'ingressIssue.order': 'sim.fased_ingress_order', 'top.fased_host_requests': 'ingressIssue.requests'})
    for ch in ('aw', 'w', 'ar'): expected['ingressIssue.' + ch] = 'sim.fased_ingress_' + ch + '_deq'
    for n in ('awEmpty', 'wEmpty'): expected['ingressIssue.' + n] = 'sim.fased_ingress_credits.' + n
    for n in RESPONSE: expected['sim.fased_host_transactions.' + n] = 'top.fased_host_responses.' + n
    for n, leaf in REQUEST.items(): expected['sim.fased_host_transactions.' + n] = 'top.fased_host_requests.' + leaf
    assert wiring(text, TOP, aggregate=True) == expected, 'selected feedback/reset/request wiring differs'
    prior = json.loads(before.with_name(before.stem + '-all.json').read_text())
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    assert updated == transaction_transfer(transfer(prior, OLD, TOP, {n for _, n, _ in copied})), 'annotation identity differs'
    cases, collisions = issue_cases(text)
    rtl = clean(after.with_suffix('.sv').read_text())
    header, body = module(rtl, HELPER)
    assert 'always @(posedge clock)' in body and 'do_hwrite_data_reg' in body
    assert re.search(r"if \(reset\).*?do_hwrite_data_reg <= 1'h0;", body, re.S), 'emitted reset differs'
    for ch in ('aw', 'w', 'ar'):
        for leaf in ('valid', 'ready'): assert f'requests_{ch}_{leaf}' in header
    return dict(before=str(before), after=str(after), unchanged_modules=len(old), wrapper_connections=len(expected),
                annotation_rows=len(updated), issue_cases=cases, aw_final_w_collisions=collisions,
                consumed_boundaries=sorted(CONSUMED), helper_equal_to_shared_native=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    oracle(a.sfc)
    report = dict(golden=str(a.sfc.resolve()), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC host issue gates/retirement/reset/priority, selected feedback/payload/RTL, retained modules/annotations')


if __name__ == '__main__': main()
