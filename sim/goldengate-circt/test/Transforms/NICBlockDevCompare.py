#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production NIC BlockDev assembly with the complete SFC widget.

FASED, NIC platform integration and target execution remain separate boundaries.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlWriteRoutingCompare import transfer
from NICTSICompare import token_wiring
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketBlockDevCompare import compare
from PrintRocketResponsesCompare import wiring

HELPERS = ('GGBlockDevTokenEngine', 'GGBlockDevRequestQueue10', 'GGBlockDevDataQueue32',
           'GGBlockDevWriteAckQueue4', 'GGBlockDevMMIOBank', 'GGBlockDevMCRFile',
           'GGBlockDevWriteLatencyPipe', 'GGBlockDevReadLatencyPipe', 'GGBlockDevResponseScheduler')
STAGES = ('tsi-bound', 'blockdev-token', 'blockdev-request-queue', 'blockdev-data-queue',
          'blockdev-read-response-queue', 'blockdev-write-ack-queue', 'blockdev-mmio',
          'blockdev-control', 'blockdev-bound', 'blockdev-write-latency',
          'blockdev-read-latency', 'blockdev-response-scheduler')
TOPS = ('GGTSIBridgeBoundWrapper', 'GGBlockDevTokenWrapper', 'GGBlockDevRequestQueueWrapper',
        'GGBlockDevDataQueueWrapper', 'GGBlockDevReadResponseQueueWrapper', 'GGBlockDevWriteAckQueueWrapper',
        'GGBlockDevMMIOWrapper', 'GGBlockDevBridgeControlWrapper', 'GGBlockDevBridgeBoundWrapper',
        'GGBlockDevWriteLatencyWrapper', 'GGBlockDevReadLatencyWrapper', 'GGBlockDevResponseSchedulerWrapper')
LOCALS = ('bdev_req_fwd', 'bdev_data_fwd', 'bdev_resp_rev', 'reset', 'bdev_req_rev',
          'bdev_data_rev', 'bdev_resp_fwd', 'bdev_info_nsectors', 'bdev_info_max_req_len')
FIELDS = (('bdev.req.bits.tag', 'bdev.req.bits.len', 'bdev.req.bits.offset', 'bdev.req.bits.write', 'bdev.req.valid'),
          ('bdev.data.bits.tag', 'bdev.data.bits.data', 'bdev.data.valid'), ('bdev.resp.ready',), ('reset',),
          ('bdev.req.ready',), ('bdev.data.ready',), ('bdev.resp.bits.tag', 'bdev.resp.bits.data', 'bdev.resp.valid'),
          ('bdev.info.nsectors',), ('bdev.info.max_req_len',))


def token_connections(before, after, annotations):
    bridge = next(x for x in annotations if x.get('widgetClass') == 'firechip.goldengateimplementations.BlockDevBridgeModule')
    channels = [next(x for x in annotations if x.get('globalName') == bridge['channelMapping'][n]) for n in LOCALS]
    roots = [re.search(r'>(\w+)\.', x['sources' if j < 4 else 'sinks'][0])[1] for j, x in enumerate(channels)]
    engine = 'BlockDevBridgeModule_0.'
    expected = {('sim.'+n if d=='in' else 'top.'+n): ('top.'+n if d=='in' else 'sim.'+n)
                for d,n,_ in native_ports(before) if n not in roots}
    expected.update({engine+'clock':'top.hostClock', engine+'reset':'top.hostReset',
                     engine+'hPort.toHost.hValid':'&'.join('sim.'+n+'.valid' for n in roots[:4]),
                     engine+'hPort.fromHost.hReady':'&'.join('sim.'+n+'.ready' for n in roots[4:])})
    for j,n in enumerate(roots):
        expected['sim.'+n+('.ready' if j<4 else '.valid')] = engine+('hPort.toHost.hReady' if j<4 else 'hPort.fromHost.hValid')+''.join(
            '&sim.'+p+('.valid' if j<4 else '.ready') for k,p in enumerate(roots) if k!=j and (k<4)==(j<4))
        for f in FIELDS[j]:
            # Canonical FAME payload strips the bdev interface prefix.
            suffix = f.split('.')[-1]
            remote = 'sim.'+n+'.bits'
            if len(FIELDS[j]) > 1:
                remote += '.valid' if suffix=='valid' else '.bits.'+suffix
            local = engine+'hPort.hBits.'+f
            expected[local if j<4 else remote] = remote if j<4 else local
    for external, internal in (
        ('req_enq','reqBuf_io_enq'), ('data_enq','dataBuf_io_enq'), ('rresp_deq','rRespBuf_io_deq'),
        ('wack_deq','wAckBuf_io_deq'), ('timing','timing'), ('info','info'), ('tfire','tFire'),
        ('queue_reset','queueReset'), ('write_latency_enq_valid','writeLatencyEnqValid'),
        ('read_latency_enq','readLatencyEnq'), ('resp_ready','respReady')):
        inbound = external in ('rresp_deq','wack_deq','timing','info')
        local, remote = engine+internal, 'top.blockdev_'+external
        expected[local if inbound else remote] = remote if inbound else local
    assert token_wiring(after) == {d:tuple(sorted(s.split('&'))) for d,s in expected.items()}, 'production token wiring differs'


