#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC shared host AW/W arbitration and execute emitted SSA."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from NICMMIOBankCompare import modules
from NICFASEDHostMemoryBufferCompare import run
from PrintRocketControlMasterCompare import native_ports
from PrintRocketHostMemoryWriteArbiterCompare import TOP, HELPERS, CONSUMED, compare_binding, sfc_contract
from PrintSelectedFASEDTimingCycleCompare import transfer

OLD = 'GGFASEDAddressTranslationWrapper'
BEFORE = 'post-fame-fased-host-memory-buffer'
AFTER = 'post-fame-host-memory-write-arbiter'


def remaining_channels(value):
    if isinstance(value, list): return [remaining_channels(v) for v in value]
    if isinstance(value, dict): return {k: remaining_channels(v) for k, v in value.items()}
    if not isinstance(value, str): return value
    return re.sub(r'^~' + TOP + r'\|' + OLD + r'>fased_host_mem\.(b|ar|r)(?=$|[.\[])',
                  '~' + TOP + '|' + TOP + r'>fased_host_mem.\1', value)


def composition(directory, baseline):
    before, after = [directory / (name + '.mlir') for name in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior = modules(before.read_text())
    copied = {n for _, n, _ in native_ports(prior[OLD])} - set(CONSUMED) - {'fased_host_mem'}
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


def mutation(directory, output, baseline, verifier, behavior):
    """Hold the actual reservation queue's enqueue pointer with valid FIRRTL types."""
    output.mkdir(parents=True, exist_ok=False)
    for name in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / name).symlink_to(directory / name)
    original = (directory / (AFTER + '.mlir')).read_text()
    queue = modules(original)[HELPERS[1]]
    changed, count = re.subn(
        r'firrtl.not %enq_ptr_value : \(!firrtl.uint<1>\) -> !firrtl.uint<1>',
        'firrtl.and %enq_ptr_value, %enq_ptr_value : (!firrtl.uint<1>, !firrtl.uint<1>) -> !firrtl.uint<1>',
        queue, count=1)
    assert count == 1, 'mutation missed actual reservation queue pointer'
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(queue, changed, 1))
    assert run([verifier, '--verify-ir', candidate], output / 'verify.log').returncode == 0
    assert run([behavior, '--expanded-boundary', candidate], output / 'behavior.log').returncode == 1, \
        'type-correct pointer mutation escaped actual SSA behavior test'
    assert 'queue next enq' in (output / 'behavior.log').read_text()
    try:
        composition(output, baseline)
    except AssertionError as error:
        assert str(error) == 'shared helper differs ' + HELPERS[1], str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct pointer mutation escaped comparison')
    return dict(type_correct=True, behavior_rejected=True, structured_comparison_rejected=True,
                artifact=str(candidate), sha256=hashlib.sha256(candidate.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'output', 'behavior-tool', 'verify-tool'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--sfc-queue', default='Queue_23')
    parser.add_argument('--mutation-dir', type=Path)
    args = parser.parse_args()
    baseline = args.baseline.read_text()
    report = composition(args.candidate_dir.resolve(), baseline)
    report.update(golden=str(args.golden.resolve()),
                  golden_sha256=hashlib.sha256(args.golden.read_bytes()).hexdigest(),
                  SFC_modules=['AXI4Xbar', args.sfc_queue],
                  SFC_contract=sfc_contract(args.golden.resolve(), args.sfc_queue),
                  limitation='Local AW/W arbitration only; NIC B/AR/R routing, platform integration and runtime remain pending.')
    candidate = args.candidate_dir / (AFTER + '.mlir')
    assert run([args.behavior_tool, '--expanded-boundary', candidate],
               args.output.with_suffix('.behavior.log')).returncode == 0, 'production SSA behavior failed'
    if args.mutation_dir:
        report['mutation'] = mutation(args.candidate_dir.resolve(), args.mutation_dir.resolve(), baseline,
                                      args.verify_tool.resolve(), args.behavior_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS NIC production AW/W reservations, WLAST retirement, ID routing, bindings and actual SSA behavior')


if __name__ == '__main__':
    main()
