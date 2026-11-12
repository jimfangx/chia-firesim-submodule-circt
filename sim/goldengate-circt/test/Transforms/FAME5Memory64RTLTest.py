#!/usr/bin/env python3
# See LICENSE for license details.
"""Exercise 64-thread combinational and synchronous target RAM through RTL."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--verilator', required=True)
    p.add_argument('--fixtures', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--reference-latency0', type=Path)
    p.add_argument('--reference-latency1', type=Path)
    a = p.parse_args()
    out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    fixtures = a.fixtures.resolve(); commands = []; cases = []

    def run(argv, name):
        argv = list(map(str, argv)); commands.append(argv)
        log = out / (name + '.log')
        with log.open('w') as stream:
            result = subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(log.read_text())
        return log.read_text().strip()

    for latency, scoreboard, reference in (
            (0, 'FAME5ThreadedMemoryRTL.cpp', a.reference_latency0),
            (1, 'FAME5SynchronousMemoryRTL.cpp', a.reference_latency1)):
        source = (fixtures / 'FAME5ThreadedMemory.fir').read_text()
        assert source.count('threadIdx : UInt<2>') == 1
        source = source.replace('threadIdx : UInt<2>', 'threadIdx : UInt<6>')
        if latency:
            source = source.replace('read-latency => 0', 'read-latency => 1')
        design = out / ('input-latency' + str(latency) + '.fir')
        design.write_text(source)
        generated = out / ('native-latency' + str(latency))
        run([a.compiler, design, '--annotation-file', fixtures / 'FAME5ThreadedMemory.anno.json',
             '--output-dir', generated, '--thread-fame5-memories', 'Model', '64', 'threadIdx'],
            'compile-latency' + str(latency))
        candidates = [('native', generated / 'post-fame5-memories.sv')]
        if reference:
            candidates.append(('sfc', reference.resolve()))
        results = {}; hashes = {}
        for label, rtl in candidates:
            name = label + '-latency' + str(latency)
            obj = out / (name + '-obj')
            hashes[label] = hashlib.sha256(rtl.read_bytes()).hexdigest()
            run([a.verilator, '--cc', '--exe', '--build', '-j', '8', '-Wno-fatal',
                 '--top-module', 'Model', '--Mdir', obj, rtl, fixtures / scoreboard,
                 '-CFLAGS', '-std=c++17'], name + '-build')
            results[label] = run([obj / 'VModel', '64'], name + '-run')
        if reference:
            assert results['native'] == results['sfc'], results
        cases.append({'read_latency': latency, 'threads': 64, 'results': results,
                      'input_sha256': hashlib.sha256(design.read_bytes()).hexdigest(),
                      'rtl_sha256': hashes})
    (out / 'result.json').write_text(json.dumps(
        {'passed': True, 'cases': cases, 'commands': commands}, indent=2) + '\n')
    print(cases)


if __name__ == '__main__':
    main()
