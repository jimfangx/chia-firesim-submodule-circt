#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print host AW/W arbitration with immutable SFC boundaries.

The C++ SSA interpreter checks the SFC equations on 12,001 edges. This check
anchors that oracle to recorded RTL and checks every composition route, retained
module, public interface and archive target against the prior boundary.
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

OLD, TOP = 'GGPrintLoadMemWrapper', 'GGHostMemoryWriteWrapper'
HELPERS = ['GGHostMemoryWriteArbiter', 'GGHostMemoryWriteSourceQueue2']
CONSUMED = ['loadmem_mem_' + x for x in ('aw_ready', 'aw_valid', 'aw_bits_addr', 'aw_bits_len',
                                      'w_ready', 'w_valid', 'w_bits_data', 'w_bits_last')]


def sfc_contract(path, queue_name="Queue_21"):
    rtl = clean(path.read_text())
    header, body = module(rtl, 'AXI4Xbar'); nets = equations(body)
    want = {
        'auto_anon_out_aw_valid': 'out_0_aw_valid & (latched | awOut_0_io_enq_ready)',
        'out_0_aw_ready': 'auto_anon_out_aw_ready & _anonOut_aw_valid_T',
        '_anonOut_aw_valid_T': 'latched | awOut_0_io_enq_ready',
        'awOut_0_io_enq_valid': 'out_0_aw_valid & ~latched',
        'awOut_0_io_enq_bits': '{awOut_0_io_enq_bits_muxState_1,awOut_0_io_enq_bits_muxState_0}',
        'awOut_0_io_deq_ready': 'out_0_w_valid & out_0_w_bits_last & auto_anon_out_w_ready',
        'auto_anon_out_w_valid': 'out_0_w_valid & awOut_0_io_deq_valid',
        'out_0_w_ready': 'auto_anon_out_w_ready & awOut_0_io_deq_valid',
        'awOut_0_clock': 'clock', 'awOut_0_reset': 'reset',
        '_T': 'awOut_0_io_enq_ready & awOut_0_io_enq_valid', '_GEN_0': '_T | latched',
        '_T_1': 'out_0_aw_ready & out_0_aw_valid',
        'awOut_0_io_enq_bits_anyValid': 'auto_anon_in_0_aw_valid | auto_anon_in_1_aw_valid',
        'out_0_aw_valid': 'awOut_0_io_enq_bits_idle ? awOut_0_io_enq_bits_anyValid : _awOut_0_io_enq_bits_out_0_aw_valid_T_2',
        '_awOut_0_io_enq_bits_out_0_aw_valid_T_2': 'awOut_0_io_enq_bits_state_0 & auto_anon_in_0_aw_valid | awOut_0_io_enq_bits_state_1 & auto_anon_in_1_aw_valid',
        '_GEN_3': "awOut_0_io_enq_bits_anyValid ? 1'h0 : awOut_0_io_enq_bits_idle", '_GEN_4': '_T_1 | _GEN_3',
        'awOut_0_io_enq_bits_readys_valid': '{auto_anon_in_1_aw_valid,auto_anon_in_0_aw_valid}',
        '_awOut_0_io_enq_bits_readys_filter_T': '~awOut_0_io_enq_bits_readys_mask',
        '_awOut_0_io_enq_bits_readys_filter_T_1': 'awOut_0_io_enq_bits_readys_valid & _awOut_0_io_enq_bits_readys_filter_T',
        'awOut_0_io_enq_bits_readys_filter': '{_awOut_0_io_enq_bits_readys_filter_T_1,auto_anon_in_1_aw_valid, auto_anon_in_0_aw_valid}',
        '_GEN_16': "{{1'd0}, awOut_0_io_enq_bits_readys_filter[3:1]}",
        '_awOut_0_io_enq_bits_readys_unready_T_1': 'awOut_0_io_enq_bits_readys_filter | _GEN_16',
        '_awOut_0_io_enq_bits_readys_unready_T_4': "{awOut_0_io_enq_bits_readys_mask, 2'h0}",
        '_GEN_17': "{{1'd0}, _awOut_0_io_enq_bits_readys_unready_T_1[3:1]}",
        'awOut_0_io_enq_bits_readys_unready': '_GEN_17 | _awOut_0_io_enq_bits_readys_unready_T_4',
        '_awOut_0_io_enq_bits_readys_readys_T_2': 'awOut_0_io_enq_bits_readys_unready[3:2] & awOut_0_io_enq_bits_readys_unready[1:0]',
        'awOut_0_io_enq_bits_readys_readys': '~_awOut_0_io_enq_bits_readys_readys_T_2',
        '_awOut_0_io_enq_bits_readys_mask_T': 'awOut_0_io_enq_bits_readys_readys & awOut_0_io_enq_bits_readys_valid',
        '_awOut_0_io_enq_bits_readys_mask_T_1': "{_awOut_0_io_enq_bits_readys_mask_T, 1'h0}",
        '_awOut_0_io_enq_bits_readys_mask_T_3': '_awOut_0_io_enq_bits_readys_mask_T | _awOut_0_io_enq_bits_readys_mask_T_1[1:0]',
        'in_1_aw_bits_id': "{{1'd0}, auto_anon_in_1_aw_bits_id}",
        '_awOut_0_io_enq_bits_out_0_aw_bits_T_24': "awOut_0_io_enq_bits_muxState_0 ? 5'h10 : 5'h0",
        '_awOut_0_io_enq_bits_out_0_aw_bits_T_15': "awOut_0_io_enq_bits_muxState_0 ? 3'h3 : 3'h0",
        '_awOut_0_io_enq_bits_out_0_aw_bits_T_12': "awOut_0_io_enq_bits_muxState_0 ? 2'h1 : 2'h0",
        '_out_0_w_bits_T_5': "awOut_0_io_deq_bits[0] ? 8'hff : 8'h0",
    }
    for i in (0, 1):
        want[f'auto_anon_in_{i}_aw_ready'] = f'out_0_aw_ready & awOut_0_io_enq_bits_allowed_{i}'
        want[f'auto_anon_in_{i}_w_ready'] = f'out_0_w_ready & awOut_0_io_deq_bits[{i}]'
        want[f'awOut_0_io_enq_bits_winner_{i}'] = f'awOut_0_io_enq_bits_readys_{i} & auto_anon_in_{i}_aw_valid'
        want[f'awOut_0_io_enq_bits_muxState_{i}'] = f'awOut_0_io_enq_bits_idle ? awOut_0_io_enq_bits_winner_{i} : awOut_0_io_enq_bits_state_{i}'
        want[f'awOut_0_io_enq_bits_allowed_{i}'] = f'awOut_0_io_enq_bits_idle ? awOut_0_io_enq_bits_readys_{i} : awOut_0_io_enq_bits_state_{i}'
        want[f'awOut_0_io_enq_bits_readys_{i}'] = f'awOut_0_io_enq_bits_readys_readys[{i}]'
    for field, width, number, load in [('burst', 2, 12, "2'h1"), ('size', 3, 15, "3'h3"),
                                       ('len', 8, 18, 'auto_anon_in_0_aw_bits_len'),
                                       ('addr', 34, 21, 'auto_anon_in_0_aw_bits_addr'), ('id', 5, 24, "5'h10")]:
        prefix = '_awOut_0_io_enq_bits_out_0_aw_bits_T_'
        fased = 'in_1_aw_bits_id' if field == 'id' else 'auto_anon_in_1_aw_bits_' + field
        for i, value in enumerate([load, fased]):
            want[prefix + str(number + i)] = f"awOut_0_io_enq_bits_muxState_{i} ? {value} : {width}'h0"
        want['auto_anon_out_aw_bits_' + field] = prefix + str(number) + ' | ' + prefix + str(number + 1)
    for field, width in [('lock', 1), ('cache', 4), ('prot', 3), ('qos', 4)]:
        want['auto_anon_out_aw_bits_' + field] = ('awOut_0_io_enq_bits_muxState_1 & auto_anon_in_1_aw_bits_lock'
            if width == 1 else f"awOut_0_io_enq_bits_muxState_1 ? auto_anon_in_1_aw_bits_{field} : {width}'h0")
    last = 'awOut_0_io_deq_bits[0] & auto_anon_in_0_w_bits_last | awOut_0_io_deq_bits[1] & auto_anon_in_1_w_bits_last'
    want.update(out_0_w_bits_last=last, auto_anon_out_w_bits_last=last,
                out_0_w_valid=last.replace('_bits_last', '_valid'))
    for field, width, number, load in [('strb', 8, 5, "8'hff"), ('data', 64, 8, 'auto_anon_in_0_w_bits_data')]:
        for i, value in enumerate([load, 'auto_anon_in_1_w_bits_' + field]):
            want['_out_0_w_bits_T_' + str(number + i)] = f"awOut_0_io_deq_bits[{i}] ? {value} : {width}'h0"
        want['auto_anon_out_w_bits_' + field] = f'_out_0_w_bits_T_{number} | _out_0_w_bits_T_{number + 1}'
    normalize = lambda x: re.sub(r'\s+', '', x)
    for k, v in want.items(): assert normalize(nets[k]) == normalize(v), (k, nets[k], v)
    sequential = normalize(body[body.index('always @(posedge clock)'):])
    expected_state = ["if (reset) begin latched <= 1'h0; end else if (_T_1) begin latched <= 1'h0; end else begin latched <= _GEN_0; end",
                      'awOut_0_io_enq_bits_idle <= reset | _GEN_4;',
                      "if (reset) begin awOut_0_io_enq_bits_readys_mask <= 2'h3; end else if (awOut_0_io_enq_bits_idle & |awOut_0_io_enq_bits_readys_valid) begin awOut_0_io_enq_bits_readys_mask <= _awOut_0_io_enq_bits_readys_mask_T_3; end"]
    for i in (0, 1):
        expected_state.append(f"if (reset) begin awOut_0_io_enq_bits_state_{i} <= 1'h0; end else if (awOut_0_io_enq_bits_idle) begin awOut_0_io_enq_bits_state_{i} <= awOut_0_io_enq_bits_winner_{i}; end")
    for state in expected_state: assert normalize(state) in sequential, 'SFC write arbitration state differs'
    assert re.findall(r'(Queue\w*)\s+awOut_0\s*\(', body) == [queue_name], 'SFC source queue instance differs'
    qheader, qbody = module(rtl, queue_name); qnets = equations(qbody)
    qwant = {'ptr_match': 'enq_ptr_value == deq_ptr_value', 'empty': 'ptr_match & ~maybe_full',
             'full': 'ptr_match & maybe_full', 'io_enq_ready': '~full', 'io_deq_valid': 'io_enq_valid | ~empty',
             'io_deq_bits': 'empty ? io_enq_bits : ram_io_deq_bits_MPORT_data',
             '_do_enq_T': 'io_enq_ready & io_enq_valid', '_do_deq_T': 'io_deq_ready & io_deq_valid',
             '_GEN_12': "io_deq_ready ? 1'h0 : _do_enq_T", 'do_enq': 'empty ? _GEN_12 : _do_enq_T',
             'do_deq': "empty ? 1'h0 : _do_deq_T", 'ram_MPORT_en': 'empty ? _GEN_12 : _do_enq_T',
             'ram_io_deq_bits_MPORT_en': "1'h1", 'ram_io_deq_bits_MPORT_addr': 'deq_ptr_value',
             'ram_io_deq_bits_MPORT_data': 'ram[ram_io_deq_bits_MPORT_addr]', 'ram_MPORT_data': 'io_enq_bits',
             'ram_MPORT_addr': 'enq_ptr_value', 'ram_MPORT_mask': "1'h1"}
    for k, v in qwant.items(): assert normalize(qnets[k]) == normalize(v), (k, qnets[k], v)
    assert re.search(r'reg \[1:0\] ram \[0:1\]', qbody)
    assert re.search(r'if \(ram_MPORT_en & ram_MPORT_mask\) begin\s*ram\[ram_MPORT_addr\] <= ram_MPORT_data;', qbody)
    for n in ('enq_ptr_value', 'deq_ptr_value', 'maybe_full'):
        assert re.search(r'if \(reset\) begin\s*' + n + r" <= 1'h0;", qbody)
    queue_state = [f"if (reset) begin {n}_ptr_value <= 1'h0; end else if (do_{n}) begin {n}_ptr_value <= {n}_ptr_value + 1'h1; end"
                   for n in ('enq', 'deq')]
    queue_state.append("if (reset) begin maybe_full <= 1'h0; end else if (do_enq != do_deq) begin if (empty) begin if (io_deq_ready) begin maybe_full <= 1'h0; end else begin maybe_full <= _do_enq_T; end end else begin maybe_full <= _do_enq_T; end end")
    for state in queue_state: assert normalize(state) in normalize(qbody), 'SFC source queue state differs'
    portmap = rtl_ports(header)
    assert portmap['auto_anon_out_aw_bits_id'] == ['output', 5]
    assert portmap['auto_anon_in_1_aw_bits_id'] == ['input', 4]
    assert portmap['auto_anon_out_aw_bits_addr'] == ['output', 34]
    assert portmap['auto_anon_out_w_bits_data'] == ['output', 64]
    return dict(AXI4Xbar_equations=len(want), arbitration_state_updates=len(expected_state),
                source_queue=queue_name, source_queue_equations=len(qwant),
                source_queue_state_updates=len(queue_state),
                reservation_before_AW=True, accepted_WLAST_retirement=True, queue_depth=2, flow=True, pipe=False,
                LoadMem_ID=16, FASED_IDs=[0, 15], reset_independent_RAM_write=True)


