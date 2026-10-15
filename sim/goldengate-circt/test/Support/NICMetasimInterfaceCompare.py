#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC metasim collateral with SFC and the native ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Transforms'))
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, module


def macros(text):
    return dict(re.findall(r'^`define (\S+) (\d+)$', text, re.M))


def config(text):
    block, = re.findall(r'#ifdef GET_METASIM_INTERFACE_CONFIG\n(.*?)'
                        r'#endif // GET_METASIM_INTERFACE_CONFIG', text, re.S)
    # Scala uses a raw C string for the original target name.
    return re.sub(r'\s+', '', block.replace('R"ESC(FireSim)ESC"', '"FireSim"'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate', 'sfc', 'immutable-sfc', 'native-tool', 'cxx', 'output-dir'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    before = json.loads((args.candidate / 'post-fame-f1-shim-all.json').read_text())
    after = json.loads((args.candidate / 'post-fame-metasim-header-all.json').read_text())
    assert after[:-2] == before and len(after) == len(before) + 2
    headers = args.candidate / 'metasim-interface'
    for record, suffix in zip(after[-2:], ('.const.vh', '.const.h')):
        assert record['class'] == 'midas.stage.GoldenGateOutputFileAnnotation'
        assert record['fileSuffix'] == suffix and 'target' not in record
        assert (headers / ('FireSim-generated' + suffix)).read_text() == record['body']
    # The native test checks all retained module bodies and annotation prefixes
    # before emitting from the exact production pre-header MLIR boundary.
    native = args.output_dir / 'native'
    native.mkdir()
    with (args.output_dir / 'native.log').open('w') as log:
        subprocess.run([str(args.native_tool),
                        str(args.candidate / 'post-fame-f1-shim.mlir'), str(native)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    assert json.loads((native / 'metasim-all.json').read_text()) == after
    assert {p.name for p in headers.iterdir()} == {
        p.name for p in native.iterdir() if p.name != 'metasim-all.json'}
    for path in headers.iterdir():
        assert path.read_bytes() == (native / path.name).read_bytes(), path.name

    header = headers / 'FireSim-generated.const.h'
    verilog = macros((headers / 'FireSim-generated.const.vh').read_text())
    expected = macros(args.sfc.with_suffix('.const.vh').read_text())
    assert expected.pop('MEM_HAS_CHANNEL-1') == '1'
    assert verilog == expected and len(verilog) == 12
    golden_header = args.sfc.with_suffix('.const.h')
    assert config(header.read_text()) == config(golden_header.read_text())
    comparisons = []
    for golden in (args.sfc, args.immutable_sfc):
        interface = ports(module(clean(golden.read_text()), 'FPGATop')[0])
        checks = 0
        for prefix, bus in [('CTRL', 'ctrl'), ('MEM', 'mem_0'),
                            ('CPU_MANAGED_AXI4', 'cpu_managed_axi4')]:
            master = bus == 'mem_0'
            for channel, leaf, kind in [('aw', 'id', 'ID'), ('ar', 'id', 'ID'),
                    ('b', 'id', 'ID'), ('r', 'id', 'ID'), ('aw', 'addr', 'ADDR'),
                    ('ar', 'addr', 'ADDR'), ('w', 'data', 'DATA'),
                    ('r', 'data', 'DATA'), ('w', 'strb', 'DATA')]:
                width = int(verilog[f'{prefix}_{kind}_BITS'])
                if leaf == 'strb':
                    width //= 8
                request = channel in ('aw', 'ar', 'w')
                assert interface[f'{bus}_{channel}_bits_{leaf}'] == [
                    'output' if request == master else 'input', width]
                checks += 1
        shim = ports(module(clean(golden.read_text()), 'F1Shim')[0])
        for lane in range(2):
            for direction in ('tx', 'rx'):
                assert shim[f'io_qsfp_{direction}_{lane}_bits'] == [
                    'output' if direction == 'tx' else 'input', int(verilog['QSFP_DATA_BITS'])]
                checks += 1
        comparisons.append(dict(golden=str(golden.resolve()),
            rtl_sha256=hashlib.sha256(golden.read_bytes()).hexdigest(),
            width_direction_checks=checks))

    # Compile each selected configuration against the real driver types. This
    # also checks optional endpoints, original name and single-use macro ABI.
    include = Path(__file__).resolve().parents[3] / 'midas/src/main/cc'
    for index, path in enumerate([header, golden_header]):
        source = args.output_dir / f'abi-{index}.cpp'
        source.write_text('#include "core/config.h"\n#include <string_view>\n'
            '#define GET_METASIM_INTERFACE_CONFIG\n#include ' + json.dumps(str(path.resolve())) + '\n'
            '#ifdef GET_METASIM_INTERFACE_CONFIG\n#error macro not consumed\n#endif\n'
            'static_assert(conf_target.ctrl.id_bits == 12 && conf_target.ctrl.addr_bits == 25 && conf_target.ctrl.data_bits == 32);\n'
            'static_assert(conf_target.mem.id_bits == 16 && conf_target.mem.addr_bits == 34 && conf_target.mem.data_bits == 64);\n'
            'static_assert(conf_target.cpu_managed && conf_target.cpu_managed->id_bits == 16 && conf_target.cpu_managed->addr_bits == 64 && conf_target.cpu_managed->data_bits == 512);\n'
            'static_assert(conf_target.mem_num_channels == 1 && !conf_target.fpga_managed);\n'
            'static_assert(conf_target.qsfp.data_bits == 256 && conf_target.qsfp.num_channels == 0);\n'
            'static_assert(std::string_view(conf_target.target_name) == "FireSim");\n')
        with (args.output_dir / f'abi-{index}.log').open('w') as log:
            subprocess.run([str(args.cxx), '-std=c++20', '-fsyntax-only', '-I',
                            str(include), str(source)], stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    report = dict(candidate=str(headers.resolve()), annotation_prefix_preserved=len(before),
        native_emission_identical=True, comparisons=comparisons,
        typed_config_golden=str(golden_header.resolve()), typed_config_match=True,
        typed_config_sha256=hashlib.sha256(golden_header.read_bytes()).hexdigest(),
        verilog_macros=verilog, omitted_sfc_macro='MEM_HAS_CHANNEL-1',
        compiled_driver_ABIs=2, immutable_fixture_header_available=False,
        limitation='NIC driver constructors and final RTL/runtime validation remain pending.')
    (args.output_dir / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
