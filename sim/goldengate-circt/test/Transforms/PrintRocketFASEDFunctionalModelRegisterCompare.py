#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket functional-model register attachment with SFC MMIO."""
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
    assert re.search(r'reg \[31:0\] relaxFunctionalModel;', host)
    assert eq['ingress_io_relaxed'] == 'relaxFunctionalModel[0]'
    assert eq['crFile_io_mcr_read_18_bits'] == 'relaxFunctionalModel'
    assert re.search(r"if \(reset\) begin\s*relaxFunctionalModel <= 32'h0;\s*end else if \(crFile_io_mcr_write_18_valid\) begin\s*relaxFunctionalModel <= crFile_io_mcr_write_18_bits;", host)
    baseline = (evidence / 'candidate/post-fame-fased-functional-model-register.mlir').read_text()
    old, new = 'GGFASEDLatencyRegistersWrapper', 'GGFASEDFunctionalModelRegisterWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-latency-registers{suffix}.mlir').read_text()
        text = (evidence / f'binding.mlir.rocket-fased-functional-model-register{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        consumed = {'fased_ingress_relaxed'}
        copied = [(d, n) for d, n in ports if n not in consumed]
        expected_ports = copied + [('out', 'fased_functional_model_mcr')]
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        nets.update({'functionalModelRegister.clock': 'top.hostClock', 'functionalModelRegister.reset': 'top.hostReset',
                     'sim.fased_ingress_relaxed': 'functionalModelRegister.relaxed',
                     'top.fased_functional_model_mcr': 'functionalModelRegister.mcr'})
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, new, aggregate=True)
        assert all(shared[k] == v for k, v in nets.items() if k.startswith(('functionalModelRegister.', 'sim.fased_', 'top.fased_')))
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in copied:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        bank = native_module(text, 'GGFASEDFunctionalModelRegister')
        assert bank == native_module(baseline, 'GGFASEDFunctionalModelRegister')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = true', bank) == [('relaxFunctionalModel', '72')]
        assert re.search(r'%relaxFunctionalModel = firrtl.regreset %clock, %reset, %c0_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert 'firrtl.constant 0 : !firrtl.uint<32>' in bank
        values, assertions = expressions(text, 'GGFASEDFunctionalModelRegister')
        assert not assertions
        counts = dict(transitions=0, writes=0, zero_strobe_writes=0, reset_writes=0,
                      upper_bits=0, writes_during_target_stall=0, upper_bits_without_relaxation=0)

        def sample(state, data, valid, strobe, reset, fire):
            inputs = {'relaxFunctionalModel': state, 'reset': reset, 'targetFire': fire,
                      'mcr.wstrb': strobe, 'mcr.write[0].valid': valid, 'mcr.write[0].bits': data}
            ev = lambda k: evaluate(values[k], inputs, values)
            assert ev('mcr.read[0].bits') == state
            assert ev('mcr.read[0].valid') == 1 and ev('mcr.write[0].ready') == 1
            assert ev('relaxed') == state & 1
            expected = data if valid else state
            assert ev('relaxFunctionalModel') == expected
            counts['writes'] += valid
            counts['zero_strobe_writes'] += valid and strobe == 0
            counts['reset_writes'] += valid and reset
            counts['upper_bits'] += valid and data > 1
            counts['writes_during_target_stall'] += valid and not fire
            counts['upper_bits_without_relaxation'] += state > 1 and not (state & 1)
            counts['transitions'] += 1
            return 0 if reset else expected

        edges = (0, 1, 2, 3, 0x80000000, 0x80000001, 0xfffffffe, 0xffffffff)
        for state, data, valid, strobe, reset, fire in itertools.product(
                edges, edges, range(2), range(16), range(2), range(2)):
            sample(state, data, valid, strobe, reset, fire)
        rng = random.Random(230); state = 0
        for _ in range(20000):
            state = sample(state, rng.getrandbits(32), rng.randrange(2), rng.randrange(16),
                           rng.randrange(31) == 0, rng.randrange(2))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_module='FASEDMemoryTimingModel', constructor_orders=reports,
                  reference_has_print_host=False,
                  scope='host-reset word 18, full-width readback and bit-zero ingress relaxation; decoded MCR fragment remains external')
    (evidence / 'rocket-fased-functional-model-register-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC functional-model register reset/write/readback/bit-zero semantics and allocated-bank attachment in both Print constructor orders')


if __name__ == '__main__': main()
