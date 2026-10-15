#!/usr/bin/env python3
# See LICENSE for license details.
"""Anchor emitted Print R-router SSA checks to immutable SFC AXI4Xbar equations.

Compare the full wrapper's typed ports, connections, nested annotation targets,
retained modules and shared helper. The C++ interpreter checks emitted SSA for
all ID/status/last/valid/ready combinations and four 64-bit payload patterns.
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

OLD, TOP = 'GGHostMemoryReadWrapper', 'GGHostMemoryReadResponseWrapper'
HELPER = 'GGHostMemoryReadResponseRouter'
CONSUMED = {'fased_host_mem', 'host_mem', 'loadmem_mem_r_ready',
            'loadmem_mem_r_valid', 'loadmem_mem_r_bits_data'}


def sfc_contract(path):
    header, body = module(clean(path.read_text()), 'AXI4Xbar')
    nets = equations(body)
    want = {
        'requestROI_0_0': "auto_anon_out_r_bits_id == 5'h10",
        'requestROI_0_1': '~auto_anon_out_r_bits_id[4]',
        'auto_anon_in_0_r_valid': 'auto_anon_out_r_valid & requestROI_0_0',
        'auto_anon_in_1_r_valid': 'auto_anon_out_r_valid & requestROI_0_1',
        'auto_anon_in_0_r_bits_data': 'auto_anon_out_r_bits_data',
        'auto_anon_in_1_r_bits_id': 'auto_anon_out_r_bits_id[3:0]',
        'auto_anon_in_1_r_bits_data': 'auto_anon_out_r_bits_data',
        'auto_anon_in_1_r_bits_resp': 'auto_anon_out_r_bits_resp',
        'auto_anon_in_1_r_bits_last': 'auto_anon_out_r_bits_last',
        'auto_anon_out_r_ready': 'requestROI_0_0 & auto_anon_in_0_r_ready | requestROI_0_1 & auto_anon_in_1_r_ready',
    }
    normalize = lambda x: re.sub(r'\s+', '', x)
    for n, v in want.items():
        assert normalize(nets[n]) == normalize(v), (n, nets[n], v)
    ports = rtl_ports(header)
    typed = {}
    for prefix, direction, id_bits in [('auto_anon_out_r', 'input', 5),
                                       ('auto_anon_in_1_r', 'output', 4)]:
        typed[prefix + '_ready'] = ['output' if direction == 'input' else 'input', 1]
        typed[prefix + '_valid'] = [direction, 1]
        for field, width in [('id', id_bits), ('last', 1), ('data', 64), ('resp', 2)]:
            typed[prefix + '_bits_' + field] = [direction, width]
    typed.update({'auto_anon_in_0_r_ready': ['input', 1],
                  'auto_anon_in_0_r_valid': ['output', 1],
                  'auto_anon_in_0_r_bits_data': ['output', 64]})
    assert {n: p for n, p in ports.items() if '_r_' in n} == typed, 'SFC R port types differ'
    return dict(equations=len(want), typed_ports=len(typed), host_ID_bits=5, FASED_ID_bits=4,
                LoadMem_ID=16, unmatched_IDs_stall=list(range(17, 32)),
                ready_independent_of_valid=True, payload_broadcast=True)


def compare_binding(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}
        assert len(result) == len(found), 'duplicate module definitions'
        return result
    old_modules, new_modules = modules(before), modules(after)
    assert not set(old_modules) & {TOP, HELPER}, 'R response boundary already exists'
    for n, body in old_modules.items():
        assert body == new_modules[n], 'changed prior body ' + n
    assert set(new_modules) == set(old_modules) | {TOP, HELPER}
    wrapper = new_modules[TOP]; old_ports = native_ports(old_modules[OLD])
    copied = [p for p in old_ports if p[1] not in CONSUMED]
    ports = native_ports(wrapper)
    assert ports[:len(copied)] == copied and len(ports) == len(copied) + 1
    partial = next(t for _, n, t in old_ports if n == 'host_mem')
    rest = next(t for _, n, t in old_ports if n == 'fased_host_mem')
    r = rest.removeprefix('!firrtl.bundle<r flip: ')[:-1].replace('id: uint<4>', 'id: uint<5>')
    assert ports[len(copied):] == [('out', 'host_mem', partial[:-1] + ', r flip: ' + r + '>')]
    nets = {('sim.' if d == 'in' else 'top.') + n:
            ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets.update({'top.host_mem.aw': 'sim.host_mem.aw', 'top.host_mem.w': 'sim.host_mem.w',
                 'sim.host_mem.b': 'top.host_mem.b', 'top.host_mem.ar': 'sim.host_mem.ar',
                 'read_responses.host': 'top.host_mem.r',
                 'read_responses.load.ready': 'sim.loadmem_mem_r_ready',
                 'sim.loadmem_mem_r_valid': 'read_responses.load.valid',
                 'sim.loadmem_mem_r_bits_data': 'read_responses.load.bits.data',
                 'sim.fased_host_mem.r': 'read_responses.fased'})
    assert wiring(wrapper) == nets, 'host R and retained AW/W/B/AR routes differ'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', wrapper) == [('sim', OLD), ('read_responses', HELPER)]
    assert new_modules[HELPER] == native_module(baseline, HELPER), 'shared R router differs'
    archive = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"').replace('~' + OLD, '~' + TOP)
    for _, n, _ in copied:
        archive = re.sub(r'\|' + OLD + r'>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, archive)
    archive = re.sub(r'\|' + OLD + r'>host_mem\.(aw|w|b|ar)(?=[.\["\\])',
                     lambda m: '|' + TOP + '>host_mem.' + m[1], archive)
    assert archive == after.splitlines()[1], 'nested archive target transfer differs'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(old_modules),
                copied_ports=len(copied), top_ports=len(ports), wrapper_connections=len(nets),
                identical_native_helpers=1,
                preserved_Print_modules=len([n for n in old_modules if n.startswith('GGPrintBridge')]),
                nested_archive_targets_preserved=True)


def compare(before_path, after_path, baseline):
    report = compare_binding(before_path, after_path, baseline)
    assert report['unchanged_module_bodies'] == 279
    assert report['copied_ports'] == 108 and report['top_ports'] == 109
    assert report['wrapper_connections'] == 117 and report['preserved_Print_modules'] == 21
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['AXI4Xbar'],
                  SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print host R composition only; Print production integration/runtime parity remains pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC R routing/ID trimming/backpressure/payload; 279 prior bodies, 108 copied ports, 117 routes and nested targets preserved per order')


if __name__ == '__main__': main()
