#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket FASED ingress with immutable SFC U250 RTL.

Checks the partial ingress boundary: timing/MMIO/host issue are still external.
SFC prunes unused payload fields; compare surviving queue leaves explicitly.
"""
import itertools
import json
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module, terms
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring


def boolean_nets(text, name):
    """Resolve native Boolean SSA without depending on SSA numbering."""
    body = native_module(text, name).splitlines()
    values = {v: ('leaf', v[1:]) for v in re.findall(r'%\w+(?=:)', body[0])}
    nets = {}
    for line in body[1:]:
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            dest, src, field = m.groups()
            values[dest] = ('leaf', values[src][1] + '.' + field)
        if m := re.search(r'(%\w+) = firrtl.(and|or) (%\w+), (%\w+)', line):
            dest, op, lhs, rhs = m.groups()
            values[dest] = (op, values[lhs], values[rhs])
        if m := re.search(r'(%\w+) = firrtl.not (%\w+)', line):
            values[m[1]] = ('not', values[m[2]])
        if m := re.search(r'firrtl.strictconnect (%\w+), (%\w+)', line):
            nets[values[m[1]][1]] = values[m[2]]
    return nets


def evaluate(tree, inputs):
    op = tree[0]
    if op == 'leaf': return inputs[tree[1]]
    if op == 'not': return not evaluate(tree[1], inputs)
    if op == 'and': return evaluate(tree[1], inputs) and evaluate(tree[2], inputs)
    assert op == 'or'
    return evaluate(tree[1], inputs) or evaluate(tree[2], inputs)


def main():
    evidence, golden = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    model = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    names = {'hPort_toHost_hValid': 'valid', 'hPort_fromHost_hReady': 'ready',
             'ingress_io_nastiInputs_hReady': 'ingressReady',
             'writeEgress_io_resp_hValid': 'writeValid', 'readEgress_io_resp_hValid': 'readValid',
             'tResetReady': 'resetReady'}
    assert model['tResetReady'] == '~hPort_hBits_reset | host_mem_idle'
    expected_fire = terms(model['hPort_toHost_hReady'], model, names)
    expected_ingress = terms(model['ingress_io_nastiInputs_hValid'], model, names)
    assert expected_fire == set(names.values()) and expected_ingress == expected_fire - {'ingressReady'}
    assert terms(model['hPort_fromHost_hValid'], model, names) == expected_fire
    assert model['ingress_reset'] == 'reset | hPort_hBits_reset & _ingress_io_nastiInputs_hValid_T_3'
    assert model['host_mem_idle'] == 'SatUpDownCounter_io_empty & SatUpDownCounter_1_io_empty'
    assert model['ingress_io_host_read_inflight'] == '~SatUpDownCounter_io_empty'
    counter = module(reference, 'SatUpDownCounter_1')[1]
    assert re.search(r'\breg\s+\[3:0\]\s+value\b', counter)
    for prefix in ('SatUpDownCounter', 'SatUpDownCounter_1'):
        assert model[prefix + '_clock'] == 'clock' and model[prefix + '_reset'] == 'reset'
        assert model[prefix + '_io_max'] == "4'ha"
    assert model['SatUpDownCounter_io_inc'] == 'auto_to_host_dram_out_ar_ready & nastiToHostDRAM_ar_valid'
    assert model['SatUpDownCounter_io_dec'] == 'auto_to_host_dram_out_r_valid & auto_to_host_dram_out_r_bits_last'
    assert model['SatUpDownCounter_1_io_inc'] == 'auto_to_host_dram_out_aw_ready & nastiToHostDRAM_aw_valid'
    assert model['SatUpDownCounter_1_io_dec'] == 'auto_to_host_dram_out_b_valid'
    ref_header = module(reference, 'FASEDMemoryTimingModel')[0]
    assert 'auto_to_host_dram_out_r_ready' not in ref_header and 'auto_to_host_dram_out_b_ready' not in ref_header
    ingress = equations(module(reference, 'IngressModule')[1])
    shapes = []
    for ch, sym, depth, width in [('aw', 'Queue_7', 10, 69), ('w', 'Queue_8', 16, 78), ('ar', 'Queue_9', 4, 69)]:
        required = {'io_nastiInputs_hValid', f'io_nastiInputs_hBits_{ch}_valid'}
        required |= {f'{other}Queue_io_enq_ready' for other in ('aw', 'w', 'ar') if other != ch}
        assert terms(ingress[ch + 'Queue_io_enq_valid'], ingress, {n: n for n in required}) == required
        header, queue = module(reference, sym)
        memories = re.findall(r'\breg\s+(?:\[(\d+):0\]\s+)?ram(?:_\w+)?\s*\[0:(\d+)\]', queue)
        assert memories and all(int(last) + 1 == depth for _, last in memories)
        leaves = {n: int(msb or 0) + 1 for msb, n in re.findall(r'input(?:\s+\[(\d+):0\])?\s+io_enq_bits_(\w+)', header)}
        shapes.append((ch, depth, width, leaves))
    baseline = (evidence / 'candidate/post-fame-fased-ingress-ar.mlir').read_text()
    symbols = ['GGFASEDTokenEngine', 'GGFASEDHostOutstanding', 'GGFASEDIngressAW',
               'GGFASEDIngressAWQueue10', 'GGFASEDIngressWQueue16', 'GGFASEDIngressARQueue4']
    reports = []
    for suffix in ('rocket-fased-ingress', 'rocket-fased-ingress-reverse'):
        text = (evidence / f'binding.mlir.{suffix}.mlir').read_text()
        for symbol in symbols:
            assert native_module(text, symbol) == native_module(baseline, symbol), f'{symbol} operations differ'
        token_wrapper = native_module(text, 'GGFASEDTokenWrapper')
        assert 'firrtl.strictconnect %FASEDMemoryTimingModel_0_clock, %hostClock' in token_wrapper
        assert 'firrtl.strictconnect %FASEDMemoryTimingModel_0_reset, %hostReset' in token_wrapper
        engine = boolean_nets(text, 'GGFASEDTokenEngine')
        inputs = ['hPort.toHost.hValid', 'hPort.fromHost.hReady', 'ingress.hReady',
                  'readiness.writeValid', 'readiness.readValid', 'hPort.hBits.reset', 'readiness.hostMemIdle', 'reset']
        for bits in itertools.product((False, True), repeat=len(inputs)):
            values = dict(zip(inputs, bits))
            valid, ready, ingress_ready, write, read, target_reset, idle, host_reset = bits
            reset_ready = not target_reset or idle
            ingress_valid = valid and ready and write and read and reset_ready
            fire = ingress_valid and ingress_ready
            for out, expected in [('targetFire', fire), ('hPort.toHost.hReady', fire), ('hPort.fromHost.hValid', fire),
                                  ('ingress.hValid', ingress_valid), ('ingressReset', host_reset or target_reset and ingress_valid),
                                  ('egressReset', host_reset or target_reset and fire), ('modelReset', target_reset)]:
                assert evaluate(engine[out], values) == expected, (out, values)
        counter = native_module(text, 'GGFASEDHostOutstanding')
        assert len(re.findall(r'firrtl.regreset .*?!firrtl.uint<4>', counter)) == 2
        nets = wiring(text, 'GGFASEDHostOutstandingWrapper', aggregate=True)
        assert nets['hostOutstanding.clock'] == 'top.hostClock' and nets['hostOutstanding.reset'] == 'top.hostReset'
        assert nets['hostOutstanding.transactions'] == 'top.fased_host_transactions'
        assert nets['sim.fased_readiness.hostMemIdle'] == 'hostOutstanding.hostMemIdle'
        gates = wiring(text, 'GGFASEDIngressAW', aggregate=True)
        assert gates['top.ingress.hReady'] == frozenset({'awQueue.enq.ready', 'top.w_enq.ready', 'top.ar_enq.ready'})
        for ch, depth, width, leaves in shapes:
            sym = f'GGFASEDIngress{ch.upper()}Queue{depth}'
            queue = native_module(text, sym)
            assert f'depth = {depth} : i64' in queue and f'data flip: uint<{width}>' in queue
            assert 'readLatency = 0 : i32, writeLatency = 1 : i32' in queue
            for name, size in leaves.items(): assert f'{name}: uint<{size}>' in queue
            if ch == 'aw':
                assert gates['awQueue.clock'] == 'top.clock' and gates['awQueue.reset'] == 'top.reset'
                nets = wiring(text, 'GGFASEDIngressAWWrapper', aggregate=True)
                assert nets['ingressAW.clock'] == 'top.hostClock' and nets['ingressAW.reset'] == 'sim.fased_ingress_reset'
                assert nets['ingressAW.ingress'] == 'sim.fased_ingress'
                assert nets['top.fased_ingress_aw_deq'] == 'ingressAW.aw_deq'
                dest = 'awQueue.enq.valid'
            else:
                nets = wiring(text, f'GGFASEDIngress{ch.upper()}QueueWrapper', aggregate=True)
                assert nets[ch + 'Queue.clock'] == 'top.hostClock'
                assert nets[ch + 'Queue.reset'] == 'sim.fased_ingress_reset'
                assert nets[ch + 'Queue.enq'] == 'sim.fased_ingress_' + ch + '_enq'
                assert nets['top.fased_ingress_' + ch + '_deq'] == ch + 'Queue.deq'
                dest = 'top.' + ch + '_enq.valid'
            required = {'top.ingress.hValid', f'top.ingress.hBits.{ch}.valid'}
            required |= {'awQueue.enq.ready' if other == 'aw' else f'top.{other}_enq.ready'
                         for other in ('aw', 'w', 'ar') if other != ch}
            assert gates[dest] == frozenset(required)
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append({'artifact': suffix, 'boolean_cases': 256, 'unchanged_native_helpers': len(symbols)})
    report = {'golden': str(golden), 'constructor_orders': reports,
              'queues': [{'channel': ch, 'depth': d, 'native_payload_bits': w, 'SFC_surviving_bits': sum(leaves.values()),
                          'SFC_surviving_fields': leaves} for ch, d, w, leaves in shapes],
              'host_counter_width': 4, 'host_counter_max': 10, 'FASED_slave': 1,
              'reference_has_print_host': False, 'scope': 'token, host outstanding and ingress queues; later timing/MMIO external'}
    (evidence / 'rocket-fased-ingress-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS FASED token/reset equations, host counters, three ingress queues and native SSA wiring in both Print orders')


if __name__ == '__main__': main()
