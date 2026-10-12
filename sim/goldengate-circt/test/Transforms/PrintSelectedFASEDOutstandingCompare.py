#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print host-memory counters with immutable SFC RTL.

The golden disables Print. Host issue, response readiness, ingress queues and
memory timing remain external; this establishes shared counter/reset semantics.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring

OLD = 'GGSelectedPrintFASEDTokenWrapper'
TOP = 'GGSelectedPrintFASEDHostOutstandingWrapper'
COUNTER = 'GGFASEDHostOutstanding'


def compare(before, after, baseline):
    old_text, new_text = before.read_text(), after.read_text()
    def bodies(text):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S)}
    old, new = bodies(old_text), bodies(new_text)
    assert set(new) - set(old) == {TOP, COUNTER}
    for n, body in old.items(): assert new[n] == body, 'changed prior module ' + n
    assert new[COUNTER] == native_module(baseline, COUNTER), 'native counter operations differ'
    ports = native_ports(old[OLD])
    copied = [p for p in ports if p[1] != 'fased_readiness']
    expected = {}
    for d, n, _ in copied:
        expected['sim.' + n if d == 'in' else 'top.' + n] = 'top.' + n if d == 'in' else 'sim.' + n
    for n in ('clock', 'reset'): expected['hostOutstanding.' + n] = 'top.host' + n.capitalize()
    expected['hostOutstanding.transactions'] = 'top.fased_host_transactions'
    for n in ('readValid', 'writeValid'):
        expected['sim.fased_readiness.' + n] = 'top.fased_egress_readiness.' + n
    expected['sim.fased_readiness.hostMemIdle'] = 'hostOutstanding.hostMemIdle'
    expected['top.fased_host_mem_idle'] = 'hostOutstanding.hostMemIdle'
    expected['top.fased_host_read_inflight'] = 'hostOutstanding.hostReadInflight'
    assert wiring(new_text, TOP, aggregate=True) == expected, 'host/reset/idle or copied wiring differs'
    readiness = '!firrtl.bundle<readValid: uint<1>, writeValid: uint<1>>'
    helper_ports = native_ports(new[COUNTER])
    assert native_ports(new[TOP]) == copied + [
        ('in', 'fased_egress_readiness', readiness),
        ('in', 'fased_host_transactions', helper_ports[2][2]),
        ('out', 'fased_host_mem_idle', '!firrtl.uint<1>'),
        ('out', 'fased_host_read_inflight', '!firrtl.uint<1>')]
    annotations = json.loads(before.with_name(before.stem + '-all.json').read_text())
    updated = json.loads(after.with_name(after.stem + '-all.json').read_text())
    def transfer(v):
        if isinstance(v, str):
            if v == '~' + OLD: return '~' + TOP
            if not v.startswith('~' + OLD + '|'): return v
            v = '~' + TOP + v[len(OLD)+1:]
            prefix = '|' + OLD + '>'
            if prefix in v:
                ref = v.split(prefix, 1)[1]
                for _, n, _ in copied:
                    v = re.sub(re.escape(prefix + n) + r'(?=[.\[]|$)', '|' + TOP + '>' + n, v)
                if ref in ('fased_readiness.readValid', 'fased_readiness.writeValid'):
                    v = '~' + TOP + '|' + TOP + '>fased_egress_readiness.' + ref.split('.')[1]
                elif ref == 'fased_readiness.hostMemIdle':
                    v = '~' + TOP + '|' + TOP + '>fased_host_mem_idle'
            return v
        if isinstance(v, list): return [transfer(x) for x in v]
        if isinstance(v, dict): return {k: transfer(x) for k, x in v.items()}
        return v
    assert updated == transfer(annotations), 'complete annotation archive transfer differs'
    # The emitted RTL retains both four-bit counters and the same public ABI.
    header, rtl = module(clean(after.with_suffix('.sv').read_text()), COUNTER)
    assert len(re.findall(r'\breg\s+\[3:0\]\s+hostOutstanding(?:Reads|Writes)\b', rtl)) == 2
    outputs = {name for direction, names in re.findall(
        r'\b(input|output)\s+(?:\[[^]]+\]\s*)?(.*?)(?=\b(?:input|output)\b|$)', header, re.S)
        if direction == 'output' for name in re.findall(r'\b\w+\b', names)}
    assert {'hostMemIdle', 'hostReadInflight', 'readCount', 'writeCount'} == outputs
    return dict(before=str(before), after=str(after), unchanged_modules=len(old),
                copied_ports=len(copied), wrapper_connections=len(expected), annotation_rows=len(updated))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('before', 'after'): p.add_argument('--' + n, type=Path, action='append', required=True)
    for n in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + n, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    reference = clean(a.sfc.read_text())
    header, body = module(reference, 'FASEDMemoryTimingModel'); model = equations(body)
    assert model['host_mem_idle'] == 'SatUpDownCounter_io_empty & SatUpDownCounter_1_io_empty'
    assert model['ingress_io_host_read_inflight'] == '~SatUpDownCounter_io_empty'
    assert model['tResetReady'] == '~hPort_hBits_reset | host_mem_idle'
    for prefix in ('SatUpDownCounter', 'SatUpDownCounter_1'):
        assert model[prefix + '_clock'] == 'clock' and model[prefix + '_reset'] == 'reset'
        assert model[prefix + '_io_max'] == "4'ha"
    assert model['SatUpDownCounter_io_inc'] == 'auto_to_host_dram_out_ar_ready & nastiToHostDRAM_ar_valid'
    assert model['SatUpDownCounter_io_dec'] == 'auto_to_host_dram_out_r_valid & auto_to_host_dram_out_r_bits_last'
    assert model['SatUpDownCounter_1_io_inc'] == 'auto_to_host_dram_out_aw_ready & nastiToHostDRAM_aw_valid'
    assert model['SatUpDownCounter_1_io_dec'] == 'auto_to_host_dram_out_b_valid'
    # This SFC fixture specializes host R/B ready to true, pruning those ports.
    assert 'auto_to_host_dram_out_r_ready' not in header and 'auto_to_host_dram_out_b_ready' not in header
    counter = module(reference, 'SatUpDownCounter_1')[1]; counter_eq = equations(counter)
    assert re.search(r'\breg\s+\[3:0\]\s+value\b', counter)
    assert counter_eq['io_full'] == 'value >= io_max' and counter_eq['io_empty'] == "value == 4'h0"
    assert counter_eq['_value_T_1'] == "value + 4'h1" and counter_eq['_value_T_3'] == "value - 4'h1"
    assert re.search(r'if\s*\(reset\)\s*begin\s*value\s*<=\s*4\'h0;', counter)
    assert re.search(r'else if\s*\(io_inc & ~io_dec & ~io_full\)', counter)
    assert re.search(r'else if\s*\(~io_inc & io_dec & ~io_empty\)', counter)
    report = dict(golden=str(a.sfc.resolve()), host_counter_width=4, host_counter_max=10,
                  SFC_response_ready_specialization=True,
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)], limitation=__doc__)
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS shared SFC host counters/reset/idle, native counter operations and emitted state; unchanged modules/CPU ports and complete annotation archive')


if __name__ == '__main__': main()
