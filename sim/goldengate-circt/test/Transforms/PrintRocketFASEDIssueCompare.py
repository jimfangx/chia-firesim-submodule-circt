#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket issue wiring with immutable SFC IngressModule.

Inspect actual FIRRTL SSA, including the assertion context added to existing
queue modules. SFC lacks Print; compare FASED semantics and shared allocation.
"""
import itertools
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring


def expressions(text, name):
    lines = native_module(text, name).splitlines()
    values = {v: ('leaf', v[1:]) for v in re.findall(r'%\w+(?=:)', lines[0])}
    nets, assertions = {}, []
    for line in lines[1:]:
        if m := re.search(r'(%\w+) = firrtl.constant (\d+)', line):
            values[m[1]] = ('const', int(m[2]))
        if m := re.search(r'(%\w+) = firrtl.regreset ', line):
            values[m[1]] = ('leaf', m[1][1:])
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            values[m[1]] = ('leaf', values[m[2]][1] + '.' + m[3])
        if m := re.search(r'(%\w+) = firrtl.(and|or|xor|lt|geq|eq|add|sub) (%\w+), (%\w+)', line):
            values[m[1]] = (m[2], values[m[3]], values[m[4]])
        if m := re.search(r'(%\w+) = firrtl.pad (%\w+), (\d+)', line):
            values[m[1]] = values[m[2]]  # UInt padding is zero extension.
        if m := re.search(r'(%\w+) = firrtl.not (%\w+)', line):
            values[m[1]] = ('not', values[m[2]])
        if m := re.search(r'(%\w+) = firrtl.mux\((%\w+), (%\w+), (%\w+)\)', line):
            values[m[1]] = ('mux', *(values[m[j]] for j in (2, 3, 4)))
        if m := re.search(r'(%\w+) = firrtl.bits (%\w+) (\d+) to (\d+)', line):
            values[m[1]] = ('bits', values[m[2]], int(m[3]), int(m[4]))
        if m := re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)', line):
            nets[values[m[1]][1]] = values[m[2]]
        if m := re.search(r'firrtl.assert (%\w+), (%\w+), (%\w+), "([^"]+)"', line):
            assertions.append((values[m[2]], values[m[3]], m[4]))
    return nets, assertions


def evaluate(tree, inputs, nets):
    op = tree[0]
    if op == 'const': return tree[1]
    if op == 'leaf':
        name = tree[1]
        return inputs[name] if name in inputs else evaluate(nets[name], inputs, nets)
    ev = lambda t: evaluate(t, inputs, nets)
    if op == 'not': return not ev(tree[1])
    if op == 'mux': return ev(tree[2] if ev(tree[1]) else tree[3])
    if op == 'bits': return (ev(tree[1]) >> tree[3]) & ((1 << (tree[2] - tree[3] + 1)) - 1)
    a, b = ev(tree[1]), ev(tree[2])
    if op == 'and': return a and b
    if op == 'or': return a or b
    if op == 'xor': return a ^ b
    if op == 'lt': return a < b
    if op == 'geq': return a >= b
    if op == 'eq': return a == b
    if op == 'add': return a + b
    if op == 'sub': return a - b
    raise AssertionError('unsupported native expression: ' + op)


def main():
    evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    eq = {k: ' '.join(v.split()) for k, v in equations(module(reference, 'IngressModule')[1]).items()}
    # Pin the oracle equations used by the exhaustive checks below.
    expected = {
        'do_hread': 'io_relaxed | _do_hread_T_2',
        'SatUpDownCounter_io_max': "4'ha",
        'SatUpDownCounter_1_io_max': "4'ha",
        '_GEN_2': "_T_8 & io_nastiOutputs_w_bits_last ? 1'h0 : do_hwrite_data_reg",
        '_GEN_3': '_xaction_order_io_deq_ready_T_1 | _GEN_2',

        '_do_hread_T_2': '(io_host_mem_idle | io_host_read_inflight) & xaction_order_io_deq_valid & xaction_order_io_deq_bits',
        'do_hwrite': 'io_relaxed ? ~SatUpDownCounter_1_io_empty : io_host_mem_idle & xaction_order_io_deq_valid & ~ xaction_order_io_deq_bits',
        'do_hwrite_data': 'io_relaxed ? ~SatUpDownCounter_io_empty : do_hwrite_data_reg',
        'SatUpDownCounter_io_inc': 'awQueue_io_enq_ready & awQueue_io_enq_valid',
        'SatUpDownCounter_1_io_inc': '_T_3 & wQueue_io_enq_bits_last',
        'SatUpDownCounter_io_dec': '~io_relaxed ? write_req_done : _T_1 & wQueue_io_deq_bits_last',
        'SatUpDownCounter_1_io_dec': '~io_relaxed ? write_req_done : _T_5',
        '_write_req_done_T_3': 'SatUpDownCounter_1_io_value < SatUpDownCounter_io_value & SatUpDownCounter_1_io_inc',
        '_write_req_done_T_4': 'SatUpDownCounter_1_io_value > SatUpDownCounter_io_value & SatUpDownCounter_io_inc | _write_req_done_T_3',
        '_write_req_done_T_5': 'SatUpDownCounter_1_io_inc & SatUpDownCounter_io_inc',
        'write_req_done': '_write_req_done_T_4 | _write_req_done_T_5',
        '_T_13': 'io_relaxed ? SatUpDownCounter_io_empty : ~xaction_order_io_deq_valid',
        '_T_15': '~(wQueue_io_enq_valid & ~wQueue_io_enq_ready & _T_13)',
        '_T_22': 'io_relaxed ? SatUpDownCounter_1_io_empty : _T_12',
        '_T_24': '~(awQueue_io_enq_valid & ~awQueue_io_enq_ready & _T_22)',
    }
    for name, expr in expected.items(): assert eq[name] == expr, (name, eq[name])
    for ch, enable in [('aw', 'do_hwrite'), ('w', 'do_hwrite_data'), ('ar', 'do_hread')]:
        assert eq[f'io_nastiOutputs_{ch}_valid'] == f'{enable} & {ch}Queue_io_deq_valid'
        assert eq[f'{ch}Queue_io_deq_ready'] == f'{enable} & io_nastiOutputs_{ch}_ready'
    dual = equations(module(reference, 'DualQueue')[1])
    assert dual['qA_io_enq_valid'] == 'enqPointer ^ ~io_enqA_valid ? io_enqB_valid : io_enqA_valid'
    assert dual['qB_io_enq_valid'] == 'enqPointer ^ ~io_enqA_valid ? io_enqA_valid : io_enqB_valid'
    dual_body = module(reference, 'DualQueue')[1]
    for queue in ('qA', 'qB'):
        q = re.search(r'\b(Queue_\d+)\s+' + queue + r'\s*\(', dual_body)[1]
        assert re.search(r'reg\s+ram\s*\[0:9\]', module(reference, q)[1])
    baseline = (evidence / 'candidate/post-fame-fased-ingress-deadlock.mlir').read_text()
    symbols = ['GGFASEDIngressCredits', 'GGFASEDIngressOrder20', 'GGFASEDIngressIssue', 'GGFASEDIngressDeadlock']
    reports = []
    for suffix in ('rocket-fased-issue', 'rocket-fased-issue-reverse'):
        text = (evidence / f'binding.mlir.{suffix}.mlir').read_text()
        before_suffix = suffix.replace('fased-issue', 'fased-ingress')
        before = (evidence / f'binding.mlir.{before_suffix}.mlir').read_text()
        for key in ('globalName', 'widgetClass'):
            pattern = key + r' = "([^"]+)"'
            assert sorted(re.findall(pattern, text)) == sorted(re.findall(pattern, before)), key + ' retention differs'
        banks = ['GGFASEDLatencyRegisters', 'GGFASEDRequestLimits', 'GGFASEDHistograms',
                 'GGFASEDStatistics', 'GGFASEDFunctionalModelRegister', 'GGFASEDResponseErrors', 'GGControlAddressDecode']
        for symbol in banks: assert native_module(text, symbol) == native_module(before, symbol), symbol + ' changed'
        del before
        for symbol in symbols: assert native_module(text, symbol) == native_module(baseline, symbol)
        order = native_module(text, 'GGFASEDIngressOrder20')
        memories = [line for line in order.splitlines() if ' = firrtl.mem ' in line]
        assert len(memories) == 2 and all('depth = 10 : i64' in line and
                                        'data flip: uint<1>' in line for line in memories)
        credits, _ = expressions(text, 'GGFASEDIngressCredits')
        credit_names = ['relaxed', 'awEnqFire', 'wLastEnqFire', 'awDeqFire', 'wLastDeqFire']
        for aw, w in itertools.product(range(11), repeat=2):
            for bits in itertools.product((False, True), repeat=5):
                v = dict(zip(credit_names, bits)); v.update(awCredits=aw, wCredits=w)
                done = aw > w and v['awEnqFire'] or aw < w and v['wLastEnqFire'] or v['awEnqFire'] and v['wLastEnqFire']
                assert evaluate(credits['status.writeReqDone'], v, credits) == done
                for name, value, inc, dec in [('awCredits', aw, v['wLastEnqFire'], v['awDeqFire']),
                                              ('wCredits', w, v['awEnqFire'], v['wLastDeqFire'])]:
                    dec = dec if v['relaxed'] else done
                    expected_value = value + 1 if inc and not dec and value < 10 else value - 1 if dec and not inc and value > 0 else value
                    assert evaluate(credits[name], v, credits) == expected_value
                assert evaluate(credits['status.awEmpty'], v, credits) == (aw == 0)
                assert evaluate(credits['status.wEmpty'], v, credits) == (w == 0)
        issue, _ = expressions(text, 'GGFASEDIngressIssue')
        names = ['relaxed', 'hostMemIdle', 'hostReadInflight', 'awEmpty', 'wEmpty', 'order.valid',
                 'order.bits', 'aw.valid', 'requests.aw.ready', 'w.valid', 'requests.w.ready',
                 'w.bits.last', 'ar.valid', 'requests.ar.ready', 'do_hwrite_data_reg']
        for bits in itertools.product((False, True), repeat=len(names)):
            v = dict(zip(names, bits)); relaxed, idle, inflight, awempty, wempty, valid, readbit = bits[:7]
            gates = {'ar': relaxed or (idle or inflight) and valid and readbit,
                     'aw': not awempty if relaxed else idle and valid and not readbit,
                     'w': not wempty if relaxed else v['do_hwrite_data_reg']}
            fire = {}
            for ch, gate in gates.items():
                assert evaluate(issue[f'requests.{ch}.valid'], v, issue) == (gate and v[ch + '.valid'])
                assert evaluate(issue[ch + '.ready'], v, issue) == (gate and v[f'requests.{ch}.ready'])
                assert issue[f'requests.{ch}.bits'] == ('leaf', ch + '.bits')
                fire[ch] = gate and v[ch + '.valid'] and v[f'requests.{ch}.ready']
            assert evaluate(issue['order.ready'], v, issue) == (fire['aw'] or fire['ar'])
            next_data = True if fire['aw'] else False if fire['w'] and v['w.bits.last'] else v['do_hwrite_data_reg']
            assert evaluate(issue['do_hwrite_data_reg'], v, issue) == next_data
        _, checks = expressions(text, 'GGFASEDIngressDeadlock')
        assert len(checks) == 2, 'both ingress deadlock assertions must survive'
        names = ['reset', 'relaxed', 'awEmpty', 'wEmpty', 'orderValid', 'awValid', 'awReady', 'wValid', 'wReady']
        for bits in itertools.product((False, True), repeat=len(names)):
            v = dict(zip(names, bits))
            for (predicate, enabled, message), ch in zip(checks, ('w', 'aw')):
                assert message in module(reference, 'IngressModule')[1]
                blocked = v[ch + 'Valid'] and not v[ch + 'Ready'] and (v[ch + 'Empty'] if v['relaxed'] else not v['orderValid'])
                assert evaluate(predicate, v, {}) == (not blocked)
                assert evaluate(enabled, v, {}) == (not v['reset'])
        nets = wiring(text, 'GGFASEDIngressCreditsWrapper', aggregate=True)
        assert nets['ingressCredits.clock'] == 'top.hostClock' and nets['ingressCredits.reset'] == 'sim.fased_ingress_reset'
        assert nets['ingressCredits.awEnqFire'] == 'sim.fased_ingress_aw_enq_fire'
        assert nets['ingressCredits.wLastEnqFire'] == 'sim.fased_ingress_w_last_fire'
        assert nets['ingressCredits.awDeqFire'] == frozenset({'top.fased_ingress_aw_deq.valid', 'top.fased_ingress_aw_deq.ready'})
        assert nets['ingressCredits.wLastDeqFire'] == frozenset({'top.fased_ingress_w_deq.valid', 'top.fased_ingress_w_deq.ready', 'top.fased_ingress_w_deq.bits.last'})
        nets = wiring(text, 'GGFASEDIngressOrderWrapper', aggregate=True)
        assert nets['xactionOrder.clock'] == 'top.hostClock' and nets['xactionOrder.reset'] == 'sim.fased_ingress_reset'
        assert nets['xactionOrder.enqA_valid'] == 'sim.fased_ingress_ar_enq_fire'
        assert nets['xactionOrder.enqB_valid'] == 'sim.fased_ingress_credits.writeReqDone'
        nets = wiring(text, 'GGFASEDIngressIssueWrapper', aggregate=True)
        assert nets['ingressIssue.clock'] == 'top.hostClock' and nets['ingressIssue.reset'] == 'sim.fased_ingress_reset'
        assert nets['top.fased_host_requests'] == 'ingressIssue.requests'
        for ch in ('aw', 'w', 'ar'): assert nets['ingressIssue.' + ch] == 'sim.fased_ingress_' + ch + '_deq'
        for name in ('rReady', 'rValid', 'rLast', 'bReady', 'bValid'):
            assert nets['sim.fased_host_transactions.' + name] == 'top.fased_host_responses.' + name
        assert nets['sim.fased_ingress_deadlock_context.relaxed'] == 'top.fased_ingress_relaxed'
        for name in ('awEmpty', 'wEmpty'):
            assert nets['sim.fased_ingress_deadlock_context.' + name] == 'top.fased_ingress_credits.' + name
        assert nets['sim.fased_ingress_deadlock_context.orderValid'] == 'sim.fased_ingress_order.valid'
        gates = wiring(text, 'GGFASEDIngressAW', aggregate=True)
        for ch in ('aw', 'w'):
            for leaf in ('Valid', 'Ready'):
                src = 'awQueue.enq.' if ch == 'aw' else 'top.w_enq.'
                assert gates['ingressDeadlock.' + ch + leaf] == src + leaf.lower()
        assert gates['ingressDeadlock.clock'] == 'top.clock'
        assert gates['ingressDeadlock.reset'] == 'top.reset'
        for name in ('relaxed', 'awEmpty', 'wEmpty', 'orderValid'):
            assert gates['ingressDeadlock.' + name] == 'top.fased_ingress_deadlock_context.' + name
        for name in ('GGFASEDIngressOrderWrapper', 'GGFASEDIngressCreditsWrapper',
                     'GGFASEDIngressARQueueWrapper', 'GGFASEDIngressWQueueWrapper', 'GGFASEDIngressAWWrapper'):
            nets = wiring(text, name, aggregate=True)
            instance = 'ingressAW' if name == 'GGFASEDIngressAWWrapper' else 'sim'
            assert nets[instance + '.fased_ingress_deadlock_context'] == 'top.fased_ingress_deadlock_context'
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append({'artifact': suffix, 'credit_cases': 3872, 'issue_cases': 32768, 'deadlock_cases': 512, 'native_helpers': 4})
    report = {'golden': str(golden), 'constructor_orders': reports, 'order_queue_depths': [10, 10],
              'reference_has_print_host': False, 'scope': 'FASED ingress issue and deadlock; timing, egress, MMIO remain external'}
    (evidence / 'rocket-fased-issue-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC ingress credits/order/issue/deadlock equations and actual native issue/assertion wiring in both Print orders')


if __name__ == '__main__': main()
