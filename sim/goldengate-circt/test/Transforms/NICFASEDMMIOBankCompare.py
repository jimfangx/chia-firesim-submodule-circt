#!/usr/bin/env python3
# See LICENSE for license details.
"""Check production FASED lane assembly against the archived SFC MMIO ABI.

Request capacity changes producer occupancy widths but not this UInt32 ABI.
Execute the actual cloned CIRCT routing with independent fragment/aggregate
pins. This proves the combinational carrier, not the register state machines,
Nasti decoder, NIC execution or platform integration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, equations, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDMMIOBankCompare import FRAGMENTS, NAMES, wiring

OLD, NEW = 'GGFASEDHistogramsWrapper', 'GGFASEDMMIOWrapper'


def oracle(golden, candidate, header=None):
    reference = clean(golden.read_text())
    host = module(reference, 'FASEDMemoryTimingModel')[1]
    kind, = re.findall(r'\b(MCRFile\w*) crFile', host)
    body = module(reference, kind)[1]
    eq, mcr = equations(host), equations(body)
    assert 'rIndex <= io_nasti_ar_bits_addr[6:2];' in body
    for i, name in enumerate(NAMES):
        if i in (8, 13):
            assert any(f"5'h{i:x} == rIndex ? 32'h0 : " in v for v in mcr.values())
        else:
            expected = ("{{30'd0}, " + name + '}') if i >= 19 else (
                'model_tNasti_io_mmReg_' + name if 4 <= i < 18 else name)
            assert eq[f'crFile_io_mcr_read_{i}_bits'] == expected, (i, name)
        if i < 4 or i == 18:
            assert f'end else if (crFile_io_mcr_write_{i}_valid) begin' in host
            assert f'{name} <= crFile_io_mcr_write_{i}_bits;' in host
        else:
            assert 'Register ' + name + ' is read only' in host
            assert f'if (~reset & ~(~crFile_io_mcr_write_{i}_valid)) begin' in host

    header = header or golden.with_suffix('.const.h')
    widget, = re.findall(r'registry.add_widget\(new FASEDMemoryTimingModel\((.*?)\n  0,', header.read_text(), re.S)
    tables = re.findall(r'std::vector<std::pair<std::string, uint32_t>>\{(.*?)\n    }', widget, re.S)
    assert len(tables) == 2
    addresses = [dict((n, int(a)) for n, a in re.findall(r'R"ESC\((\w+)\)ESC", (\d+)', t)) for t in tables]
    assert list(addresses[0]) == NAMES and list(addresses[1]) == [NAMES[i] for i in (0, 1, 2, 3, 18)]
    base = addresses[0][NAMES[0]]
    assert addresses == [{n: base+4*i for i, n in enumerate(NAMES)},
                         {NAMES[i]: base+4*i for i in (0, 1, 2, 3, 18)}]
    regions = re.findall(r'\{name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64}', candidate.read_text())
    assert regions and len(set(regions)) == 1
    size, slave, start = map(int, regions[0])
    assert start == base and size >= 84
    return dict(golden=str(golden.resolve()), sha256=hashlib.sha256(golden.read_bytes()).hexdigest(),
                SFC_modules=['FASEDMemoryTimingModel', kind], header=str(header.resolve()),
                header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(), base=base, size=size, slave=slave,
                registers=[dict(name=n, offset=4*i, address=base+4*i, readable=True, writeable=i<4 or i==18)
                           for i, n in enumerate(NAMES)])


def composition(directory):
    prior = modules((directory/'post-fame-fased-histograms.mlir').read_text())
    text = (directory/'post-fame-fased-mmio.mlir').read_text()
    after = modules(text)
    assert set(after)-set(prior) == {NEW} and set(prior) <= set(after)
    assert all(after[n] == body for n, body in prior.items()), 'existing module changed'
    copied = [p for p in native_ports(prior[OLD]) if p[1] not in {f for f, _, _ in FRAGMENTS}]
    added = native_ports(after[NEW])
    assert added[:-1] == copied and added[-1][:2] == ('out', 'fasedBridge_mcr')
    token = 'vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 21>'
    assert added[-1][2] == '!firrtl.bundle<read: '+token+', write flip: '+token+', wstrb flip: uint<4>>'
    rows = re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = (true|false)', after[NEW].splitlines()[0])
    assert rows == [(n, str(4*i), str(i<4 or i==18).lower()) for i, n in enumerate(NAMES)]
    nets = {('sim.' if d=='in' else 'top.')+n: ('top.' if d=='in' else 'sim.')+n for d, n, _ in copied}
    for fragment, start, count in FRAGMENTS:
        nets['sim.'+fragment+'.wstrb'] = 'top.fasedBridge_mcr.wstrb'
        for k in range(count):
            nets[f'top.fasedBridge_mcr.read[{start+k}]'] = f'sim.{fragment}.read[{k}]'
            nets[f'sim.{fragment}.write[{k}]'] = f'top.fasedBridge_mcr.write[{start+k}]'
    assert wiring(after[NEW]) == nets, 'fragment/aggregate routing differs'
    assert not re.search(r'firrtl.(?:reg|regreset|mem)\b', after[NEW]), 'assembly introduced state'

    def transfer(v):
        if isinstance(v, dict): return {k: transfer(x) for k, x in v.items()}
        if isinstance(v, list): return [transfer(x) for x in v]
        if not isinstance(v, str): return v
        if v == '~'+OLD: return '~'+NEW
        if not v.startswith('~'+OLD+'|'): return v
        suffix = v[len('~'+OLD):]
        prefix = '|'+OLD+'>'
        if suffix.startswith(prefix):
            ref = suffix[len(prefix):]
            local = re.split(r'[.\[]', ref)[0]
            if local in {n for _, n, _ in copied}: suffix = '|'+NEW+'>'+ref
            for fragment, start, count in FRAGMENTS:
                if local != fragment: continue
                tail = ref[len(local):]
                if tail == '.wstrb': suffix = '|'+NEW+'>fasedBridge_mcr.wstrb'
                elif lane := re.fullmatch(r'\.(read|write)\[(\d+)\](.*)', tail):
                    group, index, rest = lane.groups()
                    if int(index)<count: suffix = f'|{NEW}>fasedBridge_mcr.{group}[{start+int(index)}]{rest}'
        return '~'+NEW+suffix
    old = json.loads((directory/'post-fame-fased-histograms-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-mmio-all.json').read_text())
    assert new == transfer(old), 'annotation transfer differs'
    return dict(unchanged_modules=len(prior), copied_typed_ports=len(copied),
                connections=len(nets), aggregate_routes=48, annotation_records=len(new))


def execute(candidate, export_tool, out):
    def run(cmd, name, check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=check,
                                  preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    exported, rtl = out/'routing.mlir', out/'routing.sv'
    run([export_tool, candidate, exported], 'export')
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', rtl], 'firtool')
    probe = modules(exported.read_text().replace('firrtl.module public @', 'firrtl.module @'))['GGFASEDMMIOProbe']
    # The clone contains every original routing operation, with only boundary
    # values renamed; the compiler output remains the source of the behavior.
    assert sum('firrtl.'+kind+' ' in line for line in probe.splitlines()
               for kind in ('connect', 'strictconnect')) == 48
    pins = []
    tb = '''module check;
reg [31:0] rd[0:20],wd[0:20];
reg [20:0] rv,wr,wv,rr;
reg [3:0] strb;
wire [31:0] rout[0:20],wout[0:20];
wire [20:0] rvout,wrout,wvout,rrout;
wire [3:0] sout[0:5];
reg [1431:0] stimulus;
integer i,j,samples=0;
'''
    for f, (fragment, start, count) in enumerate(FRAGMENTS):
        pins += [f'.{fragment}_wstrb(sout[{f}])']
        for k in range(count):
            i = start+k
            for suffix, signal in [('read_bits','rd'), ('read_valid','rv'), ('read_ready','rrout'),
                                   ('write_bits','wout'), ('write_valid','wvout'), ('write_ready','wr')]:
                group, leaf = suffix.split('_')
                pins += [f'.{fragment}_{group}_{k}_{leaf}({signal}[{i}])']
    pins += ['.fasedBridge_mcr_wstrb(strb)']
    for i in range(21):
        for suffix, signal in [('read_bits','rout'), ('read_valid','rvout'), ('read_ready','rr'),
                               ('write_bits','wd'), ('write_valid','wv'), ('write_ready','wrout')]:
            group, leaf = suffix.split('_')
            pins += [f'.fasedBridge_mcr_{group}_{i}_{leaf}({signal}[{i}])']
    tb += 'GGFASEDMMIOProbe candidate('+','.join(pins)+');\n'
    tb += '''task sample;
begin
  for(integer k=0;k<21;k=k+1) begin
    rd[k]=stimulus[68*k+:32];wd[k]=stimulus[68*k+32+:32];
    rv[k]=stimulus[68*k+64];wr[k]=stimulus[68*k+65];
    wv[k]=stimulus[68*k+66];rr[k]=stimulus[68*k+67];
  end
  strb=stimulus[1428+:4];#1;
  for(integer k=0;k<21;k=k+1) begin
    if(rout[k]!==rd[k] || wout[k]!==wd[k] || rvout[k]!==rv[k] ||
       wrout[k]!==wr[k] || wvout[k]!==wv[k] || rrout[k]!==rr[k])
      $fatal(1,"MMIO route word %0d sample %0d",k,samples);
  end
  for(integer k=0;k<6;k=k+1) if(sout[k]!==strb) $fatal(1,"strobe route %0d",k);
  samples=samples+1;
end
endtask
initial begin
  stimulus='0;sample;stimulus='1;sample;
  for(i=0;i<1432;i=i+1) begin
    stimulus='0;stimulus[i]=1;sample;
    stimulus='1;stimulus[i]=0;sample;
  end
  for(i=0;i<8192;i=i+1) begin
    for(j=0;j<1432;j=j+1) stimulus[j]=$urandom_range(0,1);
    sample;
  end
  $display("PASS production FASED MMIO: %0d samples, 1432 independent input bits, all 21 words and six strobes",samples);
  $finish;
end
endmodule
'''
    test = out/'check.sv'; test.write_text(tb)
    obj = out/'obj'
    run(['verilator', '--binary', '--timing', '--top-module', 'check', '-Wno-fatal',
         '--Mdir', obj, '-j', '2', rtl, test], 'verilator-build')
    run([obj/'Vcheck'], 'verilator-run')
    assert '11058 samples' in (out/'verilator-run.log').read_text()
    # A valid FIRRTL mutation swaps words zero/one in both aggregate vectors.
    # Export/lowering still succeed; the independent bit walk must fail.
    swapped, changes = re.subn(
        r'(firrtl.subindex %\w+\[)([01])(\] : !firrtl.vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 21>)',
        lambda m: m[1]+str(1-int(m[2]))+m[3], exported.read_text())
    assert changes == 4
    mutation = out/'routing-swapped.mlir'; mutation.write_text(swapped)
    mutated_rtl = out/'routing-swapped.sv'
    run(['firtool', mutation, '--format=mlir', '--disable-all-randomization', '-o', mutated_rtl], 'mutation-firtool')
    mutated_obj = out/'mutated-obj'
    run(['verilator', '--binary', '--timing', '--top-module', 'check', '-Wno-fatal',
         '--Mdir', mutated_obj, '-j', '2', mutated_rtl, test], 'mutation-build')
    assert run([mutated_obj/'Vcheck'], 'mutation-run', False).returncode != 0
    assert 'MMIO route word 0 sample 2' in (out/'mutation-run.log').read_text()
    return dict(samples=11058, independently_toggled_input_bits=1432, words=21, strobe_routes=6,
                valid_IR_swapped_lane_mutation_detected=True,
                clone=str(exported), lowered_rtl=str(rtl), execution_log=str(out/'verilator-run.log'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'golden', 'export-tool', 'output-dir'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--header', type=Path, help='explicit SFC driver header when archived separately from RTL')
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    candidate = a.candidate_dir/'post-fame-fased-mmio.mlir'
    report = dict(oracle=oracle(a.golden, candidate, a.header), composition=composition(a.candidate_dir),
                  execution=execute(candidate, a.export_tool, out),
                  scope='decoded combinational MMIO lane assembly; control transport and NIC runtime remain pending')
    (out/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print('PASS SFC 21-word map/driver ABI, typed production routing, annotation transfer and 11058 routing samples')


if __name__ == '__main__': main()