def assembly_connections(text):
    """Check the production connections reproduced by the wire-only assembly."""
    checks = {}
    for kind, inst, stem, inbound in (
        ('Request', 'reqBuf', 'req', False), ('Data', 'dataBuf', 'data', False),
        ('ReadResponse', 'rRespBuf', 'rresp', True), ('WriteAck', 'wAckBuf', 'wack', True)):
        checks['GGBlockDev'+kind+'QueueWrapper'] = (
            {inst+'.enq': 'top.blockdev_'+stem+'_enq', 'sim.blockdev_'+stem+'_deq': inst+'.deq'} if inbound else
            {inst+'.enq': 'sim.blockdev_'+stem+'_enq', 'top.blockdev_'+stem+'_deq': inst+'.deq'})
    checks['GGBlockDevMMIOWrapper'] = {
        'blockdevRegisters.timing.wAckStallN': 'sim.blockdev_timing.wAckStallN',
        'blockdevRegisters.timing.rRespStallN': 'sim.blockdev_timing.rRespStallN',
        'top.blockdevBridge_mcr': 'blockdevRegisters.mcr'}
    checks['GGBlockDevBridgeControlWrapper'] = {
        'crFile.clock': 'top.hostClock', 'crFile.reset': 'top.hostReset',
        'crFile.nasti': 'top.blockdevBridge_ctrl', 'crFile.mcr': 'sim.blockdevBridge_mcr'}
    for kind in ('Read', 'Write'):
        stem = kind.lower(); inst = stem+'LatencyPipe'; port = 'blockdev_'+stem+'_latency_'
        nets = {inst+'.enq_valid': 'sim.'+port+('enq.valid' if kind=='Read' else 'enq_valid'),
                inst+'.deq_ready': 'top.'+port+'deq.ready',
                'top.'+port+'deq.valid': inst+'.deq_valid',
                'top.'+port+'enq_ready': inst+'.enq_ready'}
        if kind=='Read':
            nets.update({inst+'.enq_bits': 'sim.'+port+'enq.bits', 'top.'+port+'deq.bits': inst+'.deq_bits'})
        checks['GGBlockDev'+kind+'LatencyWrapper'] = nets
    for wrapper, expected in checks.items():
        actual = wiring(text, wrapper, aggregate=True)
        for dest, src in expected.items(): assert actual.get(dest)==src, (wrapper, dest, actual.get(dest), src)
    return sum(len(nets) for nets in checks.values())


