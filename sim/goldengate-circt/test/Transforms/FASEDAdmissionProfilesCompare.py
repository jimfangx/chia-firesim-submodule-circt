#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare constructor-sized production admission cones to an actual SFC model.

Uses cloned CIRCT helpers, wrapper connections, final-R and B handshakes, and
verbatim SFC counters/equations. This checks the admission boundary, not queues,
latency scheduling, the clock-gate shim's delta cycles or full target execution.
"""
import argparse
import hashlib
import json
from pathlib import Path
from NICMMIOBankCompare import modules
from NICFASEDReadAdmissionCompare import oracle as read_oracle, differential as read_diff
from NICFASEDWritePairingCompare import oracle as write_oracle, differential as write_diff
from NICFASEDWriteAdmissionCompare import admission_oracle
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring


def compare(candidate, golden, tools, output, width):
    assert width in (4, 5)
    output.mkdir(parents=True, exist_ok=False)
    paths = {kind: candidate/f'post-fame-fased-{kind}.mlir' for kind in (
        'read-admission', 'write-admission', 'write-retirement', 'write-pairing',
        'request-limits', 'request-limits-bank')}
    read = modules(paths['read-admission'].read_text())['GGFASEDReadAdmission']
    write = modules(paths['write-pairing'].read_text())['GGFASEDWritePairing']
    assert ('in', 'readMaxReqs', f'!firrtl.uint<{width}>') in native_ports(read)
    assert ('in', 'writeMaxReqs', f'!firrtl.uint<{width}>') in native_ports(write)
    for kind, names in [('read-admission', ('value',)), ('write-pairing', ('awValue', 'wValue'))]:
        helper = read if kind == 'read-admission' else write
        for name in names:
            assert f'{name}: uint<{width}>' in helper
    limits_text = paths['request-limits'].read_text()
    bank = modules(limits_text)['GGFASEDRequestLimits']
    assert bank == modules(paths['request-limits-bank'].read_text())['GGFASEDRequestLimits']
    assert f'!firrtl.bundle<write: uint<{width}>, read: uint<{width}>>' in bank
    attached = wiring(limits_text, 'GGFASEDRequestLimitsWrapper', aggregate=True)
    assert attached['sim.fased_read_max_reqs'] == 'requestLimits.limits.read'
    assert attached['sim.fased_write_max_reqs'] == 'requestLimits.limits.write'
    cone, gate = read_oracle(golden, width)
    report = dict(golden=str(golden.resolve()), golden_sha256=hashlib.sha256(golden.read_bytes()).hexdigest(),
                  width=width, candidate_artifacts={k: dict(path=str(p.resolve()),
                    sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for k, p in paths.items()},
                  request_limit_bank_preserved_and_attached=True)
    report['read'] = read_diff(paths['read-admission'], cone, gate,
        tools/'goldengate-fased-read-admission-test', output/'read', width=width)
    cone, gate = write_oracle(golden, width)
    report['write'] = write_diff(paths['write-retirement'], cone+admission_oracle(golden), gate,
        tools/'goldengate-fased-write-retirement-test', output/'write', retirement=True,
        admission=(paths['write-admission'], tools/'goldengate-fased-write-admission-test'), width=width)
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'golden', 'tools-dir', 'output-dir'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--width', type=int, choices=(4, 5), required=True)
    a = p.parse_args()
    report = compare(a.candidate_dir, a.golden, a.tools_dir, a.output_dir, a.width)
    print('PASS', a.golden, 'width', a.width, 'read cycles', report['read']['cycles'],
          'write cycles', report['write']['cycles'])


if __name__ == '__main__':
    main()
