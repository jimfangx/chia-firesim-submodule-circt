#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print/Rocket response-error observations with the SFC oracle."""
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
    baseline = baseline_path.read_text()
    old, new = 'GGSelectedPrintFASEDFunctionalModelRegisterWrapper', 'GGSelectedPrintFASEDResponseErrorsWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration161-print-fased-functional-model-register' / allocation
        before = (root / 'post-print-fased-functional-model-register.mlir').read_text()
        text = (root / 'post-print-fased-response-errors.mlir').read_text()
        for ext in ('.mlir', '.sv', '-all.json'):
            name = 'post-print-fased-functional-model-register' + ext
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding boundary changed'
        for name in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'allocated collateral changed'
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        prior_ports = native_ports(native_module(before, old))
        copied = [(d, n) for d, n, _ in prior_ports]
        expected_ports = copied + [('out', 'fased_response_errors_mcr')]
        actual_ports = native_ports(native_module(text, new))
        assert [(d, n) for d, n, _ in actual_ports] == expected_ports
        assert actual_ports[:-1] == prior_ports, 'copied port types differ'
        assert actual_ports[-1][2] == native_ports(native_module(baseline, 'GGFASEDResponseErrorsWrapper'))[-1][2]
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        observed = {'responseErrors.clock': 'top.hostClock', 'responseErrors.reset': 'top.hostReset',
                    'top.fased_response_errors_mcr': 'responseErrors.mcr'}
        for ch, port in [('r', 'fased_host_read_response'), ('b', 'fased_host_write_response')]:
            observed.update({f'responseErrors.{ch}Valid': f'top.{port}.valid',
                             f'responseErrors.{ch}Ready': f'sim.{port}.ready',
                             f'responseErrors.{ch}Resp': f'top.{port}.bits.resp'})
        nets.update(observed)
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, 'GGFASEDResponseErrorsWrapper', aggregate=True)
        assert all(shared[k] == v for k, v in observed.items())
        prior_annos = json.loads((root / 'post-print-fased-functional-model-register-all.json').read_text())
        annos = json.loads((root / 'post-print-fased-response-errors-all.json').read_text())
        assert annos == transfer(prior_annos, old, new, {n for _, n in copied}), 'annotation retarget differs'
        rtl = clean((root / 'post-print-fased-response-errors.sv').read_text())
        rtl_top = module(rtl, new)[1]
        rtl_nets = {k: ' '.join(v.split()) for k, v in equations(rtl_top).items()}
        def pins(mod, instance):
            child = re.search(r'\b' + mod + r'\s+' + instance + r'\s*\((.*?)\);', rtl_top, re.S)
            assert child
            return {k: ' '.join(v.split()) for k, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', child[1])}
        bank_pins = pins('GGFASEDResponseErrors', 'responseErrors')
        sim_pins = pins(old, 'sim')
        def source(net):
            seen = set()
            while net in rtl_nets and net not in seen:
                seen.add(net); net = rtl_nets[net]
            return net
        assert bank_pins['clock'] == 'hostClock' and bank_pins['reset'] == 'hostReset'
        for ch, port in [('r', 'fased_host_read_response'), ('b', 'fased_host_write_response')]:
            assert source(bank_pins[ch + 'Valid']) == source(port + '_valid')
            assert source(bank_pins[ch + 'Resp']) == source(port + '_bits_resp')
            assert source(bank_pins[ch + 'Ready']) == source(sim_pins[port + '_ready'])
        mcr_pins = {k: v for k, v in bank_pins.items() if k.startswith('mcr_')}
        assert len(mcr_pins) == 13
        rtl_bank = module(rtl, 'GGFASEDResponseErrors')[1]
        bank_eq = equations(rtl_bank)
        for pin, net in mcr_pins.items():
            external = source('fased_response_errors_' + pin)
            if pin in ('mcr_read_0_valid', 'mcr_write_0_ready', 'mcr_read_1_valid', 'mcr_write_1_ready'):
                assert external == bank_eq[pin] == "1'h1", 'constant MCR handshake differs'
            else:
                assert external == source(net), 'RTL MCR fragment wiring differs'
        assert 'always @(posedge clock)' in rtl_bank and 'targetFire' not in rtl_bank
        assert re.search(r"if \(reset\) begin\s*rrespError <= 2'h0;\s*brespError <= 2'h0;", rtl_bank)
        for ch, name in [('r', 'rrespError'), ('b', 'brespError')]:
            assert re.search(r'if \(' + ch + r'Valid & ' + ch + r'Ready & \(\|' + ch + r'Resp\)\)\s*' + name + r' <= rResp;', rtl_bank)
        assert re.search(r'if \(~reset\) begin\s*assert\(~mcr_write_0_valid\).*?assert\(~mcr_write_1_valid\)', rtl_bank, re.S)
        for i, name in enumerate(('rrespError', 'brespError')):
            assert re.search(r'reg \[1:0\] ' + name + ';', rtl_bank)
            assert name + " <= 2'h0;" in rtl_bank
            assert name + ' <= rResp;' in rtl_bank, 'B must also capture current R response bits'
            assert bank_eq[f'mcr_read_{i}_bits'] == "{30'h0, " + name + '}'
            assert 'Register ' + name + ' is read only' in rtl_bank
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
        reports.append(dict(allocation=allocation, annotations_transferred=len(annos), rtl_mcr_pins_compared=len(mcr_pins), unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_module='FASEDMemoryTimingModel', allocations=reports,
                  reference_has_print_host=False,
                  scope='host-reset read-only words 19/20, accepted nonzero R/B responses and SFC B-captures-R behavior; decoded MCR fragment remains external')
    (evidence / 'selected-fased-response-errors-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC response-error capture/reset/readback/read-only semantics and allocated-bank attachment in both selected Print allocations')


if __name__ == '__main__': main()
