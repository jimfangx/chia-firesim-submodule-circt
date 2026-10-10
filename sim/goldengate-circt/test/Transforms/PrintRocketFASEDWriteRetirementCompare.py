#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket target B retirement with immutable SFC RTL."""
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
    for index in (1, 2):
        assert pipe[f'SatUpDownCounter_{index}_io_dec'] == 'tNasti_io_tNasti_b_ready & tNasti_io_tNasti_b_valid'
    assert pipe['tNasti_io_tNasti_b_valid'] == 'xactionRelease_io_b_valid'
    assert pipe['xactionRelease_io_b_ready'] == 'tNasti_io_tNasti_b_ready'
    release = equations(module(reference, 'AXI4Releaser')[1])
    assert release['io_b_valid'] == 'currentWrite_q_io_deq_valid'
    assert release['currentWrite_q_io_deq_ready'] == 'io_b_ready & io_b_valid'
    assert release['io_egressReq_b_valid'] == 'io_nextWrite_ready & io_nextWrite_valid'
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
    baseline = (evidence / 'candidate/post-fame-fased-write-retirement.mlir').read_text()
    observed = ['GGFASEDWritePairingWrapper', 'GGFASEDTimingAWQueueWrapper',
                'GGFASEDWriteLatencyWrapper', 'GGFASEDReadLatencyWrapper',
                'GGFASEDTimingCycleWrapper', 'GGFASEDResponseReleaserWrapper']
    old, new = observed[0], 'GGFASEDWriteRetirementWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-write-retirement{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-write-pairing{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
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
            expected['top.fased_accepted_b_fire'] = ('sim.fased_accepted_b_fire' if j < 5 else
                frozenset(['releaser.b.ready', 'releaser.b.valid']))
            assert actual == expected, f'{name} observation wiring differs'
            assert re.search(r', out %fased_accepted_b_fire: !firrtl.uint<1>\)', changed.splitlines()[0])
            compared += len(actual)
            # Shared SFC ingestion and expanded Print compositions use the
            # same native B-handshake observation, independent of allocation.
            assert wiring(baseline, name, aggregate=True)['top.fased_accepted_b_fire'] == expected['top.fased_accepted_b_fire']
        old_ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        expected_ports = [(d, n) for d, n in old_ports if n != 'fased_target_b_fire']
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        connections = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n
                       for d, n in expected_ports}
        connections['sim.fased_target_b_fire'] = 'sim.fased_accepted_b_fire'
        assert wiring(text, new, aggregate=True) == connections
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in expected_ports:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        helper = native_module(text, 'GGFASEDWritePairing')
        assert helper == native_module(before, 'GGFASEDWritePairing') == native_module(baseline, 'GGFASEDWritePairing')
        for n in ('pendingAW', 'pendingW'):
            reg = re.search('%' + n + r' = firrtl.regreset %clock, (%\w+), (%\w+) :', helper)
            assert reg and re.search(re.escape(reg[1]) + r' = firrtl.and %reset, %targetFire ', helper)
            assert re.search(re.escape(reg[2]) + r' = firrtl.constant 0 : !firrtl.uint<4>', helper)
        nets, assertions = expressions(helper, 'GGFASEDWritePairing')
        assert not assertions
        counts = dict(transitions=0, accepted_b=0, stalled_b=0, valid_backpressure=0,
                      ready_without_valid=0, empty_retirement=0, simultaneous_inc_dec=0,
                      target_resets=0, stalled_resets=0)

        def sample(aw, w, maximum, reset, fire, aw_fire, w_last, b_ready, b_valid):
            b_fire = bool(b_ready and b_valid)
            inputs = dict(clock=1, reset=reset, targetFire=fire, pendingAW=aw, pendingW=w,
                          writeMaxReqs=maximum, awFire=aw_fire, wLastFire=w_last, bFire=b_fire)
            value = lambda n: evaluate(nets[n], inputs, nets)
            next_values = []
            for n, v, inc, prefix in [('pendingAW', aw, aw_fire, 'aw'), ('pendingW', w, w_last, 'w')]:
                assert value('pending.' + prefix + 'Value') == v
                assert value('pending.' + prefix + 'Full') == (v >= maximum)
                next_v = v + 1 if inc and not b_fire and v < maximum else v - 1 if not inc and b_fire and v else v
                assert value(n) == (next_v if fire else v)
                next_values.append(0 if reset and fire else next_v if fire else v)
            counts['transitions'] += 1
            counts['accepted_b'] += b_fire
            counts['stalled_b'] += b_fire and not fire
            counts['valid_backpressure'] += b_valid and not b_ready
            counts['ready_without_valid'] += b_ready and not b_valid
            counts['empty_retirement'] += fire and not reset and b_fire and ((not aw_fire and not aw) or (not w_last and not w))
            counts['simultaneous_inc_dec'] += fire and not reset and b_fire and (aw_fire or w_last)
            counts['target_resets'] += bool(reset and fire)
            counts['stalled_resets'] += reset and not fire
            return next_values

        for aw, w, maximum, flags in itertools.product(range(16), range(16), range(16), range(64)):
            sample(aw, w, maximum, *(flags >> bit & 1 for bit in range(6)))
        rng = random.Random(228); aw = w = 0
        for i in range(30000):
            aw, w = sample(aw, w, rng.randrange(16), i % 997 == 0 or rng.randrange(113) == 0,
                           rng.randrange(3) != 0, *(rng.randrange(2) for _ in range(4)))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_modules=len(prior)-len(observed),
                            module_identities_retained=len(prior), observation_hops=len(observed),
                            prior_and_observation_drivers_compared=compared,
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'AXI4Releaser',
                  'SatUpDownCounter_1', 'FASEDMemoryTimingModel'], constructor_orders=reports,
                  reference_has_print_host=False,
                  scope='target-visible B retirement feedback; host acknowledgements and egress acceptance stay distinct; timing admission and runtime maximum MMIO attachment remain external')
    (evidence / 'rocket-fased-write-retirement-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC target B retirement, pending counter transitions and preserved Print banks in both orders')


if __name__ == '__main__': main()
