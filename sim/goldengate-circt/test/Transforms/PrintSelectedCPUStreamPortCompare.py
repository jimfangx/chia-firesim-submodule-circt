#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print CPU platform AXI ABI and wiring with immutable SFC.

CPUStreamPortTest checks actual adapter connections for every payload bit.
SFC has Print disabled: this proves the shared CPU ABI, not Print runtime parity.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlResponsesCompare import clean, module
from PrintRocketCPUStreamPortCompare import HELPER, UNUSED, leaves, axi_type, sfc_contract
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring
from PrintControlMasterCompare import ports as rtl_ports

OLD = 'GGSelectedPrintCPUStreamWriteResponseBufferWrapper'
TOP = 'GGSelectedPrintCPUStreamPlatformWrapper'


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}; assert len(result) == len(found)
        return result
    old, new = modules(before), modules(after)
    assert set(new) - set(old) == {TOP, HELPER}
    for name, body in old.items(): assert new[name] == body, 'changed prior body ' + name
    previous = native_ports(old[OLD])
    consumed = {n for _, n, _ in previous if n.startswith('cpu_stream_')}
    assert consumed == {'cpu_stream_' + n for n in leaves() if n not in UNUSED}
    copied = [(d, n, t) for d, n, t in previous if n not in consumed]
    assert native_ports(new[TOP]) == copied + [('in', 'cpu_managed_axi4', axi_type())]
    expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n)
                for d, n, _ in copied}
    expected['cpu_port.cpu_managed_axi4'] = 'top.cpu_managed_axi4'
    for n, (d, _) in leaves().items():
        if n in UNUSED: continue
        child, adapter = 'sim.cpu_stream_' + n, 'cpu_port.stream_' + n
        expected[child if d == 'in' else adapter] = adapter if d == 'in' else child
    assert wiring(new[TOP]) == expected
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
    header, _ = module(clean(after_path.with_suffix('.sv').read_text()), TOP)
    emitted = {n: v for n, v in rtl_ports(header).items() if n.startswith('cpu_managed_axi4_')}
    assert emitted == {'cpu_managed_axi4_' + n: ['input' if d == 'in' else 'output', w]
                       for n, (d, w) in leaves().items()}, 'emitted selected CPU slave ABI differs'
    line = after.splitlines()[1]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied:
        original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive archive transfer differs'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(old),
                public_ports=len(copied)+1, copied_ports=len(copied), consumed_CPU_scalars=25, wrapper_connections=len(expected),
                identical_shared_native_adapter=True, emitted_AXI_leaf_ABI_matches_SFC=True,
                recursive_copied_consumed_targets=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='CPU platform ABI matches the disabled-Print SFC golden; production enabled-Print integration and runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC 37-leaf CPU ABI, 25 buffered identity routes and 12 unused inputs; prior bodies and complete annotation archive retained')


if __name__ == '__main__': main()
