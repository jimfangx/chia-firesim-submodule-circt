#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare NIC target-visible B retirement with SFC and execute its counter cone.

The native exporter checks all six production observation hops, then clones the
actual B handshake, helper bindings and counter body at accepted AW/last-W and
target B field boundaries. Response queue generation, runtime maximum MMIO and
recorded clock-gate delta-cycle behavior are outside this differential.
"""
import argparse
import hashlib
import json
from pathlib import Path
from NICMMIOBankCompare import modules
from NICFASEDWritePairingCompare import oracle, differential
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer

OLD, NEW = 'GGFASEDWritePairingWrapper', 'GGFASEDWriteRetirementWrapper'
OBSERVED = [OLD] + ['GGFASED'+n+'Wrapper' for n in
    ('TimingAWQueue', 'WriteLatency', 'ReadLatency', 'TimingCycle', 'ResponseReleaser')]


def composition(directory, baseline):
    before_text = (directory/'post-fame-fased-write-pairing.mlir').read_text()
    text = (directory/'post-fame-fased-write-retirement.mlir').read_text()
    before, after = modules(before_text), modules(text)
    reference = baseline.read_text()
    assert set(after)-set(before) == {NEW}
    assert all(after[n] == body for n, body in before.items() if n not in OBSERVED)
    count = 0
    for j, name in enumerate(OBSERVED):
        assert native_ports(after[name]) == native_ports(before[name]) + [('out', 'fased_accepted_b_fire', '!firrtl.uint<1>')]
        expected = wiring(before_text, name, aggregate=True)
        expected['top.fased_accepted_b_fire'] = ('sim.fased_accepted_b_fire' if j < 5 else
            frozenset({'releaser.b.ready', 'releaser.b.valid'}))
        assert wiring(text, name, aggregate=True) == expected, name
        assert wiring(reference, name, aggregate=True)['top.fased_accepted_b_fire'] == expected['top.fased_accepted_b_fire']
        count += len(expected)
    copied = [p for p in native_ports(before[OLD]) if p[1] != 'fased_target_b_fire']
    assert native_ports(after[NEW]) == copied
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied}
    expected['sim.fased_target_b_fire'] = 'sim.fased_accepted_b_fire'
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-write-pairing-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-write-retirement-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after['GGFASEDWritePairing'] == before['GGFASEDWritePairing'] == modules(reference)['GGFASEDWritePairing']
    return dict(prior_modules=len(before), unchanged_modules=len(before)-len(OBSERVED),
                observation_hops=6, prior_and_observation_drivers_compared=count,
                wrapper_connections_compared=len(expected), annotation_records_retained=len(new))


def source_binding(path):
    text = clean(path.read_text())
    pipe = {k: ' '.join(v.split()) for k, v in equations(module(text, 'LatencyPipe')[1]).items()}
    for i in (1, 2):
        assert pipe[f'SatUpDownCounter_{i}_io_dec'] == 'tNasti_io_tNasti_b_ready & tNasti_io_tNasti_b_valid'
    assert pipe['tNasti_io_tNasti_b_valid'] == 'xactionRelease_io_b_valid'
    assert pipe['xactionRelease_io_b_ready'] == 'tNasti_io_tNasti_b_ready'
    release = equations(module(text, 'AXI4Releaser')[1])
    expected = {'io_b_valid': 'currentWrite_q_io_deq_valid',
                'currentWrite_q_io_deq_ready': 'io_b_ready & io_b_valid',
                'io_egressReq_b_valid': 'io_nextWrite_ready & io_nextWrite_valid'}
    assert all(release[n] == v for n, v in expected.items())
    return expected


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args()
    assert source_binding(a.golden) == source_binding(a.immutable)
    cone, gate = oracle(a.golden)
    assert tuple(' '.join(x.split()) for x in (cone, gate)) == tuple(' '.join(x.split()) for x in oracle(a.immutable))
    candidate = a.candidate_dir/'post-fame-fased-write-retirement.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['LatencyPipe', 'AXI4Releaser', 'SatUpDownCounter_1', 'FASEDMemoryTimingModel'],
                  rtl_differential=differential(candidate, cone, gate, a.export_tool, a.output_dir, retirement=True),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC target B retirement, preserved observation hierarchy and SFC counter differential')


if __name__ == '__main__': main()
