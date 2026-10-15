#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC host memory platform bindings and emitted adapter SSA."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from NICMMIOBankCompare import modules
from NICFASEDHostMemoryBufferCompare import run
from PrintRocketControlMasterCompare import native_ports
from PrintRocketHostMemoryPortCompare import OLD, TOP, HELPER, compare_binding, sfc_contract

BEFORE = 'post-fame-host-memory-output-buffer'
AFTER = 'post-fame-host-memory-platform'


def composition(directory, baseline):
    before, after = [directory / (name + '.mlir') for name in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior, current = modules(before.read_text()), modules(after.read_text())
    previous, annotations = [json.loads((directory / (name + '-all.json')).read_text())
                             for name in (BEFORE, AFTER)]
    copied = {n for _, n, _ in native_ports(prior[OLD]) if n != 'host_mem'}

    def retarget(value):
        if isinstance(value, list):
            return [retarget(x) for x in value]
        if isinstance(value, dict):
            return {k: retarget(v) for k, v in value.items()}
        if isinstance(value, str):
            if value == '~' + OLD:
                return '~' + TOP
            if value.startswith('~' + OLD + '|'):
                suffix = value[len('~' + OLD):]
                prefix = '|' + OLD + '>'
                if suffix.startswith(prefix):
                    root = re.split(r'[.\[]', suffix[len(prefix):], maxsplit=1)[0]
                    if root in copied:
                        suffix = '|' + TOP + '>' + suffix[len(prefix):]
                return '~' + TOP + suffix
        return value

    assert retarget(previous) == annotations, 'copied/consumed annotation target transfer differs'
    streams = sorted(n for _, n, _ in native_ports(current[TOP]) if n.startswith('cpu_stream_'))
    assert len(streams) == 26, 'missing CPU stream boundary'
    regions = re.findall(r'name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64',
                         current['GGControlAddressDecode'].splitlines()[0])
    assert len(regions) == 12 and ('SimpleNICBridgeModule_0', '32', '7', '512') in regions
    assert 'GGNICTokenQueue10' in current and 'GGNICCPUStreamQueue3072' in current
    report.update(annotation_records=len(annotations), retained_control_regions=regions,
                  copied_CPU_stream_ports=streams,
                  sha256=hashlib.sha256(after.read_bytes()).hexdigest())
    return report


def mutations(directory, output, baseline, verifier, behavior):
    output.mkdir(parents=True, exist_ok=False)
    original = (directory / (AFTER + '.mlir')).read_text()
    helper = modules(original)[HELPER]
    response, count = re.subn(r'firrtl.bits (%\w+) 4 to 0', r'firrtl.bits \1 5 to 1', helper, count=1)
    assert count == 1, 'response ID mutation missed actual operation'
    request, count = re.subn(r'(      )(%\w+) = firrtl.pad (%\w+), 16 :',
                            r'\1%mut_request_id = firrtl.not \3 : (!firrtl.uint<5>) -> !firrtl.uint<5>\n'
                            r'\1\2 = firrtl.pad %mut_request_id, 16 :', helper, count=1)
    assert count == 1, 'request ID mutation missed actual operation'
    reports = []
    for name, changed in [('response-id-slice', response), ('request-id-invert', request)]:
        child = output / name
        child.mkdir()
        for filename in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
            (child / filename).symlink_to(directory / filename)
        candidate = child / (AFTER + '.mlir')
        candidate.write_text(original.replace(helper, changed, 1))
        assert run([verifier, '--verify-ir', candidate], child / 'verify.log').returncode == 0
        assert run([behavior, '--expanded-boundary', candidate], child / 'behavior.log').returncode == 1
        assert 'platform memory ID/handshake/payload mismatch' in (child / 'behavior.log').read_text()
        try:
            composition(child, baseline)
        except AssertionError as error:
            assert str(error) == 'shared native adapter differs', str(error)
            (child / 'rejection.log').write_text(str(error) + '\n')
        else:
            raise AssertionError('type-correct ID mutation escaped comparison')
        reports.append(dict(name=name, type_correct=True, behavior_rejected=True,
                            structured_comparison_rejected=True, artifact=str(candidate),
                            sha256=hashlib.sha256(candidate.read_bytes()).hexdigest()))
    return reports


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
                  SFC_contract=sfc_contract(args.golden),
                  limitation='Local NIC host memory platform only; CPU stream integration and runtime remain pending.')
    assert run([args.behavior_tool, '--expanded-boundary', args.candidate_dir / (AFTER + '.mlir')],
               args.output.with_suffix('.behavior.log')).returncode == 0, 'production SSA behavior failed'
    if args.mutation_dir:
        report['mutations'] = mutations(args.candidate_dir.resolve(), args.mutation_dir.resolve(),
                                        baseline, args.verify_tool.resolve(), args.behavior_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS NIC production mem_0 ABI, 37 leaf equations, all request/response IDs and handshakes, bindings and annotation transfer')


if __name__ == '__main__':
    main()
