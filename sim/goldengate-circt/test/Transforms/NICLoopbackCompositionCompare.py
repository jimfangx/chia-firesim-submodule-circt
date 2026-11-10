#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the actual selected NIC loopback/control/CPU composition.

The unchanged complete SFC emitter retains its three unassigned pauser outputs.
Preserve that syntax failure. A distinct diagnostic copy explicitly invalidates
only those dead outputs; the native compiler proves their model payloads unused
and rejects live consumers. This comparison is supporting evidence, never a full
unnormalized SFC syntax pass, LEC/SEC, F2 shell or enabled Linux acceptance.
"""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
from NICLoopbackCompare import DRIVER as TOKEN_DRIVER


def driver(clock, reset):
    text=TOKEN_DRIVER.replace('cycle<12000','cycle<60000').replace('cycle>=2111&&cycle<2115','cycle>=20000&&cycle<20004').replace('cycle>=7901&&cycle<7905','cycle>=40000&&cycle<40004')
    text=text.replace('PASS cycles=12000','PASS cycles=60000')
    # Align synchronous initialized state before probing controls. Independent
    # random pre-reset flops are not an initial-state correspondence. No RAM
    # initialization is added; all 60,000 measured cycles remain strict.
    text=text.replace('d=new VLoop;', 'd=new VLoop;d->@RESET@=1;d->@CLOCK@=0;d->eval();d->@CLOCK@=1;d->eval();')
    text=text.replace('VLoop*d;','VLoop*d;unsigned ctrlAW=0,ctrlW=0,ctrlAR=0,cpuAW=0,cpuW=0,ctrlReads=0,cpuWrites=0;')
    text=text.replace(' d->@RESET@=reset;d->@CLOCK@=0;',r'''
 d->@RESET@=reset;d->@CLOCK@=0;
 if(reset){ctrlAW=ctrlW=ctrlAR=cpuAW=cpuW=0;}
 d->ctrl_aw_valid=!reset;d->ctrl_aw_bits_addr=16+4*(ctrlAW%2);d->ctrl_aw_bits_id=3;d->ctrl_aw_bits_len=0;d->ctrl_aw_bits_size=2;
 d->ctrl_aw_bits_burst=1;d->ctrl_aw_bits_lock=0;d->ctrl_aw_bits_cache=0;d->ctrl_aw_bits_prot=0;d->ctrl_aw_bits_qos=0;
 d->ctrl_w_valid=!reset;d->ctrl_w_bits_data=ctrlW*37+1;d->ctrl_w_bits_strb=ctrlW&15;d->ctrl_w_bits_last=1;d->ctrl_w_bits_id=3;
 d->ctrl_b_ready=cycle%11<7;
 const unsigned addresses[]={0,4,8,12,16,20,65536};
 d->ctrl_ar_valid=!reset;d->ctrl_ar_bits_addr=addresses[ctrlAR%7];d->ctrl_ar_bits_id=5;d->ctrl_ar_bits_len=0;d->ctrl_ar_bits_size=2;
 d->ctrl_ar_bits_burst=1;d->ctrl_ar_bits_lock=0;d->ctrl_ar_bits_cache=0;d->ctrl_ar_bits_prot=0;d->ctrl_ar_bits_qos=0;
 d->ctrl_r_ready=cycle%19<13;
 d->cpu_managed_axi4_aw_valid=!reset;d->cpu_managed_axi4_aw_bits_addr=262144ULL*(cpuAW%2);d->cpu_managed_axi4_aw_bits_id=17;d->cpu_managed_axi4_aw_bits_len=0;d->cpu_managed_axi4_aw_bits_size=6;
 d->cpu_managed_axi4_aw_bits_burst=1;d->cpu_managed_axi4_aw_bits_lock=0;d->cpu_managed_axi4_aw_bits_cache=0;d->cpu_managed_axi4_aw_bits_prot=0;d->cpu_managed_axi4_aw_bits_qos=0;
 d->cpu_managed_axi4_w_valid=!reset;d->cpu_managed_axi4_w_bits_strb=UINT64_MAX;d->cpu_managed_axi4_w_bits_last=1;
 for(unsigned i=0;i<16;++i)d->cpu_managed_axi4_w_bits_data[i]=0x9e3779b9u*(cpuW+i+1);
 d->cpu_managed_axi4_b_ready=cycle%7<5;
 d->cpu_managed_axi4_ar_valid=!reset;d->cpu_managed_axi4_ar_bits_addr=262144ULL*(cycle%2);d->cpu_managed_axi4_ar_bits_id=19;d->cpu_managed_axi4_ar_bits_len=0;d->cpu_managed_axi4_ar_bits_size=6;
 d->cpu_managed_axi4_ar_bits_burst=1;d->cpu_managed_axi4_ar_bits_lock=0;d->cpu_managed_axi4_ar_bits_cache=0;d->cpu_managed_axi4_ar_bits_prot=0;d->cpu_managed_axi4_ar_bits_qos=0;
 d->cpu_managed_axi4_r_ready=cycle%13<9;
''')
    text=text.replace(' d->eval();',r'''
 d->eval();
 if(!reset){ctrlAW+=d->ctrl_aw_valid&&d->ctrl_aw_ready;ctrlW+=d->ctrl_w_valid&&d->ctrl_w_ready;ctrlAR+=d->ctrl_ar_valid&&d->ctrl_ar_ready;
 cpuAW+=d->cpu_managed_axi4_aw_valid&&d->cpu_managed_axi4_aw_ready;cpuW+=d->cpu_managed_axi4_w_valid&&d->cpu_managed_axi4_w_ready;
 ctrlReads+=d->ctrl_r_valid&&d->ctrl_r_ready;cpuWrites+=d->cpu_managed_axi4_b_valid&&d->cpu_managed_axi4_b_ready;
 if(d->cpu_managed_axi4_r_valid)throw std::runtime_error("disabled NIC outgoing CPU queue became valid");}
''',1)
    text=text.replace(' std::printf(" d=%u:%u\\n",unsigned(d->nic0_done),unsigned(d->nic1_done));',r'''
 std::printf(" ctrl=%u:%u:%u:%u:%u cpu=%u:%u:%u:%u:%u",unsigned(d->ctrl_aw_ready),unsigned(d->ctrl_w_ready),unsigned(d->ctrl_ar_ready),unsigned(d->ctrl_b_valid),unsigned(d->ctrl_r_valid),unsigned(d->cpu_managed_axi4_aw_ready),unsigned(d->cpu_managed_axi4_w_ready),unsigned(d->cpu_managed_axi4_ar_ready),unsigned(d->cpu_managed_axi4_b_valid),unsigned(d->cpu_managed_axi4_r_valid));
 if(d->ctrl_b_valid)std::printf(" cb=%u:%u",unsigned(d->ctrl_b_bits_id),unsigned(d->ctrl_b_bits_resp));
 if(d->ctrl_r_valid)std::printf(" cr=%u:%u:%u:%u",unsigned(d->ctrl_r_bits_id),unsigned(d->ctrl_r_bits_data),unsigned(d->ctrl_r_bits_resp),unsigned(d->ctrl_r_bits_last));
 if(d->cpu_managed_axi4_b_valid)std::printf(" db=%u:%u",unsigned(d->cpu_managed_axi4_b_bits_id),unsigned(d->cpu_managed_axi4_b_bits_resp));
 std::puts("");
''')
    text=text.replace('std::fprintf(stderr,"METRICS', 'if(ctrlReads<1000||cpuWrites<15000)throw std::runtime_error("missing shared MMIO/CPU queue traffic");std::fprintf(stderr,"BUS reads=%u CPUwrites=%u\\n",ctrlReads,cpuWrites);\nstd::fprintf(stderr,"METRICS')
    values={f:''.join(f'case {w*15+j}:return d->w{w}_p{j}_{f.lower()};'for w in range(2)for j in range(12 if f=='BITS' else 15))for f in ['BITS','VALID','READY']}
    assignments=[]
    for w in range(2):
        for j,field in enumerate(['data','keep','last','valid']):assignments.append(f'd->w{w}_p{j}_bits=payload[{w}].{field};d->w{w}_p{j}_valid=source[{w}][{j}];')
        for j in range(4,15):assignments.append(f'd->w{w}_p{j}_ready=sink[{w}][{j-4}];')
    values.update(INPUTS='\n'.join(assignments),CLOCK=clock,RESET=reset)
    for key,value in values.items():text=text.replace('@'+key+'@',value)
    if clock=='clock':
        for w in range(2):
            for j in range(12,15):
                text=text.replace(f'd->w{w}_p{j}_ready',f'd->tokens_t_ready_w{w}_p{j}_ready')
                text=text.replace(f'd->w{w}_p{j}_valid',f'd->tokens_w{w}_p{j}_valid')
    return text


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('native-rtl','oracle','output','firtool','verilator'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);commands=[]
    def run(cmd,name,expected=0,trace=None):
        cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with(out/(name+'.log')).open('w')as log:
            if trace:
                with trace.open('w')as data:r=subprocess.run(cmd,stdout=data,stderr=log)
            else:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        assert (r.returncode==0)==(expected==0),(name,r.returncode)
        return r
    original=a.oracle.resolve()/'bound.fir'
    run([a.firtool.resolve(),original,'--disable-all-randomization','--disable-annotation-unknown','-o',out/'original.sv'],'retained-sfc-syntax',expected=1)
    diagnostic=(out/'retained-sfc-syntax.log').read_text();assert all('hPort_hBits_nic_pauser_'+n in diagnostic for n in ['threshold','quanta','refresh'])
    text=original.read_text();modules=re.findall(r'^  module SimpleNICBridgeModule(?:_\d+)? :$',text,re.M);assert len(modules)==2,modules
    # Diagnostic-only copy. Both exact production widgets retain their original
    # bodies; only their unassigned dead output payloads gain explicit invalids.
    for decl in modules:
        pos=text.index(decl);end=text.find('\n  module ',pos+1)
        if end<0:end=len(text)
        text=text[:end]+'\n    hPort.hBits.nic.pauser is invalid\n'+text[end:]
    normalized=out/'dead-pauser-invalidated.fir';normalized.write_text(text)
    run([a.firtool.resolve(),normalized,'--disable-all-randomization','--disable-annotation-unknown','-o',out/'sfc.sv'],'diagnostic-sfc-lower')
    traces=[]
    for kind,rtl,top,clock,reset in [('native',a.native_rtl.resolve(),'GGNICLoopbackCPUStreamPlatformWrapper','hostClock','hostReset'),('sfc',out/'sfc.sv','LazyModule','clock','reset')]:
        cpp=out/(kind+'.cpp');cpp.write_text(driver(clock,reset));obj=out/(kind+'-obj')
        run([a.verilator.resolve(),'--cc','--exe','--build','-j','1','-Wno-fatal','--top-module',top,'--prefix','VLoop','--Mdir',obj,rtl,cpp],kind+'-build')
        trace=out/(kind+'.trace');run([obj/'VLoop','+verilator+seed+101','+verilator+rand+reset+2'],kind+'-run',trace=trace);traces.append(trace)
    lines=[[x for x in f.read_text().splitlines()if x and x[0].isdigit()]for f in traces]
    assert len(lines[0])==len(lines[1])==60000
    for cycle,(native,sfc)in enumerate(zip(*lines)):
        assert native==sfc,('cycle mismatch',cycle,native,sfc)
    receipt=dict(passed_supporting_comparison=True,full_feature_acceptance=False,unnormalized_sfc_syntax=False,
        scope='actual two unchanged LoopbackNIC widgets, shared HasWidgets MMIO and buffered CPU engine; diagnostic invalidation of three provably dead pauser outputs/widget',cycles=60000,
        checks=['all 30 pipe handshakes','independent written packet data/keep/last/order and seed ledger','shared control reads/writes, IDs, strobes, response backpressure and DECERR','allocated disabled CPU queues filled through full AXI ingress; strict occupancy control reads and empty CPU output','three host resets without clearing RAM'],
        limits=['original complete SFC retains pauser initialization failure','native must reject any live pauser consumer','native driver corrects known SFC singleton ABI incompatibility','no complete target/model/clock/memory/F2 shell or enabled Linux representative','no LEC or SEC signoff'],
        initial_state='one synchronous host-reset edge before the first measured probe; RAM never cleared',
        metrics={k:'\n'.join(x for x in (out/(k+'-run.log')).read_text().splitlines()if x.startswith(('BUS ','METRICS ','PASS ')))for k in ['native','sfc']},
        sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest()for f in [a.native_rtl,original,normalized,out/'sfc.sv',*traces,out/'commands.json',Path(__file__)]})
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
