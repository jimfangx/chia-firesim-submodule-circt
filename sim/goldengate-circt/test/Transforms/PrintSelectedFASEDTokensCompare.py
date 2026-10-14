#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print FASED token composition with immutable SFC RTL.

The golden disables Print; only shared FASED handshake/reset semantics apply.
Ingress capacity, egress readiness and timing are explicit external boundaries.
"""
import argparse
import itertools
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module, terms
from PrintRocketMasterCompare import native_module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDIngressCompare import boolean_nets, evaluate

OLD = 'GGSelectedPrintCPUStreamPlatformWrapper'
TOP = 'GGSelectedPrintFASEDTokenWrapper'
ENGINE = 'GGFASEDTokenEngine'
LOCALS = ['axi4_aw_fwd', 'axi4_w_fwd', 'axi4_b_rev', 'axi4_ar_fwd', 'axi4_r_rev', 'reset',
          'axi4_aw_rev', 'axi4_w_rev', 'axi4_b_fwd', 'axi4_ar_rev', 'axi4_r_fwd']


def routes(body):
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', body.splitlines()[0])}
    nets = {}
    def leaves(v): return {v} if isinstance(v, str) else set(v)
    for line in body.splitlines()[1:]:
        if ' = firrtl.instance ' in line:
            left, right = line.split(' = firrtl.instance ', 1)
            values.update(zip(re.findall(r'%\w+', left),
                              (right.split()[0] + '.' + p for p in re.findall(r'\b(?:in|out) (\w+):', right))))
        if m := re.search(r'(%\w+) = firrtl.constant 1 :', line): values[m[1]] = frozenset()
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line): values[m[1]] = values[m[2]] + '.' + m[3]
        if m := re.search(r'(%\w+) = firrtl.and (%\w+), (%\w+)', line): values[m[1]] = frozenset(leaves(values[m[2]]) | leaves(values[m[3]]))
        if m := re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)', line):
            assert values[m[1]] not in nets, 'duplicate driver'
            nets[values[m[1]]] = values[m[2]]
    return nets


def compare(before, after, baseline, old_name=OLD, top_name=TOP):
    OLD, TOP = old_name, top_name
    old_text, new_text = before.read_text(), after.read_text()
    def bodies(text): return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S)}
    old, new = bodies(old_text), bodies(new_text)
    assert set(new) - set(old) == {TOP, ENGINE}
    for n, body in old.items(): assert new[n] == body, 'changed prior module ' + n
    assert new[ENGINE] == native_module(baseline, ENGINE), 'shared native FASED engine differs'
    annotations = json.loads(before.with_name(before.stem + '-all.json').read_text())
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    bridge = next(a for a in annotations if a.get('widgetClass') == 'midas.models.FASEDMemoryTimingModel')
    channels = {a['globalName']: a for a in annotations if a.get('globalName') in bridge['channelMapping'].values()}
    assert len(channels) == 11
    ports = native_ports(old[OLD]); endpoints = []
    expected = {}; targets = {}
    for j, local in enumerate(LOCALS):
        row = channels[bridge['channelMapping'][local]]
        side = 'sources' if j < 6 else 'sinks'
        pin = row[side][0].split('>')[1].split('.')[0]; endpoints.append(pin)
        paths = []
        for target in row[side]:
            field = target.split('>')[1].split('.', 1)[1]
            if field == 'bits': leaf = 'reset' if j == 5 else local[5:local.rfind('_')] + '.ready'
            else: leaf = local[5:local.rfind('_')] + '.' + field.removeprefix('bits.')
            host = 'FASEDMemoryTimingModel_0.hPort.hBits.' + (leaf if j == 5 else 'axi4.' + leaf)
            child = 'sim.' + target.split('>')[1]
            expected[host if j < 6 else child] = child if j < 6 else host
            paths.append('~' + TOP + '|' + ENGINE + '>hPort.hBits.' + (leaf if j == 5 else 'axi4.' + leaf))
        targets[row['globalName']] = ('sinks' if j < 6 else 'sources', paths)
    assert len(set(endpoints)) == 11
    copied = [(d, n, t) for d, n, t in ports if n not in endpoints]
    boundary = ['fased_timing', 'fased_ingress', 'fased_readiness', 'fased_tfire', 'fased_ingress_reset', 'fased_egress_reset', 'fased_model_reset']
    engine_ports = native_ports(new[ENGINE])[3:]
    assert native_ports(new[TOP]) == copied + [(d, n, t) for n, (d, _, t) in zip(boundary, engine_ports)]
    for d, n, _ in copied: expected['sim.' + n if d == 'in' else 'top.' + n] = 'top.' + n if d == 'in' else 'sim.' + n
    for n in ('clock', 'reset'): expected['FASEDMemoryTimingModel_0.' + n] = 'top.host' + n.capitalize()
    for n, (d, engine_pin, _) in zip(boundary, engine_ports):
        expected['FASEDMemoryTimingModel_0.' + engine_pin if d == 'in' else 'top.' + n] = 'top.' + n if d == 'in' else 'FASEDMemoryTimingModel_0.' + engine_pin
    for outgoing in (True, False):
        indices = range(6) if outgoing else range(6, 11)
        predicate = 'valid' if outgoing else 'ready'; gate = 'hReady' if outgoing else 'hValid'
        host = 'FASEDMemoryTimingModel_0.hPort.' + ('toHost' if outgoing else 'fromHost')
        expected[host + '.' + ('hValid' if outgoing else 'hReady')] = frozenset('sim.' + endpoints[j] + '.' + predicate for j in indices)
        for j in indices: expected['sim.' + endpoints[j] + '.' + ('ready' if outgoing else 'valid')] = frozenset([host + '.' + gate] + ['sim.' + endpoints[k] + '.' + predicate for k in indices if k != j])
    assert routes(new[TOP]) == expected, 'eleven-channel/copy/control wiring differs'
    def transfer(v):
        if isinstance(v, str):
            if v == '~' + OLD: return '~' + TOP
            if v.startswith('~' + OLD + '|'):
                v = '~' + TOP + v[len(OLD)+1:]
                for _, n, _ in copied:
                    v = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\[]|$)', '|' + TOP + '>' + n, v)
            return v
        if isinstance(v, list): return [transfer(x) for x in v]
        if isinstance(v, dict): return {k: transfer(x) for k, x in v.items()}
        return v
    wanted = transfer(annotations)
    for row in wanted:
        if row.get('widgetClass') == 'midas.models.FASEDMemoryTimingModel': row['target'] = '~' + TOP + '|' + ENGINE + '>hPort'
        if row.get('globalName') in targets:
            side, paths = targets[row['globalName']]; row[side] = paths
    assert updated == wanted, 'complete annotation archive transfer differs'
    engine = boolean_nets(new_text, ENGINE)
    inputs = ['hPort.toHost.hValid', 'hPort.fromHost.hReady', 'ingress.hReady', 'readiness.writeValid', 'readiness.readValid', 'hPort.hBits.reset', 'readiness.hostMemIdle', 'reset']
    for bits in itertools.product((False, True), repeat=8):
        valid, ready, capacity, write, read, target_reset, idle, reset = bits
        offer = valid and ready and write and read and (not target_reset or idle); fire = offer and capacity
        for pin, value in [('targetFire', fire), ('hPort.toHost.hReady', fire), ('hPort.fromHost.hValid', fire), ('ingress.hValid', offer), ('ingressReset', reset or target_reset and offer), ('egressReset', reset or target_reset and fire), ('modelReset', target_reset)]:
            assert evaluate(engine[pin], dict(zip(inputs, bits))) == value
    for ch in ('aw', 'w', 'ar'):
        prefix = 'hPort.hBits.axi4.' + ch
        for ready, valid in itertools.product((False, True), repeat=2):
            assert evaluate(engine['ingress.hBits.' + ch + '.valid'], {prefix + '.ready': ready, prefix + '.valid': valid}) == (ready and valid)
    return dict(before=str(before), after=str(after), unchanged_modules=len(old), copied_ports=len(copied), memory_channels=11, payload_routes=46, boolean_cases=256, annotation_rows=len(updated), wrapper_connections=len(expected))


def compare_sfc(golden):
    sfc = equations(module(clean(golden.read_text()), 'FASEDMemoryTimingModel')[1])
    names = {'hPort_toHost_hValid': 'valid', 'hPort_fromHost_hReady': 'ready', 'ingress_io_nastiInputs_hReady': 'capacity', 'writeEgress_io_resp_hValid': 'write', 'readEgress_io_resp_hValid': 'read', 'tResetReady': 'resetReady'}
    assert sfc['tResetReady'] == '~hPort_hBits_reset | host_mem_idle'
    assert terms(sfc['targetFire'], sfc, names) == set(names.values())
    assert terms(sfc['hPort_toHost_hReady'], sfc, names) == set(names.values())
    assert terms(sfc['hPort_fromHost_hValid'], sfc, names) == set(names.values())
    assert terms(sfc['ingress_io_nastiInputs_hValid'], sfc, names) == set(names.values()) - {'capacity'}
    assert sfc['ingress_reset'] == 'reset | hPort_hBits_reset & _ingress_io_nastiInputs_hValid_T_3'
    assert sfc['readEgress_reset'] == 'reset | hPort_hBits_reset & targetFire'
    assert sfc['writeEgress_reset'] == 'reset | _readEgress_reset_T_1'
    assert sfc['_readEgress_reset_T_1'] == 'hPort_hBits_reset & targetFire'
    assert sfc['model_reset'] == 'hPort_hBits_reset'
    for ch in ('aw', 'w', 'ar'):
        assert sfc['ingress_io_nastiInputs_hBits_' + ch + '_valid'] == 'hPort_hBits_axi4_' + ch + '_ready & hPort_hBits_axi4_' + ch + '_valid'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    compare_sfc(a.sfc)
    report = dict(golden=str(a.sfc.resolve()), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS shared SFC FASED token/reset equations, eleven channels and 46 payload routes; unchanged allocation/modules and complete annotation archive')


if __name__ == '__main__': main()
