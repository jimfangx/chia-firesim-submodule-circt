#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket response-error observations with the SFC oracle."""
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
    host = module(clean(golden.read_text()), 'FASEDMemoryTimingModel')[1]
    eq = equations(host)
    for name, word, channel in [('rrespError', 19, 'r'), ('brespError', 20, 'b')]:
        assert re.search(r'reg \[1:0\] ' + name + ';', host)
        assert eq[f'crFile_io_mcr_read_{word}_bits'] == "{{30'd0}, " + name + '}'
        # This optimized oracle has no response-ready input in either guard.
        # General accepted-handshake behavior comes from Scala RegEnable(...fire).
        assert re.search(r"if \(reset\) begin\s*" + name + r" <= 2'h0;\s*end else if \(auto_to_host_dram_out_" +
                         channel + r"_bits_resp != 2'h0 & auto_to_host_dram_out_" + channel + r"_valid\) begin\s*" +
                         name + r" <= auto_to_host_dram_out_r_bits_resp;", host)
        assert 'Register ' + name + ' is read only' in host
        assert f'if (~reset & ~(~crFile_io_mcr_write_{word}_valid)) begin' in host
    baseline = (evidence / 'candidate/post-fame-fased-response-errors.mlir').read_text()
    old, new = 'GGFASEDFunctionalModelRegisterWrapper', 'GGFASEDResponseErrorsWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-functional-model-register{suffix}.mlir').read_text()
        text = (evidence / f'binding.mlir.rocket-fased-response-errors{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        copied = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        expected_ports = copied + [('out', 'fased_response_errors_mcr')]
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        observed = {'responseErrors.clock': 'top.hostClock', 'responseErrors.reset': 'top.hostReset',
                    'top.fased_response_errors_mcr': 'responseErrors.mcr'}
        for ch, port in [('r', 'fased_host_read_response'), ('b', 'fased_host_write_response')]:
            observed.update({f'responseErrors.{ch}Valid': f'top.{port}.valid',
                             f'responseErrors.{ch}Ready': f'sim.{port}.ready',
                             f'responseErrors.{ch}Resp': f'top.{port}.bits.resp'})
        nets.update(observed)
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, new, aggregate=True)
        assert all(shared[k] == v for k, v in observed.items())
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in copied:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        bank = native_module(text, 'GGFASEDResponseErrors')
        assert bank == native_module(baseline, 'GGFASEDResponseErrors')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = false', bank) == [('rrespError', '76'), ('brespError', '80')]
        for name in ('rrespError', 'brespError'):
            assert re.search('%' + name + r' = firrtl.regreset %clock, %reset, %c0_ui2 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<2>, !firrtl.uint<2>', bank)
        assert 'firrtl.constant 0 : !firrtl.uint<2>' in bank
        assert len(re.findall(r'firrtl.pad %\w+, 32', bank)) == 2
        values, assertions = expressions(text, 'GGFASEDResponseErrors')
        assert [a[2] for a in assertions] == ['Register rrespError is read only', 'Register brespError is read only']
        counts = dict(transitions=0, accepted_r_errors=0, accepted_b_errors=0, blocked_errors=0,
                      b_captures_different_r=0, b_captures_zero_r=0, reset_errors=0,
                      captures_during_target_stall=0, read_only_write_violations=0, reset_suppressed_writes=0)

        def sample(state, r, b, rv, rr, bv, br, writes, reset, fire, strobe):
            inputs = dict(rrespError=state[0], brespError=state[1], rResp=r, bResp=b,
                          rValid=rv, rReady=rr, bValid=bv, bReady=br, reset=reset, targetFire=fire)
            inputs.update({'mcr.wstrb': strobe, 'mcr.write[0].valid': writes & 1,
                           'mcr.write[1].valid': (writes >> 1) & 1})
            ev = lambda k: evaluate(values[k], inputs, values)
            for i in range(2):
                assert ev(f'mcr.read[{i}].bits') == state[i]
                assert ev(f'mcr.read[{i}].valid') == 1 and ev(f'mcr.write[{i}].ready') == 1
                predicate, enabled, _ = assertions[i]
                assert evaluate(enabled, inputs, values) == (not reset)
                assert evaluate(predicate, inputs, values) == (not (writes & (1 << i)))
            rf, bf = bool(rv and rr and r), bool(bv and br and b)
            nxt = (r if rf else state[0], r if bf else state[1])
            assert (ev('rrespError'), ev('brespError')) == nxt
            counts['accepted_r_errors'] += rf and not reset
            counts['accepted_b_errors'] += bf and not reset
            counts['blocked_errors'] += bool((rv and not rr and r) or (bv and not br and b)) and not reset
            counts['b_captures_different_r'] += bf and r != b and not reset
            counts['b_captures_zero_r'] += bf and r == 0 and not reset
            counts['reset_errors'] += bool(rf or bf) and reset
            counts['captures_during_target_stall'] += bool(rf or bf) and not reset and not fire
            counts['read_only_write_violations'] += writes.bit_count() if not reset else 0
            counts['reset_suppressed_writes'] += writes.bit_count() if reset else 0
            counts['transitions'] += 1
            return (0, 0) if reset else nxt

        for sr, sb, r, b, rv, rr, bv, br, writes, reset, fire in itertools.product(
                range(4), range(4), range(4), range(4), range(2), range(2), range(2), range(2), range(4), range(2), range(2)):
            sample((sr, sb), r, b, rv, rr, bv, br, writes, reset, fire, counts['transitions'] % 16)
        rng = random.Random(231); state = (0, 0)
        for _ in range(20000):
            state = sample(state, rng.randrange(4), rng.randrange(4), *(rng.randrange(2) for _ in range(4)),
                           rng.randrange(4), rng.randrange(31) == 0, rng.randrange(2), rng.randrange(16))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_module='FASEDMemoryTimingModel', constructor_orders=reports,
                  reference_has_print_host=False,
                  scope='host-reset read-only words 19/20, accepted nonzero R/B responses and SFC B-captures-R behavior; decoded MCR fragment remains external')
    (evidence / 'rocket-fased-response-errors-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC response-error capture/reset/readback/read-only semantics and allocated-bank attachment in both Print constructor orders')


if __name__ == '__main__': main()
