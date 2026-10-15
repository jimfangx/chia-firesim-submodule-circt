#!/usr/bin/env python3
# See LICENSE for license details.
"""Check production NIC local FASED host-memory binding against SFC.

This stateless NASTI-to-AXI boundary precedes address translation. Preserve NIC
streams and clocks, check every native route and both annotation archives, and
compare the surviving local FASED interface/nets in actual SFC output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from NICMMIOBankCompare import modules
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDHostMemoryCompare import (
    OLD, NEW, CONSUMED, compare_binding, memory_routes, sfc_contract)

BEFORE, AFTER = 'post-fame-control-master', 'post-fame-fased-host-memory'


def transfer(value, copied, mapped):
    if isinstance(value, list): return [transfer(v, copied, mapped) for v in value]
    if isinstance(value, dict): return {k: transfer(v, copied, mapped) for k, v in value.items()}
    if not isinstance(value, str): return value
    if value == '~' + OLD: return '~' + NEW
    prefix = '~' + OLD + '|'
    if not value.startswith(prefix): return value
    result = value[len(prefix):]
    if result.startswith(OLD + '>'):
        ref = result[len(OLD) + 1:]
        if ref in mapped: result = NEW + '>' + mapped[ref]
        elif re.split(r'[.\[]', ref)[0] in copied: result = NEW + '>' + ref
    return '~' + NEW + '|' + result


def composition(directory, baseline=None):
    before, after = [directory / (n + '.mlir') for n in (BEFORE, AFTER)]
    report = compare_binding(before, after, baseline)
    prior = modules(before.read_text())
    copied = {n for _, n, _ in native_ports(prior[OLD])} - CONSUMED
    _, mapped, _ = memory_routes()
    previous = json.loads((directory / (BEFORE + '-all.json')).read_text())
    annotations = json.loads((directory / (AFTER + '-all.json')).read_text())
    assert annotations == transfer(previous, copied, mapped), 'emitted annotation transfer differs'
    regions = re.findall(r'name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64',
                         prior['GGControlAddressDecode'].splitlines()[0])
    assert len(regions) == 12 and ('SimpleNICBridgeModule_0', '32', '7', '512') in regions
    assert 'GGNICTokenQueue10' in prior and 'GGNICCPUStreamQueue3072' in prior
    streams = [n for n in copied if n.startswith('cpu_stream_')]
    assert len(streams) == 26, 'missing CPU stream boundary'
    report.update(annotation_records=len(annotations), retained_control_regions=regions,
                  copied_CPU_stream_ports=sorted(streams),
                  sha256=hashlib.sha256(after.read_bytes()).hexdigest())
    return report


def mutation(directory, output, verifier):
    """Crosswire the actual AR address from AW; both have UInt<35> type."""
    output.mkdir(parents=True, exist_ok=False)
    for n in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
        (output / n).symlink_to(directory / n)
    original = (directory / (AFTER + '.mlir')).read_text()
    body = modules(original)[NEW]
    values = {v: v[1:] for v in re.findall(r'%\w+(?=:)', body.splitlines()[0])}
    values['%sim_fased_host_requests'] = 'sim.fased_host_requests'
    address_sources = {}
    changed = []
    count = 0
    for line in body.splitlines():
        if m := re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]', line):
            dest, src, field = m.groups()
            if src in values:
                values[dest] = values[src] + '.' + field
                if values[dest] in ('sim.fased_host_requests.aw.bits.addr',
                                    'sim.fased_host_requests.ar.bits.addr'):
                    address_sources[values[dest].split('.')[2]] = dest
        if m := re.search(r'firrtl.strictconnect (%\w+), (%\w+)', line):
            if values.get(m[1]) == 'fased_host_mem.ar.bits.addr':
                assert m[2] == address_sources['ar']
                line = line[:m.start(2)] + address_sources['aw'] + line[m.end(2):]
                count += 1
        changed.append(line)
    assert count == 1, 'mutation missed actual AR address connection'
    candidate = output / (AFTER + '.mlir')
    candidate.write_text(original.replace(body, '\n'.join(changed), 1))
    with (output / 'verify.log').open('w') as log:
        subprocess.run([str(verifier), '--verify-ir', str(candidate)], check=True,
                       stdout=log, stderr=subprocess.STDOUT)
    try:
        composition(output)
    except AssertionError as error:
        assert str(error) == 'actual host memory SSA wiring differs', str(error)
        (output / 'rejection.log').write_text(str(error) + '\n')
    else:
        raise AssertionError('type-correct AW/AR address crosswire escaped comparison')
    return dict(verified_by=str(verifier) + ' --verify-ir', caught=True,
                crosswire='AR address from AW address', artifact=str(candidate),
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
    _, _, fields = memory_routes()
    declared, optimized, checked = sfc_contract(args.golden.resolve(), fields)
    report = composition(args.candidate_dir.resolve(), args.baseline.read_text() if args.baseline else None)
    report.update(golden=str(args.golden.resolve()), SFC_module='FASEDMemoryTimingModel',
                  SFC_boundary='auto_to_host_dram_out', address_bits=35, data_bits=64, id_bits=4,
                  sfc_surviving_ports=declared, sfc_surviving_connections=checked,
                  sfc_optimized_response_ready_leaves=optimized,
                  limitation='Local stateless host-memory adaptation only; NIC translation, platform and runtime remain pending.')
    if args.mutation_dir:
        report['mutation'] = mutation(args.candidate_dir.resolve(), args.mutation_dir.resolve(), args.verify_tool.resolve())
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print('PASS NIC local FASED host-memory ABI / ingress, egress and response-error routes')


if __name__ == '__main__':
    main()
