#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual emitted NIC loopback MMIO with unchanged Widget helpers.

The status input substitutes the already compared tFire boundary. This covers
one-word local F2 AXI, not stream allocation, header/driver or Linux acceptance.
"""
import argparse, hashlib, json, subprocess
from pathlib import Path
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('native-mlir','sfc-fir','output-dir'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args(); out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False);commands=[]
    def run(cmd,name):
        cmd=list(map(str,cmd));commands.append(cmd)
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with(out/(name+'.log')).open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    for kind,source in [('native',a.native_mlir),('sfc',a.sfc_fir)]:
        run(['firtool',source.resolve(),'--disable-all-randomization','--disable-annotation-unknown','-o',out/(kind+'.sv')],kind+'-lower')
    native=ports(module(clean((out/'native.sv').read_text()),'GGNICLoopbackControlTest')[0])
    sfc=ports(module(clean((out/'sfc.sv').read_text()),'Widget')[0])
    assert sfc['io_ctrl_aw_bits_addr']==['input',32] and sfc['io_ctrl_aw_bits_id']==['input',12]
    inputs={n:w for n,(d,w)in native.items()if d=='input'}
    outputs={n:w for n,(d,w)in native.items()if d=='output'}
    # SFC preserves the complete WidgetIO boundary, including unused fields.
    for i in range(2):
        for n,(d,w)in sfc.items():
            mapped={'clock':'hostClock','reset':'hostReset','doneInput':f'done{i}_input'}.get(n,n.replace('io_ctrl_',f'nicBridge_{i}_ctrl_'))
            assert native[mapped]==[d,w],(mapped,native.get(mapped),[d,w])
    test=['module check;']+[f'reg [{w-1}:0] {n}=0;'for n,w in inputs.items()]
    test += [f'wire [{w-1}:0] {pre}_{n};'for pre in ('n','s')for n,w in outputs.items()]
    test += ['GGNICLoopbackControlTest native('+','.join(f'.{n}({"n_"+n if d=="output" else n})'for n,(d,w)in native.items())+');']
    for i in range(2):
        pins=[]
        for n,(d,w)in sfc.items():
            mapped={'clock':'hostClock','reset':'hostReset','doneInput':f'done{i}_input'}.get(n,n.replace('io_ctrl_',f'nicBridge_{i}_ctrl_'))
            pins.append(f'.{n}({"s_"+mapped if d=="output" else mapped})')
        test += [f'Widget sfc{i}('+','.join(pins)+');']
    test+=['integer cycles=0,writes0=0,writes1=0,reads0=0,reads1=0,bStalls0=0,bStalls1=0,rStalls0=0,rStalls1=0,zeroStrobes=0,resetPending=0;','task compare;begin']
    for n,w in outputs.items():
        prefix=n[:n.index('_ctrl_')+6];leaf=n[len(prefix):]
        guard='cycles>0'
        if leaf.startswith('b_bits_'):guard+=f'&&n_{prefix}b_valid'
        if leaf.startswith('r_bits_'):guard+=f'&&n_{prefix}r_valid'
        test += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    test+=['end endtask','task tick;begin hostClock=0;#2;compare();']
    for i in range(2):
        pre=f'nicBridge_{i}_ctrl_'
        test += [f'writes{i}+=({pre}aw_valid&&n_{pre}aw_ready);reads{i}+=({pre}ar_valid&&n_{pre}ar_ready);',
                 f'bStalls{i}+=(n_{pre}b_valid&&!{pre}b_ready);rStalls{i}+=(n_{pre}r_valid&&!{pre}r_ready);',
                 f'zeroStrobes+=({pre}w_valid&&n_{pre}w_ready&&!{pre}w_bits_strb);resetPending+=(hostReset&&(!n_{pre}aw_ready||!n_{pre}ar_ready));']
    test+=['hostClock=1;#2;hostClock=0;#2;cycles++;compare();end endtask','initial begin hostReset=1;repeat(4)tick();hostReset=0;',
           'for(integer k=0;k<100000;k++)begin']
    # Independent request gaps/response stalls, all address bits/byte strobes,
    # status changes during held R, and reset while transactions are pending.
    for n,w in inputs.items():
        if n=='hostClock':continue
        val="$urandom"
        if n=='hostReset':val='(k%173==0)||(k%173==1)'
        elif n.endswith('_bits_len'):val='0'
        elif n.startswith('nicBridge_1_ctrl_')and n.endswith('_valid'):val='($urandom_range(0,7)==0)'
        elif n.startswith('nicBridge_1_ctrl_')and n.endswith('_ready'):val='($urandom_range(0,9)==0)'
        test+=[f'{n}={val};']
    test+=['tick();end',
           'if(writes0<1000||writes1<100||reads0<1000||reads1<100||bStalls0<100||bStalls1<100||rStalls0<100||rStalls1<100||zeroStrobes<100||resetPending<100)$fatal(1,"coverage");',
           '$display("PASS cycles=%0d writes=%0d/%0d reads=%0d/%0d Bstalls=%0d/%0d Rstalls=%0d/%0d zeroStrobes=%0d resetPending=%0d",cycles,writes0,writes1,reads0,reads1,bStalls0,bStalls1,rStalls0,rStalls1,zeroStrobes,resetPending);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','--assert','-j','2','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc.sv'],'build')
    run([out/'obj/check'],'simulation')
    result=dict(passed=True,full_feature_acceptance=False,scope=__doc__,cycles=100004,widgets=2,simulation=(out/'simulation.log').read_text().strip(),
                assumptions=['Aligned host reset; status sampled at already compared tFire boundary','Valid-qualified response data/IDs; unreset captures unspecified until handshake','No RAM clearing; singleton control bank contains no memory'],
                remaining=['Production LoopbackNIC selection','Allocated disabled CPU streams, header and driver initialization','Enabled representative semantic/LEC/SEC/F2 Linux packet checks'])
    result['sha256']={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest()for f in [a.native_mlir,a.sfc_fir,Path(__file__),out/'native.sv',out/'sfc.sv',out/'check.sv',out/'commands.json']}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(result['simulation'])


if __name__=='__main__':main()
