#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production write acknowledgment state with complete SFC RTL.

Check the emitted helper, wrapper connections and recursive annotation transfer.
Clone the actual helper for RTL execution against the complete archived SFC
WriteEgress module. Response release, timing/control and NIC platform support
remain incomplete; this comparison establishes the write egress boundary only.
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
from PrintSelectedFASEDWriteEgressCompare import transfer

HELPER = 'GGFASEDWriteEgress'
OLD, NEW = 'GGFASEDReadSchedulerWrapper', 'GGFASEDWriteEgressWrapper'


def composition(directory, baseline):
    prior = modules((directory / 'post-fame-fased-read-scheduler.mlir').read_text())
    actual = modules((directory / 'post-fame-fased-write-egress.mlir').read_text())
    assert prior and all(actual.get(n) == b for n, b in prior.items()), 'existing module changed'
    assert set(actual) - set(prior) == {HELPER, NEW}
    baseline_modules = modules(baseline.read_text()) if baseline is not None else None
    if baseline_modules is not None:
        assert actual[HELPER] == baseline_modules[HELPER], 'shared helper changed'
    copied = [p for p in native_ports(prior[OLD]) if p[1] not in (
        'fased_write_egress_valid', 'fased_host_write_responses')]
    ports = native_ports(actual[NEW])
    assert ports[:len(copied)] == copied
    assert [p[1] for p in ports[len(copied):]] == [
        'fased_host_write_response', 'fased_write_egress_req', 'fased_write_egress_resp']
    if baseline_modules is not None:
        assert ports[len(copied):] == native_ports(baseline_modules[NEW])[-3:], 'write boundary types differ'
    read_wiring = wrapper_wiring(prior[OLD])
    assert read_wiring['sim.fased_egress_readiness.writeValid'] == 'top.fased_write_egress_valid'
    assert read_wiring['sim.fased_egress_readiness.readValid'] == 'readScheduler.resp.hValid'
    expected = {'writeEgress.clock': 'top.hostClock',
        'writeEgress.reset': 'sim.fased_egress_reset',
        'writeEgress.targetFire': 'sim.fased_tfire',
        'writeEgress.req': 'top.fased_write_egress_req',
        'writeEgress.enq': 'top.fased_host_write_response',
        'top.fased_write_egress_resp': 'writeEgress.resp',
        'sim.fased_write_egress_valid': 'writeEgress.resp.hValid',
        'sim.fased_host_write_responses.bReady': 'top.fased_host_write_response.ready',
        'sim.fased_host_write_responses.bValid': 'top.fased_host_write_response.valid'}
    for direction, name, typ in copied:
        dest, src = ('sim.' + name, 'top.' + name) if direction == 'in' else ('top.' + name, 'sim.' + name)
        expected[dest] = src
    assert wrapper_wiring(actual[NEW]) == expected, 'production write egress wiring differs'
    raw_before = json.loads((directory / 'post-fame-fased-read-scheduler-all.json').read_text())
    raw_after = json.loads((directory / 'post-fame-fased-write-egress-all.json').read_text())
    assert transfer(raw_before, OLD, NEW, {p[1] for p in copied}) == raw_after
    assert len(re.findall(' = firrtl.regreset ', actual[HELPER])) == 18
    assert len(re.findall(' = firrtl.reg ', actual[HELPER])) == 1
    return dict(existing_modules_retained=len(prior), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(raw_after), reset_registers=18, unreset_ID_registers=1)


