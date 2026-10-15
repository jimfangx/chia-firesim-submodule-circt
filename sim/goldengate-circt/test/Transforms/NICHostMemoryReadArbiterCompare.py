#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC host AR request arbitration and execute emitted SSA."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from NICMMIOBankCompare import modules
from NICFASEDHostMemoryBufferCompare import run
from PrintRocketControlMasterCompare import native_ports
from PrintRocketHostMemoryReadArbiterCompare import OLD, TOP, HELPER, CONSUMED, compare_binding, sfc_contract
from PrintSelectedFASEDTimingCycleCompare import transfer

BEFORE = 'post-fame-host-memory-write-responses'
AFTER = 'post-fame-host-memory-read-arbiter'


def remaining_channels(value):
    if isinstance(value, list): return [remaining_channels(v) for v in value]
    if isinstance(value, dict): return {k: remaining_channels(v) for k, v in value.items()}
    if not isinstance(value, str): return value
    for name in ('fased_host_mem.r',):
        value = re.sub(r'^~' + TOP + r'\|' + OLD + '>' + re.escape(name) + r'(?=$|[.\[])',
                       '~' + TOP + '|' + TOP + '>' + name, value)
    value = re.sub(r'^~' + TOP + r'\|' + OLD + r'>host_mem_write\.',
                   '~' + TOP + '|' + TOP + '>host_mem.', value)
    return value


def composition(directory, baseline):
    before, after = [directory / (name + '.mlir') for name in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior = modules(before.read_text())
    copied = {n for _, n, _ in native_ports(prior[OLD])} - set(CONSUMED)
    previous = json.loads((directory / (BEFORE + '-all.json')).read_text())
    annotations = json.loads((directory / (AFTER + '-all.json')).read_text())
    assert annotations == remaining_channels(transfer(previous, OLD, TOP, copied)), 'emitted annotation transfer differs'
    streams = sorted(n for n in copied if n.startswith('cpu_stream_'))
    assert len(streams) == 26, 'missing CPU stream boundary'
    regions = re.findall(r'name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64',
                         prior['GGControlAddressDecode'].splitlines()[0])
    assert len(regions) == 12 and ('SimpleNICBridgeModule_0', '32', '7', '512') in regions
    assert 'GGNICTokenQueue10' in prior and 'GGNICCPUStreamQueue3072' in prior
    report.update(annotation_records=len(annotations), retained_control_regions=regions,
                  copied_CPU_stream_ports=streams,
                  sha256=hashlib.sha256(after.read_bytes()).hexdigest())
    return report


def mutation(directory, output, baseline, verifier, behavior, kind):
    """Inject a type-correct wrong ID or selection that changes during stalls."""
    output.mkdir(parents=True, exist_ok=False)
    for name in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / name).symlink_to(directory / name)
    original = (directory / (AFTER + '.mlir')).read_text()
    helper = modules(original)[HELPER]
    if kind == 'loadmem-id':
        changed, count = re.subn(r'firrtl.constant 16 : !firrtl.uint<5>',
                                 'firrtl.constant 17 : !firrtl.uint<5>', helper, count=1)
    else:
        assert kind == 'stall-selection'
        one = re.search(r'(%\w+) = firrtl.constant 1 : !firrtl.uint<1>', helper)[1]
        changed, count = re.subn(r'firrtl.mux\(%idle, (%[^,]+), %state_0\)',
                                 lambda m: 'firrtl.mux(' + one + ', ' + m[1] + ', %state_0)',
                                 helper, count=1)
    assert count == 1, 'mutation missed actual AR helper operation: ' + kind
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(helper, changed, 1))
    assert run([verifier, '--verify-ir', candidate], output / 'verify.log').returncode == 0
    assert run([behavior, '--expanded-boundary', candidate], output / 'behavior.log').returncode == 1, \
        'type-correct AR mutation escaped actual SSA behavior test'
    assert 'AR payload mismatch' in (output / 'behavior.log').read_text()
    try:
        composition(output, baseline)
    except AssertionError as error:
        assert str(error) == 'shared AR arbiter differs', str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct AR mutation escaped comparison')
    return dict(type_correct=True, behavior_rejected=True, structured_comparison_rejected=True,
                artifact=str(candidate), sha256=hashlib.sha256(candidate.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'output', 'behavior-tool', 'verify-tool'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--mutation-dir', type=Path)
    args = parser.parse_args()
    baseline = args.baseline.read_text()
    report = composition(args.candidate_dir.resolve(), baseline)
    report.update(golden=str(args.golden.resolve()),
                  golden_sha256=hashlib.sha256(args.golden.read_bytes()).hexdigest(),
                  SFC_modules=['AXI4Xbar'],
                  SFC_contract=sfc_contract(args.golden.resolve()),
                  limitation='Local AR arbitration only; NIC R responses, platform integration and runtime remain pending.')
    candidate = args.candidate_dir / (AFTER + '.mlir')
    assert run([args.behavior_tool, '--expanded-boundary', candidate],
               args.output.with_suffix('.behavior.log')).returncode == 0, 'production SSA behavior failed'
    if args.mutation_dir:
        report['mutations'] = {kind: mutation(args.candidate_dir.resolve(), args.mutation_dir.resolve() / kind,
                                              baseline, args.verify_tool.resolve(), args.behavior_tool.resolve(), kind)
                               for kind in ('loadmem-id', 'stall-selection')}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS NIC production AR arbitration, locked selection, idle rotation, ID insertion, backpressure, reset, bindings and actual SSA behavior')


if __name__ == '__main__':
    main()
