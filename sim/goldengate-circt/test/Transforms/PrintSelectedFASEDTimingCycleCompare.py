#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print model cycles with immutable SFC LatencyPipe.

Pin the oracle's counter/deadline equations and gated model clock, then
interpret actual CIRCT SSA. The reference disables Print; retain allocated
banks and modules independently in both selected Print boundaries.
"""
import itertools
import json
import random
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketFASEDReadBufferCompare import wrapper_wiring
from PrintRocketFASEDIssueCompare import expressions, evaluate
from PrintRocketControlMasterCompare import native_ports


def transfer(value, old, new, copied):
    if isinstance(value, list): return [transfer(v, old, new, copied) for v in value]
    if isinstance(value, dict): return {k: transfer(v, old, new, copied) for k, v in value.items()}
    if not isinstance(value, str): return value
    if value == '~' + old: return '~' + new
    prefix = '~' + old + '|'
    if not value.startswith(prefix): return value
    result = value[len(prefix):]
    if result.startswith(old + '>'):
        ref = result[len(old) + 1:]
        if re.split(r'[.\[]', ref)[0] in copied: result = new + '>' + ref
    return '~' + new + '|' + result


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    pipe = module(reference, 'LatencyPipe')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(pipe).items()}
    assert re.search(r'reg\s+\[63:0\] tCycle;', pipe)
    assert eq['_tCycle_T_1'] == "tCycle + 64'h1"
    assert re.search(r"always @\(posedge clock\) begin\s*if \(reset\) begin\s*"
                     r"tCycle <= 64'h0;\s*end else begin\s*tCycle <= _tCycle_T_1;", pipe)
    for channel, extension in [('write', '_GEN_12'), ('read', '_GEN_13')]:
        assert eq[extension] == "{{32'd0}, tNasti_io_mmReg_" + channel + 'Latency}'
        add = '_' + channel + 'Pipe_io_enq_bits_releaseCycle_T_1'
        assert eq[add] == extension + ' + tCycle'
        assert eq[channel + 'Pipe_io_enq_bits_releaseCycle'] == add + " - 64'h1"
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['gate_I'] == 'clock' and host['model_clock'] == 'gate_O'
    assert host['model_reset'] == 'hPort_hBits_reset'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    baseline = baseline_path.read_text()
    reports = []
    mask = (1 << 64) - 1
    for allocation in ('candidate', 'compatibility'):
        output = evidence / allocation
        text = (output / 'post-print-fased-timing-cycle.mlir').read_text()
        before = (output / 'post-print-fased-response-releaser.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior = dict(re.findall(pattern, before, re.M | re.S))
        after = dict(re.findall(pattern, text, re.M | re.S))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDTimingCycle', 'GGSelectedPrintFASEDTimingCycleWrapper'}
        old, new = 'GGSelectedPrintFASEDResponseReleaserWrapper', 'GGSelectedPrintFASEDTimingCycleWrapper'
        old_header = native_module(before, old).splitlines()[0]
        old_ports = re.findall(r'\b(in|out) %(\w+):', old_header)
        previous_annotations = json.loads((output / 'post-print-fased-response-releaser-all.json').read_text())
        annotations = json.loads((output / 'post-print-fased-timing-cycle-all.json').read_text())
        assert annotations == transfer(previous_annotations, old, new, {n for _, n in old_ports}), 'annotation identity differs'
        helper = native_module(text, 'GGFASEDTimingCycle')
        assert helper == native_module(baseline, 'GGFASEDTimingCycle')
        assert len(re.findall(' = firrtl.regreset ', helper)) == 1
        assert ' = firrtl.reg ' not in helper and 'firrtl.mem ' not in helper
        state = re.search(r'(%\w+) = firrtl.regreset %clock, (%\w+), (%\w+) \{name = "tCycle"\}'
                          r' : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<64>, !firrtl.uint<64>', helper)
        assert state and re.search(re.escape(state[2]) + r' = firrtl.and %reset, %targetFire ', helper)
        assert re.search(re.escape(state[3]) + r' = firrtl.constant 0 : !firrtl.uint<64>', helper)
        for channel in ('read', 'write'):
            assert re.search('firrtl.pad %' + channel + r'Latency, 64 : \(!firrtl.uint<32>\) -> !firrtl.uint<64>', helper)
        nets, assertions = expressions(helper, 'GGFASEDTimingCycle')
        assert not assertions
        counts = dict(transitions=0, stalls=0, stalled_resets=0, enabled_resets=0,
                      counter_wraps=0, deadline_underflows=0, deadline_overflows=0)

        def sample(cycle, read, write, flags):
            reset, fire = flags & 1, (flags >> 1) & 1
            inputs = {'reset': reset, 'targetFire': fire, state[1][1:]: cycle,
                      'readLatency': read, 'writeLatency': write}
            value = lambda name: evaluate(nets[name], inputs, nets)
            assert value('tCycle') == cycle
            for channel, latency in [('read', read), ('write', write)]:
                assert value(channel + 'ReleaseCycle') == (cycle + latency - 1) & mask
                counts['deadline_underflows'] += cycle + latency == 0
                counts['deadline_overflows'] += cycle + latency - 1 > mask
            expected = 0 if fire and reset else (cycle + 1) & mask if fire else cycle
            # The reset operand and zero value are pinned to the actual
            # RegResetOp above; evaluate its next-state SSA on other edges.
            actual = 0 if fire and reset else value(state[1][1:])
            assert actual == expected
            counts['transitions'] += 1
            counts['stalls'] += not fire
            counts['stalled_resets'] += reset and not fire
            counts['enabled_resets'] += reset and fire
            counts['counter_wraps'] += fire and not reset and cycle == mask
            return actual

        cycles = [0, 1, 1234, (1 << 32) - 1, 1 << 32, 1 << 63, mask - 1, mask]
        latencies = [0, 1, 2, 30, (1 << 31) - 1, 1 << 31, (1 << 32) - 2, (1 << 32) - 1]
        for cycle, read, write, flags in itertools.product(cycles, latencies, latencies, range(4)):
            sample(cycle, read, write, flags)
        rng = random.Random(220)
        for _ in range(10000): sample(rng.getrandbits(64), rng.getrandbits(32), rng.getrandbits(32), rng.randrange(4))
        cycle = mask - 10
        for _ in range(10000): cycle = sample(cycle, rng.getrandbits(32), rng.getrandbits(32), rng.randrange(4))
        assert all(counts.values()), 'missing counter/deadline coverage'
        wrapper = native_module(text, new)
        assert native_ports(wrapper)[:len(old_ports)] == native_ports(native_module(before, old)), 'copied port identities differ'
        wiring = wrapper_wiring(wrapper)
        connections = {'timer.clock': 'top.hostClock', 'timer.reset': 'sim.fased_model_reset',
                       'timer.targetFire': 'sim.fased_tfire', 'top.fased_timing_cycle': 'timer.tCycle',
                       'timer.readLatency': 'top.fased_read_latency', 'timer.writeLatency': 'top.fased_write_latency',
                       'top.fased_read_release_cycle': 'timer.readReleaseCycle',
                       'top.fased_write_release_cycle': 'timer.writeReleaseCycle'}
        for direction, name in old_ports:
            connections[('sim.' if direction == 'in' else 'top.') + name] = ('top.' if direction == 'in' else 'sim.') + name
        assert wiring == connections, 'cycle wrapper or retained boundary wiring differs'
        added = {'fased_timing_cycle': ('out', 64), 'fased_read_latency': ('in', 32),
                 'fased_write_latency': ('in', 32), 'fased_read_release_cycle': ('out', 64),
                 'fased_write_release_cycle': ('out', 64)}
        ports = re.findall(r'\b(?:in|out) %(\w+):', wrapper.splitlines()[0])
        assert set(ports) == {name for _, name in old_ports} | added.keys()
        for name, (direction, width) in added.items():
            assert f'{direction} %{name}: !firrtl.uint<{width}>' in wrapper.splitlines()[0]
        # This partial selected boundary allocates Print/count banks; the FASED
        # latency register bank is still external. Pin both selected layouts.
        decoder = native_module(text, 'GGControlAddressDecode')
        print_slave, print_start, count_size = (1, 64, 8) if allocation == 'candidate' else (0, 0, 4)
        assert f'name = "PrintBridgeModule_0", size = 32 : i64, slave = {print_slave} : i32, start = {print_start} : i64' in decoder
        assert f'name = "CPUManagedStreamEngine_0", size = {count_size} : i64, slave = {print_slave + 1} : i32, start = {print_start + 32} : i64' in decoder
        rtl = clean((output / 'post-print-fased-timing-cycle.sv').read_text())
        timer_rtl = module(rtl, 'GGFASEDTimingCycle')[1]
        rtl_eq = equations(timer_rtl)
        assert rtl_eq['tCycle'] == 'tCycle_0'
        assert re.search(r"if \(reset & targetFire\)\s*tCycle_0 <= 64'h0;\s*"
                         r"else if \(targetFire\)\s*tCycle_0 <= 64'\(tCycle_0 \+ 64'h1\);", timer_rtl)
        for ch in ('read', 'write'):
            # CIRCT reassociates the unsigned modular sum and retains explicit
            # 64-bit truncations; SSA interpretation above checks its arithmetic.
            assert rtl_eq[ch + 'ReleaseCycle'] == "64'({32'h0, " + ch + "Latency} + 64'(tCycle_0 - 64'h1))"
        top_body = module(rtl, new)[1]
        top_rtl = equations(top_body)
        instance = re.search(r'GGFASEDTimingCycle timer \((.*?)\);', top_body, re.S)
        assert instance
        timer_ports = dict(re.findall(r'\.(\w+)\s*\(\s*(\w+)\s*\)', instance[1]))
        assert timer_ports == {'clock': 'hostClock', 'reset': top_rtl['fased_model_reset'],
                               'targetFire': top_rtl['fased_tfire'], 'tCycle': 'fased_timing_cycle',
                               'readLatency': 'fased_read_latency', 'writeLatency': 'fased_write_latency',
                               'readReleaseCycle': 'fased_read_release_cycle',
                               'writeReleaseCycle': 'fased_write_release_cycle'}
        reports.append({'allocation': allocation, 'existing_modules_retained': len(prior), **counts,
                        'annotation_records_preserved': len(annotations),
                        'wrapper_connections_compared': len(connections)})
    report = {'golden': str(golden), 'SFC_modules': ['LatencyPipe', 'FASEDMemoryTimingModel'],
              'allocations': reports, 'reference_has_print_host': False,
              'scope': 'enabled counter/reset and 64-bit release offsets; latency bank and completion queues remain external'}
    (evidence / 'timing-cycle-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC gated model counter, latency release offsets and selected Print composition in both allocations')


if __name__ == '__main__': main()
