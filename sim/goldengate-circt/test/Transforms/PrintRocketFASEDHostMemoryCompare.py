#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket pre-translation FASED memory assembly.

The immutable SFC disables Print. Compare its 35/64/4-bit local AXI boundary,
surviving ingress/egress nets, and response-error capture, not the later DRAM
translation or final platform memory ABI. Native assembly retains ready leaves
removed by SFC optimization and drives discarded response users as DontCare.
"""
import argparse
import json
import re
from pathlib import Path

from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, instance, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDControlCompare import transfer
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintRocketMasterCompare import native_module

OLD, NEW = 'GGControlMasterWrapper', 'GGFASEDHostMemoryWrapper'
ADDRESS = [('id', 4), ('qos', 4), ('prot', 3), ('cache', 4), ('lock', 1),
           ('burst', 2), ('size', 3), ('len', 8), ('addr', 35)]
DATA = [('strb', 8), ('last', 1), ('data', 64)]
WRITE = [('id', 4), ('resp', 2)]
READ = [('id', 4), ('last', 1), ('data', 64), ('resp', 2)]
CHANNELS = [('aw', ADDRESS), ('w', DATA), ('b', WRITE), ('ar', ADDRESS), ('r', READ)]
CONSUMED = {'fased_host_requests', 'fased_host_read_response', 'fased_host_write_response'}


def memory_type():
    def token(fields):
        payload = ', '.join(f'{name}: uint<{width}>' for name, width in fields)
        return 'bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<' + payload + '>>'
    return '!firrtl.bundle<' + ', '.join(
        ch + (' flip' if ch in ('b', 'r') else '') + ': ' + token(fields)
        for ch, fields in CHANNELS) + '>'


def memory_routes():
    nets, mapped, fields = {}, {}, {}
    for ch, bit_fields in CHANNELS:
        request = ch in ('aw', 'w', 'ar')
        boundary = 'fased_host_requests.' + ch if request else (
            'fased_host_read_response' if ch == 'r' else 'fased_host_write_response')
        for leaf, width in [('ready', 1), ('valid', 1)] + [('bits.' + n, w) for n, w in bit_fields]:
            outward = request != (leaf == 'ready')
            inside, outside = 'sim.' + boundary + '.' + leaf, 'top.fased_host_mem.' + ch + '.' + leaf
            nets[outside if outward else inside] = inside if outward else outside
            mapped[boundary + '.' + leaf] = 'fased_host_mem.' + ch + '.' + leaf
            fields[ch + '_' + leaf.replace('.', '_')] = ['output' if outward else 'input', width]
    for boundary in ('fased_host_read_response', 'fased_host_write_response'):
        nets['sim.' + boundary + '.bits.user'] = 'invalid<uint<1>>'
    return nets, mapped, fields


def memory_wiring(body):
    """Use the common SSA extractor, adding explicit InvalidValueOp sinks."""
    invalid = set(re.findall(r'(%\w+) = firrtl.invalidvalue : !firrtl.uint<1>', body))
    assert len(invalid) == 2, 'response users must each be an invalid UInt<1>'
    def invalid_connect(line):
        match = re.search(r'firrtl.strictconnect (%\w+), (%\w+)', line)
        return match is not None and match[2] in invalid
    filtered = '\n'.join(line for line in body.splitlines() if not invalid_connect(line))
    nets = wiring(filtered)
    values = {v: 'top.' + v[1:] for v in re.findall(r'%\w+(?=:)', body.splitlines()[0])}
    for line in body.splitlines()[1:]:
        if ' = firrtl.instance ' in line:
            left, right = line.split(' = firrtl.instance ', 1)
            values.update(zip(re.findall(r'%\w+', left),
                              (right.split()[0] + '.' + p for p in re.findall(r'\b(?:in|out) (\w+):', right))))
        if match := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            dest, src, field = match.groups()
            values[dest] = values[src] + '.' + field
        if match := re.search(r'firrtl.strictconnect (%\w+), (%\w+)', line):
            dest, src = match.groups()
            if src in invalid:
                assert values[dest] not in nets, 'duplicate invalid driver'
                nets[values[dest]] = 'invalid<uint<1>>'
    return nets


def sfc_contract(path, fields):
    reference = clean(path.read_text())
    header, body = module(reference, 'FASEDMemoryTimingModel')
    declared = {name[len('auto_to_host_dram_out_'):]: spec for name, spec in rtl_ports(header).items()
                if name.startswith('auto_to_host_dram_out_')}
    optimized = {'r_ready', 'b_ready'}
    assert declared == {name: spec for name, spec in fields.items() if name not in optimized}
    assert len(declared) == 35, 'SFC local memory ABI differs'
    eq = equations(body)
    pins = instance(body, 'IngressModule', 'ingress')
    checked = {}
    for ch in ('aw', 'w', 'ar'):
        for leaf in ('ready', 'valid', *('bits_' + n for n, _ in dict(CHANNELS)[ch])):
            inner = 'ingress_io_nastiOutputs_' + ch + '_' + leaf
            outside = 'auto_to_host_dram_out_' + ch + '_' + leaf
            assert pins['io_nastiOutputs_' + ch + '_' + leaf] == inner
            assert eq[inner if leaf == 'ready' else outside] == (outside if leaf == 'ready' else inner)
            checked[ch + '_' + leaf] = inner
    for ch, kind, name, leaves in [('r', 'ReadEgress', 'readEgress', ['valid', 'bits_id', 'bits_data', 'bits_last']),
                                  ('b', 'WriteEgress', 'writeEgress', ['valid', 'bits_id'])]:
        pins = instance(body, kind, name)
        for leaf in leaves:
            inner = name + '_io_enq_' + leaf
            outside = 'auto_to_host_dram_out_' + ch + '_' + leaf
            assert pins['io_enq_' + leaf] == inner and eq[inner] == outside
            checked[ch + '_' + leaf] = inner
        assert 'auto_to_host_dram_out_' + ch + '_bits_resp != 2\'h0 & auto_to_host_dram_out_' + ch + '_valid' in body
        checked[ch + '_bits_resp'] = ch + 'respError capture predicate'
    assert len(checked) == 35
    return declared, sorted(optimized), checked


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
    prior, result = (dict(re.findall(pattern, text, re.M | re.S)) for text in (before, after))
    assert len(prior) == 260 and set(result) - set(prior) == {NEW}
    assert all(result.get(name) == body for name, body in prior.items()), 'existing module body changed'
    old, new = native_module(before, OLD), native_module(after, NEW)
    old_ports = native_ports(old)
    assert len(old_ports) == 112 and CONSUMED <= {n for _, n, _ in old_ports}
    copied = [(d, n, t) for d, n, t in old_ports if n not in CONSUMED]
    assert len(copied) == 109
    assert native_ports(new) == copied + [('out', 'fased_host_mem', memory_type())]
    routes, mapped, fields = memory_routes()
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n
            for d, n, _ in copied}
    nets.update(routes)
    assert len(routes) == 39 and len(nets) == 148 and memory_wiring(new) == nets
    assert len(re.findall(r' = firrtl.instance sim @' + OLD + r'\b', new)) == 1
    assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', new), 'assembly duplicated state'
    if baseline is not None:
        shared = native_module(baseline, NEW)
        assert ('out', 'fased_host_mem', memory_type()) in native_ports(shared)
        shared_nets = memory_wiring(shared)
        assert all(shared_nets.get(key) == value for key, value in routes.items())
    header = before.splitlines()[1]
    probes = []
    assert after.splitlines()[1].count('class = "test.HostMemoryBoundary"') == 8, 'missing identity probes'
    if 'class = "test.HostMemoryBoundary"' in after.splitlines()[1]:
        probes = ['~' + OLD + '|' + OLD + '>' + ref for ref in (
            'ctrl.aw.bits.addr', 'fased_host_requests.aw.bits.addr',
            'fased_host_read_response.bits.data', 'fased_host_write_response.ready',
            'fased_host_requests.w.bits.id', 'fased_host_requests.aw.bits.user', 'fased_host_requests')]
        probes.append('~' + OLD)
        end = header.rfind(']} {')
        assert end >= 0 and 'class = "test.HostMemoryBoundary"' not in header
        header = header[:end] + ', ' + ', '.join(
            '{class = "test.HostMemoryBoundary", target = "' + target + '"}' for target in probes) + header[end:]
    for old_ref, new_ref in mapped.items():
        header = re.sub(re.escape('~' + OLD + '|' + OLD + '>' + old_ref) + r'(?=["\\])',
                        '~' + OLD + '|' + NEW + '>' + new_ref, header)
    header = transfer(header, OLD, NEW, [(d, n) for d, n, _ in copied])
    assert after.splitlines()[1] == header, 'annotation archive transfer differs'
    print_modules = [name for name in prior if name.startswith('GGPrintBridge')]
    assert len(print_modules) == 21 and 'GGPrintBridgeHostQueued' in print_modules
    regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', after)[1]
    print_regions = re.findall(r'name = "(PrintBridgeModule_\d+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64', regions)
    assert print_regions == [('PrintBridgeModule_0', '32', '8', '544'), ('PrintBridgeModule_1', '32', '9', '576')]
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(prior),
                copied_ports=len(copied), consumed_ports=sorted(CONSUMED), AXI_leaves=len(fields),
                invalid_response_users=2, wrapper_connections=len(nets), memory_connections=routes,
                retained_Print_modules=len(print_modules), retained_Print_regions=print_regions,
                annotation_archive_transfer=True, annotation_identity_probes=len(probes),
                shared_baseline_compared=baseline is not None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, action='append', required=True)
    parser.add_argument('--after', type=Path, action='append', required=True)
    parser.add_argument('--sfc', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--report-json', type=Path, required=True)
    args = parser.parse_args()
    assert len(args.before) == len(args.after)
    _, _, fields = memory_routes()
    declared, optimized, checked = sfc_contract(args.sfc.resolve(), fields)
    baseline = args.baseline.read_text() if args.baseline else None
    reports = [compare(before.resolve(), after.resolve(), baseline)
               for before, after in zip(args.before, args.after)]
    report = dict(golden=str(args.sfc.resolve()), SFC_module='FASEDMemoryTimingModel',
                  SFC_boundary='auto_to_host_dram_out', address_bits=35, data_bits=64, id_bits=4,
                  sfc_surviving_ports=declared, sfc_surviving_connections=checked,
                  sfc_optimized_response_ready_leaves=optimized, orders=reports,
                  limitation='SFC disables Print and removes two response-ready leaves. Comparison covers local pre-translation memory assembly; DRAM translation and Print-enabled driver/runtime parity remain pending.')
    args.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS 35 surviving SFC local AXI widths/directions and ingress/egress/error nets; '
          '37 native AXI leaves, two DontCare users, 148 wrapper connections, '
          '260 prior modules and annotation archive preserved')


if __name__ == '__main__':
    main()
