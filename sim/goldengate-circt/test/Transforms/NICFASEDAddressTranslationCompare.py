#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production NIC FASED translation boundary with actual SFC RTL."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from NICMMIOBankCompare import modules
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDAddressTranslationCompare import (
    OLD, NEW, HELPER, SHIFT, compare_binding, sfc_contract)
from PrintSelectedFASEDTimingCycleCompare import transfer

BEFORE = 'post-fame-fased-host-memory'
AFTER = 'post-fame-fased-address-translation'


def composition(directory, baseline=None):
    before, after = [directory / (n + '.mlir') for n in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior = modules(before.read_text())
    copied = {n for _, n, _ in native_ports(prior[OLD])} - {'fased_host_mem'}
    previous = json.loads((directory / (BEFORE + '-all.json')).read_text())
    annotations = json.loads((directory / (AFTER + '-all.json')).read_text())
    assert annotations == transfer(previous, OLD, NEW, copied), 'emitted annotation transfer differs'
    regions = re.findall(r'name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64',
                         prior['GGControlAddressDecode'].splitlines()[0])
    assert len(regions) == 12 and ('SimpleNICBridgeModule_0', '32', '7', '512') in regions
    assert 'GGNICTokenQueue10' in prior and 'GGNICCPUStreamQueue3072' in prior
    streams = sorted(n for n in copied if n.startswith('cpu_stream_'))
    assert len(streams) == 26, 'missing CPU stream boundary'
    report.update(annotation_records=len(annotations), retained_control_regions=regions,
                  copied_CPU_stream_ports=streams,
                  sha256=hashlib.sha256(after.read_bytes()).hexdigest())
    return report


def mutation(directory, output, verifier):
    """Change the actual translation offset by one, retaining valid FIRRTL types."""
    output.mkdir(parents=True, exist_ok=False)
    for name in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / name).symlink_to(directory / name)
    original = (directory / (AFTER + '.mlir')).read_text()
    helper = modules(original)[HELPER]
    changed, count = re.subn(r'(firrtl.constant )' + str(SHIFT) + r'( : !firrtl.uint<35>)',
                            lambda m: m[1] + str(SHIFT + 1) + m[2], helper)
    assert count == 1, 'mutation missed actual offset constant'
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(helper, changed, 1))
    with (output / 'verify.log').open('w') as log:
        subprocess.run([str(verifier), '--verify-ir', str(candidate)], check=True,
                       stdout=log, stderr=subprocess.STDOUT)
    try:
        composition(output)
    except AssertionError as error:
        assert str(error) == 'actual address translation SSA routes differ', str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct address offset mutation escaped comparison')
    return dict(verified_by=str(verifier) + ' --verify-ir', caught=True,
                offset_delta=1, artifact=str(candidate),
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
    ports, connections, predicates = sfc_contract(args.golden.resolve())
    report = composition(args.candidate_dir.resolve(), args.baseline.read_text() if args.baseline else None)
    report.update(golden=str(args.golden.resolve()), SFC_module='AXI4AddressTranslation',
                  SFC_ports=ports, SFC_channel_connections=connections,
                  SFC_assertion_predicates=predicates,
                  optimized_SFC_AXI_leaves=['auto_in_b_ready', 'auto_out_b_ready'],
                  limitation='Local translation only; NIC deinterleaving, platform integration and runtime remain pending.')
    if args.mutation_dir:
        report['mutation'] = mutation(args.candidate_dir.resolve(), args.mutation_dir.resolve(), args.verify_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print('PASS NIC FASED translation: two addresses, four host-clock assertions, 1024 semantic vectors')


if __name__ == '__main__':
    main()
