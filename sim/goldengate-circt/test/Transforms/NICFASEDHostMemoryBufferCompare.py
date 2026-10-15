#!/usr/bin/env python3
# See LICENSE for license details.
"""Check production NIC AXI host-memory buffers against SFC and execute their SSA."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from NICMMIOBankCompare import modules
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDHostMemoryBufferCompare import (
    TOP, QUEUES, compare_binding, sfc_contract)

BEFORE = 'post-fame-fased-read-deinterleaver'
AFTER = 'post-fame-fased-host-memory-buffer'


def composition(directory, baseline):
    report = compare_binding(directory / (BEFORE + '.mlir'),
                             directory / (AFTER + '.mlir'), baseline)
    prior = modules((directory / (BEFORE + '.mlir')).read_text())
    previous = json.loads((directory / (BEFORE + '-all.json')).read_text())
    annotations = json.loads((directory / (AFTER + '-all.json')).read_text())
    assert annotations == previous, 'emitted annotation archive changed'
    streams = sorted(n for _, n, _ in native_ports(prior[TOP]) if n.startswith('cpu_stream_'))
    assert len(streams) == 26, 'missing CPU stream boundary'
    regions = re.findall(r'name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64',
                         prior['GGControlAddressDecode'].splitlines()[0])
    assert len(regions) == 12 and ('SimpleNICBridgeModule_0', '32', '7', '512') in regions
    assert 'GGNICTokenQueue10' in prior and 'GGNICCPUStreamQueue3072' in prior
    report.update(annotation_records=len(annotations), retained_control_regions=regions,
                  preserved_CPU_stream_ports=streams,
                  sha256=hashlib.sha256((directory / (AFTER + '.mlir')).read_bytes()).hexdigest())
    return report


def run(command, log):
    with log.open('w') as output:
        return subprocess.run(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT)


def mutation(directory, output, baseline, verifier, behavior):
    """Replace the actual enqueue-pointer toggle with a type-correct hold."""
    output.mkdir(parents=True, exist_ok=False)
    for name in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / name).symlink_to(directory / name)
    original = (directory / (AFTER + '.mlir')).read_text()
    queue = modules(original)[QUEUES[0]]
    changed, count = re.subn(
        r'firrtl.not %enq_ptr_value : \(!firrtl.uint<1>\) -> !firrtl.uint<1>',
        'firrtl.and %enq_ptr_value, %enq_ptr_value : (!firrtl.uint<1>, !firrtl.uint<1>) -> !firrtl.uint<1>',
        queue, count=1)
    assert count == 1, 'mutation missed actual queue pointer increment'
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(queue, changed, 1))
    assert run([verifier, '--verify-ir', candidate], output / 'verify.log').returncode == 0
    assert run([behavior, '--expanded-boundary', candidate], output / 'behavior.log').returncode == 1, \
        'type-correct pointer mutation escaped actual SSA behavior test'
    assert 'occupancy reset/next state mismatch' in (output / 'behavior.log').read_text()
    try:
        composition(output, baseline)
    except AssertionError as error:
        assert str(error) == 'shared semantics changed: ' + QUEUES[0], str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct pointer mutation escaped comparison')
    return dict(type_correct=True, behavior_rejected=True, structured_comparison_rejected=True,
                artifact=str(candidate), sha256=hashlib.sha256(candidate.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'output', 'behavior-tool', 'verify-tool'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--sfc-queues', nargs=4, default=['Queue_31', 'Queue_25', 'Queue_33', 'Queue_35'])
    parser.add_argument('--mutation-dir', type=Path)
    args = parser.parse_args()
    baseline = args.baseline.read_text()
    report = composition(args.candidate_dir.resolve(), baseline)
    report.update(golden=str(args.golden.resolve()),
                  golden_sha256=hashlib.sha256(args.golden.read_bytes()).hexdigest(),
                  SFC_modules=['AXI4Buffer_1', *args.sfc_queues],
                  SFC_contract=sfc_contract(args.golden.resolve(), args.sfc_queues),
                  optimized_SFC_ports=['auto_in_b_ready', args.sfc_queues[2] + '.io_deq_ready'],
                  limitation='Local AXI buffering only; NIC shared host memory arbitration, platform integration and runtime remain pending.')
    candidate = args.candidate_dir / (AFTER + '.mlir')
    assert run([args.behavior_tool, '--expanded-boundary', candidate],
               args.output.with_suffix('.behavior.log')).returncode == 0, 'production SSA behavior failed'
    if args.mutation_dir:
        report['mutation'] = mutation(args.candidate_dir.resolve(), args.mutation_dir.resolve(), baseline,
                                      args.verify_tool.resolve(), args.behavior_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS NIC production host memory buffer: SFC five queues, bindings and actual SSA behavior')


if __name__ == '__main__':
    main()