def behavior(native, golden, out, run):
    text = clean(native.read_text())
    interfaces = {n:ports(module(text,n)[0]) for n in HELPERS}
    top = {'clock':['input',1], 'reset':['input',1]}
    top.update({n:v for n,v in interfaces[HELPERS[0]].items() if n.startswith('hPort_')})
    top.update({'io_ctrl_'+n.removeprefix('nasti_'):v for n,v in interfaces[HELPERS[5]].items() if n.startswith('nasti_')})
    # Wires only: all behavior comes from cloned, verified production MLIR.
    instances = [
        ('engine', HELPERS[0], {'reqBuf_io_enq':'req_enq', 'dataBuf_io_enq':'data_enq',
          'rRespBuf_io_deq':'rresp_deq', 'wAckBuf_io_deq':'wack_deq',
          'writeLatencyEnqValid':'write_enq_valid', 'readLatencyEnq':'read_enq', 'respReady':'resp_ready'}),
        ('reqBuf', HELPERS[1], {'enq':'req_enq','deq':'req_deq','reset':'queueReset'}),
        ('dataBuf', HELPERS[2], {'enq':'data_enq','deq':'data_deq','reset':'queueReset'}),
        ('rRespBuf', HELPERS[2], {'enq':'rresp_enq','deq':'rresp_deq','reset':'queueReset'}),
        ('wAckBuf', HELPERS[3], {'enq':'wack_enq','deq':'wack_deq','reset':'queueReset'}),
        ('registers', HELPERS[4], {'reqBuf':'req_deq','dataBuf':'data_deq','rRespBuf':'rresp_enq','wAckBuf':'wack_enq'}),
        ('crFile', HELPERS[5], {'nasti':'io_ctrl'}),
        ('writeLatencyPipe', HELPERS[6], {'enq':'write_enq','deq':'write_deq','latency':'latency_write_latency','tCycle':'timing_tCycle','reset':'queueReset'}),
        ('readLatencyPipe', HELPERS[7], {'enq':'read_enq','deq':'read_deq','latency':'latency_read_latency','tCycle':'timing_tCycle','reset':'queueReset'}),
        ('scheduler', HELPERS[8], {'write_valid':'write_deq_valid','write_ready':'write_deq_ready',
          'read_valid':'read_deq_valid','read_ready':'read_deq_ready','read_bits':'read_deq_bits',
          'returnWrite':'timing_returnWrite','readRespBusy':'timing_readRespBusy','reset':'queueReset'})]
    def net(n, mapping):
        for prefix,replacement in mapping.items():
            if n==prefix or n.startswith(prefix+'_'): return replacement+n[len(prefix):]
        return n
    nets=dict(top); drivers={}
    for inst,symbol,mapping in instances:
        for n,(d,w) in interfaces[symbol].items():
            wire=net(n,mapping)
            assert wire not in nets or nets[wire][1]==w, wire
            nets[wire]=[d,w]
            if d=='output':
                assert wire not in drivers, ('duplicate driver',wire)
                drivers[wire]=inst+'.'+n
    assert all(n in drivers for n,(d,w) in top.items() if d=='output')
    assert all(n in drivers or n in top for n in nets), ('undriven assembly wire',set(nets)-set(drivers)-set(top))
    lines=['module NativeBlockDev('+','.join(f'{d} [{w-1}:0] {n}' for n,(d,w) in top.items())+');']
    lines += [f'wire [{w-1}:0] {n};' for n,(d,w) in nets.items() if n not in top]
    for inst,symbol,mapping in instances:
        lines += [symbol+' '+inst+'('+','.join(f'.{n}({net(n,mapping)})' for n in interfaces[symbol])+');']
    (out/'native-wrapper.sv').write_text('\n'.join(lines+['endmodule'])+'\n')
    reference=clean(golden.read_text()); sh,sb=module(reference,'BlockDevBridgeModule'); sp=ports(sh)
    assert all(top[n]==v for n,v in sp.items())
    names=['BlockDevBridgeModule']
    for inst in ('reqBuf','dataBuf','rRespBuf','wAckBuf','writeLatencyPipe','readLatencyPipe','crFile'):
        names.append(re.search(r'\b(\w+)\s+'+inst+r'\s*\(',sb)[1])
    (out/'sfc.sv').write_text('\n'.join('module '+n+'('+module(reference,n)[0]+');'+module(reference,n)[1]+'endmodule' for n in dict.fromkeys(names))+'\n')
    inputs=[(n,w) for n,(d,w) in top.items() if d=='input']; outputs=[(n,w) for n,(d,w) in top.items() if d=='output']
    constants={'io_ctrl_b_bits_resp':0,'io_ctrl_b_bits_user':0,'io_ctrl_r_bits_resp':0,'io_ctrl_r_bits_last':1,'io_ctrl_r_bits_user':0}
    assert {n for n,w in outputs if n not in sp}==set(constants)
    tb=['module check;']+[f'reg [{w-1}:0] {n}=0;' for n,w in inputs]
    tb += [f'wire [{w-1}:0] {p}_{n};' for p in ('n','s') for n,w in outputs]
    for symbol,ps,p in [('NativeBlockDev',top,'n'),('BlockDevBridgeModule',sp,'s')]:
        tb += [symbol+' '+p+'ative('+','.join(f'.{n}({p+"_"+n if d=="output" else n})' for n,(d,w) in ps.items())+');']
    tb += [f'assign s_{n}={w}\'d{constants[n]};' for n,w in outputs if n in constants]
    tb += ['integer cycles=0,fires=0,responses=0,writeResponses=0,readResponses=0,stalledReset=0,blockedReset=0,bStalls=0,rStalls=0; task compare;begin']
    for n,w in outputs:
        guard = ('n_io_ctrl_b_valid' if n.startswith('io_ctrl_b_bits_') else 'n_io_ctrl_r_valid' if n.startswith('io_ctrl_r_bits_') else '1')
        tb += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    tb += ['end endtask',
        'task tick;begin clock=0;#2;compare();fires+=n_hPort_toHost_hReady;',
        'responses+=n_hPort_toHost_hReady&&n_hPort_hBits_bdev_resp_valid&&hPort_hBits_bdev_resp_ready;',
        'writeResponses+=native.engine.timing_returnWrite&&n_hPort_toHost_hReady;readResponses+=native.engine.timing_readRespBusy&&n_hPort_toHost_hReady;',
        'stalledReset+=hPort_hBits_reset&&!n_hPort_toHost_hReady;blockedReset+=native.queueReset&&!n_hPort_toHost_hReady;',
        'bStalls+=n_io_ctrl_b_valid&&!io_ctrl_b_ready;rStalls+=n_io_ctrl_r_valid&&!io_ctrl_r_ready;',
        'clock=1;#2;clock=0;#2;cycles++;compare();end endtask',
        'task idle;begin io_ctrl_aw_valid=0;io_ctrl_w_valid=0;io_ctrl_ar_valid=0;io_ctrl_b_ready=0;io_ctrl_r_ready=0;end endtask',
        'task flush;begin idle();reset=1;tick();reset=0;end endtask',
        'task writeword(input integer index,input integer data);begin idle();io_ctrl_aw_bits_addr=index<<2;io_ctrl_aw_bits_len=0;io_ctrl_aw_bits_id=$urandom;',
        'io_ctrl_w_bits_data=data;io_ctrl_w_bits_last=1;io_ctrl_w_bits_strb=$urandom;io_ctrl_aw_valid=1;io_ctrl_w_valid=1;tick();io_ctrl_aw_valid=0;io_ctrl_w_valid=0;',
        'repeat(3)tick();if(!n_io_ctrl_b_valid)$fatal(1,"missing B");io_ctrl_b_ready=1;tick();idle();end endtask',
        'task request(input bit writeReq);begin hPort_hBits_bdev_req_bits_write=writeReq;hPort_hBits_bdev_req_bits_len=1;hPort_hBits_bdev_req_bits_offset=$urandom;',
        'hPort_hBits_bdev_req_valid=1;do tick();while(!n_hPort_toHost_hReady);hPort_hBits_bdev_req_valid=0;end endtask',
        'initial begin flush();hPort_toHost_hValid=1;hPort_fromHost_hReady=1;hPort_hBits_bdev_resp_ready=1;',
        'writeword(0,1);writeword(1,1);writeword(2,4096);writeword(3,16);',
        '// Read reaches its deadline before software data; inject and consume 64 beats.',
        'request(0);repeat(6)tick();writeword(15,32\'hdeadbeef);writeword(16,32\'h12345678);writeword(17,0);',
        'repeat(64)writeword(18,1);repeat(6)tick();writeword(9,1);',
        '// One complete sector write, then a delayed acknowledgement.',
        'request(1);repeat(64)begin hPort_hBits_bdev_data_valid=1;hPort_hBits_bdev_data_bits_data={$urandom,$urandom};tick();hPort_hBits_bdev_data_valid=0;writeword(14,1);end',
        'hPort_hBits_bdev_data_valid=0;repeat(6)tick();writeword(20,0);writeword(21,1);repeat(6)tick();',
        '// Fill request queue. Target reset must flush it even when tFire is blocked.',
        'flush();repeat(12)begin hPort_hBits_bdev_req_valid=1;tick();end hPort_hBits_bdev_req_valid=0;',
        'hPort_hBits_reset=1;tick();hPort_hBits_reset=0;repeat(4)tick();',
        'for(integer j=0;j<40000;j++)begin',
        'reset=($urandom_range(0,199)==0);io_ctrl_aw_valid=$urandom;io_ctrl_w_valid=$urandom;io_ctrl_ar_valid=$urandom;',
        'io_ctrl_aw_bits_addr=($urandom&25\'h1ffff80)|($urandom_range(0,31)<<2);io_ctrl_aw_bits_id=$urandom;',
        'io_ctrl_w_bits_data=$urandom;io_ctrl_w_bits_strb=$urandom;io_ctrl_w_bits_last=1;io_ctrl_ar_bits_addr=($urandom&25\'h1ffff80)|($urandom_range(4,31)<<2);io_ctrl_ar_bits_id=$urandom;',
        'io_ctrl_b_ready=$urandom;io_ctrl_r_ready=$urandom;hPort_toHost_hValid=$urandom;hPort_fromHost_hReady=$urandom;',
        'hPort_hBits_reset=($urandom_range(0,15)==0);hPort_hBits_bdev_req_valid=0;hPort_hBits_bdev_data_valid=0;hPort_hBits_bdev_resp_ready=$urandom;tick();end',
        '$display("coverage cycles=%0d fires=%0d responses=%0d write=%0d read=%0d stalledReset=%0d blockedReset=%0d bStalls=%0d rStalls=%0d",cycles,fires,responses,writeResponses,readResponses,stalledReset,blockedReset,bStalls,rStalls);',
        'if(fires<100||responses<65||writeResponses<1||readResponses<64||stalledReset<5||blockedReset<1||bStalls<100||rStalls<100)$fatal(1,"coverage");',
        '$display("PASS SFC NIC BlockDev");$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','-j','4','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'native-wrapper.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    return names


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir','golden','export-tool','output-dir'): p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True,timeout=180)
    paths=[a.candidate_dir/('post-fame-'+s+'.mlir') for s in STAGES]
    boundaries=[modules(x.read_text()) for x in paths]
    annos=lambda path:json.loads(path.with_name(path.stem+'-all.json').read_text())
    for i,(old,new) in enumerate(zip(TOPS,TOPS[1:])):
        assert all(boundaries[i+1][n]==body for n,body in boundaries[i].items()), 'prior module changed at '+new
        copied={n for _,n,_ in native_ports(boundaries[i][old])}&{n for _,n,_ in native_ports(boundaries[i+1][new])}
        expected=transfer(annos(paths[i]),old,new,copied)
        if i==0:
            bridge=next(x for x in expected if x.get('widgetClass')=='firechip.goldengateimplementations.BlockDevBridgeModule')
            bridge['target']='~GGBlockDevTokenWrapper|GGBlockDevTokenEngine>hPort'
            for j,local in enumerate(LOCALS):
                channel=next(x for x in expected if x.get('globalName')==bridge['channelMapping'][local])
                channel['sinks' if j<4 else 'sources']=['~GGBlockDevTokenWrapper|GGBlockDevTokenEngine>hPort.hBits.'+f for f in FIELDS[j]]
        assert annos(paths[i+1])==expected, 'annotation transfer differs at '+new
    token_connections(boundaries[0][TOPS[0]],boundaries[1][TOPS[1]],annos(paths[0]))
    structural=compare(a.golden,[paths[-1]],region_count=12)
    assembly_nets=assembly_connections(paths[-1].read_text())
    run([a.export_tool,paths[-1],out/'helpers.mlir'],'export')
    for n,b in modules((out/'helpers.mlir').read_text()).items(): assert b==boundaries[-1][n], 'export rebuilt '+n
    run(['firtool',out/'helpers.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')
    deps=behavior(out/'native.sv',a.golden,out,run)
    digest=lambda path:dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    result=dict(golden=digest(a.golden),candidate=digest(paths[-1]),result='matched',stages=11,
                annotations=len(annos(paths[-1])),preserved_modules=[len(x) for x in boundaries[:-1]],
                structure=structural,additional_assembly_nets=assembly_nets,
                sfc_dependencies=deps,simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(result,indent=2)+'\n');print(result['simulation'])


if __name__=='__main__':main()
