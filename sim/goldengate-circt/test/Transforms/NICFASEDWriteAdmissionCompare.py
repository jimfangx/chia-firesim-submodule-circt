#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute NIC closed AW/W admission and pending-counter retirement against SFC.

The exporter clones the actual admission NotPrimOps and observed connections;
the retirement exporter clones the actual B handshake, pending-counter bindings
and helper body. The RTL test closes each full flag back to accepted AW/last-W.
All production wrapper wiring and annotation transfers are compared separately.
Queue generation, runtime maximum MMIO and recorded clock-gate delta-cycle
behavior remain outside this differential.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from NICMMIOBankCompare import modules
from NICFASEDWritePairingCompare import oracle, differential
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer

OLD, NEW = 'GGFASEDWriteRetirementWrapper', 'GGFASEDWriteAdmissionWrapper'


def composition(directory, baseline):
    before_text = (directory/'post-fame-fased-write-retirement.mlir').read_text()
    text = (directory/'post-fame-fased-write-admission.mlir').read_text()
    before, after = modules(before_text), modules(text)
    reference = modules(baseline.read_text())
    assert set(after)-set(before) == {NEW}
    assert all(after[n] == body for n, body in before.items())
    copied = native_ports(before[OLD])
    assert [(d, n) for d, n, _ in native_ports(after[NEW])] == [(d, n) for d, n, _ in copied]
    # The native exporter checks every field and flip; compare the full request
    # type to the already supported no-NIC boundary as an independent reference.
    new_ports = {n: (d, t) for d, n, t in native_ports(after[NEW])}
    ref_ports = {n: (d, t) for d, n, t in native_ports(reference[NEW])}
    assert new_ports['fased_timing_requests'] == ref_ports['fased_timing_requests']
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied if n != 'fased_timing_requests'}
    for channel in ('aw', 'w'):
        for side in ('sim', 'top'):
            expected[f'{side}.fased_timing_requests.{channel}.ready'] = ('not', f'sim.fased_pending_writes.{channel}Full')
        for field in ('valid', 'bits'):
            expected[f'top.fased_timing_requests.{channel}.{field}'] = f'sim.fased_timing_requests.{channel}.{field}'
    expected['top.fased_timing_requests.ar'] = 'sim.fased_timing_requests.ar'
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-write-retirement-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-write-admission-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after['GGFASEDWritePairing'] == reference['GGFASEDWritePairing']
    return dict(prior_modules_retained=len(before), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(new), request_payload_and_AR_passthrough_preserved=True)


def admission_oracle(path):
    text = clean(path.read_text())
    pipe = module(text, 'LatencyPipe')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(pipe).items()}
    identity_name = re.search(r'(\w+) nastiReqIden \(', pipe).group(1)
    identity = equations(module(text, identity_name)[1])
    for ch in ('aw', 'w'):
        assert eq[f'tNasti_io_tNasti_{ch}_ready'] == f'nastiReqIden_io_in_{ch}_ready'
        assert identity[f'io_in_{ch}_ready'] == f'io_out_{ch}_ready'
    shell = 'module SFCAdmission(input SatUpDownCounter_1_io_full, SatUpDownCounter_2_io_full, output nastiReqIden_io_out_aw_ready, nastiReqIden_io_out_w_ready);\n'
    for channel, counter in (('aw', 1), ('w', 2)):
        name = f'nastiReqIden_io_out_{channel}_ready'
        assert eq[name] == f'~SatUpDownCounter_{counter}_io_full'
        assignment = re.findall(r'\bassign\s+'+name+r'\s*=.*?;', pipe, re.S)
        assert len(assignment) == 1
        shell += assignment[0]+'\n'
    return shell+'endmodule\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'retirement-export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args()
    cone, gate = oracle(a.golden)
    assert tuple(' '.join(x.split()) for x in (cone, gate)) == tuple(' '.join(x.split()) for x in oracle(a.immutable))
    ready = admission_oracle(a.golden)
    assert ' '.join(ready.split()) == ' '.join(admission_oracle(a.immutable).split())
    candidate = a.candidate_dir/'post-fame-fased-write-admission.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['LatencyPipe', 'IdentityModule', 'SatUpDownCounter_1', 'FASEDMemoryTimingModel'],
                  rtl_differential=differential(a.candidate_dir/'post-fame-fased-write-retirement.mlir',
                      cone+ready, gate, a.retirement_export_tool, a.output_dir,
                      retirement=True, admission=(candidate, a.export_tool)), limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC independent AW/W admission, preserved payloads/AR and closed SFC counter differential')


if __name__ == '__main__': main()
