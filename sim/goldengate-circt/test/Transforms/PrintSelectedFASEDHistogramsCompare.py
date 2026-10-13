#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the selected Print/Rocket histogram boundary with immutable SFC RTL."""
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

NAMES = [group + 'OutstandingHistogram_' + str(i)
         for group in ('write', 'read') for i in range(5)]
BOUNDS = [0, 2, 4, 8]


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    pipe = module(reference, 'LatencyPipe')[1]; eq = equations(pipe)
    host = module(reference, 'FASEDMemoryTimingModel')[1]; heq = equations(host)
    assert heq['model_clock'] == 'gate_O' and heq['model_reset'] == 'hPort_hBits_reset'
    assert heq['gate_I'] == 'clock' and heq['gate_CE'] == heq['targetFire']
    for group, count, terms in [('read', 'SatUpDownCounter_io_value', [31, 36, 41, 39, 44]),
                                ('write', 'SatUpDownCounter_1_io_value', [51, 56, 61, 59, 64])]:
        for term, bound in zip(terms[:3], BOUNDS):
            assert eq[f'_T_{term}'] == f"{count} <= 4'h{bound:x}"
        assert eq[f'_T_{terms[3]}'] == f'_T_{terms[0]} | _T_{terms[1]}'
        assert eq[f'_T_{terms[4]}'] == f'_T_{terms[0]} | _T_{terms[1]} | _T_{terms[2]}'
        guards = [f"{count} <= 4'h0", f"~({count} <= 4'h0) & {count} <= 4'h2",
                  f"~_T_{terms[3]} & {count} <= 4'h4", f"~_T_{terms[4]} & {count} <= 4'h8"]
        for i, guard in enumerate(guards):
            name = group + 'OutstandingHistogram_' + str(i)
            assert re.search(r'reg \[31:0\] ' + name + ';', pipe)
            assert eq['_' + name + '_T_1'] == name + " + 32'h1"
            assert re.search(r'if \(reset\) begin\s*' + name + r" <= 32'h0;\s*end else if \(" +
                             re.escape(guard) + r'\) begin\s*' + name + r' <= _' + name + '_T_1;', pipe)
    mcr = equations(module(reference, 'MCRFile_5')[1])
    assert mcr['_GEN_85'] == "5'h8 == rIndex ? 32'h0 : _GEN_84"
    assert mcr['_GEN_90'] == "5'hd == rIndex ? 32'h0 : _GEN_89"
    for i, name in enumerate(NAMES):
        if i % 5 != 4:
            assert heq[f'crFile_io_mcr_read_{4+i}_bits'] == 'model_tNasti_io_mmReg_' + name
        assert 'Register ' + name + ' is read only' in host
        assert f'if (~reset & ~(~crFile_io_mcr_write_{4+i}_valid)) begin' in host

    baseline = baseline_path.read_text()
    old, new = 'GGSelectedPrintFASEDStatisticsWrapper', 'GGSelectedPrintFASEDHistogramsWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        previous = evidence.parent / 'iteration163-print-fased-statistics' / allocation
        before = (root / 'post-print-fased-statistics.mlir').read_text()
        text = (root / 'post-print-fased-histograms.mlir').read_text()
        for ext in ('.mlir', '.sv', '-all.json'):
            name = 'post-print-fased-statistics' + ext
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'preceding statistics boundary changed'
        for name in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            assert (root / name).read_bytes() == (previous / name).read_bytes(), 'allocated header changed'
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'existing module body changed'
        copied = native_ports(native_module(before, old))
        added = native_ports(native_module(text, new))
        assert added[:-1] == copied and added[-1][:2] == ('out', 'fased_histograms_mcr')
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n, _ in copied}
        observations = {'histograms.clock': 'top.hostClock', 'histograms.hostReset': 'top.hostReset',
                        'histograms.modelReset': 'sim.fased_model_reset', 'histograms.targetFire': 'sim.fased_tfire',
                        'histograms.pendingReads': 'sim.fased_pending_reads.value',
                        'histograms.pendingAW': 'sim.fased_pending_writes.awValue',
                        'top.fased_histograms_mcr': 'histograms.mcr'}
        nets.update(observations)
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, 'GGFASEDHistogramsWrapper', aggregate=True)
        assert all(shared[k] == v for k, v in observations.items())
        annos = json.loads((root / 'post-print-fased-statistics-all.json').read_text())
        actual = json.loads((root / 'post-print-fased-histograms-all.json').read_text())
        assert actual == transfer(annos, old, new, {n for _, n, _ in copied})
        # CIRCT-emitted RTL must bind the pre-edge observations and every MCR lane.
        rtl = clean((root / 'post-print-fased-histograms.sv').read_text())
        rtl_top = module(rtl, new)[1]; rtl_eq = equations(rtl_top)
        def source(net):
            seen = set()
            while net in rtl_eq and net not in seen:
                seen.add(net); net = rtl_eq[net]
            return net
        def pins(mod, inst):
            match = re.search(r'\b' + mod + r'\s+' + inst + r'\s*\((.*?)\);', rtl_top, re.S)
            assert match
            return {k: ' '.join(v.split()) for k, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', match[1])}
        bank_pins = pins('GGFASEDHistograms', 'histograms'); sim_pins = pins(old, 'sim')
        assert bank_pins['clock'] == 'hostClock' and bank_pins['hostReset'] == 'hostReset'
        for pin, port in [('modelReset', 'fased_model_reset'), ('targetFire', 'fased_tfire'),
                          ('pendingReads', 'fased_pending_reads_value'), ('pendingAW', 'fased_pending_writes_awValue')]:
            assert source(bank_pins[pin]) == source(sim_pins[port])
        bank_rtl = module(rtl, 'GGFASEDHistograms')[1]; bank_eq = equations(bank_rtl)
        assert 'always @(posedge clock)' in bank_rtl
        mcr_pins = {k: v for k, v in bank_pins.items() if k.startswith('mcr_')}
        assert len(mcr_pins) == 61
        for pin, net in mcr_pins.items():
            external = source('fased_histograms_' + pin)
            if pin.endswith('_valid') and pin.startswith('mcr_read_') or pin.endswith('_ready') and pin.startswith('mcr_write_'):
                assert external == bank_eq[pin] == "1'h1"
            elif pin in ('mcr_read_4_bits', 'mcr_read_9_bits'):
                assert external == bank_eq[pin] == "32'h0"
            else: assert external == source(net), 'emitted histogram MCR wiring differs'
        bank = native_module(text, 'GGFASEDHistograms')
        assert bank == native_module(baseline, 'GGFASEDHistograms')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = false', bank) == list(zip(NAMES, map(str, range(16, 53, 4))))
        resets = re.findall(r'%\w+ = firrtl.regreset %clock, (%\w+), %c0_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert len(resets) == 8 and len(set(resets)) == 1
        assert resets[0] + ' = firrtl.and %modelReset, %targetFire ' in bank
        values, assertions = expressions(text, 'GGFASEDHistograms')
        assert [a[2] for a in assertions] == ['Register ' + n + ' is read only' for n in NAMES]
        counters = [n for i, n in enumerate(NAMES) if i % 5 != 4]
        counts = dict(transitions=0, wraps=0, above_last_bound=0, stalled_model_resets=0,
                      host_only_resets=0, read_only_write_violations=0, reset_suppressed_writes=0)

        def sample(state, reads, aw, host_reset, model_reset, fire, writes, strobe):
            v = dict(zip(counters, state)); v.update(pendingReads=reads, pendingAW=aw,
                hostReset=host_reset, modelReset=model_reset, targetFire=fire)
            v.update({'mcr.wstrb': strobe, **{f'mcr.write[{i}].valid': (writes >> i) & 1 for i in range(10)}})
            ev = lambda k: evaluate(values[k], v, values)
            for i, name in enumerate(NAMES):
                assert ev(f'mcr.read[{i}].bits') == (0 if i % 5 == 4 else v[name])
                assert ev(f'mcr.read[{i}].valid') == 1 and ev(f'mcr.write[{i}].ready') == 1
                if i % 5 != 4:
                    count = aw if i < 5 else reads
                    selected = next((j for j, bound in enumerate(BOUNDS) if count <= bound), None)
                    inc = bool(fire and selected == i % 5)
                    assert ev(name) == (v[name] + inc) & 0xffffffff
                    counts['wraps'] += inc and v[name] == 0xffffffff and not model_reset
                pred, enabled, _ = assertions[i]
                assert evaluate(enabled, v, values) == (not host_reset)
                assert evaluate(pred, v, values) == (not (writes & (1 << i)))
            counts['above_last_bound'] += bool(fire and (aw > 8 or reads > 8))
            counts['stalled_model_resets'] += bool(model_reset and not fire)
            counts['host_only_resets'] += bool(host_reset and not model_reset)
            counts['read_only_write_violations'] += writes.bit_count() if not host_reset else 0
            counts['reset_suppressed_writes'] += writes.bit_count() if host_reset else 0
            counts['transitions'] += 1
            return tuple(0 if model_reset and fire else ev(n) for n in counters)

        for edge, reads, aw, controls in itertools.product([0, 1, 0xfffffffe, 0xffffffff], range(16), range(16), itertools.product(range(2), repeat=3)):
            sample((edge,)*8, reads, aw, *controls, counts['transitions'] % 1024, counts['transitions'] % 16)
        rng = random.Random(233); state = (0,)*8
        for cycle in range(20000):
            if cycle % 97 == 0: state = tuple(rng.choice([0, 1, 0xfffffffe, 0xffffffff]) for _ in counters)
            state = sample(state, rng.randrange(16), rng.randrange(16), rng.randrange(2),
                           rng.randrange(31) == 0, rng.randrange(2), rng.randrange(1024), rng.randrange(16))
        assert all(counts.values())
        reports.append(dict(allocation=allocation, unchanged_module_bodies=len(prior), copied_ports=len(copied),
                            wrapper_connections_compared=len(nets), atomic_rejections=13, RTL_MCR_pins_compared=len(mcr_pins), annotations_compared=len(actual), **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'FASEDMemoryTimingModel', 'MCRFile_5'],
                  allocations=reports, reference_has_print_host=False,
                  scope='pre-edge read/AW occupancy; target-clock reset, wrapping counters and ten read-only words; global fanout remains external')
    (evidence / 'selected-fased-histograms-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC histogram counter/reset/MMIO semantics and selected wiring in both Print allocations')


if __name__ == '__main__': main()
