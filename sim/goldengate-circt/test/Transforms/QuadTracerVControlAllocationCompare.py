#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare native quad Rocket allocation and isolated AW/AR decoding with archived SFC.

This exercises the decoder only; queued routing and full quad platform/runtime
integration are still unsupported. Run in the sourced FireSim environment.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import resource
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module
from PrintRocketControlMasterCompare import native_ports


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('before', 'decoder', 'golden', 'export-tool', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    before, after = modules(a.before.read_text()), modules(a.decoder.read_text())
    assert set(after) - set(before) == {'GGControlAddressDecode', 'GGControlDecodeWrapper'}
    assert all(after[n] == body for n, body in before.items()), 'prior module changed'
    helper = after['GGControlAddressDecode']
    rows = re.findall(r'\{name = "([^"]+)", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64\}', helper)
    assert len(rows) == 14, 'missing allocation metadata'
    regions = [dict(name=n, size=int(size), slave=int(i), start=int(start)) for n,size,i,start in rows]
    golden = clean(a.golden.read_text()); _, router = module(golden, 'NastiRouter')
    expressions, reference = {}, []
    for ch in ('aw', 'ar'):
        wires = re.findall(r'wire\s+(_'+ch+r'_route_T_\d+)\s*=\s*([^;]+);', router)
        assert len(wires) == 14
        expressions[ch] = wires
        for i, (name, expr) in enumerate(wires):
            # Retain the exact SFC expression in the isolated SV reference.
            values = [int(x,16) for x in re.findall(r"25'h([0-9a-f]+)", expr)]
            start, end = (0, values[0]) if i == 0 else values
            assert (regions[i]['start'], regions[i]['size'], regions[i]['slave']) == (start,end-start,i)
            reference.append('wire '+name+' = '+expr+';')
        concatenations = re.findall(r'wire\s+(?:\[\d+:\d+\]\s+)?'+ch+r'_route(?:_lo|_hi)?\s*=\s*[^;]+;', router)
        assert len(concatenations) == (3 if ch == 'aw' else 2)
        reference.extend(concatenations)
        reference.append('assign '+ch+'_sel = '+ch+'_route;')
        reference.append('assign '+ch+'_error = ~(|'+ch+'_sel);')
    header = a.golden.with_name('FireSim-generated.const.h'); hs = header.read_text()
    lookup = {r['name']:r for r in regions}
    names = {'SIMULATIONMASTER':'SimulationMaster', 'PEEKPOKEBRIDGEMODULE':'PeekPokeBridgeModule',
             'RESETPULSEBRIDGEMODULE':'ResetPulseBridgeModule', 'BLOCKDEVBRIDGEMODULE':'BlockDevBridgeModule',
             'UARTBRIDGEMODULE':'UARTBridgeModule', 'SIMPLENICBRIDGEMODULE':'SimpleNICBridgeModule',
             'TRACERVBRIDGEMODULE':'TracerVBridgeModule', 'TSIBRIDGEMODULE':'TSIBridgeModule',
             'CLOCKBRIDGEMODULE':'ClockBridgeModule', 'LOADMEMWIDGET':'LoadMemWidget'}
    checked = 0
    slots = {}
    for prefix, fields in re.findall(r'(\w+)_struct\{(.*?)\}', hs, re.S):
        slot = slots.get(prefix, 0); slots[prefix] = slot + 1
        r = lookup[names[prefix]+'_'+str(slot)]
        for field, addr in re.findall(r'\.(\w+) = (\d+)', fields):
            assert r['start'] <= int(addr) < r['start']+r['size']; checked += 1
    tracer_addresses = []
    for i, fields in enumerate(re.findall(r'TRACERVBRIDGEMODULE_struct\{(.*?)\}', hs, re.S)):
        addresses = [int(x) for x in re.findall(r'= (\d+)', fields)]
        assert addresses == [lookup['TracerVBridgeModule_'+str(i)]['start']+j*4 for j in range(15)]
        tracer_addresses.append(addresses)
    assert len(tracer_addresses) == 4
    counts = re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = true, writeable = false\}', before['GGCPUStreamCountBank'])
    assert len(counts) == 4
    count_addresses = []
    driver = hs.split('registry.add_widget(new CPUManagedStreamWidget(')[1].split('#endif')[0]
    for n, offset in counts:
        address = lookup['CPUManagedStreamEngine_0']['start']+int(offset)
        assert re.search(re.escape(n.removesuffix('_count'))+r'\)ESC"\),\s*\d+ULL,\s*'+str(address)+r'ULL', driver)
        count_addresses.append(address)
    old = 'GGControlErrorWrapper'; new = 'GGControlDecodeWrapper'
    def transfer(v):
        if isinstance(v,str):
            if v == '~'+old: return '~'+new
            if v.startswith('~'+old+'|'):
                v = '~'+new+v[len(old)+1:]
                if v.startswith('~'+new+'|'+old+'>'): v = v.replace('|'+old+'>','|'+new+'>',1)
            return v
        if isinstance(v,list): return [transfer(x) for x in v]
        if isinstance(v,dict): return {k:transfer(x) for k,x in v.items()}
        return v
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    assert annos(a.decoder) == transfer(annos(a.before)), 'target transfer differs'
    copied = native_ports(before[old]); added = native_ports(helper)
    assert native_ports(after[new]) == copied+[(d,'ctrl_decode_'+n,t) for d,n,t in added]
    run([a.export_tool,a.before,out/'decoder.mlir','--quad'],'production-test')
    assert modules((out/'decoder.mlir').read_text())['GGControlAddressDecode'] == helper
    run(['firtool',out/'decoder.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')
    (out/'sfc.sv').write_text('module SFCDecode(input [24:0] io_master_aw_bits_addr,io_master_ar_bits_addr, output [13:0] aw_sel,ar_sel, output aw_error,ar_error);\n'+'\n'.join(reference)+'\nendmodule\n')
    (out/'check.sv').write_text(r'''module check;
reg [24:0] aw_addr,ar_addr;
wire [13:0] aw_sel,ar_sel,s_aw_sel,s_ar_sel;
wire [3:0] aw_index,ar_index;
wire aw_error,ar_error,s_aw_error,s_ar_error;
GGControlAddressDecode n(.aw_addr(aw_addr),.ar_addr(ar_addr),.aw_route(aw_sel),.ar_route(ar_sel),.aw_target(aw_index),.ar_target(ar_index),.aw_error(aw_error),.ar_error(ar_error));
SFCDecode s(aw_addr,ar_addr,s_aw_sel,s_ar_sel,s_aw_error,s_ar_error);
integer pairs=0;
reg [63:0] random_state=64'hd7319507;
function automatic [24:0] randaddr();
 random_state=random_state^(random_state<<13);
 random_state=random_state^(random_state>>7);
 random_state=random_state^(random_state<<17);
 return random_state[24:0];
endfunction
task automatic sample(input [24:0] aw,ar);
 integer ai,ri;
 aw_addr=aw; ar_addr=ar; #1;
 ai=14;ri=14;
 for(integer i=0;i<14;i=i+1)begin
  if(s_aw_sel[i])ai=i;
  if(s_ar_sel[i])ri=i;
 end
 if(aw_sel!==s_aw_sel || ar_sel!==s_ar_sel || aw_error!==s_aw_error || ar_error!==s_ar_error || aw_index!==ai[3:0] || ar_index!==ri[3:0])
  $fatal(1,"decode mismatch AW=%h AR=%h",aw,ar);
 pairs++;
endtask
initial begin
 for(integer i=0;i<1024;i++)for(integer j=0;j<1024;j++)sample(i,j);
 for(integer i=0;i<100000;i++)sample(randaddr(),randaddr());
 sample(25'h1ffffff,25'h1ffffff);sample(25'h268,25'h267);
 $display("PASS quad SFC/native address decoding: %0d AW/AR pairs",pairs);$finish;
end
endmodule
''')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    # Reject a shifted fourth TracerV region even though all types still match.
    native = (out/'native.sv').read_text()
    mutant, changes = re.subn(r"(\d+)'h1[cC]0", r"\1'h1c4", native)
    assert changes, 'missing fourth TracerV lower bound in lowered decoder'
    (out/'mutant.sv').write_text(mutant)
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'mutant-obj','-o','check',out/'check.sv',out/'mutant.sv',out/'sfc.sv'],'mutant-verilator')
    with (out/'mutant-simulation.log').open('w') as log:
        result = subprocess.run([out/'mutant-obj/check'],stdout=log,stderr=subprocess.STDOUT)
    assert result.returncode != 0 and 'decode mismatch' in (out/'mutant-simulation.log').read_text()

    def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
    report=dict(golden=str(a.golden),golden_sha256=digest(a.golden),header=str(header),header_sha256=digest(header),
                candidate=str(a.decoder),candidate_sha256=digest(a.decoder),regions=regions,
                preserved_module_bodies=len(before),copied_ports=len(copied),annotations=len(annos(a.decoder)),
                driver_addresses_within_regions=checked,tracer_addresses=tracer_addresses,count_addresses=count_addresses,
                native_test=(out/'production-test.log').read_text(),simulation=(out/'simulation.log').read_text(),limitation=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])

if __name__ == '__main__': main()
