#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket host control binding with immutable SFC.

Check the native SSA nets, retained modules/ports/catalog and target transfer.
SFC disables Print; compare its shared 45-leaf host ABI and the 24 master
pins surviving SFC optimization, without claiming Print runtime parity.
"""
import argparse
import json
import re
from pathlib import Path

from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, instance, module
from PrintRocketFASEDControlCompare import transfer
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintRocketMasterCompare import native_module

OLD, NEW = 'GGFASEDBridgeBoundWrapper', 'GGControlMasterWrapper'
ADDRESS = [('addr', 25), ('len', 8), ('size', 3), ('burst', 2), ('lock', 1),
           ('cache', 4), ('prot', 3), ('qos', 4), ('region', 4), ('id', 12), ('user', 1)]
DATA = [('data', 32), ('last', 1), ('id', 12), ('strb', 4), ('user', 1)]
RESPONSE = [('resp', 2), ('id', 12), ('user', 1)]
READ = [('resp', 2), ('data', 32), ('last', 1), ('id', 12), ('user', 1)]
CHANNELS = [('aw', ADDRESS), ('w', DATA), ('b', RESPONSE), ('ar', ADDRESS), ('r', READ)]


def native_ports(body):
    return re.findall(r'\b(in|out) %(\w+): (.*?)(?=, (?:in|out) %|\) attributes |\) \{)',
                      body.splitlines()[0])


def control_type():
    def token(fields):
        bits = ', '.join(f'{name}: uint<{width}>' for name, width in fields)
        return 'bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<' + bits + '>>'
    return '!firrtl.bundle<' + ', '.join(
        ch + (' flip' if ch in ('b', 'r') else '') + ': ' + token(fields)
        for ch, fields in CHANNELS) + '>'


def master_routes():
    nets, fields = {}, {}
    for ch, bit_fields in CHANNELS:
        if ch == 'ar':
            nets['sim.ctrl_read_dispatch_master_ar'] = 'top.ctrl.ar'
            continue
        for leaf, width in [('ready', 1), ('valid', 1)] + [('bits.' + n, w) for n, w in bit_fields]:
            if ch in ('b', 'r'):
                prefix = 'ctrl_' + ('read' if ch == 'r' else 'write') + '_arb_out'
                boundary = prefix + '_' + leaf.replace('.', '_')
                is_input = leaf == 'ready'
            else:
                if leaf in ('ready', 'valid'):
                    boundary = f'ctrl_write_route_{ch}_{leaf}'
                elif ch == 'aw' and leaf == 'bits.addr':
                    boundary = 'ctrl_decode_aw_addr'
                elif ch == 'w' and leaf == 'bits.last':
                    boundary = 'ctrl_write_route_w_last'
                else:
                    boundary = f'ctrl_write_dispatch_master_{ch}_' + leaf.replace('.', '_')
                is_input = leaf != 'ready'
            ctrl, inner = 'top.ctrl.' + ch + '.' + leaf, 'sim.' + boundary
            nets[inner if is_input else ctrl] = ctrl if is_input else inner
            fields[boundary] = ('in' if is_input else 'out', '!firrtl.uint<' + str(width) + '>')
    return nets, fields


def sfc_contract(path):
    reference = clean(path.read_text())
    header, body = module(reference, 'FPGATop')
    declared = {n: p for n, p in rtl_ports(header).items() if re.match(r'ctrl_(aw|w|b|ar|r)_', n)}
    expected = {}
    for ch, fields in CHANNELS:
        for leaf, width in [('ready', 1), ('valid', 1)] + [('bits_' + n, w) for n, w in fields]:
            is_input = (leaf == 'ready') if ch in ('b', 'r') else (leaf != 'ready')
            expected[f'ctrl_{ch}_{leaf}'] = ['input' if is_input else 'output', width]
    assert declared == expected and len(declared) == 45, 'SFC host control ABI differs'
    eq = equations(body)
    pins = {k: v for k, v in instance(body, 'NastiRecursiveInterconnect', 'ctrlInterconnect').items()
            if k.startswith('io_masters_0_')}
    assert len(pins) == 24, 'SFC surviving master channel pins differ'
    for pin, wire in pins.items():
        leaf = pin[len('io_masters_0_'):]
        host = 'ctrl_' + leaf
        assert wire == 'ctrlInterconnect_' + pin
        if declared[host][0] == 'input':
            assert eq[wire] == host, f'SFC request field disconnected: {host}'
        else:
            assert eq[host] == wire, f'SFC response field disconnected: {host}'
    assert eq['ctrl_b_bits_user'] == "1'h0" and eq['ctrl_r_bits_user'] == "1'h0"
    return declared, pins


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
    prior, result = (dict(re.findall(pattern, text, re.M | re.S)) for text in (before, after))
    assert set(result) - set(prior) == {NEW}, 'unexpected added modules'
    assert set(prior) <= set(result), 'prior module removed'
    assert all(result[name] == body for name, body in prior.items()), 'existing module body changed'
    old, new = native_module(before, OLD), native_module(after, NEW)
    old_ports = native_ports(old)
    routes, scalar_fields = master_routes()
    assert len(scalar_fields) == 32
    old_by_name = {n: (d, t) for d, n, t in old_ports}
    assert all(old_by_name[name] == spec for name, spec in scalar_fields.items())
    ar_type = '!firrtl.' + control_type().split('aw: ', 1)[1].split(', w:', 1)[0]
    assert old_by_name['ctrl_read_dispatch_master_ar'] == ('in', ar_type)
    consumed = set(scalar_fields) | {'ctrl_read_dispatch_master_ar'}
    copied = [(d, n, t) for d, n, t in old_ports if n not in consumed]
    assert native_ports(new) == copied + [('in', 'ctrl', control_type())], 'copied boundary or full control type changed'
    forbidden = r'(?:.*_ctrl$|ctrl_(?:read|write)_dispatch_slave_|ctrl_(?:read|write)_arb_in_)'
    assert not any(re.match(forbidden, n) for _, n, _ in copied), 'unbound widget control exposed'
    nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n
            for d, n, _ in copied}
    nets.update(routes)
    assert wiring(new) == nets, 'actual host control SSA wiring differs'
    assert len(re.findall(r' = firrtl.instance sim @' + OLD + r'\b', new)) == 1
    assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', new), 'host binding duplicated state'
    if baseline is not None:
        shared = native_module(baseline, NEW)
        shared_nets = wiring(shared)
        assert all(shared_nets.get(key) == value for key, value in routes.items()), 'shared baseline master nets differ'
        assert ('in', 'ctrl', control_type()) in native_ports(shared)
    before_header = before.splitlines()[1]
    # The replay adds four identity probes immediately before invoking the
    # native composer. Preserve the input archive exactly, then require every
    # probe's copied/internalized/circuit target to obey the same transfer.
    probes = []
    if 'class = "test.MasterBoundary"' in after.splitlines()[1]:
        probes = ['~' + OLD + '|' + OLD + '>other',
                  '~' + OLD + '|' + OLD + '>ctrl_read_dispatch_master_ar.bits.addr',
                  '~' + OLD + '|' + OLD + '>ctrl_write_dispatch_master_w_bits_data', '~' + OLD]
        end = before_header.rfind(']} {')
        assert end >= 0 and 'class = "test.MasterBoundary"' not in before_header
        extra = ', '.join('{class = "test.MasterBoundary", target = "' + target + '"}'
                          for target in probes)
        before_header = before_header[:end] + ', ' + extra + before_header[end:]
    expected_header = transfer(before_header, OLD, NEW, [(d, n) for d, n, _ in copied])
    assert after.splitlines()[1] == expected_header, 'annotation archive transfer differs'
    print_modules = [name for name in prior if name.startswith('GGPrintBridge')]
    assert len(print_modules) >= 2 and 'GGPrintBridgeHostQueued' in print_modules
    regions = re.search(r'goldengate.controlRegions = \[(.*?)\]', after)[1]
    print_regions = re.findall(r'name = "(PrintBridgeModule_\d+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64', regions)
    assert print_regions == [('PrintBridgeModule_0', '32', '8', '544'),
                             ('PrintBridgeModule_1', '32', '9', '576')]
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=len(prior),
                copied_ports=len(copied), consumed_ports=len(consumed), scalar_master_bindings=32,
                aggregate_AR_bindings=1, master_connections=routes, wrapper_connections=len(nets),
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
    assert len(args.before) == len(args.after), 'one --after is required per --before'
    control, surviving = sfc_contract(args.sfc.resolve())
    baseline = args.baseline.read_text() if args.baseline else None
    reports = [compare(before.resolve(), after.resolve(), baseline)
               for before, after in zip(args.before, args.after)]
    report = dict(golden=str(args.sfc.resolve()), SFC_module='FPGATop', control_ports=control,
                  sfc_surviving_master_pins=surviving, orders=reports,
                  sfc_constant_response_users={'ctrl_b_bits_user': "1'h0", 'ctrl_r_bits_user': "1'h0"},
                  sfc_optimized_input_leaves=sorted(name for name, spec in control.items()
                      if spec[0] == 'input' and 'io_masters_0_' + name[len('ctrl_'):] not in surviving),
                  limitation='SFC disables Print and optimizes unused request fields; response users are tied zero. Print driver/runtime parity remains pending.')
    args.report_json.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS 45 SFC host control widths/directions, 24 surviving master pins, '
          '32 native scalar + aggregate AR bindings; prior modules, copied ports and annotations preserved')


if __name__ == '__main__':
    main()
