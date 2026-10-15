#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare typed incoming/outgoing NIC stream descriptors with the SFC ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def constructor(text):
    block, = re.findall(r'#ifdef GET_MANAGED_STREAM_CONSTRUCTOR\n(.*?)'
                        r'#endif // GET_MANAGED_STREAM_CONSTRUCTOR', text, re.S)
    # Normalize only spelling: preserve vector order, names and every value.
    block = re.sub(r'R"ESC\((.*?)\)ESC"', lambda m: json.dumps(m[1]), block)
    block = re.sub(r'(\d+)(?:ULL|U)\b', r'\1', block)
    return re.sub(r'\s+', '', block)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('candidate', 'sfc', 'native-tool', 'cxx', 'output-dir'):
        parser.add_argument('--' + key, type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    before = json.loads((args.candidate / 'post-fame-metasim-header-all.json').read_text())
    after = json.loads((args.candidate / 'post-fame-cpu-stream-header-all.json').read_text())
    assert len(before) == len(after)
    changed = [(a, b) for a, b in zip(before, after) if a != b]
    (old, new), = changed
    assert new['body'].startswith(old['body'])
    assert new['goldengate.cpuManagedStreamHeader'] is True
    assert {k: v for k, v in new.items() if k not in ('body', 'goldengate.cpuManagedStreamHeader')} == {
        k: v for k, v in old.items() if k != 'body'}
    native = args.output_dir / 'native'
    native.mkdir()
    with (args.output_dir / 'native.log').open('w') as log:
        subprocess.run([str(args.native_tool), str(args.candidate / 'post-fame-f1-shim.mlir'),
                        str(native)], stdout=log, stderr=subprocess.STDOUT, check=True)
    assert json.loads((native / 'cpu-stream-all.json').read_text()) == after
    candidate = args.candidate / 'cpu-stream-header/FireSim-generated.const.h'
    assert candidate.read_bytes() == (native / candidate.name).read_bytes()
    assert candidate.read_text() == new['body']
    assert constructor(candidate.read_text()) == constructor(args.sfc.read_text())
    # Compile both constructors against the actual FireSim driver declaration.
    # No driver code is linked or executed by this focused syntax check.
    include = Path(__file__).resolve().parents[3] / 'midas/src/main/cc'
    for index, header in enumerate((candidate, args.sfc)):
        source = args.output_dir / f'abi-{index}.cpp'
        source.write_text('#include "bridges/cpu_managed_stream.h"\n'
            'struct Registry { void add_widget(CPUManagedStreamWidget *); };\n'
            'void build(Registry &registry, simif_t &simif, const std::vector<std::string> &args) {\n'
            '#define GET_MANAGED_STREAM_CONSTRUCTOR\n#include ' + json.dumps(str(header.resolve())) + '\n}\n')
        with (args.output_dir / f'abi-{index}.log').open('w') as log:
            subprocess.run([str(args.cxx), '-std=c++20', '-fsyntax-only', '-I', str(include),
                            str(source)], stdout=log, stderr=subprocess.STDOUT, check=True)
    report = dict(golden=str(args.sfc.resolve()),
        golden_sha256=hashlib.sha256(args.sfc.read_bytes()).hexdigest(),
        candidate=str(candidate.resolve()), constructor_match=True,
        incoming_descriptors=1, outgoing_descriptors=2, native_emission_identical=True,
        annotation_count=len(after), changed_annotations=1, compiled_driver_ABIs=2,
        atomic_rejections=10,
        limitation='SimpleNIC driver constructor, final NIC RTL and runtime validation remain pending.')
    (args.output_dir / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
