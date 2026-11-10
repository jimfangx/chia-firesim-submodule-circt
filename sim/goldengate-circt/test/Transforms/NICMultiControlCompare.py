#!/usr/bin/env python3
"""Exercise each actual indexed NIC AXI path while its peer holds responses.

Uses the unchanged archived SFC MCRFile_4 and register cone through the existing
single-path differential checker. This is a supporting primitive comparison;
global allocation, loopback and F2 Linux acceptance require separate evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from NICControlCompare import compare_rtl
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports


def artifact(path):
    path = Path(path).resolve()
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('compose-tool', 'golden', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []

    def run_at(directory, cmd, name):
        cmd = list(map(str, cmd))
        commands.append(cmd)
        with (directory / (name + '.log')).open('w') as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True)

    run_at(out, [args.compose_tool, out / 'indexed.mlir', '--control'], 'compose')
    run_at(out, ['firtool', out / 'indexed.mlir', '--format=mlir',
                 '--disable-all-randomization', '--disable-annotation-unknown',
                 '-o', out / 'indexed.sv'], 'firtool')
    public = ports(module(clean((out / 'indexed.sv').read_text()),
                          'GGNICBridgeControlWrapper')[0])
    results = []
    for widget in range(2):
        case = out / str(widget)
        case.mkdir()
        prefix = f'nicBridge_{widget}_ctrl_'

        def canonical(name):
            if name == 'hostClock':
                return 'clock'
            if name == 'hostReset':
                return 'reset'
            if name == f'done{widget}_input':
                return 'nic_done'
            if name.startswith(f'observed{widget}_'):
                return 'nic_' + name.split('_', 1)[1]
            if name.startswith(prefix):
                return name.removeprefix(prefix)
            return None

        selected = {n: (canonical(n), d, w) for n, (d, w) in public.items()
                    if canonical(n) is not None}
        wrapper = ['module GGNICControlTest(' + ','.join(
            f'{d} [{w-1}:0] {c}' for c, d, w in selected.values()) + ');']
        bindings = []
        for name, (direction, width) in public.items():
            value = canonical(name)
            if value is None:
                if direction == 'output':
                    value = ''
                else:
                    # The peer accepts one AW/W and one AR, then keeps B/R
                    # stalled throughout the selected path's transactions.
                    peer = name.removeprefix(f'nicBridge_{1-widget}_ctrl_')
                    constant = 1 if peer in ('aw_valid', 'w_valid', 'ar_valid') else 0
                    if peer == 'ar_bits_addr':
                        constant = 20  # readable done, never write-only words
                    value = f"{width}'d{constant}"
            bindings.append(f'.{name}({value})')
        wrapper += ['GGNICBridgeControlWrapper actual(' + ','.join(bindings) + ');',
                    'endmodule']
        native = case / 'native.sv'
        native.write_text((out / 'indexed.sv').read_text() + '\n' + '\n'.join(wrapper))
        _, simulation = compare_rtl(
            native, args.golden, case,
            lambda cmd, name: run_at(case, cmd, name))
        results.append(dict(widget=widget, simulation=simulation,
                            native=artifact(native)))
    report = dict(result='matched', golden=artifact(args.golden),
                  compose_tool=artifact(args.compose_tool),
                  script=artifact(__file__), emitted_rtl=artifact(out / 'indexed.sv'),
                  commands=commands, cases=results,
                  scope='Actual two-widget native wrapper, each path compared to unchanged SFC while peer holds B/R. Uninitialized captures unspecified until valid; configuration checked after initialization.',
                  incomplete=['Global allocation/header integration proof',
                              'Loopback modes', 'Separate LEC and SEC',
                              'F2 Linux feature representative'])
    (out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS both indexed NIC AXI paths with independent stalled peers')


if __name__ == '__main__':
    main()
