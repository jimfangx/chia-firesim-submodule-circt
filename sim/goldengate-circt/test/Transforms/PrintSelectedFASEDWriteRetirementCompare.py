#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print/Rocket target B retirement with immutable SFC RTL."""
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
from PrintSelectedFASEDTimingCycleCompare import transfer
from PrintRocketControlMasterCompare import native_ports


def main():
    evidence, golden, baseline_path = (Path(arg).resolve() for arg in sys.argv[1:])
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
    baseline = baseline_path.read_text()
    observed = ['GGSelectedPrintFASEDWritePairingWrapper', 'GGSelectedPrintFASEDTimingAWQueueWrapper',
                'GGSelectedPrintFASEDWriteLatencyWrapper', 'GGSelectedPrintFASEDReadLatencyWrapper',
                'GGSelectedPrintFASEDTimingCycleWrapper', 'GGSelectedPrintFASEDResponseReleaserWrapper']
    old, new = observed[0], 'GGSelectedPrintFASEDWriteRetirementWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        output = evidence / allocation
        text = (output / 'post-print-fased-write-retirement.mlir').read_text()
        before = (output / 'post-print-fased-write-pairing.mlir').read_text()
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
            assert wiring(baseline, name.replace('GGSelectedPrintFASED', 'GGFASED'), aggregate=True)['top.fased_accepted_b_fire'] == expected['top.fased_accepted_b_fire']
        old_ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        expected_ports = [(d, n) for d, n in old_ports if n != 'fased_target_b_fire']
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == expected_ports
        connections = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n
                       for d, n in expected_ports}
        connections['sim.fased_target_b_fire'] = 'sim.fased_accepted_b_fire'
        assert wiring(text, new, aggregate=True) == connections
        assert native_ports(native_module(text, new)) == [p for p in native_ports(native_module(before, old))
                                                          if p[1] != 'fased_target_b_fire'], 'copied port metadata differs'
        for name in observed:
            assert native_ports(native_module(text, name))[:-1] == native_ports(native_module(before, name)), name
        previous_annotations = json.loads((output / 'post-print-fased-write-pairing-all.json').read_text())
        annotations = json.loads((output / 'post-print-fased-write-retirement-all.json').read_text())
        assert annotations == transfer(previous_annotations, old, new, {n for _, n in expected_ports}), 'annotation retarget differs'
        rtl = clean((output / 'post-print-fased-write-retirement.sv').read_text())
        ports = lambda body: {n: ' '.join(v.split()) for n, v in re.findall(r'\.(\w+)\s*\(([^()]*)\)', body)}
        top_header, top_body = module(rtl, new)
        assert 'fased_target_b_fire' not in top_header, 'retirement remains external'
        top_instance = re.search(old + r' sim \((.*?)\);', top_body, re.S)
        assert top_instance
        sim_ports = ports(top_instance[1])
        assert sim_ports['fased_target_b_fire'] == sim_ports['fased_accepted_b_fire'], 'emitted retirement feedback differs'
        for j, name in enumerate(observed):
            body = module(rtl, name)[1]
            if j < 5:
                child = re.search(observed[j+1] + r' sim \((.*?)\);', body, re.S)
                assert child and ports(child[1])['fased_accepted_b_fire'] == 'fased_accepted_b_fire', name
            else:
                child = re.search(r'GGFASEDResponseReleaser releaser \((.*?)\);', body, re.S)
                assert child
                child_ports = ports(child[1])
                eq = {k: ' '.join(v.split()) for k, v in equations(body).items()}
                assert eq['fased_accepted_b_fire'] == child_ports['b_ready'] + ' & ' + child_ports['b_valid'], 'emitted wrong B handshake'
        decoder = native_module(text, 'GGControlAddressDecode')
        print_slave, print_start, count_size = (1, 64, 8) if allocation == 'candidate' else (0, 0, 4)
        assert f'name = "PrintBridgeModule_0", size = 32 : i64, slave = {print_slave} : i32, start = {print_start} : i64' in decoder
        assert f'name = "CPUManagedStreamEngine_0", size = {count_size} : i64, slave = {print_slave + 1} : i32, start = {print_start + 32} : i64' in decoder
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
        reports.append(dict(allocation=allocation, annotation_records_preserved=len(annotations), unchanged_modules=len(prior)-len(observed),
                            module_identities_retained=len(prior), observation_hops=len(observed),
                            prior_and_observation_drivers_compared=compared,
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'AXI4Releaser',
                  'SatUpDownCounter_1', 'FASEDMemoryTimingModel'], allocations=reports,
                  reference_has_print_host=False,
                  scope='target-visible B retirement feedback; host acknowledgements and egress acceptance stay distinct; timing admission and runtime maximum MMIO attachment remain external')
    (evidence / 'write-retirement-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC target B retirement, pending counter transitions and preserved Print banks in both allocations')


if __name__ == '__main__': main()
