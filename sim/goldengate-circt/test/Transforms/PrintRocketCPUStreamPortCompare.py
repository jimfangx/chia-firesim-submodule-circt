#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print CPU platform AXI ABI and wiring with immutable SFC.

CPUStreamPortTest checks actual adapter connections for every payload bit.
SFC has Print disabled: this proves the shared CPU ABI, not Print runtime parity.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring
from PrintControlMasterCompare import ports as rtl_ports

OLD = 'GGPrintCPUStreamWriteResponseBufferWrapper'
TOP = 'GGPrintCPUStreamPlatformWrapper'
HELPER = 'GGCPUStreamPortAdapter'
ADDRESS = [('id', 16), ('addr', 64), ('len', 8), ('size', 3), ('burst', 2),
           ('lock', 1), ('cache', 4), ('prot', 3), ('qos', 4)]
CHANNELS = [('aw', ADDRESS), ('w', [('data', 512), ('strb', 64), ('last', 1)]),
            ('b', [('id', 16), ('resp', 2)]), ('ar', ADDRESS),
            ('r', [('id', 16), ('data', 512), ('resp', 2), ('last', 1)])]
UNUSED = {c + '_bits_' + n for c in ('aw', 'ar') for n in ('burst', 'lock', 'cache', 'prot', 'qos')}
UNUSED |= {'w_bits_data', 'w_bits_last'}


def leaves():
    result = {}
    for ch, fields in CHANNELS:
        for leaf, width in [('ready', 1), ('valid', 1)] + [('bits_' + n, w) for n, w in fields]:
            is_input = leaf == 'ready' if ch in ('b', 'r') else leaf != 'ready'
            result[ch + '_' + leaf] = ('in' if is_input else 'out', width)
    return result


def axi_type():
    def token(fields):
        return 'bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<' + ', '.join(
            f'{n}: uint<{w}>' for n, w in fields) + '>>'
    return '!firrtl.bundle<' + ', '.join(c + (' flip' if c in ('b', 'r') else '') + ': ' + token(fs)
                                          for c, fs in CHANNELS) + '>'


def sfc_contract(path):
    text = clean(path.read_text()); header, body = module(text, 'FPGATop')
    expected = {'cpu_managed_axi4_' + n: ['input' if d == 'in' else 'output', w]
                for n, (d, w) in leaves().items()}
    actual = {n: v for n, v in rtl_ports(header).items() if n.startswith('cpu_managed_axi4_')}
    assert actual == expected and len(actual) == 37, 'SFC CPU platform ABI differs'
    nets = equations(body)
    for n, (d, _) in leaves().items():
        if n in UNUSED: continue
        external, buffered = 'cpu_managed_axi4_' + n, 'axi4buf_2_auto_in_' + n
        assert nets[buffered if d == 'in' else external] == (external if d == 'in' else buffered), n
    for n in UNUSED:
        assert not any(re.search(r'\bcpu_managed_axi4_' + n + r'\b', v) for v in nets.values()), n
    return dict(module='FPGATop', slave_address_bits=64, slave_data_bits=512, slave_id_bits=16,
                external_leaves=37, buffered_identity_routes=25, unused_input_fields=sorted(UNUSED))


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}; assert len(result) == len(found)
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 297 and len(new) == 299 and set(new) - set(old) == {TOP, HELPER}
    for name, body in old.items(): assert new[name] == body, 'changed prior body ' + name
    previous = native_ports(old[OLD]); assert len(previous) == 122
    consumed = {n for _, n, _ in previous if n.startswith('cpu_stream_')}
    assert consumed == {'cpu_stream_' + n for n in leaves() if n not in UNUSED}
    copied = [(d, n, t) for d, n, t in previous if n not in consumed]
    assert len(copied) == 97 and native_ports(new[TOP]) == copied + [('in', 'cpu_managed_axi4', axi_type())]
    expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n)
                for d, n, _ in copied}
    expected['cpu_port.cpu_managed_axi4'] = 'top.cpu_managed_axi4'
    for n, (d, _) in leaves().items():
        if n in UNUSED: continue
        child, adapter = 'sim.cpu_stream_' + n, 'cpu_port.stream_' + n
        expected[child if d == 'in' else adapter] = adapter if d == 'in' else child
    assert wiring(new[TOP]) == expected and len(expected) == 123
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [('sim', OLD), ('cpu_port', HELPER)]
    assert new[HELPER] == native_module(baseline, HELPER), 'shared native CPU adapter differs'
    helper_nets = {}
    for n, (d, _) in leaves().items():
        if n in UNUSED: continue
        ch, rest = n.split('_', 1)
        external = 'top.cpu_managed_axi4.' + ch + '.' + rest.replace('bits_', 'bits.')
        scalar = 'top.stream_' + n
        helper_nets[scalar if d == 'in' else external] = external if d == 'in' else scalar
    assert wiring(new[HELPER]) == helper_nets and len(helper_nets) == 25
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.CPUStreamPortTargets", nested = \{targets = \[([^]]+)\]\}\}', line); assert probe
    targets = ['cpu_stream_b_valid', 'cpu_stream_b_bits_id', 'hostClock', 'hostReset',
               'mem_0.aw.bits.id', 'cpu_stream_aw_valid', 'cpu_stream_ar_bits_addr']
    assert re.findall(r'"([^"]+)"', probe[1]) == [f'~{TOP}|{OLD if n in consumed else TOP}>' + n for n in targets]
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied:
        original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=297,
                public_ports=98, copied_ports=97, consumed_CPU_scalars=25, wrapper_connections=123,
                identical_shared_native_adapter=True, recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='CPU platform ABI matches the disabled-Print SFC golden; production enabled-Print integration and runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC 37-leaf CPU ABI, 25 buffered routes and 12 unused inputs; 297 prior bodies, 98 ports and 123 wrapper routes per order')


if __name__ == '__main__': main()
