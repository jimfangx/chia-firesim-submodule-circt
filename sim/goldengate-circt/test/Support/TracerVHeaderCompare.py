#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the native TracerV shared-stream constructor to the completed SFC ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def fragments(text):
    # Headers are deliberately multi-include files. Select exactly the TracerV
    # section, preserving every address, stream argument and struct assertion.
    blocks = re.findall(r'(#ifdef GET_INCLUDES\n#include "bridges/tracerv.h"\n.*?'
                        r'#endif // GET_BRIDGE_CONSTRUCTOR)', text, re.S)
    assert blocks, "missing TracerV constructors"
    return blocks


def normalize(text):
    # SFC's StringBuilder emits literal line continuations on static_asserts.
    # Remove only lexical spelling/whitespace; retain constructor argument order.
    text = re.sub(r'R"ESC\((.*?)\)ESC"', lambda m: json.dumps(m[1]), text)
    text = re.sub(r'(\d+)U\b', r'\1', text)
    return re.sub(r'\s+', '', text.replace('\\\n', '\n'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('candidate', 'sfc', 'native-tool', 'cxx', 'output-dir'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument("--quad", action="store_true")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    native = args.output_dir / 'native'
    native.mkdir()
    with (args.output_dir / 'native.log').open('w') as log:
        subprocess.run([str(args.native_tool), str(args.candidate / 'post-fame-f1-shim.mlir'),
                        str(native), "--quad" if args.quad else "--nic"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    if args.quad:
        before = json.loads((args.candidate / 'post-fame-f1-shim-all.json').read_text())
        after = json.loads((native / 'tracerv-all.json').read_text())
        # The boundary exporter also prepares the physical interface and other
        # widget headers. Every change must still belong to OutputFile records.
        def retained(rows):
            return [a for a in rows if a['class'] != 'midas.stage.GoldenGateOutputFileAnnotation']
        assert retained(after) == retained(before)
        assert sum(a.get('fileSuffix') == '.const.h' for a in after) == 1
        candidate = native / 'FireSim-generated.const.h'
        assert candidate.read_text() == next(a['body'] for a in after
            if a.get('fileSuffix') == '.const.h')
    else:
        before = json.loads((args.candidate / 'post-fame-nic-header-all.json').read_text())
        after = json.loads((args.candidate / 'post-fame-tracerv-header-all.json').read_text())
        assert len(before) == len(after)
        (old, new), = [(a, b) for a, b in zip(before, after) if a != b]
        assert new['body'].startswith(old['body']) and new['goldengate.tracervHeader'] is True
        assert {k: v for k, v in new.items() if k not in ('body', 'goldengate.tracervHeader')} == {
            k: v for k, v in old.items() if k != 'body'}
        candidate = args.candidate / 'tracerv-header/FireSim-generated.const.h'
        assert candidate.read_text() == new['body']
        assert json.loads((native / 'tracerv-all.json').read_text()) == after
        assert candidate.read_bytes() == (native / candidate.name).read_bytes()
    expected, actual = fragments(args.sfc.read_text()), fragments(candidate.read_text())
    assert len(actual) == len(expected) == (4 if args.quad else 1)
    assert list(map(normalize, actual)) == list(map(normalize, expected)), (actual, expected)
    # Typecheck against the real driver, including its ordered address struct.
    tree = Path(__file__).resolve().parents[3]
    repo = tree.parents[2]
    includes = [tree / 'midas/src/main/cc',
                repo / 'generators/firechip/bridgestubs/src/main/cc']
    for index, block in enumerate(("\n".join(actual), "\n".join(expected))):
        header = args.output_dir / f'fragment-{index}.h'
        header.write_text(block + '\n')
        source = args.output_dir / f'abi-{index}.cpp'
        source.write_text('#include "bridges/tracerv.h"\n'
            'struct Registry { StreamEngine *get_stream_engine(); void add_widget(bridge_driver_t *); };\n'
            '#define GET_SUBSTRUCT_CHECKS\n#include ' + json.dumps(str(header.resolve())) + '\n'
            '#undef GET_SUBSTRUCT_CHECKS\n'
            'void build(Registry &registry, simif_t &simif, const std::vector<std::string> &args) {\n'
            '#define GET_BRIDGE_CONSTRUCTOR\n#include ' + json.dumps(str(header.resolve())) + '\n}\n')
        with (args.output_dir / f'abi-{index}.log').open('w') as log:
            subprocess.run([str(args.cxx), '-std=c++20', '-fsyntax-only',
                            *[arg for path in includes for arg in ('-I', str(path))],
                            str(source)], stdout=log, stderr=subprocess.STDOUT, check=True)
    report = dict(golden=str(args.sfc.resolve()),
        golden_sha256=hashlib.sha256(args.sfc.read_bytes()).hexdigest(),
        candidate=str(candidate.resolve()), complete_TracerV_fragment_match=True,
        mmio_addresses=[list(range(256 + 64*i, 316 + 64*i, 4)) for i in range(len(actual))],
        widget_indices=list(range(len(actual))),
        stream_arguments=[[i if args.quad else 1, 6144, 1] for i in range(len(actual))],
        outgoing_stream_count=4 if args.quad else 2,
        native_emission_identical=True, compiled_driver_ABIs=2,
        annotation_count=len(after),
        changed_annotations=sum(a != b for a, b in zip(before, after)) + abs(len(before) - len(after)),
        limitation='Fragment ABI comparison does not establish complete simulator runtime parity.')
    (args.output_dir / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
