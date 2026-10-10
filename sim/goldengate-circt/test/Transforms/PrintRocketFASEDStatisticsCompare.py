#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket statistics and accepted R beats with SFC RTL."""
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

HIERARCHY = ['GGFASEDResponseErrorsWrapper', 'GGFASEDFunctionalModelRegisterWrapper',
             'GGFASEDLatencyRegistersWrapper', 'GGFASEDRequestLimitsWrapper', 'GGFASEDReadAdmissionWrapper',
             'GGFASEDWriteAdmissionWrapper', 'GGFASEDWriteRetirementWrapper', 'GGFASEDWritePairingWrapper',
             'GGFASEDTimingAWQueueWrapper', 'GGFASEDWriteLatencyWrapper', 'GGFASEDReadLatencyWrapper',
             'GGFASEDTimingCycleWrapper', 'GGFASEDResponseReleaserWrapper']
COUNTERS = ['totalWriteBeats', 'totalReadBeats', 'totalWrites', 'totalReads']
EVENTS = ['wFire', 'rFire', 'awFire', 'arFire']


def main():
    evidence, golden, previous = (Path(arg).resolve() for arg in sys.argv[1:])
    reference = clean(golden.read_text())
    pipe = module(reference, 'LatencyPipe')[1]; eq = equations(pipe)
    host = module(reference, 'FASEDMemoryTimingModel')[1]; heq = equations(host)
    assert heq['model_clock'] == 'gate_O' and heq['model_reset'] == 'hPort_hBits_reset'
    guards = ['_T_5', '_T_1', 'SatUpDownCounter_1_io_inc', 'SatUpDownCounter_io_inc']
    for i, (name, guard, ch) in enumerate(zip(COUNTERS, guards, ['w', 'r', 'aw', 'ar'])):
        assert re.search(r'reg \[31:0\] ' + name + ';', pipe)
        assert eq['_' + name + '_T_1'] == name + " + 32'h1"
        assert eq[guard] == f'tNasti_io_tNasti_{ch}_ready & tNasti_io_tNasti_{ch}_valid'
        assert re.search(r'if \(reset\) begin\s*' + name + r" <= 32'h0;\s*end else if \(" + guard +
                         r'\) begin\s*' + name + r' <= _' + name + '_T_1;', pipe)
        assert heq[f'crFile_io_mcr_read_{14+i}_bits'] == 'model_tNasti_io_mmReg_' + name
        assert 'Register ' + name + ' is read only' in host
        assert f'if (~reset & ~(~crFile_io_mcr_write_{14+i}_valid)) begin' in host
    baseline = (evidence / 'candidate/post-fame-fased-statistics.mlir').read_text()
    old, new, observation = HIERARCHY[0], 'GGFASEDStatisticsWrapper', 'fased_accepted_r_fire'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        before = (previous / f'binding.mlir.rocket-fased-response-errors{suffix}.mlir').read_text()
        text = (evidence / f'binding.mlir.rocket-fased-statistics{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items() if n not in HIERARCHY)
        for n in HIERARCHY:
            p = re.findall(r'\b(in|out) %(\w+):', native_module(before, n).splitlines()[0])
            assert re.findall(r'\b(in|out) %(\w+):', native_module(text, n).splitlines()[0]) == p + [('out', observation)]
            expected = wiring(before, n, aggregate=True)
            accepted = 'sim.' + observation if n != HIERARCHY[-1] else frozenset({'releaser.r.ready', 'releaser.r.valid'})
            expected['top.' + observation] = accepted
            assert wiring(text, n, aggregate=True) == expected, n + ' changed original drivers'
            assert wiring(baseline, n, aggregate=True)['top.' + observation] == accepted
        copied = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == copied + [('out', 'fased_statistics_mcr')]
        nets = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n for d, n in copied}
        events = {'statistics.clock': 'top.hostClock', 'statistics.hostReset': 'top.hostReset',
                  'statistics.modelReset': 'sim.fased_model_reset', 'statistics.targetFire': 'sim.fased_tfire',
                  'statistics.rFire': 'sim.' + observation, 'top.fased_statistics_mcr': 'statistics.mcr'}
        for ch in ('aw', 'ar', 'w'):
            events['statistics.' + ch + 'Fire'] = frozenset({'sim.fased_timing_requests.' + ch + '.' + f for f in ('ready', 'valid')})
        nets.update(events)
        assert wiring(text, new, aggregate=True) == nets
        shared = wiring(baseline, new, aggregate=True)
        assert all(shared[k] == v for k, v in events.items())
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in copied:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        bank = native_module(text, 'GGFASEDStatistics')
        assert bank == native_module(baseline, 'GGFASEDStatistics')
        assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = false', bank) == list(zip(COUNTERS, map(str, range(56, 69, 4))))
        resets = re.findall(r'%\w+ = firrtl.regreset %clock, (%\w+), %c0_ui32 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<32>, !firrtl.uint<32>', bank)
        assert len(resets) == 4 and len(set(resets)) == 1
        assert resets[0] + ' = firrtl.and %modelReset, %targetFire ' in bank
        values, assertions = expressions(text, 'GGFASEDStatistics')
        assert [a[2] for a in assertions] == ['Register ' + n + ' is read only' for n in COUNTERS]
        counts = dict(transitions=0, wraps=0, accepted_nonfinal_r_beats=0, blocked_r_beats=0,
                      stalled_model_resets=0, host_only_resets=0, read_only_write_violations=0, reset_suppressed_writes=0)

        def sample(state, handshakes, host_reset, model_reset, fire, last, writes, strobe):
            accepted = [handshakes[2*i] and handshakes[2*i+1] for i in range(4)]
            v = dict(zip(COUNTERS, state)); v.update(zip(EVENTS, accepted))
            v.update(hostReset=host_reset, modelReset=model_reset, targetFire=fire)
            v.update({'mcr.wstrb': strobe, **{f'mcr.write[{i}].valid': (writes >> i) & 1 for i in range(4)}})
            ev = lambda k: evaluate(values[k], v, values)
            for i, name in enumerate(COUNTERS):
                assert ev(f'mcr.read[{i}].bits') == state[i]
                assert ev(f'mcr.read[{i}].valid') == 1 and ev(f'mcr.write[{i}].ready') == 1
                assert ev(name) == (state[i] + int(fire and accepted[i])) & 0xffffffff
                pred, enabled, _ = assertions[i]
                assert evaluate(enabled, v, values) == (not host_reset)
                assert evaluate(pred, v, values) == (not (writes & (1 << i)))
                counts['wraps'] += bool(fire and accepted[i] and state[i] == 0xffffffff and not model_reset)
            nxt = tuple(0 if model_reset and fire else ev(n) for n in COUNTERS)
            counts['accepted_nonfinal_r_beats'] += bool(fire and accepted[1] and not last and not model_reset)
            counts['blocked_r_beats'] += bool(handshakes[2] and not handshakes[3])
            counts['stalled_model_resets'] += bool(model_reset and not fire)
            counts['host_only_resets'] += bool(host_reset and not model_reset)
            counts['read_only_write_violations'] += writes.bit_count() if not host_reset else 0
            counts['reset_suppressed_writes'] += writes.bit_count() if host_reset else 0
            counts['transitions'] += 1
            return nxt

        for edge, bits in itertools.product([0, 1, 0xfffffffe, 0xffffffff], itertools.product(range(2), repeat=12)):
            sample((edge,)*4, bits[:8], *bits[8:], counts['transitions'] % 16, counts['transitions'] % 16)
        rng = random.Random(232); state = (0,)*4
        for cycle in range(20000):
            if cycle % 97 == 0: state = tuple(rng.choice([0, 1, 0xfffffffe, 0xffffffff]) for _ in range(4))
            state = sample(state, [rng.randrange(2) for _ in range(8)], rng.randrange(2), rng.randrange(31) == 0,
                           rng.randrange(2), rng.randrange(2), rng.randrange(16), rng.randrange(16))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_module_bodies=len(prior)-len(HIERARCHY),
                            retained_module_identities=len(prior), observation_modules=len(HIERARCHY),
                            observation_instances=len(HIERARCHY)-1, copied_ports=len(copied),
                            wrapper_connections_compared=len(nets), atomic_rejections=12, **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'FASEDMemoryTimingModel'], constructor_orders=reports,
                  reference_has_print_host=False, scope='accepted target beats/transactions; target-clock model reset and four read-only MCR words; global fanout remains external')
    (evidence / 'rocket-fased-statistics-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC statistics counter/reset/MMIO semantics and 13-module accepted-R observation in both Print constructor orders')


if __name__ == '__main__': main()
