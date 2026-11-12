#!/usr/bin/env python3
# See LICENSE for license details.
"""Exercise all 64 scheduler slots through emitted RTL public ports."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def expand(source, count):
    lines = []
    for line in source.splitlines():
        lines.append(line)
        if re.search(r'\b(?:in|out|worker)3\b', line):
            for index in range(4, count):
                clone = line
                for prefix in ('in', 'out', 'worker'):
                    clone = re.sub(r'\b' + prefix + r'3\b', prefix + str(index), clone)
                lines.append(clone)
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--verilator', required=True)
    p.add_argument('--fixtures', required=True)
    p.add_argument('--output-dir', required=True)
    p.add_argument('--reference', type=Path)
    a = p.parse_args()
    out = Path(a.output_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixtures = Path(a.fixtures).resolve()
    source = expand((fixtures / 'FAME5GroupedModels.fir').read_text(), 64)
    design = out / 'input.fir'
    design.write_text(source)
    order = [int(i) for i in re.findall(r'^\s*inst worker(\d+) of Worker$', source, re.M)]
    assert len(order) == len(set(order)) == 64
    ports = out / 'ports.h'
    ports.write_text('\n'.join([
        '#define INPUTS(suffix) ' + ', '.join('&top.in' + str(i) + '_##suffix' for i in range(64)),
        '#define OUTPUTS(suffix) ' + ', '.join('&top.out' + str(i) + '_##suffix' for i in range(64)),
        '#define FAME5_MEMBER_ORDER ' + ', '.join(map(str, order)), '']))
    commands = []

    def run(argv, name):
        argv = list(map(str, argv)); commands.append(argv)
        with (out / (name + '.log')).open('w') as log:
            result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError((out / (name + '.log')).read_text())
        return (out / (name + '.log')).read_text()

    run([a.compiler, design, '--annotation-file', fixtures / 'FAME5GroupedModels.anno.json',
         '--output-dir', out / 'native', '--multi-thread-fame5-models'], 'compile')
    results = {}
    candidates = [('native', out / 'native/post-fame5-models.sv')]
    if a.reference: candidates.append(('sfc', a.reference.resolve()))
    # Both compilers run the same independent public-interface scoreboard.
    for label, rtl in candidates:
        obj = out / (label + '-obj')
        run([a.verilator, '--cc', '--exe', '--build', '-j', '8', '-Wno-fatal',
             '--top-module', 'Top', '--Mdir', obj, rtl,
             fixtures / 'FAME5GroupedModelsRTL.cpp', '-CFLAGS',
             '-std=c++17 -DTHREADS=64 -include ' + str(ports)], label + '-build')
        results[label] = run([obj / 'VTop'], label + '-run').strip()
    if a.reference: assert results['native'] == results['sfc'], results
    report = {'passed': True, 'threads': 64, 'cycles': 8192,
              'input_sha256': hashlib.sha256(design.read_bytes()).hexdigest(),
              'declaration_order': order, 'results': results, 'commands': commands}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(results)


if __name__ == '__main__':
    main()
