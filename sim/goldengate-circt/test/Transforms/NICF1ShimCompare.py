#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production incoming-NIC U250 shim boundary with archived SFC."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, module
from PrintRocketFPGATopShellCompare import modules
from PrintRocketF1ShimCompare import counter_mutations, counter_schedule, routes


def retarget(value):
    if isinstance(value, str):
        return re.sub(r'^~FPGATop(?=\||$)', '~F1Shim', value)
    if isinstance(value, list):
        return [retarget(v) for v in value]
    if isinstance(value, dict):
        return {k: retarget(v) for k, v in value.items()}
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate', 'sfc', 'native-tool', 'firtool', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--reference-shim', type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    before = modules((args.candidate / 'post-fame-fpga-top.mlir').read_text())
    boundary = args.candidate / 'post-fame-f1-shim.mlir'
    after = modules(boundary.read_text())
    assert set(after) == set(before) | {'F1Shim'} and 'F1Shim' not in before
    assert all(after[n] == body for n, body in before.items()), 'retained module changed'
    if args.reference_shim:
        assert after['F1Shim'] == modules(args.reference_shim.read_text())['F1Shim']
    old = json.loads((args.candidate / 'post-fame-fpga-top-all.json').read_text())
    new = json.loads((args.candidate / 'post-fame-f1-shim-all.json').read_text())
    assert len(new) == len(old) + 2 and new[:-2] == retarget(old)
    assert new[-2]['class'] == 'midas.stage.GoldenGateOutputFileAnnotation'
    assert new[-2]['fileSuffix'] == '.defines.vh' and 'target' not in new[-2]
    assert new[-1] == dict(
        **{'class': 'midas.targetutils.xdc.XDCPathToCircuitAnnotation'},
        postLinkPath='firesim_top', preLinkPath='firesim_top')
    assert [a['target'] for a in new if a['class'] == 'midas.passes.HostClockSource'] == [
        '~F1Shim|FPGATop>clock']

    def run(command, log):
        with (args.output_dir / log).open('w') as output:
            subprocess.run([str(x) for x in command], stdout=output,
                           stderr=subprocess.STDOUT, check=True)
    stub = args.output_dir / 'f1-shim.mlir'
    run([args.native_tool, '--platform-boundary', boundary, stub], 'native.log')
    rtl = args.output_dir / 'f1-shim.sv'
    run([args.firtool, '--format=mlir', '--verilog', '--preserve-values=all',
         '--disable-all-randomization', '--strip-debug-info', stub, '-o', rtl], 'lower.log')
    golden = clean(args.sfc.read_text())
    actual = clean(rtl.read_text())
    golden_ports = ports(module(golden, 'F1Shim')[0])
    assert len(golden_ports) == 143
    assert ports(module(actual, 'F1Shim')[0]) == golden_ports
    expected = routes(golden)
    observed = routes(actual, module(golden, 'FPGATop')[0])
    assert observed == expected, [(k, observed.get(k), v)
                                  for k, v in expected.items() if observed.get(k) != v]
    golden_body = module(golden, 'F1Shim')[1]
    counters = counter_schedule(after['F1Shim'], golden_body)
    mutations = counter_mutations(after['F1Shim'], golden_body)
    defines = args.output_dir / 'FireSim-generated.defines.vh'
    golden_defines = args.sfc.with_suffix('.defines.vh')
    assert defines.read_bytes() == golden_defines.read_bytes()
    assert json.loads((args.output_dir / 'f1-shim-all.json').read_text()) == new
    report = dict(
        golden=str(args.sfc.resolve()),
        golden_sha256=hashlib.sha256(args.sfc.read_bytes()).hexdigest(),
        candidate=str(boundary.resolve()), unchanged_module_bodies=len(before),
        annotation_records=len(new), complete_archive_retargeting=True,
        unique_retained_host_clock=True, public_port_leaves=len(golden_ports),
        matched_combinational_routes=len(expected), counters=counters,
        counter_mutations_rejected=mutations,
        DDR_collateral=dict(golden=str(golden_defines.resolve()), complete_body_match=True),
        limitation='Focused production shim; FPGATop is external only for this comparison. '
                   'NIC metasim/driver headers and final RTL/runtime integration remain pending.')
    (args.output_dir / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
