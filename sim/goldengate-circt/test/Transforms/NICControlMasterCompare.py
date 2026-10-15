#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production NIC host control master with SFC FPGATop.

The composer is stateless. Check all typed ports and SSA nets, both annotation
representations, and SFC's full host ABI and surviving interconnect pins.
Host-memory integration and NIC runtime are separate boundaries.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from NICMMIOBankCompare import modules
from PrintRocketControlMasterCompare import OLD, NEW, compare_binding, native_ports, sfc_contract
from PrintSelectedFASEDTimingCycleCompare import transfer

BEFORE, AFTER = 'post-fame-fased-bound', 'post-fame-control-master'


def composition(directory, baseline=None):
    before, after = [directory / (name + '.mlir') for name in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior = modules(before.read_text())
    result = modules(after.read_text())[NEW]
    copied = {name for _, name, _ in native_ports(result)} - {'ctrl'}
    previous = json.loads((directory / (BEFORE + '-all.json')).read_text())
    annotations = json.loads((directory / (AFTER + '-all.json')).read_text())
    assert annotations == transfer(previous, OLD, NEW, copied), 'emitted annotation transfer differs'
    # The catalog is copied with the inner module body by compare_binding;
    # require an actual allocation, including NIC, rather than a tiny probe.
    catalog = prior['GGControlAddressDecode'].splitlines()[0]
    regions = re.findall(r'name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64', catalog)
    assert len(regions) == 12
    assert ('SimpleNICBridgeModule_0', '32', '7', '512') in regions
    report.update(annotation_records=len(annotations), full_control_leaves=45,
                  retained_control_regions=regions,
                  sha256=hashlib.sha256(after.read_bytes()).hexdigest())
    return report


def mutation(directory, output, verifier):
    """A type-correct AR bundle from AW must fail the route comparison."""
    output.mkdir(parents=True, exist_ok=False)
    for name in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / name).symlink_to(directory / name)
    original = (directory / (AFTER + '.mlir')).read_text()
    body = modules(original)[NEW]
    # Resolve fields by SSA names rather than assuming printer numbering.
    aw = re.search(r'(%\w+) = firrtl.subfield %ctrl\[aw\]', body)
    assert aw, 'missing AW bundle projection'
    changed, count = re.subn(r'(firrtl.connect %sim_ctrl_read_dispatch_master_ar, )%\w+( : )',
                            lambda m: m[1] + aw[1] + m[2], body)
    assert count == 1 and changed != body, 'AR bundle mutation missed actual connection'
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(body, changed, 1))
    with (output / 'verify.log').open('w') as log:
        subprocess.run([str(verifier), '--verify-ir', str(candidate)], check=True,
                       stdout=log, stderr=subprocess.STDOUT)
    try:
        composition(output)
    except AssertionError as error:
        assert str(error) == 'actual host control SSA wiring differs', str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct AW/AR crosswire escaped comparison')
    return dict(verified_by=str(verifier) + ' --verify-ir', caught=True,
                crosswire='AR bundle from AW bundle', artifact=str(candidate),
                sha256=hashlib.sha256(candidate.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'golden', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--mutation-dir', type=Path)
    parser.add_argument('--verify-tool', type=Path)
    args = parser.parse_args()
    if args.mutation_dir and not args.verify_tool:
        parser.error('--mutation-dir requires --verify-tool')
    directory = args.candidate_dir.resolve()
    control, pins = sfc_contract(args.golden.resolve())
    report = composition(directory, args.baseline.read_text() if args.baseline else None)
    report.update(golden=str(args.golden.resolve()), control_ports=control,
                  sfc_surviving_master_pins=pins,
                  limitation='Stateless host control binding only; NIC host-memory/platform/runtime remain pending.')
    if args.mutation_dir:
        report['mutation'] = mutation(directory, args.mutation_dir.resolve(), args.verify_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print('PASS NIC host control master / SFC ABI and interconnect; platform integration remains open')


if __name__ == '__main__':
    main()
