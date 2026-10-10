#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket request-limit bank attachment with SFC MMIO."""
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
    for slot, name, direction in ((2, 'writeMaxReqs', 'write'), (3, 'readMaxReqs', 'read')):
        assert re.search(r'reg \[31:0\] ' + name + ';', host)
        assert eq[f'model_tNasti_io_mmReg_{name}'] == name + '[3:0]'
        assert eq[f'crFile_io_mcr_read_{slot}_bits'] == name
        assert re.search(r"if \(reset\) begin\s*" + name + r" <= 32'ha;\s*end else if \(crFile_io_mcr_write_" + str(slot) + r'_valid\) begin\s*' + name + r' <= crFile_io_mcr_write_' + str(slot) + '_bits;', host)
    baseline = (evidence / 'candidate/post-fame-fased-request-limits.mlir').read_text()
    old, new = 'GGFASEDReadAdmissionWrapper', 'GGFASEDRequestLimitsWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-read-admission{suffix}.mlir').read_text()
        text = (evidence / f'binding.mlir.rocket-fased-request-limits{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        consumed = {'fased_write_max_reqs', 'fased_read_max_reqs'}
        copied = [(d, n) for d, n in ports if n not in consumed]
        expected_ports = copied + [('out', 'fased_request_limits_mcr')]
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        nets.update({'requestLimits.clock': 'top.hostClock', 'requestLimits.reset': 'top.hostReset',
                     'sim.fased_write_max_reqs': 'requestLimits.limits.write',
                     'sim.fased_read_max_reqs': 'requestLimits.limits.read',
                     'top.fased_request_limits_mcr': 'requestLimits.mcr'})
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, new, aggregate=True)
        assert all(shared[k] == v for k, v in nets.items() if k.startswith(('requestLimits.', 'sim.fased_', 'top.fased_')))
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in copied:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        bank = native_module(text, 'GGFASEDRequestLimits')
        assert bank == native_module(baseline, 'GGFASEDRequestLimits')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', bank) == [('writeMaxReqs', '8'), ('readMaxReqs', '12')]
        for name in ('writeMaxReqs', 'readMaxReqs'):
            assert re.search(r'%' + name + r' = firrtl.regreset %clock, %reset, %c10_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert 'firrtl.constant 10 : !firrtl.uint<32>' in bank
        values, assertions = expressions(text, 'GGFASEDRequestLimits')
        assert not assertions
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
        reports.append(dict(reverse=reverse, unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_module='FASEDMemoryTimingModel', constructor_orders=reports,
                  reference_has_print_host=False,
                  scope='host-reset words 2/3 and low-four-bit admission maxima; decoded MCR fragment remains external')
    (evidence / 'rocket-fased-request-limits-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC request-limit reset/write/readback semantics and allocated-bank attachment in both Print constructor orders')


if __name__ == '__main__': main()
