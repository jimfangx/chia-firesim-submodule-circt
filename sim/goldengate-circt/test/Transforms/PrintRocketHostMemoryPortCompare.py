#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print memory port assembly with immutable SFC FPGATop.

The C++ interpreter separately evaluates the emitted adapter SSA across every
16-bit response ID, 5-bit request ID and valid/ready combination.
"""
import argparse
import json
import re
from pathlib import Path
from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketMasterCompare import native_module
from PrintRocketLoadMemEngineCompare import wiring

OLD = 'GGHostMemoryReadResponseWrapper'
TOP = 'GGHostMemoryPlatformWrapper'
HELPER = 'GGHostMemoryPortAdapter'
ADDRESS = dict(id=16, qos=4, prot=3, cache=4, lock=1, burst=2, size=3, len=8, addr=34)
CHANNELS = dict(aw=ADDRESS, w=dict(strb=8, last=1, data=64),
                b=dict(id=16, resp=2), ar=ADDRESS, r=dict(id=16, last=1, data=64, resp=2))


def sfc_contract(path):
    header, body = module(clean(path.read_text()), 'FPGATop')
    ports, nets = rtl_ports(header), equations(body)
    expected, checked = {}, {}
    for channel, fields in CHANNELS.items():
        response = channel in ('b', 'r')
        for leaf, width in dict(ready=1, valid=1, **{'bits_' + k: v for k, v in fields.items()}).items():
            output = response if leaf == 'ready' else not response
            pin, internal = f'mem_0_{channel}_{leaf}', f'axi4buf_auto_out_{channel}_{leaf}'
            expected[pin] = ['output' if output else 'input', width]
            dest, value = (pin, internal) if output else (internal, pin)
            if leaf == 'bits_id':
                if response:
                    value += '[4:0]'
                else:
                    alias = f'memAXI4NodesIn_{channel}_bits_id'
                    assert nets[alias] == internal
                    value = "{{11'd0}, " + alias + '}'
            assert re.sub(r'\s+', '', nets[dest]) == re.sub(r'\s+', '', value), (dest, nets[dest], value)
            checked[dest] = value
    assert {k: v for k, v in ports.items() if k.startswith('mem_0_')} == expected
    assert len(checked) == 37
    return dict(module='FPGATop', platform_ports=expected, leaf_equations=37,
                request_ID_zero_extension=[5, 16], response_ID_truncation=[16, 5],
                conversion_independent_of_valid_ready=True)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        matches = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in matches}
        assert len(result) == len(matches), 'duplicate module'
        return result
    old, new = modules(before), modules(after)
    assert len(old) == 286 and len(new) == 288
    for name, body in old.items():
        assert new[name] == body, 'changed prior module ' + name
    assert set(new) - set(old) == {TOP, HELPER}
    copied = [p for p in native_ports(old[OLD]) if p[1] != 'host_mem']
    host = next(p for p in native_ports(old[OLD]) if p[1] == 'host_mem')
    assert len(copied) == 108 and host[0] == 'out'
    assert native_ports(new[TOP]) == copied + [('out', 'mem_0', host[2].replace('id: uint<5>', 'id: uint<16>'))]
    expected = {('sim.' + n if d == 'in' else 'top.' + n):
                ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    expected.update({'memory_port.in': 'sim.host_mem', 'top.mem_0': 'memory_port.out'})
    assert wiring(new[TOP]) == expected and len(expected) == 110
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', new[TOP]) == [
        ('sim', OLD), ('memory_port', HELPER)]
    assert new[HELPER] == native_module(baseline, HELPER), 'shared native adapter differs'
    assert len([n for n in old if n.startswith('GGPrintBridge')]) == 21
    # Only the test's nested target probe is added to the archived annotations.
    line = after.splitlines()[1]
    probe = re.search(r', \{class = "test.HostMemoryPortTargets", nested = \{targets = \[([^]]+)\]\}\}', line)
    assert probe, 'missing recursive target probe'
    targets = re.findall(r'"([^"]+)"', probe[1])
    assert targets == [f'~{TOP}|{TOP}>hostClock', f'~{TOP}|{TOP}>hostReset',
                       f'~{TOP}|{OLD}>host_mem', f'~{TOP}|{OLD}>host_mem.aw.bits.id',
                       f'~{TOP}|{OLD}>host_mem.r.bits.id']
    line = line[:probe.start()] + line[probe.end():]
    original = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"')
    original = original.replace('~' + OLD + '|', '~' + TOP + '|').replace('"~' + OLD + '"', '"~' + TOP + '"')
    for _, n, _ in copied:
        original = re.sub(r'\|' + OLD + '>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, original)
    assert line == original, 'recursive annotation archive transfer differs'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=286,
                public_ports=109, copied_ports=108, wrapper_connections=110,
                identical_native_helpers=1, preserved_Print_modules=21,
                recursive_copied_and_consumed_target_transfer=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'):
        p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'):
        p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args()
    assert len(a.before) == len(a.after)
    baseline = a.baseline.read_text()
    report = dict(golden=str(a.sfc.resolve()), SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, baseline) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print memory port composition; production Print integration and runtime parity remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC 34/64/16 mem_0 ABI and all 37 leaf equations; 286 bodies, 109 ports, 110 routes and recursive targets per order')


if __name__ == '__main__':
    main()
