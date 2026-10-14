#!/usr/bin/env python3
"""Compare production NIC TSI assembly and behavior with the archived SFC widget.

Only the TSI widget is isolated. This does not establish NIC runtime or the
remaining BlockDev/FASED and platform integration semantics.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlWriteRoutingCompare import transfer
from PrintControlResponsesCompare import clean, module, equations
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring
from PrintRocketResponsesCompare import wiring as binding_wiring

HELPERS = ('GGTSITokenEngine', 'GGTSIWordQueue16', 'GGTSIMMIOBank', 'GGTSIMCRFile')
NAMES = ['in_bits', 'in_valid', 'in_ready', 'out_bits', 'out_valid',
         'out_ready', 'step_size', 'done', 'start']
STAGES = ['simulation-master-bound', 'tsi-token', 'tsi-word-queues', 'tsi-mmio', 'tsi-control', 'tsi-bound']
TOPS = ['GGSimulationMasterBoundWrapper', 'GGTSITokenWrapper', 'GGTSIWordQueuesWrapper',
        'GGTSIMMIOWrapper', 'GGTSIBridgeControlWrapper', 'GGTSIBridgeBoundWrapper']


def behavior(native, golden, out, run):
    text = clean(native.read_text())
    interfaces = {n: ports(module(text, n)[0]) for n in HELPERS}
    engine, queue, bank, mcr = [interfaces[n] for n in HELPERS]
    top = {'clock': ['input', 1], 'reset': ['input', 1]}
    top.update({n: v for n,v in engine.items() if n.startswith('hPort_')})
    top.update({'io_ctrl_'+n.removeprefix('nasti_'): v for n,v in mcr.items() if n.startswith('nasti_')})
    instances = [
        ('engine', HELPERS[0], engine, {'inBuf_io_deq': 'in_deq', 'outBuf_io_enq': 'out_enq'}),
        ('inBuf', HELPERS[1], queue, {'enq': 'in_enq', 'deq': 'in_deq', 'reset': 'queueReset'}),
        ('outBuf', HELPERS[1], queue, {'enq': 'out_enq', 'deq': 'out_deq', 'reset': 'queueReset'}),
        ('registers', HELPERS[2], bank, {'inBuf': 'in_enq', 'outBuf': 'out_deq'}),
        ('transport', HELPERS[3], mcr, {'nasti': 'io_ctrl'})]
    def net(n, mapping):
        for prefix, replacement in mapping.items():
            if n == prefix or n.startswith(prefix+'_'): return replacement+n[len(prefix):]
        return n
    nets = dict(top); drivers = {}
    for inst, name, ps, mapping in instances:
        for n, (d,w) in ps.items():
            wire = net(n,mapping)
            assert wire not in nets or nets[wire][1] == w
            nets[wire] = [d,w]
            if d == 'output':
                assert wire not in drivers, 'duplicate assembled driver'
                drivers[wire] = inst+'.'+n
    assert all(n in drivers for n,(d,w) in top.items() if d=='output')
    lines = ['module NativeTSI('+','.join(f'{d} [{w-1}:0] {n}' for n,(d,w) in top.items())+');']
    lines += [f'wire [{w-1}:0] {n};' for n,(d,w) in nets.items() if n not in top]
    for inst,name,ps,mapping in instances:
        lines += [name+' '+inst+'('+','.join(f'.{n}({net(n,mapping)})' for n in ps)+');']
    (out/'native-wrapper.sv').write_text('\n'.join(lines+['endmodule'])+'\n')
    reference = clean(golden.read_text()); sh, sb = module(reference,'TSIBridgeModule')
    sp = ports(sh)
    assert all(top[n] == v for n,v in sp.items())
    # Keep the full SFC module and its actual Queue/MCRFile dependencies.
    names = ['TSIBridgeModule']
    for pattern in (r'\b(\w+)\s+inBuf\s*\(', r'\b(\w+)\s+outBuf\s*\(', r'\b(\w+)\s+crFile\s*\('):
        names.append(re.search(pattern,sb)[1])
    refs = ['module '+n+'('+module(reference,n)[0]+');'+module(reference,n)[1]+'endmodule\n' for n in dict.fromkeys(names)]
    (out/'sfc.sv').write_text('\n'.join(refs))
    inputs = [(n,w) for n,(d,w) in top.items() if d=='input']
    outputs = [(n,w) for n,(d,w) in top.items() if d=='output']
    constants = {'io_ctrl_b_bits_resp':0,'io_ctrl_b_bits_user':0,
                 'io_ctrl_r_bits_resp':0,'io_ctrl_r_bits_last':1,'io_ctrl_r_bits_user':0}
    assert {n for n,w in outputs if n not in sp} == set(constants)
    tb = ['module check;']+[f'reg [{w-1}:0] {n}=0;' for n,w in inputs]
    tb += [f'wire [{w-1}:0] {p}_{n};' for p in ('n','s') for n,w in outputs]
    for name, ps, prefix in [('NativeTSI',top,'n'),('TSIBridgeModule',sp,'s')]:
        tb += [name+' '+prefix+'ative('+','.join(f'.{n}({prefix+"_"+n if d=="output" else n})' for n,(d,w) in ps.items())+');']
    tb += [f'assign s_{n}={w}\'d{constants[n]};' for n,w in outputs if n in constants]
    tb += ['integer cycles=0,fires=0,bStalls=0,rStalls=0,fullIn=0,fullOut=0,stalledReset=0,acceptedReset=0,simultaneous=0;',
           'task compare;begin']
    for n,w in outputs:
        guard = ('n_io_ctrl_b_valid' if n.startswith('io_ctrl_b_bits_') else
                 'n_io_ctrl_r_valid' if n.startswith('io_ctrl_r_bits_') else
                 'n_hPort_hBits_tsi_in_valid' if n=='hPort_hBits_tsi_in_bits' else '1')
        tb += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    tb += ['end endtask',
           'task tick;begin clock=0;#2;compare();fires+=n_hPort_toHost_hReady;',
           'bStalls+=(n_io_ctrl_b_valid&&!io_ctrl_b_ready);rStalls+=(n_io_ctrl_r_valid&&!io_ctrl_r_ready);',
           'fullIn+=!native.in_enq_ready;fullOut+=!n_hPort_hBits_tsi_out_ready;',
           'stalledReset+=(hPort_hBits_reset&&!n_hPort_toHost_hReady);acceptedReset+=(hPort_hBits_reset&&n_hPort_toHost_hReady);',
           'simultaneous+=(native.in_deq_ready&&native.in_deq_valid&&native.in_enq_ready&&native.in_enq_valid);',
           'clock=1;#2;clock=0;#2;cycles++;compare();end endtask',
           'task idle;begin io_ctrl_aw_valid=0;io_ctrl_w_valid=0;io_ctrl_ar_valid=0;io_ctrl_b_ready=0;io_ctrl_r_ready=0;end endtask',
           'task flush;begin idle();reset=1;tick();reset=0;end endtask',
           'task writeword(input integer index,input integer data,input integer order,input integer strobe);begin',
           'idle();io_ctrl_aw_bits_addr=320+(index<<2);io_ctrl_aw_bits_id=$urandom;io_ctrl_aw_bits_len=0;',
           'io_ctrl_w_bits_data=data;io_ctrl_w_bits_last=1;io_ctrl_w_bits_strb=strobe;',
           'if(order==0)begin io_ctrl_aw_valid=1;tick();io_ctrl_aw_valid=0;repeat(2)tick();io_ctrl_w_valid=1;tick();io_ctrl_w_valid=0;end',
           'else if(order==1)begin io_ctrl_w_valid=1;tick();io_ctrl_w_valid=0;repeat(2)tick();io_ctrl_aw_valid=1;tick();io_ctrl_aw_valid=0;end',
           'else begin io_ctrl_aw_valid=1;io_ctrl_w_valid=1;tick();io_ctrl_aw_valid=0;io_ctrl_w_valid=0;end',
           'repeat(3)tick();if(index<9)begin if(!n_io_ctrl_b_valid)$fatal(1,"missing B");io_ctrl_b_ready=1;tick();end',
           'else begin if(n_io_ctrl_b_valid)$fatal(1,"hole write committed");flush();end idle();end endtask',
           'initial begin flush();hPort_toHost_hValid=1;hPort_fromHost_hReady=1;',
           'for(integer k=0;k<9;k++)writeword(k,0,2,15);',
           '// Fill input queue while scheduler is stopped; then start and fill output queue.',
           'for(integer k=0;k<20;k++)begin writeword(0,k,2,15);writeword(1,1,2,15);end',
           'writeword(6,1024,2,15);writeword(8,1,2,15);hPort_hBits_tsi_out_valid=1;hPort_hBits_tsi_out_bits=32\'hdeadbeef;repeat(40)tick();',
           'hPort_hBits_tsi_in_ready=1;writeword(1,1,2,15);repeat(30)tick();',
           'for(integer k=0;k<20;k++)writeword(5,1,2,15);',
           '// Both stalled and accepted target resets, and host reset with queued words.',
           'hPort_hBits_reset=1;hPort_fromHost_hReady=0;repeat(5)tick();hPort_fromHost_hReady=1;repeat(5)tick();hPort_hBits_reset=0;flush();',
           'for(integer k=0;k<16;k++)for(integer order=0;order<3;order++)for(integer strobe=0;strobe<16;strobe++)writeword(k,$urandom,order,strobe);',
           'for(integer j=0;j<50000;j++)begin',
           'reset=($urandom_range(0,199)==0);io_ctrl_aw_valid=$urandom;io_ctrl_w_valid=$urandom;',
           'io_ctrl_aw_bits_addr=($urandom&25\'h1ffffc0)|($urandom_range(0,15)<<2)|($urandom&3);io_ctrl_aw_bits_id=$urandom;',
           'io_ctrl_w_bits_data=$urandom;io_ctrl_w_bits_strb=$urandom;io_ctrl_w_bits_last=1;',
           'io_ctrl_ar_valid=$urandom;io_ctrl_ar_bits_addr=$urandom;io_ctrl_ar_bits_id=$urandom;io_ctrl_b_ready=$urandom;io_ctrl_r_ready=$urandom;',
           'hPort_toHost_hValid=$urandom;hPort_fromHost_hReady=$urandom;hPort_hBits_reset=($urandom_range(0,31)==0);',
           'hPort_hBits_tsi_in_ready=$urandom;hPort_hBits_tsi_out_valid=$urandom;hPort_hBits_tsi_out_bits=$urandom;tick();end',
           '$display("coverage cycles=%0d fires=%0d bStalls=%0d rStalls=%0d fullIn=%0d fullOut=%0d stalledReset=%0d acceptedReset=%0d simultaneous=%0d",cycles,fires,bStalls,rStalls,fullIn,fullOut,stalledReset,acceptedReset,simultaneous);',
           'if(fires<100||bStalls<100||rStalls<100||fullIn<10||fullOut<10||stalledReset<5||acceptedReset<5||simultaneous<1)$fatal(1,"coverage");',
           '$display("PASS SFC NIC TSI: cycles=%0d fires=%0d bStalls=%0d rStalls=%0d fullIn=%0d fullOut=%0d stalledReset=%0d acceptedReset=%0d simultaneous=%0d indices=16 orders=3 strobes=16 random=50000",cycles,fires,bStalls,rStalls,fullIn,fullOut,stalledReset,acceptedReset,simultaneous);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','-j','4','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'native-wrapper.sv',out/'sfc.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    return names



def token_wiring(body):
    # Retain every AND operand, including multiplicity, when checking the
    # HostPort helper's exclusion of each channel's own valid/ready predicate.
    values = {v: 'top.'+v[1:] for v in re.findall(r'%\w+(?=:)', body.splitlines()[0])}
    nets = {}
    for line in body.splitlines()[1:]:
        if ' = firrtl.instance ' in line:
            left,right=line.split(' = firrtl.instance ',1)
            values.update(zip(re.findall(r'%\w+',left),(right.split()[0]+'.'+p for p in re.findall(r'\b(?:in|out) (\w+):',right))))
        if m:=re.search(r'(%\w+) = firrtl.constant (\d+) :',line): values[m[1]]=m[2]
        if m:=re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]',line): values[m[1]]=values[m[2]]+'.'+m[3]
        if m:=re.search(r'(%\w+) = firrtl.and (%\w+), (%\w+)',line): values[m[1]]=values[m[2]]+'&'+values[m[3]]
        if m:=re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)',line):
            assert values[m[1]] not in nets
            nets[values[m[1]]]=values[m[2]]
    return {d: tuple(sorted(x for x in s.split('&') if x!='1')) for d,s in nets.items()}


def check_tokens(before, after, annotations):
    bridge=next(x for x in annotations if x.get('widgetClass')=='firechip.goldengateimplementations.TSIBridgeModule')
    locals=['tsi_in_rev','tsi_out_fwd','reset','tsi_in_fwd','tsi_out_rev']
    fields=['tsi_in_ready','tsi_out','reset','tsi_in','tsi_out_ready']
    channels=[next(x for x in annotations if x.get('globalName')==bridge['channelMapping'][n]) for n in locals]
    roots=[re.search(r'>(\w+)\.',x['sources' if j<3 else 'sinks'][0])[1] for j,x in enumerate(channels)]
    oldports=native_ports(before); engine='TSIBridgeModule_0.'
    expected={('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in oldports if n not in roots}
    for p in ('hostClock','hostReset'): expected[engine+('clock' if p=='hostClock' else 'reset')]='top.'+p
    expected[engine+'hPort.toHost.hValid']='&'.join('sim.'+n+'.valid' for n in roots[:3])
    expected[engine+'hPort.fromHost.hReady']='&'.join('sim.'+n+'.ready' for n in roots[3:])
    for j,n in enumerate(roots):
        expected['sim.'+n+('.ready' if j<3 else '.valid')]=engine+('hPort.toHost.hReady' if j<3 else 'hPort.fromHost.hValid')+''.join('&sim.'+p+('.valid' if j<3 else '.ready') for k,p in enumerate(roots) if k!=j and (k<3)==(j<3))
        for suffix in (['bits','valid'] if j in (1,3) else ['']):
            local=engine+'hPort.hBits.'+fields[j]+('_'+suffix if suffix else '')
            remote='sim.'+n+'.bits'+('.'+suffix if suffix else '')
            expected[local if j<3 else remote]=remote if j<3 else local
    for p in ('control','in_deq','out_enq','queue_reset'):
        internal=engine+{'control':'control','in_deq':'inBuf_io_deq','out_enq':'outBuf_io_enq','queue_reset':'queueReset'}[p]
        external='top.tsi_'+p
        expected[internal if p=='in_deq' else external]=external if p=='in_deq' else internal
    assert token_wiring(after)=={d:tuple(sorted(s.split('&'))) for d,s in expected.items()}, 'production token assembly differs'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir','golden','export-tool','output-dir'): p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args(); out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    paths=[a.candidate_dir/('post-fame-'+s+'.mlir') for s in STAGES]
    boundaries=[modules(x.read_text()) for x in paths]
    annos=lambda path: json.loads(path.with_name(path.stem+'-all.json').read_text())
    for i,(old,new) in enumerate(zip(TOPS,TOPS[1:])):
        assert all(boundaries[i+1][n]==body for n,body in boundaries[i].items()), 'prior module changed'
        copied={n for _,n,_ in native_ports(boundaries[i][old])}&{n for _,n,_ in native_ports(boundaries[i+1][new])}
        expected=transfer(annos(paths[i]),old,new,copied)
        if i==0:
            bridge=next(x for x in expected if x.get('widgetClass')=='firechip.goldengateimplementations.TSIBridgeModule')
            bridge['target']='~GGTSITokenWrapper|GGTSITokenEngine>hPort'
            locals=['tsi_in_rev','tsi_out_fwd','reset','tsi_in_fwd','tsi_out_rev']
            fields=[['tsi_in_ready'],['tsi_out_bits','tsi_out_valid'],['reset'],['tsi_in_bits','tsi_in_valid'],['tsi_out_ready']]
            for j,local in enumerate(locals):
                channel=next(x for x in expected if x.get('globalName')==bridge['channelMapping'][local])
                channel['sinks' if j<3 else 'sources']=['~GGTSITokenWrapper|GGTSITokenEngine>hPort.hBits.'+f for f in fields[j]]
        assert annos(paths[i+1])==expected, 'annotation transfer differs at '+new
    check_tokens(boundaries[0][TOPS[0]],boundaries[1][TOPS[1]],annos(paths[0]))
    reference=clean(a.golden.read_text());h,body=module(reference,'TSIBridgeModule');eq=equations(body)
    assert eq['tFire']=="hPort_toHost_hValid & hPort_fromHost_hReady & tokensToEnqueue != 32'h0"
    assert eq['inBuf_reset']==eq['outBuf_reset']=='reset | targetReset'
    regions=re.search(r'goldengate.controlRegions = \[(.*?)\]',paths[-1].read_text())[1]
    assert re.search(r'name = "TSIBridgeModule_0", size = 64 : i64, slave = 3 : i32, start = 320 : i64',regions)
    bank=boundaries[-1]['GGTSIMMIOBank']
    assert re.findall(r'name = "([^"]+)", offset = (\d+) : i32, readable = true, writeable = true',bank)==[(n,str(4*i)) for i,n in enumerate(NAMES)]
    q=wiring(boundaries[2][TOPS[2]])
    for inst in ('inBuf','outBuf'):
        assert q[inst+'.clock']=='top.hostClock' and q[inst+'.reset']=='sim.tsi_queue_reset'
    assert q['sim.tsi_in_deq']=='inBuf.deq' and q['outBuf.enq']=='sim.tsi_out_enq'
    m=wiring(boundaries[3][TOPS[3]])
    for dest,src in {'tsiRegisters.clock':'top.hostClock','tsiRegisters.reset':'top.hostReset',
                     'sim.tsi_in_enq':'tsiRegisters.inBuf','tsiRegisters.outBuf':'sim.tsi_out_deq','tsiRegisters.control':'sim.tsi_control'}.items(): assert m[dest]==src
    mapped=wiring(boundaries[4][TOPS[4]])
    expected={('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n)
              for d,n,_ in native_ports(boundaries[3][TOPS[3]]) if n!='tsiBridge_mcr'}
    expected.update({'crFile.clock':'top.hostClock','crFile.reset':'top.hostReset',
                     'crFile.nasti':'top.tsiBridge_ctrl','crFile.mcr':'sim.tsiBridge_mcr'})
    assert mapped==expected, 'production transport assembly differs'
    scalar=binding_wiring(paths[-1].read_text(),TOPS[-1]); assert len(scalar)==32
    assert binding_wiring(paths[-1].read_text(),TOPS[-1],aggregate=True)['sim.tsiBridge_ctrl.ar']=='sim.ctrl_read_dispatch_slave_3_ar'
    fpga=equations(module(reference,'FPGATop')[1]);pins=[]
    for pin in re.findall(r'\b(io_ctrl_\w+)\b',h):
        ch,leaf=pin.removeprefix('io_ctrl_').split('_',1); sp='TSIBridgeModule_0_'+pin;rp='ctrlInterconnect_io_slaves_3_'+ch+'_'+leaf
        assert fpga.get(sp)==rp or fpga.get(rp)==sp
        np='sim.tsiBridge_ctrl.'+ch+'.'+leaf.replace('bits_','bits.')
        if ch!='ar':
            bp='sim.ctrl_'+('write_dispatch_slave_3_'+ch+'_' if ch in ('aw','w') else ('read' if ch=='r' else 'write')+'_arb_in_3_')+leaf
            assert scalar[bp if (ch in ('aw','w'))==(leaf=='ready') else np]==(np if (ch in ('aw','w'))==(leaf=='ready') else bp)
        pins.append(pin)
    assert len(pins)==20
    run([a.export_tool,paths[-1],out/'helpers.mlir'],'export')
    for n,b in modules((out/'helpers.mlir').read_text()).items(): assert b==boundaries[-1][n]
    run(['firtool',out/'helpers.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')
    deps=behavior(out/'native.sv',a.golden,out,run)
    digest=lambda path: dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    result=dict(golden=digest(a.golden),candidate=digest(paths[-1]),result='matched',slave=3,register_addresses=[320+4*i for i in range(9)],
                annotations=len(annos(paths[-1])),preserved_modules=[len(x) for x in boundaries[:-1]],sfc_dependencies=deps,sfc_control_pins=pins,
                simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(result,indent=2)+'\n');print(result['simulation'])


if __name__=='__main__': main()
