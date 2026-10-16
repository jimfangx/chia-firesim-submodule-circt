#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the emitted NIC ReadEgress composition with complete SFC RTL.

Storage and request scheduling only; write/timing/control responses and NIC
platform/runtime support remain incomplete. Exclude uninitialized memory and
undefined same-address read/write payloads, while comparing all control state.
After a collision, stall retirement until the next defined RAM read: last is
also payload, and consuming its undefined value would make scheduler state
undefined rather than establish a compiler mismatch.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICFASEDReadBufferCompare import composition as buffer_composition, helper_name
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDReadBufferCompare import wrapper_wiring
from PrintSelectedFASEDReadSchedulerCompare import transfer

SCHEDULER = 'GGFASEDReadScheduler'
OLD, NEW = 'GGFASEDReadBufferWrapper', 'GGFASEDReadSchedulerWrapper'


def composition(directory, baseline):
    before = (directory / 'post-fame-fased-read-buffer.mlir').read_text()
    after = (directory / 'post-fame-fased-read-scheduler.mlir').read_text()
    prior, actual = modules(before), modules(after)
    assert all(actual.get(n) == b for n, b in prior.items()), 'existing module changed'
    assert set(actual) - set(prior) == {SCHEDULER, NEW}
    if baseline is not None:
        for name in (SCHEDULER, helper_name(after)):
            assert actual[name] == modules(baseline.read_text())[name], 'shared helper changed'
    copied = [p for p in native_ports(prior[OLD]) if p[1] not in (
        'fased_egress_readiness', 'fased_read_buffer_address', 'fased_read_buffer_deq')]
    assert native_ports(actual[NEW])[:len(copied)] == copied
    assert [p[1] for p in native_ports(actual[NEW])[len(copied):]] == [
        'fased_write_egress_valid', 'fased_read_egress_req', 'fased_read_egress_resp']
    expected = {'readScheduler.clock': 'top.hostClock',
        'readScheduler.reset': 'sim.fased_egress_reset',
        'readScheduler.targetFire': 'sim.fased_tfire',
        'readScheduler.req': 'top.fased_read_egress_req',
        'readScheduler.bufferDeq': 'sim.fased_read_buffer_deq',
        'sim.fased_read_buffer_address': 'readScheduler.bufferAddress',
        'top.fased_read_egress_resp': 'readScheduler.resp',
        'sim.fased_egress_readiness.readValid': 'readScheduler.resp.hValid',
        'sim.fased_egress_readiness.writeValid': 'top.fased_write_egress_valid'}
    for direction, name, typ in copied:
        dest, src = ('sim.' + name, 'top.' + name) if direction == 'in' else ('top.' + name, 'sim.' + name)
        expected[dest] = src
    assert wrapper_wiring(actual[NEW]) == expected, 'production scheduler wiring differs'
    raw_before = json.loads((directory / 'post-fame-fased-read-buffer-all.json').read_text())
    raw_after = json.loads((directory / 'post-fame-fased-read-scheduler-all.json').read_text())
    assert transfer(raw_before, OLD, NEW, {p[1] for p in copied}) == raw_after
    return dict(existing_modules_retained=len(prior), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(raw_after),
                storage_composition=buffer_composition(directory, baseline))


