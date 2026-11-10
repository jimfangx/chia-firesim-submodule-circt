#!/usr/bin/env python3
"""Finite selected FRFCFS register-bank replay against immutable SFC snippets.

Cuts model status outputs at the widget boundary. This is not full-target
semantic parity, an AXI transport comparison, formal proof, F2 or speed evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import yaml


RECIPES = {
    'megaboomv3': 'firesim_megaboom_singlecore_no_nic_l2_llc4mb_ddr3',
    'shuttle': 'firesim_shuttle_singlecore_no_nic_l2_llc4mb_ddr3',
    'saturn_shuttle': 'firesim_saturn_GENV256D128_shuttle_singlecore_no_nic_l2_llc4mb_ddr3',
}
TARGETS = set(RECIPES)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def resources(value):
    if isinstance(value, dict):
        if {'path', 'bytes', 'sha256'} <= value.keys():
            yield value
        else:
            for child in value.values():
                yield from resources(child)
    elif isinstance(value, list):
        for child in value:
            yield from resources(child)


def one(pattern, text):
    found = list(re.finditer(pattern, text, re.M))
    assert len(found) == 1, (pattern, len(found))
    return found[0]


def identity(path):
    return {'path': str(Path(path).resolve()), 'bytes': Path(path).stat().st_size,
            'sha256': digest(path)}


def read_golden(receipt_path):
    receipt = json.loads(receipt_path.read_text())
    assert receipt['status'] == 'complete' and receipt['target'] in TARGETS
    manifest_path = Path(receipt['manifest']['path'])
    assert digest(manifest_path) == receipt['manifest']['sha256']
    manifest = json.loads(manifest_path.read_text())
    assert manifest['status'] == 'complete' and manifest['target'] == receipt['target']
    assert manifest['recipe']['PLATFORM_CONFIG'] == 'WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config'
    paths = [receipt_path, manifest_path]
    for item in resources(manifest['preMIDAS']):
        path = Path(item['path'])
        assert path.stat().st_size == item['bytes'] and digest(path) == item['sha256']
        paths.append(path)
    directory = manifest_path.parent / 'sfc'
    for name in ['FireSim-generated.sv', 'FireSim-generated.const.h']:
        path = directory / name
        assert digest(path) == manifest['sfc_artifacts'][name]['sha256']
        paths.append(path)
    rtl = (directory / 'FireSim-generated.sv').read_text()
    parent = one(r'(?s)^module FASEDMemoryTimingModel\b.*?^endmodule\b', rtl).group()
    header = (directory / 'FireSim-generated.const.h').read_text().split(
        'registry.add_widget(new FASEDMemoryTimingModel(', 1)[1].split('  0,\n  args', 1)[0]
    maps = header.split('std::vector<std::pair<std::string, uint32_t>>{')
    assert len(maps) == 3
    pairs = lambda section: [(name, int(addr)) for name, addr in re.findall(
        r'R"ESC\((.*?)\)ESC", (\d+)', section)]
    reads, writes = pairs(maps[1]), pairs(maps[2])
    assert len(reads) == 68 and [a for _, a in reads] == list(range(0, 272, 4))
    assert len(writes) == 32 and set(writes) <= set(reads)
    rows, declarations, blocks, read_assigns, config_assigns, cut_inputs = [], [], [], [], [], {}
    cr_name = one(r'\b(MCRFile\w*) crFile\s*\(', parent).group(1)
    cr_module = one(r'(?s)^module ' + cr_name + r'\b.*?^endmodule\b', rtl).group()
    for index, (name, address) in enumerate(reads):
        writable = (name, address) in writes
        row = dict(name=name, index=index, writeable=writable)
        if writable:
            declarations.append(one(r'^  reg \[31:0\] ' + name + r';[^\n]*', parent).group())
            block = one(r'    if \(reset\) begin[^\n]*\n\s*' + name +
                        r' <= [^;]+;[^\n]*\n    end else if \([^\n]*\) begin[^\n]*\n\s*' +
                        name + r' <= [^;]+;[^\n]*\n    end', parent).group()
            assert f'crFile_io_mcr_write_{index}_valid' in block
            assert f'crFile_io_mcr_write_{index}_bits' in block
            row['initial'] = int(one(name + r" <= 32'h([0-9a-f]+);", block).group(1), 16)
            blocks.append(block)
            if index == 0:
                # Widget allocates this setting but selected SFC eliminates its
                # consumer. Observe stored value without inventing a model use.
                row['width'], row['config_source'] = 32, name
                config_assigns.append(f'  assign cfg_{index} = {name};')
            else:
                destination = 'ingress_io_relaxed' if index == 65 else 'model_tNasti_io_mmReg_' + name
                assignment = one(r'^  assign ' + destination + r' = ([^;]+);[^\n]*', parent)
                row['config_source'] = assignment.group(1)
                wire = one(r'^  wire(?: \[(\d+):0\])?\s+' + destination + r';[^\n]*', parent)
                row['width'] = int(wire.group(1)) + 1 if wire.group(1) else 1
                config_assigns.append(assignment.group().replace(destination, f'cfg_{index}', 1))
        if index in (47, 52):
            # These all-overflow histogram bins are constant-zero inside the
            # optimized MCRFile decode; the parent no longer has a read wire.
            constant = one(r'^  wire \[31:0\] \w+ = 7\'h' + f'{index:x}' +
                           r" == rIndex \? 32'h0 : \w+;[^\n]*", cr_module).group()
            row['read_source'] = "32'h0"
            row['constant_decode'] = constant
            read_assigns.append(f"  assign crFile_io_mcr_read_{index}_bits = 32'h0;")
        else:
            assignment = one(r'^  assign crFile_io_mcr_read_' + str(index) +
                             r'_bits = ([^;]+);[^\n]*', parent)
            row['read_source'] = assignment.group(1)
            read_assigns.append(assignment.group())
            if not writable and index not in (66, 67):
                source = one(r'(model_tNasti_io_mmReg_\w+)', assignment.group(1)).group(1)
                wire = one(r'^  wire(?: \[(\d+):0\])?\s+' + source + r';[^\n]*', parent)
                width = int(wire.group(1)) + 1 if wire.group(1) else 1
                cut_inputs[source] = width
                row['status_source'], row['width'] = source, width
        rows.append(row)
    for name in ['rrespError', 'brespError']:
        declarations.append(one(r'^  reg \[1:0\] ' + name + r';[^\n]*', parent).group())
        blocks.append(one(r'    if \(reset\) begin[^\n]*\n\s*' + name +
                          r' <= [^;]+;[^\n]*\n    end else if \([^\n]*\) begin[^\n]*\n\s*' +
                          name + r' <= [^;]+;[^\n]*\n    end', parent).group())
    ports = ['input clock', 'input reset', 'input auto_to_host_dram_out_r_valid',
             'input auto_to_host_dram_out_b_valid', 'input [1:0] auto_to_host_dram_out_r_bits_resp',
             'input [1:0] auto_to_host_dram_out_b_bits_resp']
    ports += [f'input [{width-1}:0] {name}' for name, width in cut_inputs.items()]
    for row in rows:
        i = row['index']
        ports.append(f'output [31:0] crFile_io_mcr_read_{i}_bits')
        if row['writeable']:
            ports += [f'input crFile_io_mcr_write_{i}_valid', f'input [31:0] crFile_io_mcr_write_{i}_bits',
                      f'output [{row["width"]-1}:0] cfg_{i}']
    cone = ('module SFCFRFCFSMMIO(\n  ' + ',\n  '.join(ports) + '\n);\n' +
            '\n'.join(declarations + read_assigns + config_assigns) +
            '\n  always @(posedge clock) begin\n' + '\n'.join(blocks) + '\n  end\nendmodule\n')
    info = dict(target=receipt['target'], recipe=manifest['recipe'], recipe_name=manifest['recipe_name'],
                identities=[identity(p) for p in dict.fromkeys(paths)], catalogue=rows,
                cone_sha256=hashlib.sha256(cone.encode()).hexdigest(), cut_inputs=cut_inputs)
    return info, cone


def testbench(rows, native_rows):
    native_by_index = {r['index']: r for r in native_rows}
    assert len(native_by_index) == 68
    lines = ['module tb;', 'logic clock=0,reset=1;', 'logic rValid=0,bValid=0;',
             'logic [1:0] rResp=0,bResp=0;', 'logic [3:0] wstrb=0;',
             'integer cycles=0,resets=0,writes=0,zeroStrobes=0,partialStrobes=0,fullStrobes=0;',
             'integer readStalls=0,errorEvents=0,bErrorDifferent=0;',
             'integer i,j;', 'integer writeCoverage[68];', 'integer strobeCoverage[16];']
    nc = ['.clock(clock)', '.reset(reset)', '.rValid(rValid)', '.rReady(1\'b1)',
          '.bValid(bValid)', '.bReady(1\'b1)', '.rResp(rResp)', '.bResp(bResp)', '.mcr_wstrb(wstrb)']
    sc = ['.clock(clock)', '.reset(reset)', '.auto_to_host_dram_out_r_valid(rValid)',
          '.auto_to_host_dram_out_b_valid(bValid)', '.auto_to_host_dram_out_r_bits_resp(rResp)',
          '.auto_to_host_dram_out_b_bits_resp(bResp)']
    for row in rows:
        i = row['index']; native = native_by_index[i]
        assert native['name'] == row['name'] and native['writeable'] == row['writeable']
        if row['writeable']:
            assert native['width'] == row['width'] and native['initial'] == row['initial']
        lines += [f'logic rdReady_{i}=0,wrValid_{i}=0;logic [31:0] wrBits_{i}=0;',
                  f'wire [31:0] nRead_{i},sRead_{i};wire nReadValid_{i},nWriteReady_{i};']
        nc += [f'.mcr_read_{i}_ready(rdReady_{i})', f'.mcr_read_{i}_valid(nReadValid_{i})',
               f'.mcr_read_{i}_bits(nRead_{i})', f'.mcr_write_{i}_valid(wrValid_{i})',
               f'.mcr_write_{i}_bits(wrBits_{i})', f'.mcr_write_{i}_ready(nWriteReady_{i})']
        sc.append(f'.crFile_io_mcr_read_{i}_bits(sRead_{i})')
        if row['writeable']:
            w = row['width']
            lines.append(f'wire [{w-1}:0] nConfig_{i},sConfig_{i};')
            nc.append(f'.{native["native_port"]}(nConfig_{i})')
            sc += [f'.crFile_io_mcr_write_{i}_valid(wrValid_{i})', f'.crFile_io_mcr_write_{i}_bits(wrBits_{i})',
                   f'.cfg_{i}(sConfig_{i})']
        elif i not in (66, 67):
            w = row.get('width', 32)
            lines.append(f'logic [{w-1}:0] status_{i}=0;')
            nc.append(f'.{native["native_port"]}(status_{i})')
            if i not in (47, 52):
                sc.append(f'.{row["status_source"]}(status_{i})')
    lines += ['GGFASEDFRFCFSMMIO nativeBank(' + ','.join(nc) + ');',
              'SFCFRFCFSMMIO sfcBank(' + ','.join(sc) + ');',
              'task compare;begin']
    for row in rows:
        i = row['index']
        lines += [f'if(nRead_{i}!==sRead_{i})$fatal(1,"read mismatch word={i} cycle=%0d native=%h sfc=%h",cycles,nRead_{i},sRead_{i});',
                  f'if(nReadValid_{i}!==1 || nWriteReady_{i}!==1)$fatal(1,"handshake word={i}");']
        if row['writeable']:
            lines.append(f'if(nConfig_{i}!==sConfig_{i})$fatal(1,"config mismatch word={i}");')
    lines += ['end endtask', 'task step;begin #2;compare();', 'strobeCoverage[wstrb]++;',
              'if(reset)resets++;if(wstrb==0)zeroStrobes++;else if(wstrb==15)fullStrobes++;else partialStrobes++;',
              'if(rValid&&rResp!=0 || bValid&&bResp!=0)errorEvents++;',
              'if(bValid&&bResp!=0&&rResp!=bResp)bErrorDifferent++;']
    for row in rows:
        i = row['index']
        lines.append(f'if(!rdReady_{i})readStalls++;')
        if row['writeable']:
            lines.append(f'if(wrValid_{i})begin writes++;writeCoverage[{i}]++;end')
    lines += ['clock=1;#2;compare();clock=0;#2;cycles++;end endtask',
              'task driveInputs;begin', 'rValid=1\'($urandom);bValid=1\'($urandom);rResp=2\'($urandom);bResp=2\'($urandom);wstrb=4\'($urandom);']
    for row in rows:
        i = row['index']
        lines.append(f'rdReady_{i}=1\'($urandom);')
        if row['writeable']:
            lines += [f'wrValid_{i}=($urandom_range(0,7)==0);wrBits_{i}=$urandom;']
        elif i not in (66, 67, 47, 52):
            lines.append(f'status_{i}={row["width"]}\'($urandom);')
    lines += ['end endtask', 'initial begin', 'for(i=0;i<68;i++)writeCoverage[i]=0;',
              'for(i=0;i<16;i++)strobeCoverage[i]=0;', 'step();reset=0;',
              # All words written together proves independent enable and full
              # storage/readback, including high bits of narrow consumers.
              'for(j=0;j<16;j++)begin wstrb=4\'(j);']
    for row in rows:
        if row['writeable']:
            i = row['index']
            lines.append(f'wrValid_{i}=1;wrBits_{i}=32\'hffff0000+{i}+j;')
    lines += ['step();end', 'for(i=0;i<25000;i++)begin driveInputs();reset=($urandom_range(0,255)==0);step();end']
    for row in rows:
        if row['writeable']:
            lines.append(f'if(writeCoverage[{row["index"]}]<100)$fatal(1,"missing write coverage");')
    lines += ['for(i=0;i<16;i++)if(strobeCoverage[i]<5)$fatal(1,"missing strobe coverage");',
              'if(resets<20||readStalls<1000||zeroStrobes<5||partialStrobes<20||fullStrobes<5||errorEvents<100||bErrorDifferent<100)$fatal(1,"missing coverage");',
              '$display("PASS FRFCFS MMIO cycles=%0d fields=236 resets=%0d writes=%0d zeroStrobes=%0d partialStrobes=%0d fullStrobes=%0d readStalls=%0d errorEvents=%0d bErrorDifferent=%0d",cycles,resets,writes,zeroStrobes,partialStrobes,fullStrobes,readStalls,errorEvents,bErrorDifferent);',
              '$finish;end endmodule']
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for option in ['fixture', 'firtool', 'verilator', 'output-dir']:
        p.add_argument('--' + option, type=Path, required=True)
    p.add_argument('--golden-receipt', type=Path, action='append', required=True)
    p.add_argument('--jobs', type=int, default=1, choices=range(1, 5))
    a = p.parse_args()
    d = a.output_dir.resolve(); d.mkdir(parents=True, exist_ok=False)
    base = Path(__file__).resolve().parents[2]
    root = base.parents[3]
    selected_path = root / 'automation/chia_firesim_circt/selected_config_build_recipes.yaml'
    sample_path = root / 'sims/firesim-staging/sample_config_build_recipes.yaml'
    selected = yaml.safe_load(selected_path.read_text())
    sample = yaml.safe_load(sample_path.read_text())
    for target, name in RECIPES.items():
        if target == 'shuttle':
            derived = dict(sample[RECIPES['megaboomv3']])
            derived['TARGET_CONFIG'] = 'WithDefaultFireSimBridges_WithFireSimTestChipConfigTweaks_chipyard.ShuttleConfig'
            assert selected[name] == derived
        else:
            assert selected[name] == sample[name]
    source_files = [Path(__file__), selected_path, sample_path, base / 'lib/Transforms/FASEDFRFCFSMMIO.cpp',
                    base / 'include/goldengate/FASEDFRFCFSMMIO.h',
                    base / 'test/Transforms/FASEDFRFCFSMMIOTest.cpp']
    oracle = base.parent / 'midas/src/main/scala/midas'
    source_files += [oracle / p for p in ['widgets/Widget.scala', 'widgets/Lib.scala',
                                        'models/dram/FASEDMemoryTimingModel.scala',
                                        'models/dram/FRFCFSModel.scala']]
    report = dict(passed=False, timing_scope=None, component='FRFCFS MMIO register bank',
                  scope='Literal selected SFC parent register declaration/reset/write/read/truncation snippets. Model status values are independently generated at a cut; their counters/timing are excluded. Histogram overflow bins47/52 are constrained zero, matching literal optimized MCRFile decode. R/Bready remain true as in selected SFC parent. B error deliberately latches R response on nonzero B response. Full32bit storage/readback versus truncated model configuration is checked. Config/error state uses host reset and host clock; targetFire/targetReset are outside this bank and do not gate its host writes. MCR read.valid/write.ready constanttrue follows Scala bindReg; byte strobes are ignored. Readonly writes are excluded as illegal. No AXI transport, timing/functional composition, target parity, formal proof, F2 runtime or compiler speed claim.',
                  sources=[identity(path) for path in source_files],
                  tools=[identity(path) for path in [a.fixture, a.firtool, a.verilator]],
                  versions={str(path.resolve()): subprocess.check_output([str(path.resolve()), '--version'], text=True).strip()
                            for path in [a.firtool, a.verilator]}, targets=[], cones=[])
    try:
        fixture_cmd = [str(a.fixture.resolve()), '--fixture', str(d / 'native.mlir')]
        catalogue_cmd = [str(a.fixture.resolve()), '--catalogue', str(d / 'catalogue.json')]
        commands = [fixture_cmd, catalogue_cmd,
                    [str(a.firtool.resolve()), str(d / 'native.mlir'), '--format=mlir', '--verilog', '-o', str(d / 'native.sv')]]
        report['setup_commands'] = commands
        for i, command in enumerate(commands):
            with (d / f'setup-{i}.log').open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
        native_rows = json.loads((d / 'catalogue.json').read_text())
        seen = {}
        for receipt in a.golden_receipt:
            info, cone = read_golden(receipt)
            assert info['recipe_name'] == RECIPES[info['target']]
            assert info['recipe'] == selected[info['recipe_name']]
            report['targets'].append(info)
            key = info['cone_sha256']
            if key in seen:
                info['evidence'] = seen[key]
                continue
            work = d / key[:12]; work.mkdir()
            (work / 'sfc.sv').write_text(cone)
            (work / 'tb.sv').write_text(testbench(info['catalogue'], native_rows))
            commands = [[str(a.verilator.resolve()), '--binary', '--timing', '-DSYNTHESIS', '--top-module', 'tb',
                         '-Wno-fatal', '-j', str(a.jobs), '--Mdir', str(work / 'obj'), str(d / 'native.sv'),
                         str(work / 'sfc.sv'), str(work / 'tb.sv')], [str(work / 'obj/Vtb')]]
            entry = dict(cone_sha256=key, commands=commands, passed=False)
            report['cones'].append(entry)
            info['evidence'] = seen[key] = str(work / 'stage-1.log')
            for i, command in enumerate(commands):
                with (work / f'stage-{i}.log').open('w') as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
            entry['evidence'] = (work / 'stage-1.log').read_text()
            assert 'PASS FRFCFS MMIO' in entry['evidence']
            entry.update(passed=True, artifacts=[identity(f) for f in work.iterdir() if f.is_file()] +
                         [identity(work / 'obj/Vtb')])
        assert len(report['targets']) == 3 and {i['target'] for i in report['targets']} == TARGETS
        for group in [report['sources'], report['tools']] + [i['identities'] for i in report['targets']]:
            assert all(identity(Path(i['path'])) == i for i in group), 'input changed during comparison'
        report['artifacts'] = [identity(f) for f in d.iterdir() if f.is_file() and f.name != 'result.json']
        report['passed'] = True
    finally:
        (d / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('\n'.join(i['evidence'] for i in report['cones']))


if __name__ == '__main__':
    main()
