#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production FPGA shell with SFC, including its lowered wiring.

Only the new shell is lowered with an external declaration for the retained
child. This proves its ABI and unconditional identities, not NIC runtime or
equivalence of the already assembled child implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from NICMMIOBankCompare import modules
from NICFASEDHostMemoryBufferCompare import run
from PrintControlMasterCompare import ports as rtl_ports
from PrintControlResponsesCompare import clean, equations, instance, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring

OLD, TOP = 'GGCPUStreamPlatformWrapper', 'FPGATop'
BEFORE, AFTER = 'post-fame-cpu-stream-platform', 'post-fame-fpga-top'
PUBLIC = dict(hostClock='clock', hostReset='reset', ctrl='ctrl', mem_0='mem_0',
              cpu_managed_axi4='cpu_managed_axi4')


def composition(directory):
    prior, current = [modules((directory / (n + '.mlir')).read_text()) for n in (BEFORE, AFTER)]
    assert set(current) - set(prior) == {TOP}
    for name, body in prior.items():
        assert current[name] == body, 'changed prior module ' + name
    old_ports = native_ports(prior[OLD])
    expected = [(d, PUBLIC[n], t) for n in PUBLIC for d, name, t in old_ports if name == n]
    assert len(expected) == 5 and native_ports(current[TOP]) == expected, 'public shell types differ'
    assert re.findall(r'^\s*(?:%[^=\n]+ = )?firrtl\.(\w+)\b', current[TOP], re.M) == ['module', 'instance'] + ['connect'] * 5, 'shell adds operations'
    routes = {('sim.' + n if d == 'in' else 'top.' + PUBLIC[n]):
              ('top.' + PUBLIC[n] if d == 'in' else 'sim.' + n)
              for n in PUBLIC for d, name, _ in old_ports if name == n}
    assert wiring(current[TOP]) == routes, 'shell identity wiring differs'
    assert re.findall(r' = firrtl.instance (\w+) @(\w+)\b', current[TOP]) == [('sim', OLD)]

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
                    if root in PUBLIC:
                        suffix = '|' + TOP + '>' + PUBLIC[root] + suffix[len(prefix) + len(root):]
                return '~' + TOP + suffix
        return value
    old, annotations = [json.loads((directory / (n + '-all.json')).read_text()) for n in (BEFORE, AFTER)]
    source = {'class': 'midas.passes.HostClockSource', 'target': '~FPGATop|FPGATop>clock'}
    assert annotations == transfer(old) + [source], 'annotation archive transfer differs'
    for cls, target in [('midas.passes.fame.FAMEHostClock', 'clock'),
                        ('midas.passes.fame.FAMEHostReset', 'reset'),
                        ('midas.passes.HostClockSource', 'clock')]:
        assert [a for a in annotations if a.get('class') == cls] == [
            {'class': cls, 'target': '~FPGATop|FPGATop>' + target}]
    return dict(unchanged_prior_modules=len(prior), public_aggregate_ports=5,
                private_ports_retained=len(old_ports)-5, identity_connections=5,
                annotation_records=len(annotations), complete_recursive_archive_transfer=True)


