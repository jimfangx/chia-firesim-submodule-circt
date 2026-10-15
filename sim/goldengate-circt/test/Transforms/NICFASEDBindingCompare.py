#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production FASED slave binding with archived SFC platform wiring.

This boundary is stateless: check every typed port and SSA connection, the
catalog-selected slot, all surviving SFC channel pins and recursive annotation
transfer. It does not establish NIC host-master or platform/runtime parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from NICFASEDMMIOBankCompare import oracle
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import control_type, native_ports
from PrintRocketFASEDControlCompare import ADDRESS, DATA, READ, RESPONSE, transfer
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer as transfer_annotations

OLD, NEW = 'GGFASEDBridgeControlWrapper', 'GGFASEDBridgeBoundWrapper'


def routes(copied, slave):
    nets = {('sim.' if d == 'in' else 'top.') + n:
            ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
    nets['sim.fasedBridge_ctrl.ar'] = f'sim.ctrl_read_dispatch_slave_{slave}_ar'
    for ch, fields in [('aw', ADDRESS), ('w', DATA)]:
        prefix = f'sim.ctrl_write_dispatch_slave_{slave}_{ch}'
        nets[prefix + '_ready'] = f'sim.fasedBridge_ctrl.{ch}.ready'
        nets[f'sim.fasedBridge_ctrl.{ch}.valid'] = prefix + '_valid'
        for field in fields:
            local = field in (['addr', 'len', 'id'] if ch == 'aw' else ['data', 'last'])
            source = prefix if local else 'sim.ctrl_write_dispatch_master_' + ch
            nets[f'sim.fasedBridge_ctrl.{ch}.bits.{field}'] = source + '_bits_' + field
    for ch, fields in [('r', READ), ('b', RESPONSE)]:
        prefix = f'sim.ctrl_{"read" if ch == "r" else "write"}_arb_in_{slave}'
        nets[f'sim.fasedBridge_ctrl.{ch}.ready'] = prefix + '_ready'
        nets[prefix + '_valid'] = f'sim.fasedBridge_ctrl.{ch}.valid'
        for field in fields:
            nets[prefix + '_bits_' + field] = f'sim.fasedBridge_ctrl.{ch}.bits.{field}'
    return nets


def composition(directory, abi):
    before = (directory / 'post-fame-fased-control.mlir').read_text()
    after = (directory / 'post-fame-fased-bound.mlir').read_text()
    prior, result = modules(before), modules(after)
    assert set(result) - set(prior) == {NEW} and set(prior) <= set(result)
    assert all(result[n] == body for n, body in prior.items()), 'existing module changed'
    old, new = prior[OLD], result[NEW]
    ports = native_ports(old)
    slave = abi['slave']
    consumed = {'fasedBridge_ctrl', f'ctrl_read_dispatch_slave_{slave}_ar'}
    for ch, fields in [('aw', ['addr', 'len', 'id']), ('w', ['data', 'last'])]:
        prefix = f'ctrl_write_dispatch_slave_{slave}_{ch}'
        consumed.update(prefix + '_' + n for n in ['ready', 'valid'] + ['bits_' + f for f in fields])
    for ch, fields in [('r', READ), ('b', RESPONSE)]:
        prefix = f'ctrl_{"read" if ch == "r" else "write"}_arb_in_{slave}'
        consumed.update(prefix + '_' + n for n in ['ready', 'valid'] + ['bits_' + f for f in fields])
    assert len(consumed) == 23 and consumed <= {n for _, n, _ in ports}
    assert {n: (d, t) for d, n, t in ports}['fasedBridge_ctrl'] == ('in', control_type())
    copied = [p for p in ports if p[1] not in consumed]
    assert native_ports(new) == copied, 'copied port type, direction or order changed'
    assert f'goldengate.fasedSlave = {slave} : i32' in new.splitlines()[0]
    expected = routes(copied, slave)
    assert len(expected) == len(copied) + 33
    assert wiring(new) == expected, 'five-channel FASED binding differs'
    assert re.search(r'firrtl.instance sim @' + OLD + r'\b', new)
    assert not re.search(r'firrtl\.(?:reg|regreset|mem)\b', new), 'binding introduced state'
    for _, name, _ in ports:
        if name.startswith('ctrl_write_dispatch_master_') or 'tracker' in name:
            assert name not in consumed, 'shared metadata/tracker boundary consumed'
    copied_pairs = [(d, n) for d, n, _ in copied]
    assert after.splitlines()[1] == transfer(before.splitlines()[1], OLD, NEW, copied_pairs)
    old_annotations = json.loads((directory / 'post-fame-fased-control-all.json').read_text())
    annotations = json.loads((directory / 'post-fame-fased-bound-all.json').read_text())
    assert annotations == transfer_annotations(old_annotations, OLD, NEW, {n for _, n, _ in copied})
    return dict(unchanged_modules=len(prior), copied_typed_ports=len(copied),
                consumed_ports=len(consumed), connections=len(expected),
                scalar_channel_connections=32, aggregate_AR_leaves=13,
                annotation_records=len(annotations),
                catalog_regions=len(re.findall(r'name = "[^"]+", size = \d+ : i64, slave = ',
                                               prior['GGControlAddressDecode'].splitlines()[0]))), expected


def sfc_binding(golden, abi):
    reference = clean(golden.read_text())
    header, _ = module(reference, 'FASEDMemoryTimingModel')
    eq = equations(module(reference, 'FPGATop')[1])
    router = module(reference, 'NastiRouter')[1]
    assert abi['base'] == 128 and abi['size'] == 128 and abi['slave'] == 1
    for ch in ('aw', 'ar'):
        assert f"25'h80 <= io_master_{ch}_bits_addr & io_master_{ch}_bits_addr < 25'h100" in router
    pins = re.findall(r'\b(io_ctrl_\w+)\b', header)
    assert len(pins) == 20
    channels = set()
    for pin in pins:
        ch, field = pin[len('io_ctrl_'):].split('_', 1)
        channels.add(ch)
        widget = 'FASEDMemoryTimingModel_0_' + pin
        interconnect = f'ctrlInterconnect_io_slaves_{abi["slave"]}_{ch}_{field}'
        assert eq.get(widget) == interconnect or eq.get(interconnect) == widget, pin
    assert channels == {'aw', 'w', 'b', 'ar', 'r'}
    return dict(surviving_control_pins=len(pins), channels=sorted(channels),
                half_open_address_range=[abi['base'], abi['base'] + abi['size']])


def mutation(directory, abi, output, verifier):
    """Verify a real, type-correct crosswire is rejected by the route check."""
    output.mkdir(parents=True, exist_ok=False)
    for name in ('post-fame-fased-control.mlir', 'post-fame-fased-control-all.json',
                 'post-fame-fased-bound-all.json'):
        (output / name).symlink_to(directory / name)
    text = (directory / 'post-fame-fased-bound.mlir').read_text()
    body = modules(text)[NEW]
    changed, count = re.subn(
        r'(firrtl.strictconnect %\w+, )%sim_ctrl_write_dispatch_slave_' + str(abi['slave']) +
        r'_aw_bits_id( : !firrtl.uint<12>)',
        r'\1%sim_ctrl_write_dispatch_master_w_bits_id\2', body)
    assert count == 1, 'AW ID mutation did not find one actual connection'
    candidate = output / 'post-fame-fased-bound.mlir'
    candidate.write_text(text.replace(body, changed, 1))
    with (output / 'verify.log').open('w') as log:
        subprocess.run([str(verifier), '--verify-ir', str(candidate)], check=True,
                       stdout=log, stderr=subprocess.STDOUT)
    try:
        composition(output, abi)
    except AssertionError as error:
        assert str(error) == 'five-channel FASED binding differs', str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct AW ID crosswire escaped comparison')
    return dict(verified_by=str(verifier) + ' --verify-ir',
                crosswire='AW ID from shared W ID', caught=True, artifact=str(candidate),
                sha256=hashlib.sha256(candidate.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'golden', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--header', type=Path)
    parser.add_argument('--baseline-dir', type=Path)
    parser.add_argument('--mutation-dir', type=Path)
    parser.add_argument('--verify-tool', type=Path)
    args = parser.parse_args()
    if args.mutation_dir and not args.verify_tool:
        parser.error('--mutation-dir requires --verify-tool')
    directory = args.candidate_dir.resolve()
    boundary = directory / 'post-fame-fased-bound.mlir'
    abi = oracle(args.golden.resolve(), boundary, args.header)
    report, nets = composition(directory, abi)
    report.update(abi=abi, sfc_binding=sfc_binding(args.golden, abi), boundary=str(boundary),
                  sha256=hashlib.sha256(boundary.read_bytes()).hexdigest())
    if args.baseline_dir:
        baseline = modules((args.baseline_dir / boundary.name).read_text())[NEW]
        shared = wiring(baseline)
        channel_nets = {k: v for k, v in nets.items() if not (k.startswith('top.') or v.startswith('top.'))}
        assert len(channel_nets) == 33 and all(shared.get(k) == v for k, v in channel_nets.items())
        report['baseline_channel_connections_matched'] = len(channel_nets)
    if args.mutation_dir:
        report['mutation'] = mutation(directory, abi, args.mutation_dir.resolve(), args.verify_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'abi'}, indent=2))
    print('PASS production FASED slave binding / SFC five-channel wiring; platform integration remains open')


if __name__ == '__main__':
    main()
