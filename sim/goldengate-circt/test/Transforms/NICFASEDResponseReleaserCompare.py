#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute production NIC response release against complete SFC gated-clock RTL.

This establishes response occupancy, forwarding and boundary composition only.
NIC timing/control responses and platform integration remain unsupported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDReadBufferCompare import wrapper_wiring
from PrintSelectedFASEDResponseReleaserCompare import transfer

HELPER = 'GGFASEDResponseReleaser'
OLD, NEW = 'GGFASEDWriteEgressWrapper', 'GGFASEDResponseReleaserWrapper'


def composition(directory, baseline):
    prior = modules((directory / 'post-fame-fased-write-egress.mlir').read_text())
    actual = modules((directory / 'post-fame-fased-response-releaser.mlir').read_text())
    assert prior and all(actual.get(n) == b for n,b in prior.items()), 'existing module changed'
    assert set(actual) - set(prior) == {HELPER, NEW}
    reference = modules(baseline.read_text())
    assert actual[HELPER] == reference[HELPER], 'shared helper changed'
    copied = [p for p in native_ports(prior[OLD]) if p[1] not in (
        'fased_timing', 'fased_read_egress_req', 'fased_read_egress_resp',
        'fased_write_egress_req', 'fased_write_egress_resp')]
    ports = native_ports(actual[NEW])
    assert ports[:len(copied)] == copied
    assert ports[len(copied):] == native_ports(reference[NEW])[-3:]
    expected = {'releaser.clock': 'top.hostClock', 'releaser.reset': 'sim.fased_model_reset',
                'releaser.targetFire': 'sim.fased_tfire',
                'releaser.nextRead': 'top.fased_next_read',
                'releaser.nextWrite': 'top.fased_next_write'}
    for ch in ('aw','w','ar'):
        expected['top.fased_timing_requests.' + ch] = 'sim.fased_timing.' + ch
    for ch,kind in [('r','read'),('b','write')]:
        expected.update({f'releaser.{ch}.ready': f'sim.fased_timing.{ch}.ready',
            f'sim.fased_timing.{ch}.valid': f'releaser.{ch}.valid',
            f'sim.fased_timing.{ch}.bits': f'releaser.{ch}.bits',
            f'sim.fased_{kind}_egress_req': f'releaser.egressReq.{ch}',
            f'releaser.egressResp.{ch}Bits': f'sim.fased_{kind}_egress_resp.tBits',
            f'sim.fased_{kind}_egress_resp.tReady': f'releaser.egressResp.{ch}Ready'})
    for direction,name,typ in copied:
        dest,src = ('sim.'+name,'top.'+name) if direction=='in' else ('top.'+name,'sim.'+name)
        expected[dest] = src
    assert wrapper_wiring(actual[NEW]) == expected, 'production releaser wiring differs'
    before = json.loads((directory / 'post-fame-fased-write-egress-all.json').read_text())
    after = json.loads((directory / 'post-fame-fased-response-releaser-all.json').read_text())
    assert transfer(before,OLD,NEW,{p[1] for p in copied}) == after
    assert len(re.findall(' = firrtl.regreset ',actual[HELPER])) == 2
    assert ' = firrtl.reg ' not in actual[HELPER] and 'firrtl.mem ' not in actual[HELPER]
    return dict(existing_modules_retained=len(prior), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(after), occupancy_registers=2)


def reference_boundary(golden, immutable):
    text,reference = clean(golden.read_text()),clean(immutable.read_text())
    release = module(text,'AXI4Releaser')
    assert release == module(reference,'AXI4Releaser'), 'immutable SFC releaser differs'
    queues = re.findall(r'\b(\w+) current(?:Read|Write)_q \(',release[1])
    assert len(queues)==2
    for queue in queues:
        assert module(text,queue)==module(reference,queue), 'immutable SFC occupancy queue differs'
    host = equations(module(text,'FASEDMemoryTimingModel')[1])
    assert host['gate_I']=='clock' and host['model_clock']=='gate_O'
    assert host['model_reset']=='hPort_hBits_reset'
    assert ' '.join(host['gate_CE'].split())==' '.join(host['targetFire'].split())
    pipe = equations(module(text,'LatencyPipe')[1])
    assert pipe['xactionRelease_clock']=='clock' and pipe['xactionRelease_reset']=='reset'
    for ch in ('r','b'):
        assert pipe[f'xactionRelease_io_{ch}_ready']==f'tNasti_io_tNasti_{ch}_ready'
        assert host[f'model_tNasti_io_egressResp_{ch}Bits_id']==f'{"read" if ch=="r" else "write"}Egress_io_resp_tBits_id'
    return queues


