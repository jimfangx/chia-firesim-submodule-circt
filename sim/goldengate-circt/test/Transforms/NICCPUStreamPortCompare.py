#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production CPU slave assembly with the archived SFC platform ABI.

This boundary adds no state: native SSA checks prove unconditional identity
for every payload bit. Existing buffered stream bodies must remain identical.
The comparison does not establish final NIC shell or runtime parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from NICMMIOBankCompare import modules
from NICFASEDHostMemoryBufferCompare import run
from PrintRocketControlMasterCompare import native_ports
from PrintRocketCPUStreamPortCompare import axi_type, leaves, UNUSED, sfc_contract
from PrintRocketLoadMemEngineCompare import wiring

OLD, TOP, HELPER = 'GGHostMemoryPlatformWrapper', 'GGCPUStreamPlatformWrapper', 'GGCPUStreamPortAdapter'
BEFORE, AFTER = 'post-fame-host-memory-platform', 'post-fame-cpu-stream-platform'


def composition(directory, incoming):
    before, after = [(directory / (n + '.mlir')).read_text() for n in (BEFORE, AFTER)]
    prior, current = modules(before), modules(after)
    assert set(current) - set(prior) == {TOP, HELPER}
    for name, body in prior.items():
        assert current[name] == body, 'changed prior module ' + name
    unused = UNUSED - ({'w_bits_data'} if incoming else set())
    active = {n: v for n, v in leaves().items() if n not in unused}
    previous = native_ports(prior[OLD])
    consumed = {n for _, n, _ in previous if n.startswith('cpu_stream_')}
    assert consumed == {'cpu_stream_' + n for n in active}
    copied = [(d, n, t) for d, n, t in previous if n not in consumed]
    assert native_ports(current[TOP]) == copied + [('in', 'cpu_managed_axi4', axi_type())]
    expected = {('sim.' + n if d == 'in' else 'top.' + n):
                ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in copied}
    expected['cpu_port.cpu_managed_axi4'] = 'top.cpu_managed_axi4'
    helper_nets = {}
    for n, (d, _) in active.items():
        child, adapter = 'sim.cpu_stream_' + n, 'cpu_port.stream_' + n
        expected[child if d == 'in' else adapter] = adapter if d == 'in' else child
        ch, rest = n.split('_', 1)
        external = 'top.cpu_managed_axi4.' + ch + '.' + rest.replace('bits_', 'bits.')
        scalar = 'top.stream_' + n
        helper_nets[scalar if d == 'in' else external] = external if d == 'in' else scalar
    assert wiring(current[TOP]) == expected, 'CPU wrapper binding differs'
    assert wiring(current[HELPER]) == helper_nets, 'CPU helper identity differs'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', current[TOP]) == [('sim', OLD), ('cpu_port', HELPER)]
    assert not re.search(r'firrtl\.(?:reg|mem|instance)\b', current[HELPER])

    def transfer(value):
        if isinstance(value, list): return [transfer(v) for v in value]
        if isinstance(value, dict): return {k: transfer(v) for k, v in value.items()}
        if isinstance(value, str):
            if value == '~' + OLD: return '~' + TOP
            if value.startswith('~' + OLD + '|'):
                suffix = value[len(OLD) + 1:]
                prefix = '|' + OLD + '>'
                if suffix.startswith(prefix):
                    root = re.split(r'[.\[]', suffix[len(prefix):], maxsplit=1)[0]
                    if root not in consumed: suffix = '|' + TOP + '>' + suffix[len(prefix):]
                return '~' + TOP + suffix
        return value
    previous, annotations = [json.loads((directory / (n + '-all.json')).read_text()) for n in (BEFORE, AFTER)]
    assert transfer(previous) == annotations, 'copied/consumed annotation transfer differs'
    if incoming:
        assert 'GGNICTokenQueue10' in current and 'GGNICCPUStreamQueue3072' in current
        assert len(re.findall(r'name = "[^"]+", size = \d+ : i64, slave = \d+ : i32, start = \d+ : i64',
                              current['GGControlAddressDecode'].splitlines()[0])) == 12
    return dict(unchanged_prior_modules=len(prior), public_ports=len(copied)+1,
                copied_ports=len(copied), consumed_CPU_scalars=len(active),
                wrapper_connections=len(expected), helper_identity_connections=len(helper_nets),
                annotation_records=len(annotations), incoming=incoming)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'golden', 'behavior-tool', 'verify-tool', 'output'):
        parser.add_argument('--' + n, type=Path, required=True)
    parser.add_argument('--incoming', action='store_true')
    parser.add_argument('--mutation-dir', type=Path)
    a = parser.parse_args(); directory = a.candidate_dir.resolve()
    report = composition(directory, a.incoming)
    report.update(golden=str(a.golden.resolve()), golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  SFC_contract=sfc_contract(a.golden, a.incoming))
    candidate = directory / (AFTER + '.mlir')
    assert run([a.behavior_tool, '--platform-boundary', candidate], a.output.with_suffix('.behavior.log')).returncode == 0
    if a.mutation_dir:
        assert a.incoming
        a.mutation_dir.mkdir(parents=True, exist_ok=False)
        original = candidate.read_text(); helper = modules(original)[HELPER]
        report['mutations'] = []
        for name, leaf, replacement in [('write-payload-from-read', 'w_bits_data', 'r_bits_data'),
                                         ('write-address-from-read', 'aw_bits_addr', 'ar_bits_addr')]:
            changed, count = re.subn(r'(firrtl.strictconnect %stream_' + leaf + r', )%\w+',
                                     r'\1%stream_' + replacement, helper, count=1)
            # Read scalars are inputs to the adapter; address scalars are
            # outputs. Use the external AR address subfield for the latter.
            if leaf == 'aw_bits_addr':
                ar = re.search(r'(%\w+) = firrtl.subfield %\w+\[addr\].*', helper[helper.index('firrtl.subfield %cpu_managed_axi4[ar]'):])
                assert ar
                changed, count = re.subn(r'(firrtl.strictconnect %stream_aw_bits_addr, )%\w+',
                                         r'\g<1>' + ar[1], helper, count=1)
            assert count == 1
            child = a.mutation_dir / name; child.mkdir()
            for n in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
                (child / n).symlink_to(directory / n)
            altered = child / candidate.name; altered.write_text(original.replace(helper, changed, 1))
            assert run([a.verify_tool, '--verify-ir', altered], child / 'verify.log').returncode == 0
            assert run([a.behavior_tool, '--platform-boundary', altered], child / 'behavior.log').returncode == 1
            assert 'CPU leaf must be an unconditional identity connection' in (child / 'behavior.log').read_text()
            try: composition(child, True)
            except AssertionError as error: assert str(error) == 'CPU helper identity differs'
            else: raise AssertionError('type-correct misroute escaped comparison')
            report['mutations'].append(dict(name=name, verified=True, behavior_rejected=True, comparison_rejected=True))
    a.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS CPU platform ABI, unconditional payload wiring, retained bodies and annotation transfer')


if __name__ == '__main__': main()