def reference_boundary(golden, immutable):
    text = clean(golden.read_text())
    reference = clean(immutable.read_text())
    for name in ('ReadEgress', 'MultiQueue'):
        assert module(text, name) == module(reference, name), 'immutable SFC module differs'
    host = equations(module(text, 'FASEDMemoryTimingModel')[1])
    assert host['readEgress_clock'] == 'clock'
    assert host['readEgress_reset'] == 'reset | hPort_hBits_reset & targetFire'
    assert ' '.join(host['readEgress_io_req_hValid'].split()) == ' '.join(host['targetFire'].split())
    for port, net in (('io_req_t_valid', 'model_tNasti_io_egressReq_r_valid'),
                      ('io_req_t_bits', 'model_tNasti_io_egressReq_r_bits'),
                      ('io_resp_tReady', 'model_tNasti_io_egressResp_rReady')):
        assert host['readEgress_' + port] == net


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    exported, native = out / 'read-egress.mlir', out / 'read-egress.sv'
    run([export_tool, candidate, exported], 'export')
    isolated = modules(exported.read_text().replace('firrtl.module public @', 'firrtl.module @'))
    buffer = helper_name(candidate.read_text())
    entries = int(buffer.split('x')[-1])
    assert set(isolated) == {SCHEDULER, buffer, 'GGNICReadEgress'}
    actual = modules(candidate.read_text())
    for name in (SCHEDULER, buffer):
        assert isolated[name] == actual[name], 'export rebuilt production helper'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    reference = clean(golden.read_text())
    queue = module(reference, 'MultiQueue')[1]
    assert re.search(r'reg \[63:0\] ram_data \[0:' + str(16 * entries - 1) + r'\]', queue)
    assert 'ReorderBuffer' not in module(reference, 'ReadEgress')[1]
    oracle = ''
    for name in ('ReadEgress', 'MultiQueue'):
        header, body = module(clean(golden.read_text()), name)
        # Preserve complete actual SFC sequential/RAM operations. Rename only
        # module symbols so both implementations can run in the same design.
        body = re.sub(r'\bMultiQueue(?=\s+multiQueue\s*\()', 'SFCMultiQueue', body)
        oracle += 'module SFC' + name + '(' + header + ');' + body + 'endmodule\n'
    (out / 'oracle.sv').write_text(oracle)
    checks = []
    for name in ('enqPtrs', 'deqPtrs', 'maybe_full'):
        for i in range(16):
            checks.append(f'if(candidate.readBuffer.{name}_{i} !== oracle.multiQueue.{name}_{i})'
                          f'$fatal(1,"{name}_{i} differs cycle=%0d",cycles);')
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0,req_valid=0,resp_tReady=0,enq_valid=0,enq_bits_last=0;
reg [63:0] enq_bits_data=0;
reg [3:0] req_bits=0,enqAddr=0;
wire enq_ready,resp_hValid,resp_tBits_last,resp_tBits_user;
wire [63:0] resp_tBits_data;
wire [3:0] resp_tBits_id;
wire [1:0] resp_tBits_resp;
wire sfc_valid,sfc_last;
wire [63:0] sfc_data;
wire [3:0] sfc_id;
integer cycles=0,payloadChecks=0,collisions=0,undefinedPayloadSkips=0;
integer starts=0,retirements=0,chained=0,resetStarts=0,stalls=0,fullCycles=0;
integer resetWrites=0,switches=0,wraps=0,i,j;
reg comparePayload=0,payloadInitialized=0,idInitialized=0,collision;
reg [3:0] previous=0;
reg [31:0] rng=32'h19c8;
GGNICReadEgress candidate(.*);
SFCReadEgress oracle(.clock(clock),.reset(reset),.io_enq_valid(enq_valid),
 .io_enq_bits_data(enq_bits_data),.io_enq_bits_last(enq_bits_last),.io_enq_bits_id(enqAddr),
 .io_resp_tReady(resp_tReady),.io_req_hValid(targetFire),.io_req_t_valid(req_valid),
 .io_req_t_bits(req_bits),.io_resp_hValid(sfc_valid),.io_resp_tBits_id(sfc_id),
 .io_resp_tBits_data(sfc_data),.io_resp_tBits_last(sfc_last));
