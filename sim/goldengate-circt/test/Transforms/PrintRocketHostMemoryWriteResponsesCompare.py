#!/usr/bin/env python3
# See LICENSE for license details.
"""Anchor expanded Print host B response composition to immutable SFC RTL.

The C++ SSA interpreter exhausts every ID/response/valid/sink-ready combination
on the emitted router. This comparison verifies its recorded SFC oracle, all
wrapper routes, public ports, retained module bodies and annotation targets.
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

OLD, TOP = 'GGHostMemoryWriteWrapper', 'GGHostMemoryWriteResponseWrapper'
HELPER = 'GGHostMemoryWriteResponseRouter'
CONSUMED = ['fased_host_mem', 'host_mem_write', 'loadmem_mem_b_ready', 'loadmem_mem_b_valid']


def sfc_contract(path):
    header, body = module(clean(path.read_text()), 'AXI4Xbar')
    nets = equations(body)
    want = {
        'requestBOI_0_0': "auto_anon_out_b_bits_id == 5'h10",
        'requestBOI_0_1': '~auto_anon_out_b_bits_id[4]',
        'auto_anon_in_0_b_valid': 'auto_anon_out_b_valid & requestBOI_0_0',
        'auto_anon_in_1_b_valid': 'auto_anon_out_b_valid & requestBOI_0_1',
        'auto_anon_in_1_b_bits_id': 'auto_anon_out_b_bits_id[3:0]',
        'auto_anon_in_1_b_bits_resp': 'auto_anon_out_b_bits_resp',
        'auto_anon_out_b_ready': 'requestBOI_0_0 & auto_anon_in_0_b_ready | requestBOI_0_1 & auto_anon_in_1_b_ready',
    }
    normalize = lambda s: re.sub(r'\s+', '', s)
    for n, v in want.items(): assert normalize(nets[n]) == normalize(v), (n, nets[n], v)
    ports = rtl_ports(header)
    for n, d, w in [('auto_anon_out_b_bits_id', 'input', 5),
                    ('auto_anon_in_1_b_bits_id', 'output', 4),
                    ('auto_anon_in_1_b_bits_resp', 'output', 2),
                    ('auto_anon_out_b_ready', 'output', 1)]:
        assert ports[n] == [d, w], (n, ports[n])
    return dict(equations=len(want), host_ID_bits=5, FASED_ID_bits=4, LoadMem_ID=16,
                FASED_IDs=[0, 15], unmatched_IDs_blocked=[17, 31], ready_independent_of_valid=True)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}
        assert len(result) == len(found), 'duplicate module definitions'
        return result
    old_modules, new_modules = modules(before), modules(after)
    assert len(old_modules) == 275
    for n, body in old_modules.items(): assert body == new_modules[n], 'changed prior body ' + n
    assert set(new_modules) - set(old_modules) == {TOP, HELPER}
    wrapper = new_modules[TOP]; old_ports = native_ports(old_modules[OLD])
    copied = [p for p in old_ports if p[1] not in CONSUMED]; ports = native_ports(wrapper)
    assert len(copied) == 114 and ports[:114] == copied and len(ports) == 116
    old_rest = next(t for _, n, t in old_ports if n == 'fased_host_mem')
    rest = '!firrtl.bundle<ar: ' + old_rest.split(', ar: ', 1)[1]
    response = old_rest.split(', ar: ', 1)[0].removeprefix('!firrtl.bundle<b flip: ')
    old_write = next(t for _, n, t in old_ports if n == 'host_mem_write')
    master = old_write[:-1] + ', b flip: ' + response.replace('id: uint<4>', 'id: uint<5>') + '>'
    assert ports[114:] == [('out', 'fased_host_mem', rest), ('out', 'host_mem_write', master)]
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets.update({'top.host_mem_write.aw': 'sim.host_mem_write.aw', 'top.host_mem_write.w': 'sim.host_mem_write.w',
                 'write_responses.host': 'top.host_mem_write.b',
                 'write_responses.load.ready': 'sim.loadmem_mem_b_ready',
                 'sim.loadmem_mem_b_valid': 'write_responses.load.valid',
                 'sim.fased_host_mem.b': 'write_responses.fased',
                 'top.fased_host_mem.ar': 'sim.fased_host_mem.ar', 'sim.fased_host_mem.r': 'top.fased_host_mem.r'})
    assert wiring(wrapper) == nets, 'host B and retained AW/W/AR/R routes differ'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', wrapper) == [('sim', OLD), ('write_responses', HELPER)]
    assert new_modules[HELPER] == native_module(baseline, HELPER), 'shared B router differs'
    archive = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"').replace('~' + OLD, '~' + TOP)
    for _, n, _ in copied:
        archive = re.sub(r'\|' + OLD + r'>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, archive)
    for n in ('fased_host_mem.ar', 'fased_host_mem.r', 'host_mem_write.aw', 'host_mem_write.w'):
        archive = re.sub(r'\|' + OLD + r'>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, archive)
    assert archive == after.splitlines()[1], 'nested archive target transfer differs'
    assert len([n for n in old_modules if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=275,
                copied_ports=114, top_ports=116, wrapper_connections=len(nets), identical_native_helpers=1,
                preserved_Print_modules=21, nested_archive_targets_preserved=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['AXI4Xbar'], SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print host B composition only; Print production integration/runtime parity and host AR/R composition remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC B ID selection/truncation/selected-sink backpressure; 275 prior bodies, 114 copied ports, 122 routes and nested targets preserved per order')


if __name__ == '__main__': main()
