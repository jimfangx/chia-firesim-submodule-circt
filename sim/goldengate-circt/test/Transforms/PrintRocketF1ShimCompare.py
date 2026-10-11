#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print U250 specialization with the immutable SFC F1Shim.

Checks the retained IR/archive, all leaf interfaces and combinational routes,
plus synchronous request-counter reset/stall/wrap behavior. Enabled Print runtime
parity remains pending: the immutable SFC fixture has Print disabled.
"""
import argparse
import json
import random
import re
from pathlib import Path
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, equations, instance, module
from PrintRocketFPGATopShellCompare import modules


def compare(before_path, after_path, baseline):
    before, after = before_path.read_text(), after_path.read_text()
    old, new = modules(before), modules(after)
    assert len(old) == 300 and len(new) == 301 and set(new) - set(old) == {'F1Shim'}
    assert all(new[n] == body for n, body in old.items()), 'changed retained module'
    # The expanded path reuses the platform hardware, without importing any
    # native Rocket body or discarding the enabled Print modules.
    assert new['F1Shim'] == baseline, 'shared platform implementation differs'
    assert len([n for n in new if n.startswith('GGPrintBridge')]) == 21
    original = before.splitlines()[1].replace('firrtl.circuit "FPGATop"', 'firrtl.circuit "F1Shim"')
    original = re.sub(r'"~FPGATop(?=[|"])', '"~F1Shim', original)
    extra = ', {body = "// Optionally instantiate additional memory channels if required.\\n// The first channel (C) is provided by the shell and is not optional.\\n`define USE_DDR_CHANNEL_A 0\\n`define USE_DDR_CHANNEL_B 0\\n`define USE_DDR_CHANNEL_D 0\\n", class = "midas.stage.GoldenGateOutputFileAnnotation", fileSuffix = ".defines.vh"}'
    extra = extra.replace('\\n', '\\0A')
    extra += ', {class = "midas.targetutils.xdc.XDCPathToCircuitAnnotation", postLinkPath = "firesim_top", preLinkPath = "firesim_top"}'
    line = after.splitlines()[1]
    assert extra in line, 'DDR/XDC output annotations differ'
    assert line.replace(extra, '') == original, 'complete recursive archive retargeting differs'
    assert line.count('class = "midas.passes.HostClockSource"') == 1
    assert 'class = "midas.passes.HostClockSource", target = "~F1Shim|FPGATop>clock"' in line
    return dict(before=str(before_path), after=str(after_path), unchanged_module_bodies=300,
                retained_Print_modules=21, shared_platform_body_match=True,
                complete_archive_transfer=True, unique_retained_HostClockSource=True)


def stub(after_path, path):
    bodies = modules(after_path.read_text())
    child = bodies['FPGATop'].split(' {', 1)[0].strip()
    child = child.replace('firrtl.module @', 'firrtl.extmodule private @').replace('%', '')
    child += ' attributes {defname = "FPGATop"}'
    path.write_text('module { firrtl.circuit "F1Shim" {\n' + child + '\n' + bodies['F1Shim'] + '\n} }\n')


def routes(text, child_header=None):
    # The focused candidate declares FPGATop externally; use the oracle's
    # child interface to classify the retained instance connections.
    ph = child_header if child_header is not None else module(text, 'FPGATop')[0]
    h, b = module(text, 'F1Shim')
    child = instance(b, 'FPGATop', 'top')
    eq = {k: ' '.join(v.split()) for k, v in equations(b).items()}
    outputs = {v: 'top.' + k for k, v in child.items() if ports(ph)[k][0] == 'output'}
    def resolve(v, active=()):
        if v in outputs: return outputs[v]
        if v in eq:
            assert v not in active, 'combinational loop'
            return resolve(eq[v], active + (v,))
        if m := re.fullmatch(r"\d+'h([0-9a-fA-F]+)", v): return str(int(m[1], 16))
        return v
    result = {'top.' + k: resolve(v) for k, v in child.items() if ports(ph)[k][0] == 'input'}
    for k, (d, _) in ports(h).items():
        if d == 'output': result[k] = resolve(k)
    return result


def counter_schedule(native, golden_body):
    # Interpret the actual CIRCT operation DAG feeding each register, rather
    # than computing candidate next state from a hand-written counter model.
    ops, drivers, registers = {}, {}, {}
    for line in native.splitlines()[1:]:
        if m := re.search(r'(%\w+) = firrtl\.(\w+)[ (](.*)', line):
            name, kind, operands = m.groups()
            if kind == 'regreset':
                xs = re.findall(r'%\w+', operands)
                assert len(xs) == 3 and xs[:2] == ['%clock', '%reset']
                assert operands.endswith('!firrtl.uint<12>')
                registers[name] = xs[2]
            else: ops[name] = (kind, operands)
        if m := re.search(r'firrtl.strictconnect (%\w+), (%\w+)', line):
            assert m[1] not in drivers
            drivers[m[1]] = m[2]
    assert set(registers) == {'%wCounterValue', '%rCounterValue'}
    eq = equations(golden_body)
    for name, enable, next_name, ch in [('wCounterValue', '_T', '_wrap_value_T_1', 'aw'),
                                      ('rCounterValue', '_T_1', '_wrap_value_T_3', 'ar')]:
        assert re.search(r'reg\s+\[11:0\]\s+' + name + r';', golden_body)
        assert eq[enable] == f'io_master_{ch}_ready & io_master_{ch}_valid'
        assert eq[next_name] == name + " + 12'h1"
        assert re.search(r'if \(reset\) begin\s*' + name + r" <= 12'h0;\s*end else if \(" + enable + r'\) begin\s*' + name + r' <= ' + next_name + ';', golden_body)
    state = dict.fromkeys(registers, 0); reference = dict(state); rng = random.Random(120)
    wraps = dict.fromkeys(state, 0); stalls = resets = 0
    for cycle in range(30000):
        reset = cycle in (0, 19000, 19001)
        # Long uninterrupted periods force modulo rollover. Later schedules
        # independently stall AW/AR and change ignored host request IDs.
        inputs = {'%reset': int(reset), '%io_master': {
            ch: {'valid': int(cycle < 9000 or rng.randrange(2)),
                 'ready': int(cycle < 9000 or rng.randrange(2)),
                 'bits': {'id': rng.randrange(4096)}} for ch in ('aw', 'ar')}}
        def evaluate(v):
            if v in state: return state[v]
            if v in inputs: return inputs[v]
            kind, operands = ops[v]
            xs = re.findall(r'%\w+', operands)
            if kind == 'constant': return int(operands.split()[0])
            if kind == 'subfield': return evaluate(xs[0])[re.search(r'\[(\w+)\]', operands)[1]]
            if kind == 'and': return evaluate(xs[0]) & evaluate(xs[1])
            if kind == 'add': return evaluate(xs[0]) + evaluate(xs[1])
            if kind == 'bits':
                hi, lo = map(int, re.search(r' (\d+) to (\d+)', operands).groups())
                return (evaluate(xs[0]) >> lo) & ((1 << (hi-lo+1))-1)
            if kind == 'mux': return evaluate(xs[1] if evaluate(xs[0]) else xs[2])
            raise AssertionError('unsupported counter operation ' + kind)
        next_state = {n: evaluate(registers[n] if reset else drivers[n]) for n in state}
        for n, ch in [('%wCounterValue', 'aw'), ('%rCounterValue', 'ar')]:
            fire = inputs['%io_master'][ch]['ready'] & inputs['%io_master'][ch]['valid']
            wraps[n] += int(not reset and fire and reference[n] == 4095)
            stalls += int(not fire and not reset)
            reference[n] = 0 if reset else (reference[n] + fire) & 4095
        resets += reset
        assert next_state == reference, f'counter state differs at host cycle {cycle}'
        state = next_state
    assert min(wraps.values()) >= 2 and stalls and resets == 3
    return dict(host_cycles=30000, counter_wraps=wraps, independent_stalls=stalls,
                synchronous_reset_cycles=resets, actual_CIRCT_DAG_matches_SFC=True)


def counter_mutations(native, golden_body):
    mutants = [
        native.replace('firrtl.constant 0 : !firrtl.uint<12>', 'firrtl.constant 1 : !firrtl.uint<12>', 1),
        native.replace('firrtl.constant 1 : !firrtl.uint<12>', 'firrtl.constant 2 : !firrtl.uint<12>', 1),
        native.replace('11 to 0 : (!firrtl.uint<13>)', '10 to 0 : (!firrtl.uint<13>)', 1),
        re.sub(r'(firrtl.and) (%\w+), %\w+', r'\1 \2, \2', native, count=1),
    ]
    for mutant in mutants:
        assert mutant != native
        try: counter_schedule(mutant, golden_body)
        except AssertionError: pass
        else: raise AssertionError('counter mutation escaped differential schedule')
    return len(mutants)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('before', 'after'): p.add_argument('--' + k, type=Path, action='append', required=True)
    for k in ('baseline', 'sfc', 'report-json'): p.add_argument('--' + k, type=Path, required=True)
    for k in ('stub', 'rtl', 'defines'): p.add_argument('--' + k, type=Path)
    a = p.parse_args(); assert len(a.before) == len(a.after)
    native = modules(a.baseline.read_text())['F1Shim']
    sfc = clean(a.sfc.read_text()); golden_body = module(sfc, 'F1Shim')[1]
    report = dict(golden=str(a.sfc.resolve()), orders=[compare(x, y, native) for x, y in zip(a.before, a.after)],
                  counters=counter_schedule(modules(a.after[0].read_text())['F1Shim'], golden_body))
    report['counter_mutations_rejected'] = counter_mutations(modules(a.after[0].read_text())['F1Shim'], golden_body)
    if a.stub: stub(a.after[0], a.stub)
    if a.rtl:
        rtl = clean(a.rtl.read_text())
        golden_ports = ports(module(sfc, 'F1Shim')[0]); candidate_ports = ports(module(rtl, 'F1Shim')[0])
        assert len(golden_ports) == 143 and candidate_ports == golden_ports
        expected, actual = routes(sfc), routes(rtl, module(sfc, 'FPGATop')[0])
        assert actual == expected, [(k, actual.get(k), v) for k, v in expected.items() if actual.get(k) != v]
        report['SFC_platform_ABI'] = dict(leaves=143, names_widths_directions_match=True,
                                          matched_combinational_routes=len(expected))
    if a.defines:
        golden_defines = a.sfc.with_suffix('.defines.vh')
        assert a.defines.read_bytes() == golden_defines.read_bytes()
        report['DDR_collateral'] = dict(golden=str(golden_defines.resolve()), complete_body_match=True)
    report['limitation'] = 'Replay inherits explicitly seeded FAME host identities; SFC has Print disabled. Enabled-Print production ingestion/runtime parity pending.'
    a.report_json.write_text(json.dumps(report, indent=2) + '\n'); print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
