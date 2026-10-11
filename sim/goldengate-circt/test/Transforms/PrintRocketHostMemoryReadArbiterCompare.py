#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print AR composition with the immutable SFC AXI4Xbar.

The C++ interpreter evaluates the emitted helper's SSA and synchronous state.
Here we anchor that oracle to SFC equations and check the complete boundary,
retained modules, port types, connections and recursive annotation transfer.
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

OLD, TOP = 'GGHostMemoryWriteResponseWrapper', 'GGHostMemoryReadWrapper'
HELPER = 'GGHostMemoryReadArbiter'
CONSUMED = ['fased_host_mem', 'host_mem_write', 'loadmem_mem_ar_ready',
            'loadmem_mem_ar_valid', 'loadmem_mem_ar_bits_addr']


def sfc_contract(path):
    header, body = module(clean(path.read_text()), 'AXI4Xbar')
    nets = equations(body)
    want = {
        'anyValid': 'auto_anon_in_0_ar_valid | auto_anon_in_1_ar_valid',
        'readys_valid': '{auto_anon_in_1_ar_valid,auto_anon_in_0_ar_valid}',
        '_readys_filter_T': '~readys_mask',
        '_readys_filter_T_1': 'readys_valid & _readys_filter_T',
        'readys_filter': '{_readys_filter_T_1,auto_anon_in_1_ar_valid,auto_anon_in_0_ar_valid}',
        '_GEN_18': "{{1'd0}, readys_filter[3:1]}",
        '_readys_unready_T_1': 'readys_filter | _GEN_18',
        '_readys_unready_T_4': "{readys_mask, 2'h0}",
        '_GEN_19': "{{1'd0}, _readys_unready_T_1[3:1]}",
        'readys_unready': '_GEN_19 | _readys_unready_T_4',
        '_readys_readys_T_2': 'readys_unready[3:2] & readys_unready[1:0]',
        'readys_readys': '~_readys_readys_T_2',
        '_readys_mask_T': 'readys_readys & readys_valid',
        '_readys_mask_T_1': "{_readys_mask_T, 1'h0}",
        '_readys_mask_T_3': '_readys_mask_T | _readys_mask_T_1[1:0]',
        '_GEN_6': "anyValid ? 1'h0 : idle",
        '_out_0_ar_valid_T_2': 'state__0 & auto_anon_in_0_ar_valid | state__1 & auto_anon_in_1_ar_valid',
        'out_0_ar_valid': 'idle ? anyValid : _out_0_ar_valid_T_2',
        '_T_18': 'auto_anon_out_ar_ready & out_0_ar_valid',
        '_GEN_7': '_T_18 | _GEN_6',
        'auto_anon_out_ar_valid': 'idle ? anyValid : _out_0_ar_valid_T_2',
        'in_1_ar_bits_id': "{{1'd0}, auto_anon_in_1_ar_bits_id}",
        '_out_0_ar_bits_T_24': "muxState__0 ? 5'h10 : 5'h0",
        '_out_0_ar_bits_T_25': "muxState__1 ? in_1_ar_bits_id : 5'h0",
        '_out_0_ar_bits_T_12': "muxState__0 ? 2'h1 : 2'h0",
        '_out_0_ar_bits_T_15': "muxState__0 ? 3'h3 : 3'h0",
        'auto_anon_out_ar_bits_id': '_out_0_ar_bits_T_24 | _out_0_ar_bits_T_25',
        'auto_anon_out_ar_bits_len': "muxState__1 ? auto_anon_in_1_ar_bits_len : 8'h0",
    }
    for i in range(2):
        want.update({f'readys__{i}': f'readys_readys[{i}]',
                     f'winner__{i}': f'readys__{i} & auto_anon_in_{i}_ar_valid',
                     f'muxState__{i}': f'idle ? winner__{i} : state__{i}',
                     f'allowed_{i}': f'idle ? readys__{i} : state__{i}',
                     f'auto_anon_in_{i}_ar_ready': f'auto_anon_out_ar_ready & allowed_{i}'})
    for field, lo, hi, width, initial in [('burst', 12, 13, 2, 1), ('size', 15, 16, 3, 3), ('addr', 21, 22, 34, None)]:
        if initial is None:
            want[f'_out_0_ar_bits_T_{lo}'] = f"muxState__0 ? auto_anon_in_0_ar_bits_{field} : {width}'h0"
        want[f'_out_0_ar_bits_T_{hi}'] = f"muxState__1 ? auto_anon_in_1_ar_bits_{field} : {width}'h0"
        want[f'auto_anon_out_ar_bits_{field}'] = f'_out_0_ar_bits_T_{lo} | _out_0_ar_bits_T_{hi}'
    want['auto_anon_out_ar_bits_lock'] = 'muxState__1 & auto_anon_in_1_ar_bits_lock'
    for field, width in [('cache', 4), ('prot', 3), ('qos', 4)]:
        want[f'auto_anon_out_ar_bits_{field}'] = f"muxState__1 ? auto_anon_in_1_ar_bits_{field} : {width}'h0"
    normalize = lambda s: re.sub(r'\s+', '', s)
    for n, v in want.items(): assert normalize(nets[n]) == normalize(v), (n, nets[n], v)
    state = normalize(body)
    for update in ('idle<=reset|_GEN_7;', "readys_mask<=2'h3;",
                   'elseif(idle&|readys_valid)beginreadys_mask<=_readys_mask_T_3;',
                   "state__0<=1'h0;", 'elseif(idle)beginstate__0<=winner__0;',
                   "state__1<=1'h0;", 'elseif(idle)beginstate__1<=winner__1;'):
        assert update in state, update
    ports = rtl_ports(header)
    for n, d, w in [('auto_anon_out_ar_bits_id', 'output', 5),
                    ('auto_anon_in_1_ar_bits_id', 'input', 4),
                    ('auto_anon_in_0_ar_bits_addr', 'input', 34),
                    ('auto_anon_out_ar_bits_addr', 'output', 34)]:
        assert ports[n] == [d, w], (n, ports[n])
    return dict(equations=len(want), state_update_anchors=7, host_ID_bits=5,
                FASED_ID_bits=4, LoadMem_ID=16, locked_during_stalls=True,
                priority_rotates_on_idle_selection=True)


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}
        assert len(result) == len(found), 'duplicate module definitions'
        return result
    old_modules, new_modules = modules(before), modules(after)
    assert len(old_modules) == 277
    for n, body in old_modules.items(): assert body == new_modules[n], 'changed prior body ' + n
    assert set(new_modules) - set(old_modules) == {TOP, HELPER}
    wrapper = new_modules[TOP]; old_ports = native_ports(old_modules[OLD])
    copied = [p for p in old_ports if p[1] not in CONSUMED]; ports = native_ports(wrapper)
    assert len(copied) == 111 and ports[:111] == copied and len(ports) == 113
    old_rest = next(t for _, n, t in old_ports if n == 'fased_host_mem')
    rest = '!firrtl.bundle<r flip: ' + old_rest.split(', r flip: ', 1)[1]
    address = old_rest.removeprefix('!firrtl.bundle<ar: ').split(', r flip: ', 1)[0]
    old_write = next(t for _, n, t in old_ports if n == 'host_mem_write')
    master = old_write[:-1] + ', ar: ' + address.replace('id: uint<4>', 'id: uint<5>') + '>'
    assert ports[111:] == [('out', 'fased_host_mem', rest), ('out', 'host_mem', master)]
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets.update({'top.host_mem.aw': 'sim.host_mem_write.aw', 'top.host_mem.w': 'sim.host_mem_write.w',
                 'sim.host_mem_write.b': 'top.host_mem.b', 'read_arbiter.clock': 'top.hostClock',
                 'read_arbiter.reset': 'top.hostReset', 'sim.loadmem_mem_ar_ready': 'read_arbiter.load.ready',
                 'read_arbiter.load.valid': 'sim.loadmem_mem_ar_valid',
                 'read_arbiter.load.bits.addr': 'sim.loadmem_mem_ar_bits_addr',
                 'read_arbiter.fased': 'sim.fased_host_mem.ar', 'top.host_mem.ar': 'read_arbiter.out',
                 'sim.fased_host_mem.r': 'top.fased_host_mem.r'})
    assert wiring(wrapper) == nets, 'host AR and retained AW/W/B/R routes differ'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', wrapper) == [('sim', OLD), ('read_arbiter', HELPER)]
    assert new_modules[HELPER] == native_module(baseline, HELPER), 'shared AR arbiter differs'
    archive = before.splitlines()[1].replace('firrtl.circuit "' + OLD + '"', 'firrtl.circuit "' + TOP + '"').replace('~' + OLD, '~' + TOP)
    for _, n, _ in copied:
        archive = re.sub(r'\|' + OLD + r'>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, archive)
    archive = re.sub(r'\|' + OLD + r'>fased_host_mem\.r(?=[.\["\\])', '|' + TOP + '>fased_host_mem.r', archive)
    archive = re.sub(r'\|' + OLD + r'>host_mem_write\.', '|' + TOP + '>host_mem.', archive)
    assert archive == after.splitlines()[1], 'nested archive target transfer differs'
    assert len([n for n in old_modules if n.startswith('GGPrintBridge')]) == 21
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=277,
                copied_ports=111, top_ports=113, wrapper_connections=len(nets), identical_native_helpers=1,
                preserved_Print_modules=21, nested_archive_targets_preserved=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['AXI4Xbar'], SFC_contract=sfc_contract(a.sfc),
                  orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Expanded Print host AR composition only; Print production integration/runtime parity and host R composition remain pending.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AR arbitration/state/ID insertion; 277 prior bodies, 111 copied ports, 122 routes and nested targets preserved per order')


if __name__ == '__main__': main()
