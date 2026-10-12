#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print ingress with shared SFC queue/capacity/reset semantics.

The golden disables Print. Credit/order/issue, host responses and timing remain
external; this compares the three raw ingress queues, not complete FASED parity.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module, terms
from PrintRocketMasterCompare import native_module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring

OLD = 'GGSelectedPrintFASEDHostOutstandingWrapper'
WRAPPERS = ['GGSelectedPrintFASEDIngressAWWrapper',
            'GGSelectedPrintFASEDIngressWQueueWrapper', 'GGSelectedPrintFASEDIngressARQueueWrapper']
HELPERS = ['GGFASEDIngressAW', 'GGFASEDIngressAWQueue10', 'GGFASEDIngressWQueue16', 'GGFASEDIngressARQueue4']


def transfer(value, old, new, copied):
    if isinstance(value, str):
        if value == '~' + old: return '~' + new
        if not value.startswith('~' + old + '|'): return value
        suffix = value[len(old) + 1:]
        prefix = '|' + old + '>'
        if suffix.startswith(prefix):
            name = re.split(r'[.\[]', suffix[len(prefix):], 1)[0]
            if name in copied: suffix = '|' + new + '>' + suffix[len(prefix):]
        return '~' + new + suffix
    if isinstance(value, list): return [transfer(x, old, new, copied) for x in value]
    if isinstance(value, dict): return {k: transfer(x, old, new, copied) for k, x in value.items()}
    return value


