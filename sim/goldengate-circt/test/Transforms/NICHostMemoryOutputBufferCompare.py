#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC output queues against SFC and execute emitted SSA."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from NICMMIOBankCompare import modules
from NICFASEDHostMemoryBufferCompare import run
from PrintRocketControlMasterCompare import native_ports
from PrintRocketHostMemoryOutputBufferCompare import TOP, QUEUES, compare_binding, sfc_contract

BEFORE = 'post-fame-host-memory-read-responses'
AFTER = 'post-fame-host-memory-output-buffer'


def composition(directory, baseline):
    before, after = [directory / (name + '.mlir') for name in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior = modules(before.read_text())
    previous, annotations = [json.loads((directory / (name + '-all.json')).read_text())
                             for name in (BEFORE, AFTER)]
    assert previous == annotations, 'output buffering changed annotations'
    streams = sorted(n for _, n, _ in native_ports(prior[TOP]) if n.startswith('cpu_stream_'))
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
    """Break the actual address queue's ready equation without changing its type."""
    output.mkdir(parents=True, exist_ok=False)
    for name in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / name).symlink_to(directory / name)
    original = (directory / (AFTER + '.mlir')).read_text()
    helper = modules(original)[QUEUES[0]]
    # not(full) becomes full xor full: a well-typed queue which never accepts.
    changed, count = re.subn(r'firrtl.not (%\w+) : \(!firrtl.uint<1>\) -> !firrtl.uint<1>',
                             r'firrtl.xor \1, \1 : (!firrtl.uint<1>, !firrtl.uint<1>) -> !firrtl.uint<1>',
                             helper, count=1)
    assert count == 1, 'mutation missed actual address queue operation'
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(helper, changed, 1))
    assert run([verifier, '--verify-ir', candidate], output / 'verify.log').returncode == 0
    assert run([behavior, '--expanded-boundary', candidate], output / 'behavior.log').returncode == 1, \
        'type-correct queue mutation escaped actual SSA behavior test'
    assert 'handshake mismatch' in (output / 'behavior.log').read_text()
    try:
        composition(output, baseline)
    except AssertionError as error:
        assert str(error) == 'shared queue/helper differs ' + QUEUES[0], str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct queue mutation escaped comparison')
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
    contract = sfc_contract(args.golden)
    report.update(golden=str(args.golden.resolve()),
                  golden_sha256=hashlib.sha256(args.golden.read_bytes()).hexdigest(),
                  SFC_modules=[q['module'] for q in contract['queues']] + ['AXI4Buffer'], SFC_contract=contract,
                  limitation='Local NIC output queues only; platform specialization, CPU stream integration and runtime remain pending.')
    assert run([args.behavior_tool, '--expanded-boundary', args.candidate_dir / (AFTER + '.mlir')],
               args.output.with_suffix('.behavior.log')).returncode == 0, 'production SSA behavior failed'
    if args.mutation_dir:
        report['mutation'] = mutation(args.candidate_dir.resolve(), args.mutation_dir.resolve(),
                                      baseline, args.verify_tool.resolve(), args.behavior_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS NIC production five-channel queues, no-flow/no-pipe handshakes, reset-edge RAM writes, payload order, bindings and actual SSA behavior')


if __name__ == '__main__':
    main()