def sfc_contract(path):
    header, body = module(clean(path.read_text()), TOP)
    ports = rtl_ports(header)
    assert len(ports) == 121 and ports['clock'] == ['input', 1] and ports['reset'] == ['input', 1]
    eq = equations(body)
    clocks, resets = [], []
    for pin, net in re.findall(r'\.(clock|reset)\((\w+)\)', body):
        assert eq.get(net) == pin, 'SFC host clock/reset fanout differs: ' + net
        (clocks if pin == 'clock' else resets).append(net)
    assert len(clocks) == len(resets) and len(clocks) >= 19
    return ports, dict(public_leaves=len(ports), direct_clock_consumers=clocks,
                       direct_reset_consumers=resets)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'golden', 'behavior-tool', 'verify-tool', 'firtool', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--mutations', action='store_true')
    a = p.parse_args(); directory = a.candidate_dir.resolve()
    out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    report = composition(directory)
    expected, sfc = sfc_contract(a.golden)
    report.update(golden=str(a.golden.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(), SFC=sfc)
    candidate = directory / (AFTER + '.mlir')
    stub, rtl = out / 'shell.mlir', out / 'shell.sv'
    assert run([a.behavior_tool, '--platform-boundary', candidate, stub], out / 'identity.log').returncode == 0
    assert run([a.firtool, '--format=mlir', '--verilog', '--preserve-values=all',
                '--disable-all-randomization', '--strip-debug-info', stub, '-o', rtl], out / 'lowering.log').returncode == 0
    header, body = module(clean(rtl.read_text()), TOP)
    assert rtl_ports(header) == expected, 'complete SFC platform ABI differs'
    pins = instance(body, OLD, 'sim')
    eq = equations(body)
    def identity_root(net):
        seen = set()
        while net in eq:
            assert net not in seen, 'cyclic shell identity'
            seen.add(net); net = eq[net].strip()
            assert re.fullmatch(r'\w+', net), 'shell lowering introduced an expression'
        return net
    for name in expected:
        child = 'hostClock' if name == 'clock' else 'hostReset' if name == 'reset' else name
        assert identity_root(pins[child]) == identity_root(name), 'lowered public identity differs: ' + name
    report['lowered_public_identities'] = len(expected)
    if a.mutations:
        text = candidate.read_text(); shell = modules(text)[TOP]
        report['mutations'] = []
        for name, port, replacement in [('disconnected-host-clock', 'hostClock', '%disconnected'),
                                         ('reset-from-diagnostic', 'hostReset', '%sim_ctrl_error_aw_ready')]:
            # Use a passive Clock wire or an existing UInt<1> diagnostic.
            # CIRCT verification alone cannot establish forwarding identity.
            changed, count = re.subn(r'(firrtl.connect %sim_' + port + r', )%\w+',
                                     r'\g<1>' + replacement, shell, count=1)
            assert count == 1
            if port == 'hostClock':
                pos = changed.index('\n') + 1
                changed = changed[:pos] + '      %disconnected = firrtl.wire : !firrtl.clock\n' + changed[pos:]
            child = out / name; child.mkdir()
            for n in (BEFORE + '.mlir', BEFORE + '-all.json', AFTER + '-all.json'):
                (child / n).symlink_to(directory / n)
            altered = child / candidate.name; altered.write_text(text.replace(shell, changed, 1))
            assert run([a.verify_tool, '--verify-ir', altered], child / 'verify.log').returncode == 0
            assert run([a.behavior_tool, '--platform-boundary', altered], child / 'identity.log').returncode == 1
            diagnostic = ('shell must contain only its instance and five identity connections'
                          if port == 'hostClock' else 'public forwarding connection')
            assert diagnostic in (child / 'identity.log').read_text()
            try: composition(child)
            except AssertionError as error:
                assert str(error) == ('shell adds operations' if port == 'hostClock' else 'shell identity wiring differs')
            else: raise AssertionError('misroute escaped production comparison')
            report['mutations'].append(dict(name=name, CIRCT_verified=True, identity_rejected=True, comparison_rejected=True))
        # Also exercise the independent oracle-side clock/reset extraction.
        # The immutable SFC input is read only; this copy is local evidence.
        mutant = out / 'sfc-reset-misroute.sv'
        original = a.golden.read_text()
        changed, count = re.subn(r'(assign CPUManagedStreamEngine_0_reset = )reset;',
                                 r'\1clock;', original, count=1)
        assert count == 1
        mutant.write_text(changed)
        try: sfc_contract(mutant)
        except AssertionError as error:
            assert str(error) == 'SFC host clock/reset fanout differs: CPUManagedStreamEngine_0_reset'
        else: raise AssertionError('SFC reset misroute escaped comparison')
        report['oracle_reset_mutation_rejected'] = True
    report['limitation'] = 'Shell ABI and unconditional identities only; final NIC shim, headers, RTL/runtime parity remain pending.'
    (out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS production shell: 121 SFC leaves, five aggregate identities, unchanged prior bodies and complete annotation transfer')


if __name__ == '__main__': main()