def compare(before, after, baseline, shapes):
    original, text = before.read_text(), after.read_text()
    def bodies(t):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', t, re.M | re.S)}
    old, new = bodies(original), bodies(text)
    assert set(new) - set(old) == set(WRAPPERS + HELPERS)
    for n, body in old.items(): assert new[n] == body, 'changed prior module ' + n
    for n in HELPERS: assert new[n] == native_module(baseline, n), 'changed shared native operations ' + n
    gates = wiring(text, HELPERS[0], aggregate=True)
    assert gates['top.ingress.hReady'] == frozenset({'awQueue.enq.ready', 'top.w_enq.ready', 'top.ar_enq.ready'})
    annotations = json.loads(before.with_name(before.stem + '-all.json').read_text())
    count = 0
    for (ch, depth, width, leaves), previous, top in zip(shapes, [OLD] + WRAPPERS[:-1], WRAPPERS):
        consumed = 'fased_ingress' if ch == 'aw' else 'fased_ingress_' + ch + '_enq'
        copied = [p for p in native_ports(new[previous]) if p[1] != consumed]
        expected = {'sim.' + n if d == 'in' else 'top.' + n:
                    'top.' + n if d == 'in' else 'sim.' + n for d, n, _ in copied}
        inst = 'ingressAW' if ch == 'aw' else ch + 'Queue'
        expected[inst + '.clock'] = 'top.hostClock'
        expected[inst + '.reset'] = 'sim.fased_ingress_reset'
        expected[inst + ('.ingress' if ch == 'aw' else '.enq')] = 'sim.' + consumed
        outputs = [('aw_deq', 'fased_ingress_aw_deq'), ('w_enq', 'fased_ingress_w_enq'),
                   ('ar_enq', 'fased_ingress_ar_enq'), ('aw_enq_fire', 'fased_ingress_aw_enq_fire')] if ch == 'aw' else [
                   ('deq', 'fased_ingress_' + ch + '_deq'),
                   ('enq_last_fire' if ch == 'w' else 'enq_fire', 'fased_ingress_' + ch + ('_last_fire' if ch == 'w' else '_enq_fire'))]
        helper_ports = {n: t for _, n, t in native_ports(new[HELPERS[0] if ch == 'aw' else f'GGFASEDIngress{ch.upper()}Queue{depth}'])}
        for source, dest in outputs: expected['top.' + dest] = inst + '.' + source
        assert wiring(text, top, aggregate=True) == expected, 'copied/queue/reset wiring differs ' + top
        assert native_ports(new[top]) == copied + [('out', dest, helper_ports[source]) for source, dest in outputs]
        annotations = transfer(annotations, previous, top, {p[1] for p in copied})
        count += len(expected)
        queue_name = f'GGFASEDIngress{ch.upper()}Queue{depth}'
        queue = new[queue_name]
        assert f'depth = {depth} : i64' in queue and f'data flip: uint<{width}>' in queue
        assert 'readLatency = 0 : i32, writeLatency = 1 : i32' in queue
        for name, size in leaves.items(): assert f'{name}: uint<{size}>' in queue
        required = {'top.ingress.hValid', f'top.ingress.hBits.{ch}.valid'}
        required |= {'awQueue.enq.ready' if other == 'aw' else f'top.{other}_enq.ready'
                     for other in ('aw', 'w', 'ar') if other != ch}
        dest = 'awQueue.enq.valid' if ch == 'aw' else 'top.' + ch + '_enq.valid'
        assert gates[dest] == frozenset(required)
        assert gates[('awQueue.enq.bits' if ch == 'aw' else 'top.' + ch + '_enq.bits')] == 'top.ingress.hBits.' + ch + '.bits'
        # Exported SV must retain each queue's geometry and surviving SFC fields.
        sv = clean(after.with_suffix('.sv').read_text())
        header, rtl = module(sv, queue_name)
        # CIRCT emits packed RAM in a separate memory module, unlike SFC's
        # split field arrays inside Queue_N. Compare geometry at that boundary.
        ram_name = f'ram_{depth}x{width}'
        assert re.search(r'\b' + ram_name + r'\s+ram_ext\s*\(', rtl)
        memory = module(sv, ram_name)[1]
        assert re.search(r'\breg\s+\[' + str(width - 1) + r':0\]\s+Memory\[0:' + str(depth - 1) + r'\]', memory)
        assert 'reset' not in memory
        assert "if (W0_en & 1'h1)" in memory and 'Memory[W0_addr] <= W0_data;' in memory
        assert f"assign R0_data = R0_en ? Memory[R0_addr] : {width}'bx;" in memory
        for name in leaves: assert 'enq_bits_' + name in header
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    assert updated == annotations, 'complete annotation archive transfer differs'
    return dict(before=str(before), after=str(after), unchanged_modules=len(old),
                wrapper_connections=count, annotation_rows=len(updated), unchanged_native_helpers=len(HELPERS))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    ref = clean(a.sfc.read_text()); ingress = equations(module(ref, 'IngressModule')[1])
    assert ingress['io_nastiInputs_hReady'] == '~ingressUnitStall'
    assert ingress['ingressUnitStall'] == '~_ingressUnitStall_T_1'
    ready = {f'{ch}Queue_io_enq_ready' for ch in ('aw', 'w', 'ar')}
    assert terms(ingress['_ingressUnitStall_T_1'], ingress, {n: n for n in ready}) == ready
    shapes = []
    for ch, sym, depth, width in [('aw', 'Queue_7', 10, 69), ('w', 'Queue_8', 16, 78), ('ar', 'Queue_9', 4, 69)]:
        required = {'io_nastiInputs_hValid', f'io_nastiInputs_hBits_{ch}_valid'}
        required |= {f'{other}Queue_io_enq_ready' for other in ('aw', 'w', 'ar') if other != ch}
        assert terms(ingress[ch + 'Queue_io_enq_valid'], ingress, {n: n for n in required}) == required
        header, queue = module(ref, sym)
        memories = re.findall(r'\breg\s+(?:\[(\d+):0\]\s+)?ram(?:_\w+)?\s*\[0:(\d+)\]', queue)
        assert memories and all(int(last) + 1 == depth for _, last in memories)
        leaves = {n: int(msb or 0) + 1 for msb, n in re.findall(r'input(?:\s+\[(\d+):0\])?\s+io_enq_bits_(\w+)', header)}
        queue_eq = equations(queue)
        for field in leaves:
            assert queue_eq[f'ram_{field}_MPORT_en'] == 'io_enq_ready & io_enq_valid'
            assert queue_eq[f'ram_{field}_MPORT_data'] == 'io_enq_bits_' + field
            assert queue_eq[f'ram_{field}_io_deq_bits_MPORT_en'] == "1'h1"
        shapes.append((ch, depth, width, leaves))
    model = equations(module(ref, 'FASEDMemoryTimingModel')[1])
    assert model['ingress_reset'] == 'reset | hPort_hBits_reset & _ingress_io_nastiInputs_hValid_T_3'
    report = dict(golden=str(a.sfc.resolve()), queues=[dict(channel=c, depth=d, payload_bits=w, SFC_fields=f) for c,d,w,f in shapes],
                  orders=[compare(x, y, a.baseline.read_text(), shapes) for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS shared SFC ingress capacity gates, queue geometry/payload/reset routes and emitted RAM; unchanged modules/CPU ports and complete annotations')


if __name__ == '__main__': main()
