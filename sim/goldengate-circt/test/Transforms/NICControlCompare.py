#!/usr/bin/env python3
"""Compare production native NIC Nasti+register operations with actual SFC RTL.

Retain the archived MCRFile_4 verbatim and the original six-register cone.
Only substitute a done input for !tFire at the previously compared NIC token
boundary. This does not establish global control/platform or NIC runtime parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules, SIGNALS
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring


def transfer(v):
    old, new = 'GGNICMMIOWrapper', 'GGNICBridgeControlWrapper'
    if isinstance(v, str):
        if v == '~'+old: return '~'+new
        if not v.startswith('~'+old+'|'): return v
        suffix = v[len(old)+1:]
        if suffix.startswith('|'+old+'>'): suffix = '|'+new+'>'+suffix[len(old)+2:]
        return '~'+new+suffix
    if isinstance(v, list): return [transfer(x) for x in v]
    if isinstance(v, dict): return {k: transfer(x) for k, x in v.items()}
    return v


def register_cone(golden):
    _, body = module(clean(golden), 'SimpleNICBridgeModule')
    inputs = [('clock', 1), ('reset', 1), ('nic_done', 1)]
    for i in range(6): inputs += [(f'mcr_write_{i}_valid', 1), (f'mcr_write_{i}_bits', 32), (f'mcr_read_{i}_ready', 1)]
    payloads = SIGNALS + [(f'mcr_read_{i}_bits', 32) for i in range(6)]
    decl = 'module SFCNICRegisters(' + ','.join(f'{d} [{w-1}:0] {n}' for d, ps in [('input', inputs), ('output', payloads)] for n, w in ps) + ');'
    lines = [l for l in body.splitlines() if re.match(r'\s*reg (?:\[31:0\] )?(?:macAddrRegUpper|macAddrRegLower|rlimitSettings|pauseThreshold|pauseTimes| done)\s*;', l)
             or 'wire [63:0] _hPort_hBits_nic_macAddr_T =' in l or 'wire [31:0] _GEN_5 =' in l
             or re.match(r'\s*assign hPort_hBits_nic_(?:macAddr|rlimit_|pauser_)', l)
             or re.match(r'\s*assign crFile_io_mcr_read_[0-5]_bits =', l)]
    assert sum(l.strip().startswith('reg ') for l in lines) == 6
    assert sum(l.strip().startswith('assign ') for l in lines) == 13
    start = body.index('  always @(posedge clock)')
    clocked = body[start:body.index('\n`ifdef RANDOMIZE', start)]
    assert sum(f'~crFile_io_mcr_read_{i}_ready' in clocked for i in range(5)) == 5
    ref = decl+'\n'+'\n'.join(lines)+'\n'+clocked+'\nendmodule\n'
    return ref.replace('crFile_io_', '').replace('hPort_hBits_', '').replace("{{31'd0}, _T}", "{{31'd0}, nic_done}")


def compare_rtl(native_file, golden_file, out, run, top_name="GGNICControlTest", canonical=None):
    nh,_=module(clean(native_file.read_text()),top_name); native=ports(nh)
    canon = canonical or (lambda n: {'hostClock':'clock','hostReset':'reset'}.get(n,n.removeprefix('nicBridge_ctrl_')))
    pins={canon(n):s for n,s in native.items()}; assert len(pins)==len(native)
    inputs=[(n,w) for n,(d,w) in pins.items() if d=='input']
    outputs=[(n,w) for n,(d,w) in pins.items() if d=='output']
    golden=golden_file.read_text();mh,mb=module(clean(golden),'MCRFile_4'); mp=ports(mh)
    # The SFC port list itself establishes optimization of unused Nasti fields.
    assert mp['io_nasti_aw_bits_addr']==['input',25] and mp['io_nasti_aw_bits_id']==['input',12]
    assert 'io_nasti_w_bits_strb' not in mp and 'io_nasti_r_bits_last' not in mp
    assert len(re.findall(r'\breg\s+\[2:0\]\s+[wr]Index\b',mb))==2
    (out/'sfc-mcr.sv').write_text('module MCRFile_4('+mh+');'+mb+'endmodule\n')
    (out/'sfc-registers.sv').write_text(register_cone(golden))
    ref=['module SFCNICControl('+','.join(f'{d} [{w-1}:0] {n}' for n,(d,w) in pins.items())+');']
    for i in range(6):
        ref += [f'wire mcr_write_{i}_valid,mcr_read_{i}_ready;',f'wire [31:0] mcr_write_{i}_bits,mcr_read_{i}_bits;']
    ref+=['MCRFile_4 cr('+','.join('.'+n+'('+n.removeprefix('io_nasti_').removeprefix('io_')+')' for n in mp)+');']
    rp=ports(module(clean((out/'sfc-registers.sv').read_text()),'SFCNICRegisters')[0])
    ref+=['SFCNICRegisters regs('+','.join(f'.{n}({n})' for n in rp)+');']
    sfc_outputs={n.removeprefix('io_nasti_') for n,(d,w) in mp.items() if d=='output' and n.startswith('io_nasti_')}
    config={n for n,w in SIGNALS}
    constants={n:(1 if n=='r_bits_last' else 0) for n,w in outputs if n not in sfc_outputs|config}
    assert constants=={'b_bits_resp':0,'b_bits_user':0,'r_bits_resp':0,'r_bits_last':1,'r_bits_user':0},constants
    ref += [f'assign {n}={w}\'d{constants[n]};' for n,w in outputs if n in constants]+['endmodule']
    (out/'sfc-control.sv').write_text('\n'.join(ref)+'\n')
    test=['module check;']+[f'reg [{w-1}:0] {n}=0;' for n,w in inputs]
    test += [f'wire [{w-1}:0] {pre}_{n};' for pre in ('n','s') for n,w in outputs]
    test+=[top_name+' n('+','.join('.'+n+'('+('n_'+canon(n) if native[n][0]=='output' else canon(n))+')' for n in native)+');',
           'SFCNICControl s('+','.join(f'.{n}({"s_"+n if d=="output" else n})' for n,(d,w) in pins.items())+');',
           'integer cycles=0,writes=0,reads=0,bStalls=0,rStalls=0,holes=0,resetPending=0,zeroStrobes=0;',
           'reg initialized=0;', 'task compare;begin']
    for n,w in outputs:
        guard='initialized' if n in config else '1'
        if n.startswith('b_bits_'):guard='n_b_valid'
        if n.startswith('r_bits_'):guard='n_r_valid'
        test += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    test+=['end endtask','task tick;begin clock=0;#2;compare();',
           'writes+=(aw_valid&&n_aw_ready);reads+=(ar_valid&&n_ar_ready);',
           'bStalls+=(n_b_valid&&!b_ready);rStalls+=(n_r_valid&&!r_ready);',
           'resetPending+=(reset&&(!n_aw_ready||!n_w_ready||!n_ar_ready));zeroStrobes+=(w_valid&&n_w_ready&&!w_bits_strb);',
           'clock=1;#2;clock=0;#2;cycles++;compare();end endtask',
           'task idle;begin aw_valid=0;w_valid=0;ar_valid=0;b_ready=0;r_ready=0;end endtask',
           'task flush;begin idle();reset=1;tick();reset=0;end endtask',
           'task writeword(input integer index,input integer order,input integer strobe);begin',
           'idle();aw_bits_addr=($urandom&25\'h1ffffe0)|(index<<2)|($urandom&3);aw_bits_id=$urandom;',
           'w_bits_data=$urandom;w_bits_strb=strobe;w_bits_last=$urandom;',
           'if(order==0)begin aw_valid=1;tick();aw_valid=0;repeat(3)tick();w_valid=1;tick();w_valid=0;end',
           'else if(order==1)begin w_valid=1;tick();w_valid=0;repeat(3)tick();aw_valid=1;tick();aw_valid=0;end',
           'else begin aw_valid=1;w_valid=1;tick();aw_valid=0;w_valid=0;end',
           'repeat(5)tick();if(index<6)begin if(!n_b_valid)$fatal(1,"missing B");b_ready=1;tick();b_ready=0;end',
           'else begin if(n_b_valid)$fatal(1,"hole write committed");holes++;flush();end end endtask',
           'initial begin idle();reset=1;tick();reset=0;',
           'for(integer k=0;k<5;k++)writeword(k,2,15);initialized=1;',
           'for(integer k=0;k<8;k++)for(integer order=0;order<3;order++)for(integer strobe=0;strobe<16;strobe++)begin nic_done=$urandom;writeword(k,order,strobe);end',
           'for(integer k=0;k<8;k++)begin idle();ar_valid=1;ar_bits_addr=($urandom&25\'h1ffffe0)|(k<<2)|3;ar_bits_id=$urandom;tick();ar_valid=0;',
           'repeat(8)begin nic_done=~nic_done;tick();end',
           'if(k>=5)begin r_ready=1;tick();end else begin reset=1;r_ready=1;tick();reset=0;end end',
           # Accepting read requests only for readable word five or hole aliases
           # keeps write-only assertions enabled throughout random traffic.
           'for(integer j=0;j<100000;j++)begin',
           'reset=($urandom_range(0,99)==0);nic_done=$urandom;aw_valid=$urandom;w_valid=$urandom;',
           'aw_bits_addr=$urandom;aw_bits_id=$urandom;w_bits_data=$urandom;w_bits_strb=$urandom;w_bits_last=$urandom;',
           'ar_valid=$urandom;ar_bits_addr=($urandom&25\'h1ffffe0)|($urandom_range(5,7)<<2)|($urandom&3);ar_bits_id=$urandom;',
           'b_ready=$urandom;r_ready=$urandom;tick();end',
           'if(writes<1000||reads<1000||bStalls<100||rStalls<100||holes!=96||resetPending<100||zeroStrobes<100)$fatal(1,"coverage");',
           '$display("PASS SFC NIC Nasti: cycles=%0d writes=%0d reads=%0d bStalls=%0d rStalls=%0d holes=%0d resetPending=%0d zeroStrobes=%0d orders=3 strobes=16 indices=8 random=100000",cycles,writes,reads,bStalls,rStalls,holes,resetPending,zeroStrobes);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',native_file,out/'sfc-mcr.sv',out/'sfc-registers.sv',out/'sfc-control.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    return constants, (out/'simulation.log').read_text()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('candidate','control-candidate','golden','compose-tool','output-dir'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args(); out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    run([a.compose_tool,a.candidate,out/'attached.mlir',out/'control.mlir'],'compose')
    before=modules(a.candidate.read_text()); after=modules(a.control_candidate.read_text())
    assert after==modules((out/'attached.mlir').read_text()),'composition differs from production compiler'
    assert set(after)-set(before)=={'GGNICMCRFile','GGNICBridgeControlWrapper'}
    for n,b in before.items(): assert after[n]==b,'prior module changed: '+n
    top=after['GGNICBridgeControlWrapper']; prior=native_ports(before['GGNICMMIOWrapper'])
    current=native_ports(top);assert current[:-1]==prior[:-1] and current[-1][:2]==('in','nicBridge_ctrl')
    expected={('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in prior[:-1]}
    expected.update({'crFile.clock':'top.hostClock','crFile.reset':'top.hostReset',
                     'crFile.nasti':'top.nicBridge_ctrl','crFile.mcr':'sim.nicBridge_mcr'})
    assert wiring(top)==expected,'production Nasti/decoded routes differ'
    def ann(path):return json.loads(path.with_name(path.stem+'-all.json').read_text())
    assert ann(a.control_candidate)==transfer(ann(a.candidate)),'recursive archive transfer differs'
    stand=modules((out/'control.mlir').read_text())
    for n in ('GGNICMMIOBank','GGNICMCRFile'): assert stand[n]==after[n],'standalone helper differs'
    run(['firtool',out/'control.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')
    constants, simulation = compare_rtl(out/'native.sv', a.golden, out, run)
    def artifact(path):return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report=dict(golden=artifact(a.golden),candidate=artifact(a.control_candidate),result='matched',
                unchanged_prior_modules=len(before),production_ports=len(current),annotation_records=len(ann(a.control_candidate)),
                response_constants=constants,simulation=(out/'simulation.log').read_text(),
                limitation='Local NIC Nasti/register boundary only. Global address allocation, NIC driver/platform binding and complete runtime remain pending. Uninitialized captures/config registers are unspecified.')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])


if __name__=='__main__':main()
