#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print/Rocket functional-model register attachment with SFC MMIO."""
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
    assert re.search(r'reg \[31:0\] relaxFunctionalModel;', host)
    assert eq['ingress_io_relaxed'] == 'relaxFunctionalModel[0]'
    assert eq['crFile_io_mcr_read_18_bits'] == 'relaxFunctionalModel'
    assert re.search(r"if \(reset\) begin\s*relaxFunctionalModel <= 32'h0;\s*end else if \(crFile_io_mcr_write_18_valid\) begin\s*relaxFunctionalModel <= crFile_io_mcr_write_18_bits;", host)
    ingress_eq = equations(module(reference, 'IngressModule')[1])
    for name, expression in {
        'do_hread': 'io_relaxed | _do_hread_T_2',
        '_do_hread_T_2': '(io_host_mem_idle | io_host_read_inflight) & xaction_order_io_deq_valid & xaction_order_io_deq_bits',
        'do_hwrite': 'io_relaxed ? ~SatUpDownCounter_1_io_empty : io_host_mem_idle & xaction_order_io_deq_valid & ~ xaction_order_io_deq_bits',
        'do_hwrite_data': 'io_relaxed ? ~SatUpDownCounter_io_empty : do_hwrite_data_reg',
    }.items():
        assert ' '.join(ingress_eq[name].split()) == expression
    baseline = baseline_path.read_text()
    old, new = 'GGSelectedPrintFASEDLatencyRegistersWrapper', 'GGSelectedPrintFASEDFunctionalModelRegisterWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration160-print-fased-latency-registers' / allocation
        before = (root / 'post-print-fased-latency-registers.mlir').read_text()
        text = (root / 'post-print-fased-functional-model-register.mlir').read_text()
        for ext in ('.mlir', '.sv', '-all.json'):
            name = 'post-print-fased-latency-registers' + ext
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding latency-register boundary changed'
        for name in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'allocated collateral changed'
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
        prior_ports = native_ports(native_module(before, old))
        ports = [(d, n) for d, n, _ in prior_ports]
        consumed = {'fased_ingress_relaxed'}
        copied = [(d, n) for d, n in ports if n not in consumed]
        expected_ports = copied + [('out', 'fased_functional_model_mcr')]
        actual_ports = native_ports(native_module(text, new))
        assert [(d, n) for d, n, _ in actual_ports] == expected_ports
        assert actual_ports[:-1] == [(d, n, t) for d, n, t in prior_ports if n not in consumed]
        assert actual_ports[-1][2] == native_ports(native_module(baseline, 'GGFASEDFunctionalModelRegisterWrapper'))[-1][2]
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        nets.update({'functionalModelRegister.clock': 'top.hostClock', 'functionalModelRegister.reset': 'top.hostReset',
                     'sim.fased_ingress_relaxed': 'functionalModelRegister.relaxed',
                     'top.fased_functional_model_mcr': 'functionalModelRegister.mcr'})
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, 'GGFASEDFunctionalModelRegisterWrapper', aggregate=True)
        assert all(shared[k] == v for k, v in nets.items() if k.startswith(('functionalModelRegister.', 'sim.fased_', 'top.fased_')))
        prior_annos = json.loads((root / 'post-print-fased-latency-registers-all.json').read_text())
        annos = json.loads((root / 'post-print-fased-functional-model-register-all.json').read_text())
        assert annos == transfer(prior_annos, old, new, {n for _, n in copied}), 'annotation retarget differs'
        rtl = clean((root / 'post-print-fased-functional-model-register.sv').read_text())
        rtl_top = module(rtl, new)[1]
        rtl_nets = {k: ' '.join(v.split()) for k, v in equations(rtl_top).items()}
        def pins(mod, instance):
            child = re.search(r'\b' + mod + r'\s+' + instance + r'\s*\((.*?)\);', rtl_top, re.S)
            assert child
            return {k: ' '.join(v.split()) for k, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', child[1])}
        bank_pins = pins('GGFASEDFunctionalModelRegister', 'functionalModelRegister')
        sim_pins = pins(old, 'sim')
        def source(net):
            seen = set()
            while net in rtl_nets and net not in seen:
                seen.add(net); net = rtl_nets[net]
            return net
        assert bank_pins['clock'] == 'hostClock' and bank_pins['reset'] == 'hostReset'
        assert source(sim_pins['fased_ingress_relaxed']) == source(bank_pins['relaxed'])
        mcr_pins = {k: v for k, v in bank_pins.items() if k.startswith('mcr_')}
        assert len(mcr_pins) == 7
        rtl_bank = module(rtl, 'GGFASEDFunctionalModelRegister')[1]
        bank_eq = equations(rtl_bank)
        # Constant ready/valid may fold through the wrapper during RTL emission.
        for pin, net in mcr_pins.items():
            external = source('fased_functional_model_' + pin)
            if pin in ('mcr_read_0_valid', 'mcr_write_0_ready'):
                assert external == bank_eq[pin] == "1'h1", 'constant MCR handshake differs'
            else:
                assert external == source(net), 'RTL MCR fragment wiring differs'
        assert 'always @(posedge clock)' in rtl_bank and 'targetFire' not in rtl_bank
        assert bank_eq['relaxed'] == 'relaxFunctionalModel[0]'
        assert re.search(r"if \(reset\)\s*(?:begin\s*)?relaxFunctionalModel <= 32'h0;", rtl_bank)
        assert re.search(r'if \(mcr_write_0_valid\)\s*relaxFunctionalModel <= mcr_write_0_bits;', rtl_bank)
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
        # Compose actual bank bit-zero output with the retained native ingress
        # issue equations. High control bits must not enable relaxed ordering.
        issue, _ = expressions(text, 'GGFASEDIngressIssue')
        controls = ('hostMemIdle', 'hostReadInflight', 'awEmpty', 'wEmpty',
                    'order.valid', 'order.bits', 'do_hwrite_data_reg')
        issue_cases = 0
        for word, bits in itertools.product(edges, itertools.product((False, True), repeat=len(controls))):
            inputs = dict(zip(controls, bits))
            inputs['relaxed'] = evaluate(values['relaxed'], {'relaxFunctionalModel': word}, values)
            relaxed = word & 1
            gates = dict(ar=bool(relaxed or (inputs['hostMemIdle'] or inputs['hostReadInflight']) and inputs['order.valid'] and inputs['order.bits']),
                         aw=not inputs['awEmpty'] if relaxed else inputs['hostMemIdle'] and inputs['order.valid'] and not inputs['order.bits'],
                         w=not inputs['wEmpty'] if relaxed else inputs['do_hwrite_data_reg'])
            for ch, gate in gates.items():
                inputs[ch + '.valid'] = inputs['requests.' + ch + '.ready'] = True
                assert evaluate(issue['requests.' + ch + '.valid'], inputs, issue) == gate
                assert evaluate(issue[ch + '.ready'], inputs, issue) == gate
            issue_cases += 1
        assert all(counts.values())
        reports.append(dict(allocation=allocation, ingress_issue_cases=issue_cases, annotations_transferred=len(annos), rtl_mcr_pins_compared=len(mcr_pins), unchanged_modules=len(prior), module_identities_retained=len(prior),
                            wrapper_ports=len(expected_ports), wrapper_connections_compared=len(nets), **counts))
    report = dict(golden=str(golden), SFC_modules=['FASEDMemoryTimingModel', 'IngressModule'], allocations=reports,
                  reference_has_print_host=False,
                  scope='host-reset word 18, full-width readback and bit-zero ingress relaxation; decoded MCR fragment remains external')
    (evidence / 'selected-fased-functional-model-register-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC functional-model register reset/write/readback/bit-zero semantics and allocated-bank attachment in both selected Print allocations')


if __name__ == '__main__': main()
