#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket AR admission and final-R retirement with immutable SFC RTL."""
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
    pipe = {k: ' '.join(v.split()) for k, v in equations(module(reference, 'LatencyPipe')[1]).items()}
    assert re.search(r'SatUpDownCounter_1\s+SatUpDownCounter\s*\(', module(reference, 'LatencyPipe')[1])
    assert pipe['SatUpDownCounter_io_inc'] == 'tNasti_io_tNasti_ar_ready & tNasti_io_tNasti_ar_valid'
    assert pipe['SatUpDownCounter_io_dec'] == '_T_1 & tNasti_io_tNasti_r_bits_last'
    assert pipe['_T_1'] == 'tNasti_io_tNasti_r_ready & tNasti_io_tNasti_r_valid'
    assert pipe['SatUpDownCounter_io_max'] == 'tNasti_io_mmReg_readMaxReqs'
    assert pipe['nastiReqIden_io_out_ar_ready'] == '~SatUpDownCounter_io_full'
    assert pipe['tNasti_io_tNasti_r_valid'] == 'xactionRelease_io_r_valid'
    assert pipe['xactionRelease_io_r_ready'] == 'tNasti_io_tNasti_r_ready'
    release = equations(module(reference, 'AXI4Releaser')[1])
    assert release['io_r_valid'] == 'currentRead_q_io_deq_valid'
    counter = module(reference, 'SatUpDownCounter_1')[1]
    ceq = equations(counter)
    assert ceq['io_full'] == 'value >= io_max' and ceq['io_empty'] == "value == 4'h0"
    assert ceq['_value_T_1'] == "value + 4'h1" and ceq['_value_T_3'] == "value - 4'h1"
    assert re.search(r'reg \[3:0\] value;', counter)
    assert re.search(r"if \(reset\) begin\s*value <= 4'h0;\s*end else if \(io_inc & ~io_dec & ~io_full\) begin\s*value <= _value_T_1;\s*end else if \(~io_inc & io_dec & ~io_empty\) begin\s*value <= _value_T_3;", counter)
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['model_clock'] == 'gate_O' and host['gate_I'] == 'clock'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    assert host['model_reset'] == 'hPort_hBits_reset'
    baseline = (evidence / 'candidate/post-fame-fased-read-admission.mlir').read_text()
    observed = ['GGFASEDWriteAdmissionWrapper', 'GGFASEDWriteRetirementWrapper', 'GGFASEDWritePairingWrapper', 'GGFASEDTimingAWQueueWrapper',
                'GGFASEDWriteLatencyWrapper', 'GGFASEDReadLatencyWrapper',
                'GGFASEDTimingCycleWrapper', 'GGFASEDResponseReleaserWrapper']
    old, new = observed[0], 'GGFASEDReadAdmissionWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-read-admission{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-write-admission{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new, 'GGFASEDReadAdmission'}
        assert all(after[n] == body for n, body in prior.items() if n not in observed), 'unrelated module changed'
        compared = 0
        for j, name in enumerate(observed):
            changed = native_module(text, name)
            # Existing port names, indices, types, directions and metadata are
            # checked by the native replay; compare every prior driver here.
            old_wires = wiring(before, name, aggregate=True)
            actual = wiring(text, name, aggregate=True)
            assert all(actual.get(k) == v for k, v in old_wires.items()), name
            expected = dict(old_wires)
            expected['top.fased_accepted_r_last_fire'] = ('sim.fased_accepted_r_last_fire' if j < 7 else
                frozenset(['releaser.r.ready', 'releaser.r.valid', 'releaser.r.bits.last']))
            assert actual == expected, f'{name} observation wiring differs'
            assert re.search(r', out %fased_accepted_r_last_fire: !firrtl.uint<1>\)', changed.splitlines()[0])
            compared += len(actual)
            # Shared SFC ingestion and expanded Print compositions use the
            # same native final-R observation, independent of allocation.
            assert wiring(baseline, name, aggregate=True)['top.fased_accepted_r_last_fire'] == expected['top.fased_accepted_r_last_fire']
        old_ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        expected_ports = old_ports + [('in', 'fased_read_max_reqs'), ('out', 'fased_pending_reads')]
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        connections = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n
                       for d, n in old_ports if n != 'fased_timing_requests'}
        for ch in ('aw', 'w'):
            connections[f'top.fased_timing_requests.{ch}'] = f'sim.fased_timing_requests.{ch}'
        for field in ('valid', 'bits'):
            connections[f'top.fased_timing_requests.ar.{field}'] = f'sim.fased_timing_requests.ar.{field}'
        for side in ('top', 'sim'):
            connections[f'{side}.fased_timing_requests.ar.ready'] = 'counter.arReady'
        connections.update({'counter.clock': 'top.hostClock', 'counter.reset': 'sim.fased_model_reset',
            'counter.targetFire': 'sim.fased_tfire', 'counter.arValid': 'sim.fased_timing_requests.ar.valid',
            'counter.rLastFire': 'sim.fased_accepted_r_last_fire', 'counter.readMaxReqs': 'top.fased_read_max_reqs',
            'top.fased_pending_reads': 'counter.pending'})
        assert wiring(text, new, aggregate=True) == connections
        shared = wiring(baseline, new, aggregate=True)
        for name, value in connections.items():
            if name.startswith('counter.') or '.fased_timing_requests.' in name:
                assert shared[name] == value, 'expanded read admission differs from SFC ingestion'
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in old_ports:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        helper = native_module(text, 'GGFASEDReadAdmission')
        assert helper == native_module(baseline, 'GGFASEDReadAdmission')
        reg = re.search(r'%pendingReads = firrtl.regreset %clock, (%\w+), (%\w+) :', helper)
        assert reg and re.search(re.escape(reg[1]) + r' = firrtl.and %reset, %targetFire ', helper)
        assert re.search(re.escape(reg[2]) + r' = firrtl.constant 0 : !firrtl.uint<4>', helper)
        nets, assertions = expressions(helper, 'GGFASEDReadAdmission')
        assert not assertions
        counts = dict(transitions=0, accepted_ar=0, accepted_final_r=0, nonfinal_r=0,
                      blocked_ar=0, empty_retirement=0, simultaneous_inc_dec=0,
                      lowered_maximum=0, zero_maximum=0, target_resets=0, stalled_resets=0)

        def sample(pending, maximum, reset, fire, ar_valid, r_ready, r_valid, r_last):
            # The observation is the actual three-way AND wired at the releaser.
            leaves = {'releaser.r.ready': r_ready, 'releaser.r.valid': r_valid,
                      'releaser.r.bits.last': r_last}
            r_fire = all(leaves[n] for n in observation)
            inputs = dict(clock=1, reset=reset, targetFire=fire, pendingReads=pending,
                          readMaxReqs=maximum, arValid=ar_valid, rLastFire=r_fire)
            value = lambda n: evaluate(nets[n], inputs, nets)
            assert value('pending.value') == pending and value('pending.full') == (pending >= maximum)
            assert value('arReady') == (pending < maximum)
            inc = bool(ar_valid and value('arReady'))
            next_v = pending + 1 if inc and not r_fire and pending < maximum else pending - 1 if not inc and r_fire and pending else pending
            assert value('pendingReads') == (next_v if fire else pending)
            counts['transitions'] += 1
            counts['accepted_ar'] += inc
            counts['accepted_final_r'] += r_fire
            counts['nonfinal_r'] += bool(r_ready and r_valid and not r_last)
            counts['blocked_ar'] += bool(ar_valid and not value('arReady'))
            counts['empty_retirement'] += bool(fire and not reset and r_fire and not pending and not inc)
            counts['simultaneous_inc_dec'] += bool(fire and not reset and inc and r_fire)
            counts['lowered_maximum'] += pending > maximum
            counts['zero_maximum'] += maximum == 0
            counts['target_resets'] += bool(reset and fire)
            counts['stalled_resets'] += bool(reset and not fire)
            return 0 if reset and fire else next_v if fire else pending

        # Cache the parsed observation outside the exhaustive loop.
        observation = wiring(text, observed[-1], aggregate=True)['top.fased_accepted_r_last_fire']
        for pending, maximum, flags in itertools.product(range(16), range(16), range(64)):
            sample(pending, maximum, *(flags >> bit & 1 for bit in range(6)))
        rng = random.Random(230); pending = 0
        for i in range(30000):
            pending = sample(pending, rng.randrange(16), i % 997 == 0 or rng.randrange(113) == 0,
                             rng.randrange(3) != 0, *(rng.randrange(2) for _ in range(4)))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_modules=len(prior)-len(observed),
                            module_identities_retained=len(prior), observation_hops=len(observed),
                            prior_and_observation_drivers_compared=compared,
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'AXI4Releaser',
                  'SatUpDownCounter_1', 'FASEDMemoryTimingModel'], constructor_orders=reports,
                  reference_has_print_host=False,
                  scope='AR pending-full admission and accepted final-R retirement; prior AW/W admission and Print banks preserved; runtime maximum MMIO remains external')
    (evidence / 'rocket-fased-read-admission-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AR admission, final-R retirement and pending-read transitions; preserved Print banks in both orders')


if __name__ == '__main__': main()