def differential(candidate,golden,export_tool,out,queues):
    out.mkdir(parents=True,exist_ok=False)
    def run(command,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,command)),stdout=log,stderr=subprocess.STDOUT,check=True)
    exported,native = out/'response-releaser.mlir',out/'response-releaser.sv'
    run([export_tool,candidate,exported],'export')
    assert modules(exported.read_text().replace('firrtl.module public @','firrtl.module @')) == {
        HELPER: modules(candidate.read_text())[HELPER]}, 'export rebuilt production helper'
    run(['firtool',exported,'--format=mlir','--disable-all-randomization','-o',native],'firtool')
    text = clean(golden.read_text())
    header,body = module(text,'AXI4Releaser')
    oracle = 'module SFCReleaser('+header+');'+body+'endmodule\n'
    for queue in queues:
        h,b = module(text,queue)
        oracle += 'module '+queue+'('+h+');'+b+'endmodule\n'
    (out/'oracle.sv').write_text(oracle)
    connections,decls,checks = [],[],[]
    for direction,width,name in re.findall(r'\b(input|output)\s*(\[[^\]]+\])?\s*(\w+)\s*(?:,|$)',header.strip()):
        signal = name.removeprefix('io_')
        if name=='clock': signal='modelClock'
        if direction=='output':
            decls.append(f'wire {width} sfc_{signal};')
            connections.append(f'.{name}(sfc_{signal})')
            checks.append(f'if({signal}!==sfc_{signal})$fatal(1,"{signal} cycle=%0d",cycles);')
        else: connections.append(f'.{name}({signal})')
    assert len(connections)==26 and len(checks)==14
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0;
wire modelClock=clock & targetFire;
reg b_ready=0,r_ready=0,nextRead_valid=0,nextWrite_valid=0;
reg [3:0] nextRead_bits_id=0,nextWrite_bits_id=0;
reg [7:0] nextRead_bits_len=0;
reg egressResp_bBits_user=0,egressResp_rBits_user=0,egressResp_rBits_last=0;
reg [3:0] egressResp_bBits_id=0,egressResp_rBits_id=0;
reg [1:0] egressResp_bBits_resp=0,egressResp_rBits_resp=0;
reg [63:0] egressResp_rBits_data=0;
wire b_valid,b_bits_user,r_valid,r_bits_user,r_bits_last;
wire [3:0] b_bits_id,r_bits_id,egressReq_b_bits,egressReq_r_bits;
wire [1:0] b_bits_resp,r_bits_resp;
wire [63:0] r_bits_data;
wire egressReq_b_valid,egressReq_r_valid,egressResp_bReady,egressResp_rReady;
wire nextRead_ready,nextWrite_ready;
@DECLS@
GGFASEDResponseReleaser candidate(.*);
SFCReleaser oracle(@CONNECTIONS@);
integer cycles=0,replacements=0,nonLast=0,blocked=0,stalledReset=0;
integer stalledAccept=0,emptyPush=0,invalidForward=0,i;
reg [15:0] readIDs=0,writeIDs=0;
reg [31:0] rng=32'h198ab;
task compare;
begin
 @CHECKS@
 if(candidate.currentRead_full!==oracle.currentRead_q.maybe_full ||
    candidate.currentWrite_full!==oracle.currentWrite_q.maybe_full)
   $fatal(1,"occupancy differs cycle=%0d",cycles);
 if(b_bits_user!==egressResp_bBits_user || b_bits_resp!==egressResp_bBits_resp ||
    r_bits_user!==egressResp_rBits_user || r_bits_resp!==egressResp_rBits_resp)
   $fatal(1,"metadata forwarding differs cycle=%0d",cycles);