task step;
begin
 // Both memories specify undefined RUW. Do not let an undefined last bit
 // retire the request and contaminate subsequent control comparisons.
 if(payloadInitialized && !comparePayload)begin targetFire=0;resp_tReady=0;end
 #1;
 if(enq_ready!==1'b1 || resp_hValid!==sfc_valid || resp_tBits_user!==0 || resp_tBits_resp!==0)
   $fatal(1,"response token/constants mismatch cycle=%0d",cycles);
 if(candidate.readScheduler.currReqReg_valid!==oracle.currReqReg_valid)
   $fatal(1,"active request differs cycle=%0d",cycles);
 if(idInitialized && resp_tBits_id!==sfc_id)$fatal(1,"request ID differs cycle=%0d",cycles);
 if(candidate.readBuffer.deq_valid!==oracle.multiQueue_io_deq_valid ||
    candidate.readBuffer.deq_ready!==oracle.multiQueue_io_deq_ready ||
    candidate.readBuffer.deqAddr!==oracle.multiQueue_io_deqAddr)
   $fatal(1,"queue scheduler coupling differs cycle=%0d",cycles);
 if(payloadInitialized && comparePayload && sfc_valid && oracle.currReqReg_valid) begin
   if(resp_tBits_data!==sfc_data || resp_tBits_last!==sfc_last)
     $fatal(1,"active response payload mismatch cycle=%0d",cycles);
   payloadChecks++;
 end
 if(payloadInitialized && !comparePayload && sfc_valid && oracle.currReqReg_valid)
   undefinedPayloadSkips++;
 @STATE_CHECKS@
 if(candidate.readBuffer.deqAddrReg!==oracle.multiQueue.deqAddrReg)
   $fatal(1,"prefetch ID differs cycle=%0d",cycles);
 if(targetFire&&req_valid)begin starts++;resetStarts+=reset;end
 if(targetFire&&oracle.currReqReg_valid&&resp_tReady&&sfc_last)begin
   retirements++;chained+=req_valid;
 end
 if(oracle.currReqReg_valid&&(!targetFire||!resp_tReady))stalls++;
 if(!oracle.multiQueue_io_enq_ready)fullCycles++;
 if(reset&&enq_valid&&oracle.multiQueue_io_enq_ready)resetWrites++;
 if(oracle.multiQueue_io_deq_ready&&oracle.multiQueue_io_deq_valid&&oracle.multiQueue._GEN_101==@LAST@)wraps++;
 if(oracle.multiQueue_io_deqAddr!=previous)switches++;
 collision=enq_valid&&oracle.multiQueue_io_enq_ready&&
   oracle.multiQueue.ram_data_MPORT_addr==oracle.multiQueue.ram_data_io_deq_bits_MPORT_addr;
 collisions+=collision;
 previous=oracle.multiQueue_io_deqAddr;
 clock=1;#1;clock=0;#1;cycles++;
 comparePayload=!collision;
 if(targetFire&&req_valid)idInitialized=1;
end endtask
initial begin
 // Capture an ID during reset: valid resets, but the request ID is unreset.
 reset=1;targetFire=1;req_valid=1;req_bits=5;step();step();
 reset=0;targetFire=0;req_valid=0;
 // Initialize every payload location; unspecified power-on RAM is excluded.
 for(i=0;i<16;i++)begin enqAddr=i;
   for(j=0;j<@ENTRIES@;j++)begin enq_valid=1;enq_bits_data={32'hfeed0000+i,32'h01230000+j};
     enq_bits_last=(j==@LAST@);step();end
 end
 payloadInitialized=1;enq_valid=0;resp_tReady=1;targetFire=1;
 // Retire all IDs, with bubbles after last beats, to exercise synchronous
 // prefetch and pointer wrapping before arbitrary interleaved requests.
 for(i=0;i<16;i++)begin req_valid=1;req_bits=i;step();req_valid=0;
   for(j=0;j<@DRAIN@;j++)step();end
 for(i=0;i<40000;i++)begin
   rng=rng^(rng<<13);rng=rng^(rng>>17);rng=rng^(rng<<5);
   req_bits=(i%3==0)?resp_tBits_id:rng[3:0];enqAddr=rng[7:4];
   if(i%5==0)enqAddr=resp_tBits_id;
   enq_valid=rng[8];targetFire=rng[9];resp_tReady=rng[10];req_valid=rng[11];
   reset=(i%137==0);enq_bits_data={rng,~rng};enq_bits_last=rng[12];step();
 end
 if(payloadChecks<1000||starts<1000||retirements<100||chained<100||resetStarts<20||
    stalls<1000||fullCycles<16||resetWrites<20||switches<1000||wraps<16||collisions<1)
   $fatal(1,"missing coverage");
 $display("PASS cycles=%0d payload_checks=%0d read_write_collisions=%0d undefined_payload_skips=%0d starts=%0d retirements=%0d chained=%0d reset_starts=%0d stalls=%0d full=%0d reset_writes=%0d ID_switches=%0d wraps=%0d",
   cycles,payloadChecks,collisions,undefinedPayloadSkips,starts,retirements,chained,resetStarts,
   stalls,fullCycles,resetWrites,switches,wraps);
 $finish;
end endmodule
'''.replace('@STATE_CHECKS@', '\n'.join(checks)).replace('@LAST@', str(entries-1)).replace('@ENTRIES@', str(entries)).replace('@DRAIN@', str(entries+4))
    (out / 'check.sv').write_text(tb)
    run(['verilator', '--binary', '-j', '4', '--timing', '-Wno-fatal', '--top-module', 'check',
         '--Mdir', out / 'obj', '-o', 'check', out / 'check.sv', native, out / 'oracle.sv'], 'verilator')
    run([out / 'obj/check'], 'simulation')
    log = (out / 'simulation.log').read_text()
    assert 'PASS cycles=' in log
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_helpers_cloned_verbatim=True, actual_sfc_sequential_rtl=True, beats_per_id=entries,
                initialized_payload_memory=True, undefined_RUW_payload_excluded=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    reference_boundary(a.golden, a.immutable)
    candidate = a.candidate_dir / 'post-fame-fased-read-scheduler.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  rtl_differential=differential(candidate, a.golden, a.export_tool, a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS NIC production ReadEgress scheduling/storage, wrapper/annotation transfer and actual SFC RTL differential')


if __name__ == '__main__':
    main()
