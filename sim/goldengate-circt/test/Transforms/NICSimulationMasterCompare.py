#!/usr/bin/env python3
"""Compare the production NIC master attachment, transport and slot-9 binding.

The behavioral reference is the archived SFC SimulationMaster and MCRFile,
retained verbatim. Native bank and transport operations are cloned from the
actual compiler boundary. This does not establish remaining NIC widget or
platform completion.
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


def compare_rtl(native, golden, out, run):
    # Connect the two exported modules using their actual flattened MCR ports.
    # The surrounding production aggregate connections are checked separately.
    text = clean(native.read_text())
    bank = ports(module(text, 'GGSimulationMasterBank')[0])
    transport = ports(module(text, 'GGSimulationMasterMCRFile')[0])
    mcr = {n: v for n, v in bank.items() if n.startswith('mcr_')}
    assert set(mcr) == {n for n in transport if n.startswith('mcr_')}
    assert all(transport[n][1] == w and transport[n][0] != d for n, (d,w) in mcr.items())
    ctrl = {n.removeprefix('nasti_'): v for n,v in transport.items() if n.startswith('nasti_')}
    pins = {'clock': ['input',1], 'reset': ['input',1], **ctrl}
    declaration = ','.join(f'{d} [{w-1}:0] {n}' for n,(d,w) in pins.items())
    wrapper = ['module NativeMaster('+declaration+');']
    wrapper += [f'wire [{w-1}:0] {n};' for n,(_,w) in mcr.items()]
    wrapper += ['GGSimulationMasterBank bank('+','.join(f'.{n}({n})' for n in bank)+');',
                'GGSimulationMasterMCRFile transport('+','.join(f'.{n}({n.removeprefix("nasti_")})' for n in transport)+');', 'endmodule']
    (out/'native-wrapper.sv').write_text('\n'.join(wrapper)+'\n')
    reference = clean(golden.read_text())
    refs = []
    for name in ('MCRFile','SimulationMaster'):
        h,b = module(reference,name)
        refs += ['module '+name+'('+h+');'+b+'endmodule\n']
    (out/'sfc.sv').write_text('\n'.join(refs))
    sp = ports(module(reference,'SimulationMaster')[0])
    sfc_ctrl = {n.removeprefix('io_ctrl_'): v for n,v in sp.items() if n.startswith('io_ctrl_')}
    assert len(sfc_ctrl) == 20 and all(ctrl[n] == v for n,v in sfc_ctrl.items())
    inputs = [(n,w) for n,(d,w) in pins.items() if d=='input']
    outputs = [(n,w) for n,(d,w) in pins.items() if d=='output']
    constants = {n: (1 if n=='r_bits_last' else 0) for n,w in outputs if n not in sfc_ctrl}
    assert constants == {'b_bits_resp':0,'b_bits_user':0,'r_bits_resp':0,'r_bits_last':1,'r_bits_user':0}
    tb = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n,w in inputs]
    tb += [f'wire [{w-1}:0] {p}_{n};' for p in ('n','s') for n,w in outputs]
    tb += ['NativeMaster native_master('+','.join(f'.{n}({"n_"+n if d=="output" else n})' for n,(d,w) in pins.items())+');',
           'SimulationMaster sfc_master('+','.join(f'.{n}({"s_"+n.removeprefix("io_ctrl_") if d=="output" else n.removeprefix("io_ctrl_")})' for n,(d,w) in sp.items())+');']
    tb += [f'assign s_{n}={w}\'d{constants[n]};' for n,w in outputs if n in constants]
    tb += ['integer cycles=0,writes=0,reads=0,bStalls=0,rStalls=0,holes=0,resetPending=0,zeroStrobes=0;',
           'task compare;begin']
    for n,w in outputs:
        guard = 'n_b_valid' if n.startswith('b_bits_') else 'n_r_valid' if n.startswith('r_bits_') else '1'
        tb += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    tb += ['end endtask', 'task tick;begin clock=0;#2;compare();',
           'writes+=(aw_valid&&n_aw_ready);reads+=(ar_valid&&n_ar_ready);',
           'bStalls+=(n_b_valid&&!b_ready);rStalls+=(n_r_valid&&!r_ready);',
           'resetPending+=(reset&&(!n_aw_ready||!n_w_ready||!n_ar_ready));zeroStrobes+=(w_valid&&n_w_ready&&!w_bits_strb);',
           'clock=1;#2;clock=0;#2;cycles++;compare();end endtask',
           'task idle;begin aw_valid=0;w_valid=0;ar_valid=0;b_ready=0;r_ready=0;end endtask',
           'task flush;begin idle();reset=1;tick();reset=0;end endtask',
           'task writeword(input integer index,input integer order,input integer strobe);begin',
           'idle();aw_bits_addr=($urandom&25\'h1fffff0)|(index<<2)|($urandom&3);aw_bits_id=$urandom;',
           'w_bits_data=$urandom;w_bits_strb=strobe;w_bits_last=$urandom;',
           'if(order==0)begin aw_valid=1;tick();aw_valid=0;repeat(3)tick();w_valid=1;tick();w_valid=0;end',
           'else if(order==1)begin w_valid=1;tick();w_valid=0;repeat(3)tick();aw_valid=1;tick();aw_valid=0;end',
           'else begin aw_valid=1;w_valid=1;tick();aw_valid=0;w_valid=0;end',
           'repeat(5)tick();if(index<3)begin if(!n_b_valid)$fatal(1,"missing B");b_ready=1;tick();b_ready=0;end',
           'else begin if(n_b_valid)$fatal(1,"hole write committed");holes++;flush();end end endtask',
           'initial begin idle();reset=1;tick();reset=0;repeat(70)tick();',
           'for(integer k=0;k<4;k++)for(integer order=0;order<3;order++)for(integer strobe=0;strobe<16;strobe++)writeword(k,order,strobe);',
           'for(integer k=0;k<4;k++)begin idle();ar_valid=1;ar_bits_addr=($urandom&25\'h1fffff0)|(k<<2)|3;ar_bits_id=$urandom;tick();ar_valid=0;',
           'repeat(8)tick();r_ready=1;tick();end',
           'for(integer j=0;j<100000;j++)begin',
           'reset=($urandom_range(0,99)==0);aw_valid=$urandom;w_valid=$urandom;',
           'aw_bits_addr=$urandom;aw_bits_id=$urandom;w_bits_data=$urandom;w_bits_strb=$urandom;w_bits_last=$urandom;',
           'ar_valid=$urandom;ar_bits_addr=$urandom;ar_bits_id=$urandom;b_ready=$urandom;r_ready=$urandom;tick();end',
           'if(writes<1000||reads<1000||bStalls<100||rStalls<100||holes!=48||resetPending<100||zeroStrobes<100)$fatal(1,"coverage");',
           '$display("PASS SFC NIC SimulationMaster: cycles=%0d writes=%0d reads=%0d bStalls=%0d rStalls=%0d holes=%0d resetPending=%0d zeroStrobes=%0d orders=3 strobes=16 indices=4 random=100000",cycles,writes,reads,bStalls,rStalls,holes,resetPending,zeroStrobes);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'native-wrapper.sv',out/'sfc.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    return constants


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('tracker','attached','control','bound','golden','export-tool','output-dir'):
        p.add_argument('--'+n,type=Path,required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    paths = [a.tracker,a.attached,a.control,a.bound]
    boundaries = [modules(x.read_text()) for x in paths]
    stages = [('GGControlWriteTrackerWrapper','GGSimulationMasterWrapper'),
              ('GGSimulationMasterWrapper','GGSimulationMasterControlWrapper'),
              ('GGSimulationMasterControlWrapper','GGSimulationMasterBoundWrapper')]
    for i,(old,new) in enumerate(stages):
        assert all(boundaries[i+1][n] == body for n,body in boundaries[i].items()), 'prior module changed'
        copied = {n for _,n,_ in native_ports(boundaries[i][old])} & {n for _,n,_ in native_ports(boundaries[i+1][new])}
        assert annos(paths[i+1]) == transfer(annos(paths[i]),old,new,copied), 'annotation transfer differs'
    attached = boundaries[1]['GGSimulationMasterWrapper']; nets = wiring(attached)
    old_ports = native_ports(boundaries[0]['GGControlWriteTrackerWrapper'])
    expected = {('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in old_ports}
    expected.update({'simulationMaster.clock':'top.hostClock','simulationMaster.reset':'top.hostReset',
                     'top.simulationMaster_mcr':'simulationMaster.mcr'})
    assert nets == expected, 'attached bank wiring differs'
    mapped = boundaries[2]['GGSimulationMasterControlWrapper']; before = native_ports(attached)
    expected = {('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in before if n!='simulationMaster_mcr'}
    expected.update({'crFile.clock':'top.hostClock','crFile.reset':'top.hostReset',
                     'crFile.nasti':'top.simulationMaster_ctrl','crFile.mcr':'sim.simulationMaster_mcr'})
    assert wiring(mapped) == expected, 'transport wiring differs'
    bound = boundaries[3]['GGSimulationMasterBoundWrapper']
    assert 'goldengate.simulationMasterSlave = 9 : i32' in bound
    regions = re.search(r'goldengate.controlRegions = \[(.*?)\]',a.bound.read_text())[1]
    assert re.search(r'name = "SimulationMaster_0", size = 16 : i64, slave = 9 : i32, start = 576 : i64',regions)
    scalar = binding_wiring(a.bound.read_text(),'GGSimulationMasterBoundWrapper')
    assert len(scalar)==32
    assert binding_wiring(a.bound.read_text(),'GGSimulationMasterBoundWrapper',aggregate=True)['sim.simulationMaster_ctrl.ar']=='sim.ctrl_read_dispatch_slave_9_ar'
    reference = clean(a.golden.read_text()); h,bank = module(reference,'SimulationMaster')
    eq = equations(module(reference,'FPGATop')[1]); bank_eq = equations(bank)
    slots = ['INIT_DONE','PRESENCE_READ','PRESENCE_WRITE']
    for i,name in enumerate(slots): assert bank_eq[f'crFile_io_mcr_read_{i}_bits']==name
    compared = []
    for pin in re.findall(r'\b(io_ctrl_\w+)\b',h):
        channel,leaf = pin.removeprefix('io_ctrl_').split('_',1)
        mp = 'SimulationMaster_0_'+pin; rp = 'ctrlInterconnect_io_slaves_9_'+channel+'_'+leaf
        assert eq.get(mp)==rp or eq.get(rp)==mp
        native_pin = 'sim.simulationMaster_ctrl.'+channel+'.'+leaf.replace('bits_','bits.')
        if channel in ('aw','w'):
            boundary = 'sim.ctrl_write_dispatch_slave_9_'+channel+'_'+leaf
            assert scalar[boundary if leaf=='ready' else native_pin] == (native_pin if leaf=='ready' else boundary)
        elif channel != 'ar':
            boundary = 'sim.ctrl_'+('read' if channel=='r' else 'write')+'_arb_in_9_'+leaf
            assert scalar[native_pin if leaf=='ready' else boundary] == (boundary if leaf=='ready' else native_pin)
        compared.append(pin)
    assert len(compared)==20
    router = module(reference,'NastiRouter')[1]
    for channel in ('aw','ar'):
        ranges = [(0,128)]+[(int(lo,16),int(hi,16)) for lo,hi in re.findall(r"25'h([0-9a-f]+) <= io_master_"+channel+r"_bits_addr & io_master_"+channel+r"_bits_addr < 25'h([0-9a-f]+)",router)]
        assert len(ranges)==12 and ranges[9]==(576,592)
    run([a.export_tool,a.attached,out/'mapped.mlir',out/'bound.mlir',out/'helpers.mlir'],'production')
    for n,path in [(2,out/'mapped.mlir'),(3,out/'bound.mlir')]:
        assert modules(path.read_text())==boundaries[n], 'production replay differs'
    exported = modules((out/'helpers.mlir').read_text())
    for name in ('GGSimulationMasterBank','GGSimulationMasterMCRFile'):
        assert exported[name]==boundaries[3][name], 'export rebuilt helper operations'
    run(['firtool',out/'helpers.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')
    constants = compare_rtl(out/'native.sv',a.golden,out,run)
    digest = lambda path: dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    result = dict(golden=digest(a.golden),candidate=digest(a.bound),result='matched',slave=9,
                  register_addresses=[576,580,584],sfc_pins=compared,scalar_bindings=32,
                  annotations=len(annos(a.bound)),preserved_modules=[len(x) for x in boundaries[:-1]],
                  response_constants=constants,simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(result,indent=2)+'\n');print(result['simulation'])


if __name__=='__main__': main()
