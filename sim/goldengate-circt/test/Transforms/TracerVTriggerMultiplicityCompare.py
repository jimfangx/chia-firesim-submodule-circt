#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare all four native quad-Rocket trigger banks against archived SFC RTL.

This is a compiler boundary test. MMIO transactions and stream storage remain
separate stages and this test does not establish a working quad-core simulator.
"""
import json
import random
import re
import subprocess
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, module, without_targets
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring
from PrintTracerVTriggerCompare import RTL


def compare_wiring(text):
    outer = wiring(text, 'GGTracerVTriggerWrapper', aggregate=True)
    token = native_module(text, 'GGTracerVTokenWrapper')
    constants = re.findall(r'(%\w+) = firrtl.constant 1 : !firrtl.uint<1>', token)
    token = token.replace('firrtl.module @GGTracerVTokenWrapper(',
        'firrtl.module @GGTracerVTokenWrapper(' + ''.join(f'in {v}: !firrtl.uint<1>, ' for v in constants))
    inner = wiring(token, 'GGTracerVTokenWrapper', aggregate=True)
    for n in range(4):
        suffix = f'_{n}' if n else ''
        bank, engine = 'tracervRegisters' + suffix, f'TracerVBridgeModule_{n}'
        assert outer[bank + '.clock'] == 'top.hostClock'
        assert outer[bank + '.reset'] == 'top.hostReset'
        for port in ('control', 'trace'):
            assert outer[bank + '.' + port] == 'sim.tracerv_' + port + suffix
        assert outer['top.tracerv_mcr' + suffix] == bank + '.mcr'
        assert inner['top.tracerv_control' + suffix] == engine + '.control'
        assert inner['top.tracerv_stream' + suffix] == engine + '.stream'
        base = engine + '.hPort.hBits.tiletrace_'
        assert inner['top.tracerv_trace' + suffix + '.valid'] == frozenset(
            (base + 'trace_retiredinsns_0_valid', 'not', base + 'reset'))
        for field in ('iaddr', 'insn'):
            assert inner['top.tracerv_trace' + suffix + '.' + field] == base + 'trace_retiredinsns_0_' + field


def main():
    candidate, golden, firtool, evidence = map(Path, sys.argv[1:])
    evidence.mkdir(parents=True, exist_ok=False)
    text = (candidate / 'post-fame-tracerv-trigger.mlir').read_text()
    prior = (candidate / 'post-fame-tracerv-token.mlir').read_text()
    compare_wiring(text)
    # All pre-existing modules except the observed top are byte-preserved.
    names = re.findall(r'^    firrtl.module @(\w+)\(', prior, re.M)
    for name in names:
        if name != 'GGTracerVTokenWrapper':
            assert native_module(text, name) == native_module(prior, name), name
    assert set(re.findall(r'^    firrtl.module @(\w+)\(', text, re.M)) - set(names) == {
        'GGTracerVTriggerWrapper', *(f'GGTracerVTriggerConfig_{n}' for n in range(1, 4)), 'GGTracerVTriggerConfig'}
    old = json.loads((candidate / 'post-fame-tracerv-token-all.json').read_text())
    annos = json.loads((candidate / 'post-fame-tracerv-trigger-all.json').read_text())
    assert without_targets(annos) == without_targets(old)
    clocks = lambda rows: [a for a in rows if a['class'] == 'midas.passes.ChannelClockInfoAnnotation']
    assert clocks(annos) == clocks(old)
    bridges = [a for a in annos if a.get('widgetClass', '').endswith('TracerVBridgeModule')]
    assert [a['target'] for a in bridges] == ['~GGTracerVTriggerWrapper|GGTracerVTokenEngine' +
        (f'_{n}' if n else '') + '>hPort' for n in range(4)]
    # Prove that a crossed fourth control connection is detected.
    bad = text.replace('firrtl.connect %tracervRegisters_3_control, %sim_tracerv_control_3',
                       'firrtl.connect %tracervRegisters_3_control, %sim_tracerv_control')
    assert bad != text
    try: compare_wiring(bad)
    except AssertionError: pass
    else: raise AssertionError('crossed fourth bank accepted')
    oracle_path = golden / 'FireSim-generated.sv'
    oracle = RTL(*module(clean(oracle_path.read_text()), 'TracerVBridgeModule'))
    rng = random.Random(238); samples = 0
    reports = []
    for n in range(4):
        name = 'GGTracerVTriggerConfig' + (f'_{n}' if n else '')
        source, rtl = evidence / (name + '.mlir'), evidence / (name + '.sv')
        source.write_text('module { firrtl.circuit "' + name + '" {\n' + native_module(text, name) + '\n} }\n')
        subprocess.run([str(firtool), str(source), '--format=mlir', '--disable-all-randomization',
                        '--strip-debug-info', '-o', str(rtl)], check=True)
        bank = RTL(*module(clean(rtl.read_text()), name))
        assert len(bank.registers) == 22
        for reg, width in bank.registers.items(): assert oracle.registers[reg] == width
        trajectory = {reg: 0 for reg in oracle.registers}
        for i in range(4096):
            state = ({reg: rng.getrandbits(width) for reg, width in oracle.registers.items()}
                     if i < 2048 else dict(trajectory))
            state['triggerSelector'] = i % 6
            pc, insn, cycle = rng.getrandbits(40), rng.getrandbits(32), rng.getrandbits(64)
            state['triggerPCStart'] = pc if i & 1 else pc ^ 1
            state['triggerPCEnd'] = pc if i & 2 else pc ^ 2
            state['hostTriggerStartInst'] = insn if i & 4 else insn ^ 1
            state['hostTriggerEndInst'] = insn if i & 8 else insn ^ 2
            state['hostTriggerStartInstMask'] = 0 if i & 16 else 0xffffffff
            state['hostTriggerEndInstMask'] = 0xffffffff
            state['trace_cycle_counter'] = cycle
            state['triggerCycleCountStart'] = cycle if i & 32 else cycle ^ 1
            state['triggerCycleCountEnd'] = cycle if i & 64 else cycle ^ 2
            reset, valid, target_reset = int(i % 17 == 0), (i >> 7) & 1, (i >> 8) & 1
            reference = dict(state, reset=reset, hPort_hBits_tiletrace_reset=target_reset,
                             hPort_hBits_tiletrace_trace_retiredinsns_0_valid=valid,
                             hPort_hBits_tiletrace_trace_retiredinsns_0_iaddr=pc,
                             hPort_hBits_tiletrace_trace_retiredinsns_0_insn=insn,
                             hPort_toHost_hValid=0, hPort_fromHost_hReady=0, streamToHostCPU_ready=0)
            actual = {reg: state[reg] for reg in bank.registers}
            actual.update(reset=reset, trace_valid=valid & (1 - target_reset), trace_iaddr=pc,
                          trace_insn=insn, control_tCycle=cycle)
            for slot in range(15):
                write, data = rng.getrandbits(1), rng.getrandbits(32)
                reference[f'crFile_io_mcr_write_{slot}_valid'] = write
                reference[f'crFile_io_mcr_write_{slot}_bits'] = data
                actual[f'mcr_write_{slot}_valid'] = write
                actual[f'mcr_write_{slot}_bits'] = data
            a, r = bank.edge(actual), oracle.edge(reference)
            for reg in bank.registers:
                assert a[reg] == r[reg], (n, i, reg, a[reg], r[reg]); samples += 1
            trajectory = {reg: r[reg] for reg in oracle.registers}
            for out, expected in (('control_trigger', 'trigger'), ('control_initDone', 'initDone'), ('control_traceEnable', 'traceEnable')):
                assert bank.eval(('leaf', out))[1] == oracle.eval(('leaf', expected))[1]; samples += 1
            for slot in range(2):
                assert bank.eval(('leaf', f'mcr_read_{slot}_bits')) == oracle.eval(('leaf', f'crFile_io_mcr_read_{slot}_bits')); samples += 1
        reports.append(dict(module=name, registers=22, mmio_words=15, host_cycles=4096))
    result = dict(golden_rtl=str(oracle_path), matching_state_and_output_samples=samples,
                  banks=reports, unchanged_modules=len(names)-1, unchanged_annotation_records=len(annos),
                  independent_trace_control_mcr_routes=4, rejected_crossed_control_mutation=True,
                  clock_map_preserved=True, boundary='four trigger banks; MMIO and stream storage pending')
    (evidence / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS four independent trigger banks; {samples} SFC state/output comparisons')


if __name__ == '__main__': main()