def compare_binding(before_path, after_path, baseline, old_top="GGFASEDAddressTranslationWrapper"):
    before, after = before_path.read_text(), after_path.read_text()
    def modules(text):
        found = list(re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S))
        result = {m[1]: m[0] for m in found}
        assert len(result) == len(found), 'duplicate module definitions'
        return result
    old_modules, new_modules = modules(before), modules(after)
    prior = list(old_modules)
    for n in prior: assert old_modules[n] == new_modules[n], 'changed prior body ' + n
    assert not set(prior) & {TOP, *HELPERS}, 'write arbitration boundary already exists'
    assert set(new_modules) == set(prior) | {TOP, *HELPERS}
    wrapper = new_modules[TOP]; old_ports = native_ports(old_modules[old_top])
    copied = [p for p in old_ports if p[1] not in CONSUMED + ['fased_host_mem']]
    ports = native_ports(wrapper)
    assert ports[:len(copied)] == copied and len(ports) == len(copied) + 2
    master = next(t for _, n, t in old_ports if n == 'fased_host_mem')
    rest = '!firrtl.bundle<' + master.split(', b flip: ', 1)[1]
    rest = rest.replace('!firrtl.bundle<', '!firrtl.bundle<b flip: ', 1)
    write = master.split(', b flip: ', 1)[0] + '>'
    write = write.replace('id: uint<4>', 'id: uint<5>')
    assert ports[len(copied):] == [('out', 'fased_host_mem', rest), ('out', 'host_mem_write', write)]
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets.update({'write_arbiter.clock': 'top.hostClock', 'write_arbiter.reset': 'top.hostReset',
                 'write_arbiter.fased.aw': 'sim.fased_host_mem.aw', 'write_arbiter.fased.w': 'sim.fased_host_mem.w',
                 'top.fased_host_mem.ar': 'sim.fased_host_mem.ar', 'sim.fased_host_mem.b': 'top.fased_host_mem.b',
                 'sim.fased_host_mem.r': 'top.fased_host_mem.r', 'top.host_mem_write': 'write_arbiter.out'})
    for d, n, _ in old_ports:
        if n in CONSUMED:
            peer = 'write_arbiter.load.' + n.removeprefix('loadmem_mem_').replace('_', '.', 1).replace('bits_', 'bits.')
            inner = 'sim.' + n
            nets[inner if d == 'in' else peer] = peer if d == 'in' else inner
    assert wiring(wrapper) == nets, 'host AW/W and retained B/AR/R routes differ'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', wrapper) == [('sim', old_top), ('write_arbiter', HELPERS[0])]
    for n in HELPERS: assert new_modules[n] == native_module(baseline, n), 'shared helper differs ' + n
    archive = before.splitlines()[1].replace('firrtl.circuit "' + old_top + '"', 'firrtl.circuit "' + TOP + '"').replace('~' + old_top, '~' + TOP)
    for _, n, _ in copied:
        archive = re.sub(r'\|' + old_top + r'>' + re.escape(n) + r'(?=[.\["\\])', '|' + TOP + '>' + n, archive)
    archive = re.sub(r'\|' + old_top + r'>fased_host_mem\.(b|ar|r)(?=[.\["\\])', '|' + TOP + r'>fased_host_mem.\1', archive)
    assert archive == after.splitlines()[1], 'nested archive target transfer differs'
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(prior),
                copied_ports=len(copied), top_ports=len(ports), wrapper_connections=len(nets), identical_native_helpers=2,
                preserved_Print_modules=sum(n.startswith("GGPrintBridge") for n in prior), nested_archive_targets_preserved=True)


def compare(before_path, after_path, baseline):
    report = compare_binding(before_path, after_path, baseline, OLD)
    assert report["unchanged_module_bodies"] == 272
    assert report["copied_ports"] == 116 and report["top_ports"] == 118
    assert report["preserved_Print_modules"] == 21
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('sfc', 'baseline', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    report = dict(golden=str(a.sfc.resolve()), SFC_modules=['AXI4Xbar', 'Queue_21'],
                  SFC_contract=sfc_contract(a.sfc), orders=[compare(x, y, a.baseline.read_text()) for x, y in zip(a.before, a.after)],
                  limitation='Only expanded AW/W composition; Print production integration and runtime parity remain pending. B/AR/R retain prior boundaries.')
    a.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC host write reservations/IDs/WLAST/source queue; 272 prior bodies, 116 copied ports, 132 routes, shared SSA helpers and nested targets preserved per order')


if __name__ == '__main__': main()
