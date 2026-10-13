#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print/Rocket request-limit bank attachment with SFC MMIO."""
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
    for slot, name, direction in ((2, 'writeMaxReqs', 'write'), (3, 'readMaxReqs', 'read')):
        assert re.search(r'reg \[31:0\] ' + name + ';', host)
        assert eq[f'model_tNasti_io_mmReg_{name}'] == name + '[3:0]'
        assert eq[f'crFile_io_mcr_read_{slot}_bits'] == name
        assert re.search(r"if \(reset\) begin\s*" + name + r" <= 32'ha;\s*end else if \(crFile_io_mcr_write_" + str(slot) + r'_valid\) begin\s*' + name + r' <= crFile_io_mcr_write_' + str(slot) + '_bits;', host)
    baseline = baseline_path.read_text()
    old, new = 'GGSelectedPrintFASEDReadAdmissionWrapper', 'GGSelectedPrintFASEDRequestLimitsWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration158-print-fased-read-admission' / allocation
        before = (root / 'post-print-fased-read-admission.mlir').read_text()
        text = (root / 'post-print-fased-request-limits.mlir').read_text()
        for ext in ('.mlir', '.sv', '-all.json'):
            name = 'post-print-fased-read-admission' + ext
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding admission boundary changed'
        for name in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'allocated collateral changed'
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        prior_ports = native_ports(native_module(before, old))
        ports = [(d, n) for d, n, _ in prior_ports]
        consumed = {'fased_write_max_reqs', 'fased_read_max_reqs'}
        copied = [(d, n) for d, n in ports if n not in consumed]
        expected_ports = copied + [('out', 'fased_request_limits_mcr')]
        actual_ports = native_ports(native_module(text, new))
        assert [(d, n) for d, n, _ in actual_ports] == expected_ports
        assert actual_ports[:-1] == [(d, n, t) for d, n, t in prior_ports if n not in consumed]
        assert actual_ports[-1][2] == native_ports(native_module(baseline, 'GGFASEDRequestLimitsWrapper'))[-1][2]
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        nets.update({'requestLimits.clock': 'top.hostClock', 'requestLimits.reset': 'top.hostReset',
                     'sim.fased_write_max_reqs': 'requestLimits.limits.write',
                     'sim.fased_read_max_reqs': 'requestLimits.limits.read',
                     'top.fased_request_limits_mcr': 'requestLimits.mcr'})
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, 'GGFASEDRequestLimitsWrapper', aggregate=True)
        assert all(shared[k] == v for k, v in nets.items() if k.startswith(('requestLimits.', 'sim.fased_', 'top.fased_')))
        prior_annos = json.loads((root / 'post-print-fased-read-admission-all.json').read_text())
        annos = json.loads((root / 'post-print-fased-request-limits-all.json').read_text())
        assert annos == transfer(prior_annos, old, new, {n for _, n in copied}), 'annotation retarget differs'
        rtl = clean((root / 'post-print-fased-request-limits.sv').read_text())
        rtl_top = module(rtl, new)[1]
        rtl_nets = {k: ' '.join(v.split()) for k, v in equations(rtl_top).items()}
        def pins(mod, instance):
            child = re.search(r'\b' + mod + r'\s+' + instance + r'\s*\((.*?)\);', rtl_top, re.S)
            assert child
            return {k: ' '.join(v.split()) for k, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', child[1])}
        bank_pins = pins('GGFASEDRequestLimits', 'requestLimits')
        sim_pins = pins(old, 'sim')
        def source(net):
            seen = set()
            while net in rtl_nets and net not in seen:
                seen.add(net); net = rtl_nets[net]
            return net
        assert bank_pins['clock'] == 'hostClock' and bank_pins['reset'] == 'hostReset'
        for direction in ('read', 'write'):
            assert source(sim_pins[f'fased_{direction}_max_reqs']) == source(bank_pins[f'limits_{direction}'])
        mcr_pins = {k: v for k, v in bank_pins.items() if k.startswith('mcr_')}
        rtl_bank = module(rtl, 'GGFASEDRequestLimits')[1]
        bank_eq = equations(rtl_bank)
        # CIRCT propagates constant valid/ready outputs into the wrapper and
        # leaves the corresponding instance outputs on unused wires.
        for pin, net in mcr_pins.items():
            external = source('fased_request_limits_' + pin)
            if pin in ('mcr_read_0_valid', 'mcr_read_1_valid', 'mcr_write_0_ready', 'mcr_write_1_ready'):
                assert external == bank_eq[pin] == "1'h1", 'constant MCR handshake differs'
            else:
                assert external == source(net), 'RTL MCR fragment wiring differs'
        assert 'always @(posedge clock)' in rtl_bank and 'targetFire' not in rtl_bank
        reset_body = re.search(r'if \(reset\) begin(.*?)end', rtl_bank, re.S)[1]
        for slot, name in enumerate(('writeMaxReqs', 'readMaxReqs')):
            assert re.search(name + r" <= 32'ha;", reset_body, re.I)
            assert re.search(r'if \(mcr_write_' + str(slot) + r'_valid\)\s*' + name +
                             r' <= mcr_write_' + str(slot) + '_bits;', rtl_bank)
        bank = native_module(text, 'GGFASEDRequestLimits')
        assert bank == native_module(baseline, 'GGFASEDRequestLimits')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', bank) == [('writeMaxReqs', '8'), ('readMaxReqs', '12')]
        for name in ('writeMaxReqs', 'readMaxReqs'):
            assert re.search(r'%' + name + r' = firrtl.regreset %clock, %reset, %c10_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert 'firrtl.constant 10 : !firrtl.uint<32>' in bank
        values, assertions = expressions(text, 'GGFASEDRequestLimits')
        assert not assertions
        read_values, _ = expressions(text, 'GGFASEDReadAdmission')
        write_values, _ = expressions(text, 'GGFASEDWritePairing')
        assert native_module(text, 'GGFASEDReadAdmission') == native_module(before, 'GGFASEDReadAdmission')
        assert native_module(text, 'GGFASEDWritePairing') == native_module(before, 'GGFASEDWritePairing')
        counts = dict(transitions=0, writes=0, zero_strobe_writes=0, reset_writes=0, upper_bits=0, writes_during_target_stall=0)

        def sample(state, data, mask, strobe, reset, fire):
            inputs = dict(zip(('writeMaxReqs', 'readMaxReqs'), state))
            inputs.update(reset=reset, targetFire=fire, **{'mcr.wstrb': strobe})
            for i in range(2):
                inputs[f'mcr.write[{i}].valid'] = mask >> i & 1
                inputs[f'mcr.write[{i}].bits'] = data[i]
            next_state = []
            for i, (name, direction) in enumerate((('writeMaxReqs', 'write'), ('readMaxReqs', 'read'))):
                ev = lambda k: evaluate(values[k], inputs, values)
                assert ev(f'mcr.read[{i}].bits') == state[i]
                assert ev(f'mcr.read[{i}].valid') == 1 and ev(f'mcr.write[{i}].ready') == 1
                assert ev(f'limits.{direction}') == state[i] & 15
                maximum = ev(f'limits.{direction}')
                # Feed the actual bank's low bits into the actual admission
                # comparators, including lowering a maximum below live counts.
                for pending in range(16):
                    if direction == 'read':
                        full = evaluate(read_values['pending.full'],
                            dict(pendingReads=pending, readMaxReqs=maximum), read_values)
                        assert full == (pending >= (state[i] & 15))
                    else:
                        for field, counter in [('awFull', 'pendingAW'), ('wFull', 'pendingW')]:
                            full = evaluate(write_values['pending.' + field],
                                {counter: pending, 'writeMaxReqs': maximum}, write_values)
                            assert full == (pending >= (state[i] & 15))
                written = bool(mask >> i & 1)
                expected = data[i] if written else state[i]
                assert ev(name) == expected
                next_state.append(10 if reset else expected)
                counts['writes'] += written
                counts['zero_strobe_writes'] += written and strobe == 0
                counts['reset_writes'] += written and reset
                counts['upper_bits'] += written and data[i] > 15
                counts['writes_during_target_stall'] += written and not fire
            counts['transitions'] += 1
            return next_state

        edges = (0, 1, 10, 15, 16, 31, 0x80000000, 0xffffffff)
        for state, data, mask, strobe, reset, fire in itertools.product(
                itertools.product(edges, repeat=2), ((0xffffffff, 16), (0, 10)), range(4), range(16), range(2), range(2)):
            sample(state, data, mask, strobe, reset, fire)
        rng = random.Random(231); state = [10, 10]
        for _ in range(20000):
            state = sample(state, [rng.getrandbits(32), rng.getrandbits(32)], rng.randrange(4), rng.randrange(16), rng.randrange(31) == 0, rng.randrange(2))
        assert all(counts.values())
        reports.append(dict(allocation=allocation, annotations_transferred=len(annos), rtl_mcr_pins_compared=len(mcr_pins), unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_module='FASEDMemoryTimingModel', allocations=reports,
                  reference_has_print_host=False,
                  scope='host-reset words 2/3 and low-four-bit admission maxima; decoded MCR fragment remains external')
    (evidence / 'selected-fased-request-limits-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC request-limit reset/write/readback semantics and allocated-bank attachment in both selected Print allocations')


if __name__ == '__main__': main()
