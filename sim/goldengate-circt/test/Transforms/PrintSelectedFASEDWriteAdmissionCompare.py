#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare selected Print/Rocket pending-full admission with immutable SFC RTL."""
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
    baseline = baseline_path.read_text()
    old, new = 'GGSelectedPrintFASEDWriteRetirementWrapper', 'GGSelectedPrintFASEDWriteAdmissionWrapper'
    reports = []
    for allocation in ('candidate', 'compatibility'):
        root = evidence / allocation
        text = (root / 'post-print-fased-write-admission.mlir').read_text()
        before = (root / 'post-print-fased-write-retirement.mlir').read_text()
        pattern = r'^    firrtl.module @(\w+)(\(.*?^    })'
        prior, after = (dict(re.findall(pattern, t, re.M | re.S)) for t in (before, text))
        assert set(after) - set(prior) == {new}
        assert all(after[n] == body for n, body in prior.items()), 'an existing module changed'
        prior_ports = native_ports(native_module(before, old))
        expected_ports = [(d, n, t.replace('ready flip:', 'ready:', 2) if n == 'fased_timing_requests' else t)
                          for d, n, t in prior_ports]
        assert native_ports(native_module(text, new)) == expected_ports, 'only AW/W ready flips may change'
        ports = [(d, n) for d, n, _ in prior_ports]
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
        shared = wiring(baseline, 'GGFASEDWriteAdmissionWrapper', aggregate=True)
        for name, expr in connections.items():
            if '.fased_timing_requests.' in name:
                assert shared[name] == expr, 'expanded admission differs from fresh SFC-ingestion path'
        prior_annos = json.loads((root / 'post-print-fased-write-retirement-all.json').read_text())
        annos = json.loads((root / 'post-print-fased-write-admission-all.json').read_text())
        assert annos == transfer(prior_annos, old, new, {n for _, n in ports}), 'annotation identities differ'
        for header in ('print-bridge-allocated.const.h', 'print-bridge-decoders.h'):
            previous = evidence.parent / 'iteration156-print-fased-write-retirement' / allocation / header
            assert (root / header).read_bytes() == previous.read_bytes(), 'allocated Print/CPU collateral changed'
        rtl = clean((root / 'post-print-fased-write-admission.sv').read_text())
        rtl_top = module(rtl, new)[1]
        rtl_nets = {k: ' '.join(v.split()) for k, v in equations(rtl_top).items()}
        child = re.search(r'\b' + old + r'\s+sim\s*\((.*?)\);', rtl_top, re.S)
        assert child
        pins = dict(re.findall(r'\.(\w+)\s*\(([^()]*)\)', child[1]))
        pins = {k: ' '.join(v.split()) for k, v in pins.items()}
        for ch in ('aw', 'w'):
            signal = f'fased_timing_requests_{ch}_ready'
            full = f'fased_pending_writes_{ch}Full'
            full_source = pins[full]
            assert rtl_nets.get(full, full) == full_source, 'pending-full observation differs'
            assert rtl_nets[signal] == '~' + full_source, 'RTL admission ready differs'
            assert pins[signal] in (signal, '~' + full_source) or rtl_nets.get(pins[signal]) == '~' + full_source
        assert pins['fased_timing_requests_ar_ready'] == 'fased_timing_requests_ar_ready'
        request_pins = {k: v for k, v in pins.items() if k.startswith('fased_timing_requests_')}
        assert len(request_pins) == 33
        assert all(k == v for k, v in request_pins.items()
                   if k not in ('fased_timing_requests_aw_ready', 'fased_timing_requests_w_ready')), 'RTL payload or AR handshake changed'
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
        reports.append(dict(allocation=allocation, annotations_transferred=len(annos), rtl_request_pins_compared=len(request_pins), unchanged_module_definitions=len(prior),
                            module_identities_retained=len(prior), ports_retained=len(ports),
                            wrapper_connections_compared=len(connections), **counts))
    report = dict(golden=str(golden), SFC_modules=['LatencyPipe', 'SatUpDownCounter_1', 'FASEDMemoryTimingModel'],
                  allocations=reports, reference_has_print_host=False,
                  scope='AW/W pending-full admission independent from queue capacity, reset and targetFire; accepted counter increments; AR handshake and existing Print banks preserved; runtime maximum MMIO remains external')
    (evidence / 'selected-fased-write-admission-comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS SFC pending-full AW/W admission and counter transitions, preserved AR/Print boundary in both allocations')


if __name__ == '__main__': main()