def reference_boundary(golden, immutable):
    text = clean(golden.read_text())
    if immutable is not None:
        reference = clean(immutable.read_text())
        assert module(text, 'WriteEgress') == module(reference, 'WriteEgress'), 'immutable SFC WriteEgress differs'
    host = equations(module(text, 'FASEDMemoryTimingModel')[1])
    assert host['writeEgress_clock'] == 'clock'
    assert host['writeEgress_reset'] == 'reset | _readEgress_reset_T_1'
    assert host['_readEgress_reset_T_1'] == 'hPort_hBits_reset & targetFire'
    assert ' '.join(host['writeEgress_io_req_hValid'].split()) == ' '.join(host['targetFire'].split())
    assert 'writeEgress_io_resp_hValid & readEgress_io_resp_hValid' in ' '.join(host['targetFire'].split())
    assert host['model_tNasti_io_egressResp_bBits_id'] == 'writeEgress_io_resp_tBits_id'
    for port, net in (('io_req_t_valid', 'model_tNasti_io_egressReq_b_valid'),
                      ('io_req_t_bits', 'model_tNasti_io_egressReq_b_bits'),
                      ('io_resp_tReady', 'model_tNasti_io_egressResp_bReady'),
                      ('io_enq_valid', 'auto_to_host_dram_out_b_valid'),
                      ('io_enq_bits_id', 'auto_to_host_dram_out_b_bits_id')):
        assert host['writeEgress_' + port] == net


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out / 'write-egress.mlir', out / 'write-egress.sv'
    run([export_tool, candidate, exported], 'export')
    isolated = modules(exported.read_text().replace('firrtl.module public @', 'firrtl.module @'))
    assert isolated == {HELPER: modules(candidate.read_text())[HELPER]}, 'export rebuilt production helper'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    header, body = module(clean(golden.read_text()), 'WriteEgress')
    (out / 'oracle.sv').write_text('module SFCWriteEgress(' + header + ');' + body + 'endmodule\n')
    widths = re.findall(r'reg\s*(\[1:0\])?\s+ackCounters_\d+\s*;', body)
    assert len(widths) == 16 and len(set(widths)) == 1, 'unsupported SFC counter geometry'
    counter_width = 2 if widths[0] else 1
    counter_max = (1 << counter_width) - 1
    checks = '\n'.join(f'if(candidate.{n} !== oracle.{n})$fatal(1,"{n} differs cycle=%0d",cycles);'
                        for n in ['currReqReg_valid', 'currReqReg_bits', 'haveAck'] + [f'ackCounters_{i}' for i in range(16)])
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0,req_valid=0,resp_tReady=0,enq_valid=0,enq_bits_user=0;
reg [3:0] req_bits=0,enq_bits_id=0;
reg [1:0] enq_bits_resp=0;
wire enq_ready,resp_hValid,resp_tBits_user;
wire [3:0] resp_tBits_id;
wire [1:0] resp_tBits_resp;
wire sfc_ready,sfc_valid;
wire [3:0] sfc_id;
integer cycles=0,starts=0,retirements=0,chained=0,resetStarts=0,retries=0,wraps=0;
integer cancellations=0,differentIDs=0,oldSamples=0,stalls=0,evenNonzero=0,underflows=0,i,j;
reg [15:0] seenIDs=0,cancelIDs=0,wrapIDs=0;
reg [31:0] rng=32'h197ab;
GGFASEDWriteEgress candidate(.*);
SFCWriteEgress oracle(.clock(clock),.reset(reset),.io_enq_ready(sfc_ready),
 .io_enq_valid(enq_valid),.io_enq_bits_id(enq_bits_id),.io_resp_tReady(resp_tReady),
 .io_req_hValid(targetFire),.io_req_t_valid(req_valid),.io_req_t_bits(req_bits),
 .io_resp_hValid(sfc_valid),.io_resp_tBits_id(sfc_id));
task step;
begin
 #1;
 if(cycles>0)begin
  if(enq_ready!==sfc_ready || enq_ready!==1'b1 || resp_hValid!==sfc_valid ||
     resp_tBits_id!==sfc_id || resp_tBits_user!==0 || resp_tBits_resp!==0)
    $fatal(1,"write response mismatch cycle=%0d",cycles);
  @STATE_CHECKS@
 end
 if(targetFire&&req_valid)begin starts++;resetStarts+=reset;end
 if(targetFire&&oracle.currReqReg_valid&&oracle.haveAck&&resp_tReady)begin
  retirements++;chained+=req_valid;
  if(enq_valid)begin
   if(enq_bits_id==sfc_id)begin cancellations++;cancelIDs[sfc_id]=1;end
   else differentIDs++;
  end
 end
 if(oracle.currReqReg_valid&&!oracle.haveAck&&!targetFire)retries++;
 if(oracle.currReqReg_valid&&(!targetFire||!resp_tReady))stalls++;
 if(!reset&&(oracle.retry||(targetFire&&req_valid)))begin
  @EVEN_CHECKS@
 end
 if(!reset&&targetFire&&oracle.currReqReg_valid&&oracle.haveAck&&resp_tReady&&
    !(enq_valid&&enq_bits_id==sfc_id))begin
  @UNDERFLOW_CHECKS@
 end
 if(enq_valid&&!reset)begin
  seenIDs[enq_bits_id]=1;
  if((oracle.retry||(targetFire&&req_valid))&&oracle.deqId==enq_bits_id)oldSamples++;
  if(!(targetFire&&oracle.currReqReg_valid&&oracle.haveAck&&resp_tReady&&enq_bits_id==sfc_id))begin
   if(oracle.ackCounters_0 == @COUNTER_MAX@ && enq_bits_id==0)begin wraps++;wrapIDs[0]=1;end
   @WRAP_CHECKS@
  end
 end
 clock=1;#1;clock=0;#1;cycles++;
