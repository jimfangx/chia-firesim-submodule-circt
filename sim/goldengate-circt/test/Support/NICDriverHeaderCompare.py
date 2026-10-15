#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the complete native NIC driver ABI with the archived SFC header."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def normalize(text):
    # Preserve argument/vector/register order and all values. Normalize C++
    # spellings used by the two emitters, including the equivalent PeekPoke
    # PortMap alias and its single std::make_pair initializer.
    text = re.sub(r'R"ESC\((.*?)\)ESC"', lambda m: json.dumps(m[1]), text)
    text = re.sub(r'std::string\(("(?:\\.|[^"\\])*")\)', r'\1', text)
    text = text.replace('std::map<std::string, peek_poke_t::Port, std::less<>>',
                        'peek_poke_t::PortMap')
    text = re.sub(r'std::make_pair\(("[^"]*"),\s*(peek_poke_t::Port\{[^}]*\})\)',
                  r'{\1,\2}', text)
    text = re.sub(r'(\d+)(?:ULL|U)\b', r'\1', text)
    return re.sub(r'\s+', '', text.replace('\\\n', '\n'))


def constructors(text):
    result = {}
    for guard, block in re.findall(r'#ifdef (GET_(?:CORE|BRIDGE|MANAGED_STREAM)_CONSTRUCTOR)\n'
                                  r'(.*?)#endif // \1', text, re.S):
        cls, args = re.findall(r'registry.add_widget\(new (\w+)\((.*?)\)\);', block, re.S)[0]
        assert cls not in result, cls
        result[cls] = (guard, normalize(args))
    assert text.count('registry.add_widget(new ') == len(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('candidate', 'sfc', 'native-tool', 'cxx', 'output-dir'):
        parser.add_argument('--' + key, type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    before = json.loads((args.candidate / 'post-fame-metasim-header-all.json').read_text())
    after = json.loads((args.candidate / 'post-fame-driver-header-all.json').read_text())
    assert len(before) == len(after)
    (old, new), = [(a, b) for a, b in zip(before, after) if a != b]
    markers = {k for k in new if k.startswith('goldengate.') and k not in old}
    assert len(markers) == 12 and all(new[k] is True for k in markers), markers
    assert {k: v for k, v in new.items() if k not in markers | {'body'}} == {
        k: v for k, v in old.items() if k != 'body'}
    assert new['body'].startswith(old['body'])
    candidate = args.candidate / 'driver-header/FireSim-generated.const.h'
    assert candidate.read_text() == new['body']
    expected, actual = args.sfc.read_text(), candidate.read_text()
    reference, observed = constructors(expected), constructors(actual)
    assert len(reference) == 12 and reference.keys() == observed.keys()
    for cls in reference:
        assert reference[cls] == observed[cls], (cls, observed[cls], reference[cls])
    # All fixed-layout ABI checks must also survive composition.
    checks = lambda s: sorted(normalize(x) for x in re.findall(r'static_assert\(.*?;', s, re.S))
    assert checks(actual) == checks(expected)
    interface = lambda s: normalize(re.findall(r'static constexpr TargetConfig conf_target\{.*?\n\};', s, re.S)[0])
    assert interface(actual) == interface(expected)
    native = args.output_dir / 'native'
    native.mkdir()
    with (args.output_dir / 'native.log').open('w') as log:
        subprocess.run([str(args.native_tool), '--rocket-native-driver-headers',
                        str(args.candidate / 'post-fame-f1-shim.mlir'), str(native)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    assert json.loads((native / 'driver-all.json').read_text()) == after
    assert (native / candidate.name).read_bytes() == candidate.read_bytes()
    # Exercise every real driver declaration and registry overload under the
    # same multi-include phases as entry.cc, without linking/executing a driver.
    tree = Path(__file__).resolve().parents[3]
    repo = tree.parents[2]
    includes = [tree / 'midas/src/main/cc',
                repo / 'generators/firechip/bridgestubs/src/main/cc',
                repo / '.conda-env/riscv-tools/include']
    for index, header in enumerate((candidate, args.sfc)):
        include = '#include ' + json.dumps(str(header.resolve())) + '\n'
        source = args.output_dir / f'abi-{index}.cpp'
        source.write_text('#include "core/config.h"\n#include "core/widget_registry.h"\n'
            '#define GET_INCLUDES\n' + include + '#undef GET_INCLUDES\n'
            '#define GET_SUBSTRUCT_CHECKS\n' + include + '#undef GET_SUBSTRUCT_CHECKS\n'
            '#define GET_METASIM_INTERFACE_CONFIG\n' + include +
            'void build(widget_registry_t &registry, simif_t &simif, const std::vector<std::string> &args) {\n' +
            ''.join('#define ' + guard + '\n' + include + '#undef ' + guard + '\n'
                    for guard in ('GET_CORE_CONSTRUCTOR', 'GET_MANAGED_STREAM_CONSTRUCTOR',
                                  'GET_BRIDGE_CONSTRUCTOR')) + '}\n')
        with (args.output_dir / f'abi-{index}.log').open('w') as log:
            subprocess.run([str(args.cxx), '-std=c++20', '-fsyntax-only',
                            *[arg for p in includes for arg in ('-I', str(p))], str(source)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    report = dict(golden=str(args.sfc.resolve()),
        golden_sha256=hashlib.sha256(args.sfc.read_bytes()).hexdigest(),
        candidate=str(candidate.resolve()), matching_constructors=sorted(reference),
        fixed_layout_checks=len(checks(actual)), target_config_match=True,
        annotation_count=len(after), changed_annotations=1, native_emission_identical=True,
        compiled_complete_driver_ABIs=2, atomic_rejections=12,
        limitation='Final NIC RTL integration and runtime validation remain pending.')
    (args.output_dir / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
