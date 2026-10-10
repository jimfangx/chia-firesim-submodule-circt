#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket AW/W completion pairing with immutable SFC.

Pin LatencyPipe and SatUpDownCounter_1 equations, interpret native FIRRTL SSA,
and preserve the prior module bodies, allocation and annotation archive.
"""
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
    counter = module(reference, 'SatUpDownCounter_1')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(counter).items()}
    expected = {'io_value': 'value', 'io_full': 'value >= io_max', 'io_empty': "value == 4'h0",
                '_value_T_1': "value + 4'h1", '_value_T_3': "value - 4'h1"}
    for name, expression in expected.items(): assert eq[name] == expression, name
    assert re.search(r'reg \[3:0\] value;', counter)
    assert re.search(r"if \(reset\) begin\s*value <= 4'h0;\s*end else if \(io_inc & ~io_dec & ~io_full\) begin\s*value <= _value_T_1;\s*end else if \(~io_inc & io_dec & ~io_empty\) begin\s*value <= _value_T_3;", counter)
    pipe_body = module(reference, 'LatencyPipe')[1]
    pipe = {k: ' '.join(v.split()) for k, v in equations(pipe_body).items()}
    assert re.search(r'SatUpDownCounter_1 SatUpDownCounter_1 \(', pipe_body)
    assert re.search(r'SatUpDownCounter_1 SatUpDownCounter_2 \(', pipe_body)
    for index in (1, 2):
        prefix = f'SatUpDownCounter_{index}_'
        assert pipe[prefix + 'clock'] == 'clock' and pipe[prefix + 'reset'] == 'reset'
        assert pipe[prefix + 'io_dec'] == 'tNasti_io_tNasti_b_ready & tNasti_io_tNasti_b_valid'
        assert pipe[prefix + 'io_max'] == 'tNasti_io_mmReg_writeMaxReqs'
    assert pipe['SatUpDownCounter_1_io_inc'] == 'tNasti_io_tNasti_aw_ready & tNasti_io_tNasti_aw_valid'
    assert pipe['_T_5'] == 'tNasti_io_tNasti_w_ready & tNasti_io_tNasti_w_valid'
    assert pipe['SatUpDownCounter_2_io_inc'] == '_T_5 & tNasti_io_tNasti_w_bits_last'
    for name, expression in {
        '_newWReq_T_3': 'SatUpDownCounter_2_io_value < SatUpDownCounter_1_io_value & SatUpDownCounter_2_io_inc',
        '_newWReq_T_4': 'SatUpDownCounter_2_io_value > SatUpDownCounter_1_io_value & SatUpDownCounter_1_io_inc | _newWReq_T_3',
        '_newWReq_T_5': 'SatUpDownCounter_2_io_inc & SatUpDownCounter_1_io_inc',
        'newWReq': '_newWReq_T_4 | _newWReq_T_5',
        'awQueue_io_deq_ready': '_newWReq_T_4 | _newWReq_T_5',
        'writePipe_io_enq_valid': '_newWReq_T_4 | _newWReq_T_5',
    }.items(): assert pipe[name] == expression, name
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['model_clock'] == 'gate_O' and host['gate_I'] == 'clock'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    assert host['model_reset'] == 'hPort_hBits_reset'
    baseline = (evidence / 'candidate/post-fame-fased-write-pairing.mlir').read_text()
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-write-pairing{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-timing-aw-queue{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert prior and all(after[n] == body for n, body in prior.items()), 'existing module changed'
        assert set(after) - set(prior) == {'GGFASEDWritePairing', 'GGFASEDWritePairingWrapper'}
        old, new = 'GGFASEDTimingAWQueueWrapper', 'GGFASEDWritePairingWrapper'
        consumed = {'fased_write_pair_complete'}
        old_ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in old_ports:
            if name not in consumed:
                header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        helper = native_module(text, 'GGFASEDWritePairing')
        assert helper == native_module(baseline, 'GGFASEDWritePairing')
        assert len(re.findall(' = firrtl.regreset ', helper)) == 2
        assert ' = firrtl.reg ' not in helper and ' = firrtl.mem ' not in helper
        for name in ('pendingAW', 'pendingW'):
            reg = re.search('%' + name + r' = firrtl.regreset %clock, (%\w+), (%\w+) :', helper)
            assert reg and re.search(re.escape(reg[1]) + r' = firrtl.and %reset, %targetFire ', helper)
            assert re.search(re.escape(reg[2]) + r' = firrtl.constant 0 : !firrtl.uint<4>', helper)
        nets, assertions = expressions(helper, 'GGFASEDWritePairing')
        assert not assertions
        counts = dict(transitions=0, aw_ahead_pairs=0, w_ahead_pairs=0, simultaneous_pairs=0,
                      full_saturation=0, empty_retirement=0, simultaneous_inc_dec=0,
                      lowered_maximum=0, stalls=0, stalled_resets=0, target_resets=0)

        def sample(aw, w, maximum, reset, fire, aw_fire, w_last, b_fire):
            inputs = dict(clock=1, reset=reset, targetFire=fire, pendingAW=aw, pendingW=w,
                          writeMaxReqs=maximum, awFire=aw_fire, wLastFire=w_last, bFire=b_fire)
            value = lambda name: evaluate(nets[name], inputs, nets)
            complete = (w > aw and aw_fire) or (w < aw and w_last) or (aw_fire and w_last)
            assert value('pairComplete') == complete
            next_values = []
            for n, v, inc, prefix in [('pendingAW', aw, aw_fire, 'aw'), ('pendingW', w, w_last, 'w')]:
                assert value('pending.' + prefix + 'Value') == v
                assert value('pending.' + prefix + 'Full') == (v >= maximum)
                next_v = (v + 1) & 15 if inc and not b_fire and v < maximum else v - 1 if not inc and b_fire and v else v
                assert value(n) == (next_v if fire else v)
                next_values.append(0 if reset and fire else next_v if fire else v)
            counts['transitions'] += 1
            counts['aw_ahead_pairs'] += aw > w and w_last and not aw_fire
            counts['w_ahead_pairs'] += w > aw and aw_fire and not w_last
            counts['simultaneous_pairs'] += bool(aw_fire and w_last)
            counts['full_saturation'] += fire and not reset and not b_fire and ((aw_fire and aw >= maximum) or (w_last and w >= maximum))
            counts['empty_retirement'] += fire and not reset and b_fire and ((not aw_fire and not aw) or (not w_last and not w))
            counts['simultaneous_inc_dec'] += fire and not reset and b_fire and (aw_fire or w_last)
            counts['lowered_maximum'] += aw > maximum or w > maximum
            counts['stalls'] += not fire
            counts['stalled_resets'] += reset and not fire
            counts['target_resets'] += bool(reset and fire)
            return next_values

        for aw, w, maximum, flags in itertools.product(range(16), range(16), range(16), range(32)):
            sample(aw, w, maximum, *(flags >> bit & 1 for bit in range(5)))
        rng = random.Random(227); aw = w = 0
        for i in range(30000):
            aw, w = sample(aw, w, rng.randrange(16), i % 997 == 0 or rng.randrange(113) == 0,
                           rng.randrange(3) != 0, rng.randrange(2), rng.randrange(2), rng.randrange(2))
        assert all(counts.values()), 'missing pairing coverage'
        connections = {'pairing.clock': 'top.hostClock', 'pairing.reset': 'sim.fased_model_reset',
                       'pairing.targetFire': 'sim.fased_tfire',
                       'pairing.awFire': frozenset(['sim.fased_timing_requests.aw.ready', 'sim.fased_timing_requests.aw.valid']),
                       'pairing.wLastFire': frozenset(['sim.fased_timing_requests.w.ready', 'sim.fased_timing_requests.w.valid', 'sim.fased_timing_requests.w.bits.last']),
                       'pairing.bFire': 'top.fased_target_b_fire',
                       'pairing.writeMaxReqs': 'top.fased_write_max_reqs',
                       'sim.fased_write_pair_complete': 'pairing.pairComplete',
                       'top.fased_pending_writes': 'pairing.pending'}
        for direction, name in old_ports:
            if name not in consumed:
                connections[('sim.' if direction == 'in' else 'top.') + name] = ('top.' if direction == 'in' else 'sim.') + name
        assert wiring(text, new, aggregate=True) == connections, 'pairing wrapper differs'
        top_ports = re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0])
        assert top_ports == [(d, n) for d, n in old_ports if n not in consumed] + [
            ('in', 'fased_target_b_fire'), ('in', 'fased_write_max_reqs'), ('out', 'fased_pending_writes')]
        assert re.search(r'name = "FASEDMemoryTimingModel_0", size = 128 : i64, slave = 1 : i32, start = 128 : i64', text)
        reports.append(dict(reverse=reverse, existing_modules_retained=len(prior),
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['SatUpDownCounter_1', 'LatencyPipe', 'FASEDMemoryTimingModel'],
                  constructor_orders=reports, reference_has_print_host=False,
                  scope='non-LLC AW/W completion pairing and target-qualified pending counters; admission, target B retirement attachment and runtime maximum MMIO attachment remain external')
    (evidence / 'rocket-fased-write-pairing-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC AW/W completion pairing, saturating pending counters and preserved Print banks in both orders')


if __name__ == '__main__': main()