end endtask
initial begin
 // The request ID is unreset and is defined by capture even during reset.
 reset=1;targetFire=1;req_valid=1;req_bits=5;step();step();
 // Each ID: wrap its counter, retry without targetFire, then cancel
 // simultaneous same-ID arrival/retirement and chain a new request.
 for(i=0;i<16;i++)begin
  reset=1;targetFire=0;req_valid=0;enq_valid=0;step();
  reset=0;enq_bits_id=i;enq_valid=1;
  for(j=0;j<@COUNTER_MAX@+2;j++)step();
  enq_valid=0;targetFire=1;req_valid=1;req_bits=i;step();
  targetFire=0;req_valid=0;step();
  targetFire=1;resp_tReady=1;req_valid=1;enq_valid=1;step();
  targetFire=0;req_valid=0;enq_valid=0;step();
 end
 // A count of two must acknowledge a request even though its low bit is zero.
 // Exercise each ID, including retry sampling independently of targetFire.
 if(@COUNTER_WIDTH@==2)for(i=0;i<16;i++)begin
  reset=1;enq_valid=0;targetFire=0;req_valid=0;step();
  reset=0;enq_valid=1;enq_bits_id=i;step();step();
  enq_valid=0;targetFire=1;req_valid=1;req_bits=i;step();
  targetFire=0;req_valid=0;step();step();
 end
 for(i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>17);rng=rng^(rng<<5);
  req_bits=rng[3:0];enq_bits_id=(i%3==0)?resp_tBits_id:rng[7:4];
  enq_valid=rng[8];targetFire=rng[9];resp_tReady=rng[10];req_valid=rng[11];
  enq_bits_user=rng[12];enq_bits_resp=rng[14:13];reset=(i%137==0);step();
 end
 if(starts<1000||retirements<100||chained<100||resetStarts<20||retries<1000||
    wraps<100||cancellations<100||differentIDs<100||oldSamples<100||stalls<1000||
    (@COUNTER_WIDTH@==2&&evenNonzero<16)||underflows<10||
    seenIDs!==16'hffff||cancelIDs!==16'hffff||wrapIDs!==16'hffff)
  $fatal(1,"missing coverage");
 $display("PASS cycles=%0d starts=%0d retirements=%0d chained=%0d reset_starts=%0d host_retries=%0d counter_wraps=%0d same_ID_cancellations=%0d different_ID_updates=%0d old_counter_samples=%0d stalls=%0d even_nonzero_samples=%0d underflows=%0d IDs=%0d cancel_IDs=%0d wrap_IDs=%0d",
  cycles,starts,retirements,chained,resetStarts,retries,wraps,cancellations,differentIDs,
  oldSamples,stalls,evenNonzero,underflows,seenIDs,cancelIDs,wrapIDs);
 $finish;
end endmodule
'''.replace('@STATE_CHECKS@', checks).replace('@WRAP_CHECKS@', '\n'.join(
        f'if(oracle.ackCounters_{i} == {counter_max} && enq_bits_id=={i})begin wraps++;wrapIDs[{i}]=1;end'
        for i in range(1,16))).replace('@EVEN_CHECKS@', '\n'.join(
        f'if(oracle.deqId=={i} && oracle.ackCounters_{i}==2)evenNonzero++;'
        for i in range(16))).replace('@UNDERFLOW_CHECKS@', '\n'.join(
        f'if(sfc_id=={i} && oracle.ackCounters_{i}==0)underflows++;'
        for i in range(16))).replace('@COUNTER_MAX@', str(counter_max)).replace('@COUNTER_WIDTH@', str(counter_width))
    (out / 'check.sv').write_text(tb)
    run(['verilator', '--binary', '-j', '4', '--timing', '-Wno-fatal', '--top-module', 'check',
         '--Mdir', out / 'obj', '-o', 'check', out / 'check.sv', native, out / 'oracle.sv'], 'verilator')
    run([out / 'obj/check'], 'simulation')
    log = (out / 'simulation.log').read_text()
    assert 'PASS cycles=' in log
    return dict({n: int(v) for n,v in re.findall(r'(\w+)=(\d+)',log)},
                production_helper_cloned_verbatim=True, actual_sfc_sequential_rtl=True,
                counter_width=counter_width,
                all_state_compared_after_initial_reset_and_ID_capture=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    reference_boundary(a.golden,a.immutable)
    candidate = a.candidate_dir / 'post-fame-fased-write-egress.mlir'
    result = composition(a.candidate_dir,a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  rtl_differential=differential(candidate,a.golden,a.export_tool,a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS NIC production WriteEgress, wrapper/annotation transfer and complete SFC RTL differential')


if __name__ == '__main__':
    main()