end endtask
task step;
begin
 #1;if(cycles>0)compare();
 replacements+=targetFire&&!reset&&r_valid&&r_ready&&r_bits_last&&egressReq_r_valid;
 nonLast+=r_valid&&r_ready&&!r_bits_last;
 blocked+=nextRead_valid&&!nextRead_ready;
 stalledReset+=reset&&!targetFire&&(r_valid||b_valid);
 stalledAccept+=!targetFire&&(egressReq_r_valid||egressReq_b_valid);
 emptyPush+=targetFire&&!reset&&!r_valid&&egressReq_r_valid;
 invalidForward+=!r_valid||!b_valid;
 if(targetFire&&!reset&&egressReq_r_valid)readIDs[nextRead_bits_id]=1;
 if(targetFire&&!reset&&egressReq_b_valid)writeIDs[nextWrite_bits_id]=1;
 clock=1;#1;compare();clock=0;#1;cycles++;
end endtask
initial begin
 reset=1;targetFire=1;step();reset=0;
 for(i=0;i<16;i++)begin
  nextRead_bits_id=i;nextWrite_bits_id=i;nextRead_valid=1;nextWrite_valid=1;
  r_ready=1;b_ready=1;egressResp_rBits_last=1;targetFire=1;step();
  targetFire=0;reset=1;step();reset=0;
  targetFire=1;egressResp_rBits_last=0;step();
  targetFire=0;step();
  targetFire=1;egressResp_rBits_last=1;step();
 end
 for(i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>17);rng=rng^(rng<<5);
  nextRead_bits_id=rng[3:0];nextWrite_bits_id=rng[7:4];
  nextRead_valid=rng[8];nextWrite_valid=rng[9];targetFire=rng[10];
  r_ready=rng[11];b_ready=rng[12];egressResp_rBits_last=rng[13];
  egressResp_rBits_id=rng[17:14];egressResp_bBits_id=rng[21:18];
  egressResp_rBits_data={rng,~rng};nextRead_bits_len=rng[29:22];
  egressResp_rBits_user=rng[30];egressResp_bBits_user=rng[31];
  egressResp_rBits_resp=rng[1:0];egressResp_bBits_resp=rng[3:2];
  reset=(i%137==0);step();
 end
 if(replacements<100||nonLast<100||blocked<100||stalledReset<10||
    stalledAccept<100||emptyPush<100||invalidForward<100||
    readIDs!==16'hffff||writeIDs!==16'hffff)$fatal(1,"missing coverage");
 $display("PASS cycles=%0d replacements=%0d non_last=%0d blocked=%0d stalled_reset=%0d stalled_accept=%0d empty_push=%0d invalid_forward=%0d read_IDs=%0d write_IDs=%0d",
 cycles,replacements,nonLast,blocked,stalledReset,stalledAccept,emptyPush,invalidForward,readIDs,writeIDs);
 $finish;
end endmodule
'''.replace('@DECLS@','\n'.join(decls)).replace('@CONNECTIONS@',','.join(connections)).replace('@CHECKS@','\n'.join(checks))
    (out/'check.sv').write_text(tb)
    run(['verilator','--binary','-j','4','--timing','-Wno-fatal','--top-module','check',
         '--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'oracle.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    log = (out/'simulation.log').read_text()
    assert 'PASS cycles=' in log
    return dict({n:int(v) for n,v in re.findall(r'(\w+)=(\d+)',log)},
                production_helper_cloned_verbatim=True, actual_sfc_sequential_rtl=True,
                both_occupancies_compared_before_and_after_every_edge=True,
                oracle_clock_qualified_by_targetFire=True,
                metadata_forwarding_checked_against_scala_contract=True,
                sfc_user_resp_and_unused_length_ports_pruned=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir','baseline','golden','immutable','export-tool','output-dir','report-json'):
        p.add_argument('--'+name,type=Path,required=True)
    a = p.parse_args()
    queues = reference_boundary(a.golden,a.immutable)
    candidate = a.candidate_dir/'post-fame-fased-response-releaser.mlir'
    result = composition(a.candidate_dir,a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['AXI4Releaser',*queues,'LatencyPipe','FASEDMemoryTimingModel'],
                  rtl_differential=differential(candidate,a.golden,a.export_tool,a.output_dir,queues),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS NIC production response release, composition and complete gated SFC RTL differential')


if __name__=='__main__': main()
