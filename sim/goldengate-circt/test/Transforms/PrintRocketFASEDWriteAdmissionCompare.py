#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare expanded Print/Rocket pending-full admission with immutable SFC RTL."""
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
    for ch, index in [('aw', 1), ('w', 2)]:
        assert pipe[f'nastiReqIden_io_out_{ch}_ready'] == f'~SatUpDownCounter_{index}_io_full'
        assert pipe[f'SatUpDownCounter_{index}_io_max'] == 'tNasti_io_mmReg_writeMaxReqs'
        assert pipe[f'SatUpDownCounter_{index}_io_dec'] == 'tNasti_io_tNasti_b_ready & tNasti_io_tNasti_b_valid'
    assert pipe['SatUpDownCounter_1_io_inc'] == 'tNasti_io_tNasti_aw_ready & tNasti_io_tNasti_aw_valid'
    assert pipe['SatUpDownCounter_2_io_inc'] == '_T_5 & tNasti_io_tNasti_w_bits_last'
    assert pipe['_T_5'] == 'tNasti_io_tNasti_w_ready & tNasti_io_tNasti_w_valid'
    counter = module(reference, 'SatUpDownCounter_1')[1]
    ceq = equations(counter)
    assert ceq['io_full'] == 'value >= io_max' and ceq['io_empty'] == "value == 4'h0"
    assert ceq['_value_T_1'] == "value + 4'h1" and ceq['_value_T_3'] == "value - 4'h1"
    assert re.search(r"if \(reset\) begin\s*value <= 4'h0;\s*end else if \(io_inc & ~io_dec & ~io_full\) begin\s*value <= _value_T_1;\s*end else if \(~io_inc & io_dec & ~io_empty\) begin\s*value <= _value_T_3;", counter)
    host = equations(module(reference, 'FASEDMemoryTimingModel')[1])
    assert host['model_clock'] == 'gate_O' and host['gate_I'] == 'clock'
    assert ' '.join(host['gate_CE'].split()) == ' '.join(host['targetFire'].split())
    assert host['model_reset'] == 'hPort_hBits_reset'
    baseline = (evidence / 'candidate/post-fame-fased-write-admission.mlir').read_text()
    old, new = 'GGFASEDWriteRetirementWrapper', 'GGFASEDWriteAdmissionWrapper'
    reports = []
    for reverse in (False, True):
        suffix = '-reverse' if reverse else ''
        text = (evidence / f'binding.mlir.rocket-fased-write-admission{suffix}.mlir').read_text()
        before = (previous / f'binding.mlir.rocket-fased-write-retirement{suffix}.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'an existing module changed'
        ports = re.findall(r'\b(in|out) %(\w+):', native_module(before, old).splitlines()[0])
        assert re.findall(r'\b(in|out) %(\w+):', native_module(text, new).splitlines()[0]) == ports
        # Native replay checks the complete types: only AW/W ready flips change;
        # AR retains reverse readiness and every payload field retains its type.
        connections = {('sim.' if d == 'in' else 'top.') + n: ('top.' if d == 'in' else 'sim.') + n
                       for d, n in ports if n != 'fased_timing_requests'}
        for ch, prefix in [('aw', 'aw'), ('w', 'w')]:
            ready = ('not', f'sim.fased_pending_writes.{prefix}Full')
            for side in ('sim', 'top'):
                connections[f'{side}.fased_timing_requests.{ch}.ready'] = ready
            for field in ('valid', 'bits'):
                connections[f'top.fased_timing_requests.{ch}.{field}'] = f'sim.fased_timing_requests.{ch}.{field}'
        connections['top.fased_timing_requests.ar'] = 'sim.fased_timing_requests.ar'
        actual = wiring(text, new, aggregate=True)
        assert actual == connections, 'admission wrapper wiring differs'
        shared = wiring(baseline, new, aggregate=True)
        for name, expr in connections.items():
            if '.fased_timing_requests.' in name:
                assert shared[name] == expr, 'expanded admission differs from fresh SFC-ingestion path'
        header = before.splitlines()[1].replace('firrtl.circuit "' + old + '"', 'firrtl.circuit "' + new + '"')
        for _, name in ports:
            header = header.replace('~' + old + '|' + old + '>' + name, '~' + old + '|' + new + '>' + name)
        header = header.replace('~' + old + '|', '~' + new + '|').replace('"~' + old + '"', '"~' + new + '"')
        assert text.splitlines()[1] == header, 'annotation retarget differs'
        helper = native_module(text, 'GGFASEDWritePairing')
        assert helper == native_module(baseline, 'GGFASEDWritePairing')
        for n in ('pendingAW', 'pendingW'):
            reg = re.search('%' + n + r' = firrtl.regreset %clock, (%\w+), (%\w+) :', helper)
            assert reg and re.search(re.escape(reg[1]) + r' = firrtl.and %reset, %targetFire ', helper)
            assert re.search(re.escape(reg[2]) + r' = firrtl.constant 0 : !firrtl.uint<4>', helper)
        nets, assertions = expressions(helper, 'GGFASEDWritePairing')
        assert not assertions
        counts = dict(transitions=0, aw_full=0, w_full=0, zero_maximum=0,
                      accepted_aw=0, accepted_final_w=0, blocked_aw=0, blocked_w=0,
                      stalled_acceptance=0, simultaneous_retirement=0, stalled_reset=0)

        def sample(aw, w, maximum, reset, fire, aw_valid, w_valid, w_last, b_ready, b_valid):
            inputs = dict(clock=1, reset=reset, targetFire=fire, pendingAW=aw, pendingW=w,
                          writeMaxReqs=maximum, bFire=bool(b_ready and b_valid))
            value = lambda n: evaluate(nets[n], inputs, nets)
            fulls = {'aw': value('pending.awFull'), 'w': value('pending.wFull')}
            # Interpret the actual wrapper's NotPrimOp rather than substituting
            # expected ready into the candidate's handshake/counter evaluation.
            def ready(ch):
                expr = actual[f'sim.fased_timing_requests.{ch}.ready']
                assert expr[0] == 'not'
                return not fulls['aw' if expr[1].endswith('awFull') else 'w']
            assert ready('aw') == (aw < maximum) and ready('w') == (w < maximum)
            aw_fire, w_fire = bool(ready('aw') and aw_valid), bool(ready('w') and w_valid)
            inputs.update(awFire=aw_fire, wLastFire=bool(w_fire and w_last))
            next_values = []
            for n, v, inc in [('pendingAW', aw, aw_fire), ('pendingW', w, inputs['wLastFire'])]:
                b_fire = inputs['bFire']
                next_v = v + 1 if inc and not b_fire and v < maximum else v - 1 if not inc and b_fire and v else v
                assert value(n) == (next_v if fire else v)
                next_values.append(0 if reset and fire else next_v if fire else v)
            counts['transitions'] += 1
            counts['aw_full'] += aw >= maximum
            counts['w_full'] += w >= maximum
            counts['zero_maximum'] += maximum == 0
            counts['accepted_aw'] += aw_fire
            counts['accepted_final_w'] += inputs['wLastFire']
            counts['blocked_aw'] += bool(aw_valid and not ready('aw'))
            counts['blocked_w'] += bool(w_valid and not ready('w'))
            counts['stalled_acceptance'] += bool(not fire and (aw_fire or w_fire))
            counts['simultaneous_retirement'] += bool(fire and inputs['bFire'] and (aw_fire or inputs['wLastFire']))
            counts['stalled_reset'] += bool(reset and not fire)
            return next_values

        for aw, w, maximum, flags in itertools.product(range(16), range(16), range(16), range(128)):
            sample(aw, w, maximum, *(flags >> bit & 1 for bit in range(7)))
        rng = random.Random(229); aw = w = 0
        for i in range(30000):
            aw, w = sample(aw, w, rng.randrange(16), i % 997 == 0 or rng.randrange(113) == 0,
                           rng.randrange(3) != 0, *(rng.randrange(2) for _ in range(5)))
        assert all(counts.values())
        reports.append(dict(reverse=reverse, unchanged_module_definitions=len(prior),
                            module_identities_retained=len(prior), ports_retained=len(ports),
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'SatUpDownCounter_1', 'FASEDMemoryTimingModel'],
                  constructor_orders=reports, reference_has_print_host=False,
                  scope='AW/W pending-full admission and accepted counter increments; AR handshake and existing Print banks preserved; runtime maximum MMIO remains external')
    (evidence / 'rocket-fased-write-admission-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC pending-full AW/W admission and counter transitions, preserved AR/Print boundary in both orders')


if __name__ == '__main__': main()
