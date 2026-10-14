#!/usr/bin/env python3
"""Compare the production NIC occupancy Nasti boundary with archived SFC ops.

Queue counts are isolated inputs; this proves local count/control behavior,
not global allocation, queue integration, driver execution or platform parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring


def transfer(v):
    old, new = 'GGNICBridgeControlWrapper', 'GGNICCPUStreamControlWrapper'
    if isinstance(v, str):
        if v == '~'+old: return '~'+new
        if not v.startswith('~'+old+'|'): return v
        suffix=v[len(old)+1:]
        if suffix.startswith('|'+old+'>'): suffix='|'+new+'>'+suffix[len(old)+2:]
        return '~'+new+suffix
    if isinstance(v,list): return [transfer(x) for x in v]
    if isinstance(v,dict): return {k:transfer(x) for k,x in v.items()}
    return v


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('candidate','control-candidate','golden','compose-tool','output-dir'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args(); out=a.output_dir.resolve(); out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    run([a.compose_tool,a.candidate,out/'attached.mlir',out/'control.mlir'],'compose')
    before=modules(a.candidate.read_text());after=modules(a.control_candidate.read_text())
    assert after==modules((out/'attached.mlir').read_text()),'composition differs from production'
    assert set(after)-set(before)=={'GGCPUStreamMCRFile','GGNICCPUStreamControlWrapper'}
    for n,b in before.items(): assert after[n]==b,'prior module changed: '+n
    top=after['GGNICCPUStreamControlWrapper']; prior=native_ports(before['GGNICBridgeControlWrapper'])
    current=native_ports(top)
    expected={('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in prior if n!='cpuStream_mcr'}
    expected.update({'crFile.clock':'top.hostClock','crFile.reset':'top.hostReset',
                     'crFile.nasti':'top.cpuStream_ctrl','crFile.mcr':'sim.cpuStream_mcr'})
    assert wiring(top)==expected,'Nasti/copy routes differ'
    assert current==[('in' if n=='cpuStream_mcr' else d,'cpuStream_ctrl' if n=='cpuStream_mcr' else n,t if n!='cpuStream_mcr' else current[i][2]) for i,(d,n,t) in enumerate(prior)]
    def ann(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    assert ann(a.control_candidate)==transfer(ann(a.candidate)),'annotation transfer differs'
    stand=modules((out/'control.mlir').read_text())
    for n in ('GGCPUStreamCountBank','GGCPUStreamMCRFile'): assert stand[n]==after[n]
    golden=a.golden.read_text(); mh,mb=module(clean(golden),'MCRFile');mp=ports(mh)
    _,engine=module(clean(golden),'CPUManagedStreamEngine')
    assert 'MCRFile crFile (' in engine
    names=['SIMPLENICBRIDGEMODULE_0_to_cpu_stream','TRACERVBRIDGEMODULE_0_to_cpu_stream','SIMPLENICBRIDGEMODULE_0_from_cpu_stream']
    counts=['outgoingQueueIO_q_io_count','outgoingQueueIO_q_io_count','incomingQueueIO_q_io_count']
    widths=[12,13,12]
    for i,(n,q,w) in enumerate(zip(names,counts,widths)):
        assert re.search(r'assign crFile_io_mcr_read_'+str(i)+r'_bits = \{\{'+str(32-w)+r"'d0\}, "+n+'_'+q+r'\};',engine)
        assert 'assert(~crFile_io_mcr_write_'+str(i)+'_valid);' in engine
        assert 'name = "'+n+'_count"' in after['GGCPUStreamCountBank']
        assert re.search(r'offset = '+str(4*i)+r' : i32',after['GGCPUStreamCountBank'])
    registry=re.search(r'goldengate.mmioRegisters = \[(.*?)\]',after['GGCPUStreamCountBank'],re.S).group(1)
    rows=re.findall(r'\{name = "([^"]+)", offset = (\d+) : i32, readable = (\w+), writeable = (\w+)\}',registry)
    assert rows==[(n+'_count',str(4*i),'true','false') for i,n in enumerate(names)]
    header=a.golden.with_name('FireSim-generated.const.h')
    driver=header.read_text().split('registry.add_widget(new CPUManagedStreamWidget(')[1].split('#endif')[0]
    for n,address in zip(names,[592,596,600]):
        assert re.search(re.escape(n)+r'\)ESC"\),\s*\d+ULL,\s*'+str(address)+r'ULL',driver)
    run(['firtool',out/'control.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')
    nh,_=module(clean((out/'native.sv').read_text()),'GGNICCPUControlTest');native=ports(nh)
    def canon(n):return {'hostClock':'clock','hostReset':'reset'}.get(n,n.removeprefix('cpuStream_ctrl_'))
    pins={canon(n):s for n,s in native.items()}
    assert [pins[f'count_{i}'] for i in range(3)]==[['input',w] for w in widths]
    for n,spec in mp.items():
        if n.startswith('io_nasti_'): assert pins[n.removeprefix('io_nasti_')]==spec
    ins=[(n,w) for n,(d,w) in pins.items() if d=='input'];outs=[(n,w) for n,(d,w) in pins.items() if d=='output']
    (out/'sfc-mcr.sv').write_text('module MCRFile('+mh+');'+mb+'endmodule\n')
    ref=['module SFCNICCPUControl('+','.join(f'{d} [{w-1}:0] {n}' for n,(d,w) in pins.items())+');']
    for i,w in enumerate(widths):
        ref += [f'wire [31:0] mcr_read_{i}_bits={{{{{32-w}\'d0}}, count_{i}}};',f'wire mcr_write_{i}_valid;',f'wire [31:0] mcr_write_{i}_bits;']
    ref+=['MCRFile cr('+','.join('.'+n+'('+n.removeprefix('io_nasti_').removeprefix('io_')+')' for n in mp)+');']
    sfc_outputs={n.removeprefix('io_nasti_') for n,(d,w) in mp.items() if d=='output' and n.startswith('io_nasti_')}
    constants={n:(1 if n=='r_bits_last' else 0) for n,w in outs if n not in sfc_outputs}
    assert constants=={'b_bits_resp':0,'b_bits_user':0,'r_bits_resp':0,'r_bits_last':1,'r_bits_user':0}
    ref += [f'assign {n}={w}\'d{constants[n]};' for n,w in outs if n in constants]+['endmodule']
    (out/'sfc-control.sv').write_text('\n'.join(ref)+'\n')
    test=['module check;']+[f'reg [{w-1}:0] {n}=0;' for n,w in ins]
    test += [f'wire [{w-1}:0] {pre}_{n};' for pre in ('n','s') for n,w in outs]
    test += ['GGNICCPUControlTest n('+','.join('.'+n+'('+('n_'+canon(n) if native[n][0]=='output' else canon(n))+')' for n in native)+');',
             'SFCNICCPUControl s('+','.join(f'.{n}({"s_"+n if d=="output" else n})' for n,(d,w) in pins.items())+');',
             'integer cycles=0,reads=0,stalls=0,holes=0,resets=0;task compare;begin']
    for n,w in outs:
        guard='n_b_valid' if n.startswith('b_bits_') else 'n_r_valid' if n.startswith('r_bits_') else '1'
        test += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    test += ['end endtask','task tick;begin clock=0;#2;compare();reads+=(ar_valid&&n_ar_ready);stalls+=(n_r_valid&&!r_ready);resets+=reset;',
             'clock=1;#2;clock=0;#2;cycles++;compare();end endtask',
             'initial begin reset=1;tick();reset=0;',
             'for(integer idx=0;idx<4;idx++)for(integer low=0;low<4;low++)begin',
             "ar_bits_addr=25'h1fffff0|(idx<<2)|low;ar_bits_id=$urandom;ar_valid=1;tick();ar_valid=0;",
             'repeat(20)begin count_0=$urandom;count_1=$urandom;count_2=$urandom;tick();end r_ready=1;tick();r_ready=0;end',
             # Index three never commits a write; reset releases outstanding AW/W.
             'for(integer order=0;order<3;order++)for(integer strobe=0;strobe<16;strobe++)begin',
             "aw_bits_addr=25'h1ffffff;aw_bits_id=$urandom;w_bits_data=$urandom;w_bits_strb=strobe;",
             'if(order==2)begin aw_valid=1;w_valid=1;tick();aw_valid=0;w_valid=0;end',
             'else begin if(order==0)begin aw_valid=1;tick();aw_valid=0;end',
             'w_valid=1;tick();w_valid=0;if(order==1)begin aw_valid=1;tick();aw_valid=0;end end',
             'repeat(5)tick();if(n_b_valid)$fatal(1,"hole committed");holes++;reset=1;tick();reset=0;end',
             'for(integer j=0;j<100000;j++)begin reset=($urandom_range(0,99)==0);',
             'count_0=$urandom;count_1=$urandom;count_2=$urandom;ar_valid=$urandom;r_ready=$urandom;',
             'ar_bits_addr=$urandom;ar_bits_id=$urandom;',
             "aw_valid=$urandom;w_valid=$urandom;aw_bits_addr=($urandom&25'h1fffff0)|12|($urandom&3);",
             'aw_bits_id=$urandom;w_bits_data=$urandom;w_bits_strb=$urandom;b_ready=$urandom;tick();end',
             'if(reads<1000||stalls<1000||resets<100||holes!=48)$fatal(1,"coverage");',
             '$display("PASS SFC NIC occupancy Nasti: cycles=%0d reads=%0d stalls=%0d resets=%0d holes=%0d counts=3 indices=4 lowBits=4 random=100000",cycles,reads,stalls,resets,holes);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc-mcr.sv',out/'sfc-control.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    def artifact(path):return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report=dict(golden=artifact(a.golden),golden_driver=artifact(header),candidate=artifact(a.control_candidate),result='matched',
                unchanged_prior_modules=len(before),production_ports=len(current),annotation_records=len(ann(a.control_candidate)),
                count_order=names,local_offsets=[0,4,8],sfc_global_addresses=[592,596,600],
                simulation=(out/'simulation.log').read_text(),limitation=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])


if __name__=='__main__':main()
