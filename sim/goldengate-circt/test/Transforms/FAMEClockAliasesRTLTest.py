#!/usr/bin/env python3
# See LICENSE for license details.
"""Require real RTL emission and detect a missing live alias-clock driver."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--graph-test', required=True)
    p.add_argument('--firtool', required=True)
    p.add_argument('--fixtures', required=True)
    p.add_argument('--output-dir', required=True)
    a = p.parse_args()
    out = Path(a.output_dir)
    out.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(argv, name, check=True):
        argv = list(map(str, argv))
        commands.append(argv)
        with (work / (name + '.log')).open('w') as log:
            result = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
        if check and result.returncode:
            raise RuntimeError((work / (name + '.log')).read_text())
        return result

    with tempfile.TemporaryDirectory(dir=out) as temporary:
        work = Path(temporary)
        base = work / 'aliases'
        run([a.graph_test, '--clock-aliases', a.fixtures, base], 'graph')
        flags = ['--format=mlir', '--disable-all-randomization',
                 '--disable-annotation-unknown']
        # Completed FAME metadata is not a backend annotation schema. Hardware
        # checks remain enabled, including initialization and all port wiring.
        for suffix in ('', '-transport'):
            source = Path(str(base) + suffix + '.mlir')
            target = Path(str(base) + suffix + '.sv')
            run([a.firtool, source, *flags, '-o', target], 'rtl' + suffix)
            assert target.stat().st_size > 0
        original = Path(str(base) + '.mlir').read_text()
        broken, count = re.subn(
            r'^\s*firrtl\.strictconnect %feedback_0, %[^\s]+ : !firrtl\.clock\n',
            '\n', original, flags=re.M)
        assert count == 1, 'missing unique alias clock driver'
        mutant = work / 'missing-clock.mlir'
        mutant.write_text(broken)
        result = run([a.firtool, mutant, *flags, '-o', work / 'invalid.sv'],
                     'missing-clock', check=False)
        log = (work / 'missing-clock.log').read_text()
        assert result.returncode and re.search(
            r'not (?:fully )?initialized|uninitialized', log, re.I), log
        report = {'passed': True, 'aliases': [1, 3, 63], 'domains': 2,
                  'rtl_emission': ['model-boundary', 'active-transport'],
                  'missing_live_clock_driver_detected': True,
                  'commands': commands}
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        for log in work.glob('*.log'):
            (out / log.name).write_bytes(log.read_bytes())
        print('PASS alias-clock RTL emission, transport, and missing-driver negative control')


if __name__ == '__main__':
    main()
