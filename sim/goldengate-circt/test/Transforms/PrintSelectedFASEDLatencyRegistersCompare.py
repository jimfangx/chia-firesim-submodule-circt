#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print/Rocket latency-register bank attachment with SFC MMIO."""
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
from PrintRocketControlMasterCompare import native_ports
from PrintSelectedFASEDTimingCycleCompare import transfer


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    host = module(reference, 'FASEDMemoryTimingModel')[1]
    eq = equations(host)
    for slot, name, direction in ((0, 'writeLatency', 'write'), (1, 'readLatency', 'read')):
        assert re.search(r'reg \[31:0\] ' + name + ';', host)
        assert eq[f'model_tNasti_io_mmReg_{name}'] == name
        assert eq[f'crFile_io_mcr_read_{slot}_bits'] == name
        assert re.search(r"if \(reset\) begin\s*" + name + r" <= 32'h1e;\s*end else if \(crFile_io_mcr_write_" + str(slot) + r'_valid\) begin\s*' + name + r' <= crFile_io_mcr_write_' + str(slot) + '_bits;', host)
    pipe_eq = equations(module(reference, 'LatencyPipe')[1])
    for side, extension in (('write', '_GEN_12'), ('read', '_GEN_13')):
        assert pipe_eq[extension] == "{{32'd0}, tNasti_io_mmReg_" + side + 'Latency}'
        add = '_' + side + 'Pipe_io_enq_bits_releaseCycle_T_1'
        assert pipe_eq[add] == extension + ' + tCycle'
        assert pipe_eq[side + 'Pipe_io_enq_bits_releaseCycle'] == add + " - 64'h1"
    baseline = baseline_path.read_text()
    old, new = 'GGSelectedPrintFASEDRequestLimitsWrapper', 'GGSelectedPrintFASEDLatencyRegistersWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration159-print-fased-request-limits' / allocation
        before = (root / 'post-print-fased-request-limits.mlir').read_text()
        text = (root / 'post-print-fased-latency-registers.mlir').read_text()
        for ext in ('.mlir', '.sv', '-all.json'):
            name = 'post-print-fased-request-limits' + ext
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding request-limit boundary changed'
        for name in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'allocated collateral changed'
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        prior_ports = native_ports(native_module(before, old))
        ports = [(d, n) for d, n, _ in prior_ports]
        consumed = {'fased_write_latency', 'fased_read_latency'}
        copied = [(d, n) for d, n in ports if n not in consumed]
        expected_ports = copied + [('out', 'fased_latency_mcr')]
        actual_ports = native_ports(native_module(text, new))
        assert [(d, n) for d, n, _ in actual_ports] == expected_ports
        assert actual_ports[:-1] == [(d, n, t) for d, n, t in prior_ports if n not in consumed]
        assert actual_ports[-1][2] == native_ports(native_module(baseline, 'GGFASEDLatencyRegistersWrapper'))[-1][2]
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        nets.update({'latencyRegisters.clock': 'top.hostClock', 'latencyRegisters.reset': 'top.hostReset',
                     'sim.fased_write_latency': 'latencyRegisters.latencies.write',
                     'sim.fased_read_latency': 'latencyRegisters.latencies.read',
                     'top.fased_latency_mcr': 'latencyRegisters.mcr'})
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, 'GGFASEDLatencyRegistersWrapper', aggregate=True)
        assert all(shared[k] == v for k, v in nets.items() if k.startswith(('latencyRegisters.', 'sim.fased_', 'top.fased_')))
        prior_annos = json.loads((root / 'post-print-fased-request-limits-all.json').read_text())
        annos = json.loads((root / 'post-print-fased-latency-registers-all.json').read_text())
        assert annos == transfer(prior_annos, old, new, {n for _, n in copied}), 'annotation retarget differs'
        rtl = clean((root / 'post-print-fased-latency-registers.sv').read_text())
        rtl_top = module(rtl, new)[1]
        rtl_nets = {k: ' '.join(v.split()) for k, v in equations(rtl_top).items()}
        def pins(mod, instance):
            child = re.search(r'\b' + mod + r'\s+' + instance + r'\s*\((.*?)\);', rtl_top, re.S)
            assert child
            return {k: ' '.join(v.split()) for k, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', child[1])}
        bank_pins = pins('GGFASEDLatencyRegisters', 'latencyRegisters')
        sim_pins = pins(old, 'sim')
        def source(net):
            seen = set()
            while net in rtl_nets and net not in seen:
                seen.add(net); net = rtl_nets[net]
            return net
        assert bank_pins['clock'] == 'hostClock' and bank_pins['reset'] == 'hostReset'
        for direction in ('read', 'write'):
            assert source(sim_pins[f'fased_{direction}_latency']) == source(bank_pins[f'latencies_{direction}'])
        mcr_pins = {k: v for k, v in bank_pins.items() if k.startswith('mcr_')}
        rtl_bank = module(rtl, 'GGFASEDLatencyRegisters')[1]
        bank_eq = equations(rtl_bank)
        # CIRCT propagates constant valid/ready outputs into the wrapper and
        # leaves the corresponding instance outputs on unused wires.
        for pin, net in mcr_pins.items():
            external = source('fased_latency_' + pin)
            if pin in ('mcr_read_0_valid', 'mcr_read_1_valid', 'mcr_write_0_ready', 'mcr_write_1_ready'):
                assert external == bank_eq[pin] == "1'h1", 'constant MCR handshake differs'
            else:
                assert external == source(net), 'RTL MCR fragment wiring differs'
        assert 'always @(posedge clock)' in rtl_bank and 'targetFire' not in rtl_bank
        reset_body = re.search(r'if \(reset\) begin(.*?)end', rtl_bank, re.S)[1]
        for slot, name in enumerate(('writeLatency', 'readLatency')):
            assert re.search(name + r" <= 32'h1e;", reset_body, re.I)
            assert re.search(r'if \(mcr_write_' + str(slot) + r'_valid\)\s*' + name +
                             r' <= mcr_write_' + str(slot) + '_bits;', rtl_bank)
        bank = native_module(text, 'GGFASEDLatencyRegisters')
        assert bank == native_module(baseline, 'GGFASEDLatencyRegisters')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', bank) == [('writeLatency', '0'), ('readLatency', '4')]
        for name in ('writeLatency', 'readLatency'):
            assert re.search(r'%' + name + r' = firrtl.regreset %clock, %reset, %c30_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert 'firrtl.constant 30 : !firrtl.uint<32>' in bank
        values, assertions = expressions(text, 'GGFASEDLatencyRegisters')
        assert not assertions
        timing_values, timing_assertions = expressions(text, 'GGFASEDTimingCycle')
        assert not timing_assertions
        timer_state = re.search(r'(%\w+) = firrtl.regreset .*?name = "tCycle"', native_module(text, 'GGFASEDTimingCycle'))[1][1:]
        counts = dict(transitions=0, writes=0, zero_strobe_writes=0, reset_writes=0, upper_bits=0, writes_during_target_stall=0, deadline_underflows=0, deadline_overflows=0)

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
                cycle = ((state[1 - i] << 32) | data[i])
                timing_inputs = {timer_state: cycle, 'readLatency': state[1], 'writeLatency': state[0]}
                for side, latency in (('read', state[1]), ('write', state[0])):
                    assert evaluate(timing_values[side + 'ReleaseCycle'], timing_inputs, timing_values) == ((cycle + latency - 1) & ((1 << 64) - 1))
                    counts['deadline_underflows'] += cycle + latency == 0
                    counts['deadline_overflows'] += cycle + latency - 1 > ((1 << 64) - 1)
                written = bool(mask >> i & 1)
                expected = data[i] if written else state[i]
                assert ev(name) == expected
                next_state.append(30 if reset else expected)
                counts['writes'] += written
                counts['zero_strobe_writes'] += written and strobe == 0
                counts['reset_writes'] += written and reset
                counts['upper_bits'] += written and data[i] > 65535
                counts['writes_during_target_stall'] += written and not fire
            counts['transitions'] += 1
            return next_state

        edges = (0, 1, 30, 15, 16, 31, 0x80000000, 0xffffffff)
        for state, data, mask, strobe, reset, fire in itertools.product(
                itertools.product(edges, repeat=2), ((0xffffffff, 16), (0, 30)), range(4), range(16), range(2), range(2)):
            sample(state, data, mask, strobe, reset, fire)
        rng = random.Random(233); state = [30, 30]
        for _ in range(20000):
            state = sample(state, [rng.getrandbits(32), rng.getrandbits(32)], rng.randrange(4), rng.randrange(16), rng.randrange(31) == 0, rng.randrange(2))
        assert all(counts.values())
        reports.append(dict(allocation=allocation, annotations_transferred=len(annos), rtl_mcr_pins_compared=len(mcr_pins), unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_modules=['FASEDMemoryTimingModel', 'LatencyPipe'], allocations=reports,
                  reference_has_print_host=False,
                  scope='host-reset words 0/1 and full-width timing latencies and modulo-64-bit deadlines; decoded MCR fragment remains external')
    (evidence / 'selected-fased-latency-registers-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC latency-register reset/write/readback semantics and allocated-bank attachment in both selected Print allocations')


if __name__ == '__main__': main()
