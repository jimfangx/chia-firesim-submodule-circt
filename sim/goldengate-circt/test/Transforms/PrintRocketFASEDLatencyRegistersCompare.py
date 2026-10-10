#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket latency-register bank attachment with SFC MMIO."""
import itertools
import json
import random
import re
import sys
from pathlib import Path
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketMasterCompare import native_module
from PrintRocketResponsesCompare import wiring
from PrintRocketFASEDIssueCompare import expressions, evaluate


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    host = module(reference, 'FASEDMemoryTimingModel')[1]
    eq = equations(host)
    for slot, name, direction in ((0, 'writeLatency', 'write'), (1, 'readLatency', 'read')):
        assert re.search(r'reg \[31:0\] ' + name + ';', host)
        assert eq[f'model_tNasti_io_mmReg_{name}'] == name
        assert eq[f'crFile_io_mcr_read_{slot}_bits'] == name
        assert re.search(r"if \(reset\) begin\s*" + name + r" <= 32'h1e;\s*end else if \(crFile_io_mcr_write_" + str(slot) + r'_valid\) begin\s*' + name + r' <= crFile_io_mcr_write_' + str(slot) + '_bits;', host)
    baseline = (evidence / 'candidate/post-fame-fased-latency-registers.mlir').read_text()
    old, new = 'GGFASEDRequestLimitsWrapper', 'GGFASEDLatencyRegistersWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-request-limits{suffix}.mlir').read_text()
        text = (evidence / f'binding.mlir.rocket-fased-latency-registers{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        consumed = {'fased_write_latency', 'fased_read_latency'}
        copied = [(d, n) for d, n in ports if n not in consumed]
        expected_ports = copied + [('out', 'fased_latency_mcr')]
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        nets.update({'latencyRegisters.clock': 'top.hostClock', 'latencyRegisters.reset': 'top.hostReset',
                     'sim.fased_write_latency': 'latencyRegisters.latencies.write',
                     'sim.fased_read_latency': 'latencyRegisters.latencies.read',
                     'top.fased_latency_mcr': 'latencyRegisters.mcr'})
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, new, aggregate=True)
        assert all(shared[k] == v for k, v in nets.items() if k.startswith(('latencyRegisters.', 'sim.fased_', 'top.fased_')))
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in copied:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        bank = native_module(text, 'GGFASEDLatencyRegisters')
        assert bank == native_module(baseline, 'GGFASEDLatencyRegisters')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', bank) == [('writeLatency', '0'), ('readLatency', '4')]
        for name in ('writeLatency', 'readLatency'):
            assert re.search(r'%' + name + r' = firrtl.regreset %clock, %reset, %c30_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert 'firrtl.constant 30 : !firrtl.uint<32>' in bank
        values, assertions = expressions(text, 'GGFASEDLatencyRegisters')
        assert not assertions
        counts = dict(transitions=0, writes=0, zero_strobe_writes=0, reset_writes=0, upper_bits=0, writes_during_target_stall=0)

        def sample(state, data, mask, strobe, reset, fire):
            inputs = dict(zip(('writeLatency', 'readLatency'), state))
            inputs.update(reset=reset, targetFire=fire, **{'mcr.wstrb': strobe})
            for i in range(2):
                inputs[f'mcr.write[{i}].valid'] = mask >> i & 1
                inputs[f'mcr.write[{i}].bits'] = data[i]
            next_state = []
            for i, (name, direction) in enumerate((('writeLatency', 'write'), ('readLatency', 'read'))):
                ev = lambda k: evaluate(values[k], inputs, values)
                assert ev(f'mcr.read[{i}].bits') == state[i]
                assert ev(f'mcr.read[{i}].valid') == 1 and ev(f'mcr.write[{i}].ready') == 1
                assert ev(f'latencies.{direction}') == state[i]
                written = bool(mask >> i & 1)
                expected = data[i] if written else state[i]
                assert ev(name) == expected
                next_state.append(30 if reset else expected)
                counts['writes'] += written
                counts['zero_strobe_writes'] += written and strobe == 0
                counts['reset_writes'] += written and reset
                counts['upper_bits'] += written and data[i] > 15
                counts['writes_during_target_stall'] += written and not fire
            counts['transitions'] += 1
            return next_state

        edges = (0, 1, 30, 15, 16, 31, 0x80000000, 0xffffffff)
        for state, data, mask, strobe, reset, fire in itertools.product(
                itertools.product(edges, repeat=2), ((0xffffffff, 16), (0, 30)), range(4), range(16), range(2), range(2)):
            sample(state, data, mask, strobe, reset, fire)
        rng = random.Random(229); state = [30, 30]
        for _ in range(20000):
            state = sample(state, [rng.getrandbits(32), rng.getrandbits(32)], rng.randrange(4), rng.randrange(16), rng.randrange(31) == 0, rng.randrange(2))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_module='FASEDMemoryTimingModel', constructor_orders=reports,
                  reference_has_print_host=False,
                  scope='host-reset words 0/1 and full-width timing latencies; decoded MCR fragment remains external')
    (evidence / 'rocket-fased-latency-registers-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC latency-register reset/write/readback semantics and allocated-bank attachment in both Print constructor orders')


if __name__ == '__main__': main()
