#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC read storage with the actual SFC MultiQueue RTL.

This checks per-ID storage, synchronous prefetch, reset and wrapper/annotation
composition. Request scheduling, timing/control responses and NIC runtime
remain separate. Undefined same-address read/write payloads are not compared.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDReadBufferCompare import wrapper_wiring
from PrintSelectedFASEDReadBufferCompare import reference_boundary, transfer

HELPER = 'GGFASEDReadBuffer16x8'
OLD, NEW = 'GGFASEDIngressIssueWrapper', 'GGFASEDReadBufferWrapper'


def composition(directory, baseline):
    before = (directory / 'post-fame-fased-ingress-deadlock.mlir').read_text()
    after = (directory / 'post-fame-fased-read-buffer.mlir').read_text()
    prior, actual = modules(before), modules(after)
    assert all(actual.get(n) == b for n, b in prior.items()), 'existing module changed'
    assert set(actual) - set(prior) == {HELPER, NEW}
    assert actual[HELPER] == modules(baseline.read_text())[HELPER], 'shared queue changed'
    copied_ports = [p for p in native_ports(prior[OLD]) if p[1] != 'fased_host_responses']
    copied = [p[1] for p in copied_ports]
    assert native_ports(actual[NEW])[:len(copied)] == copied_ports
    assert [p[1] for p in native_ports(actual[NEW])[len(copied):]] == [
        'fased_host_write_responses', 'fased_host_read_response',
        'fased_read_buffer_address', 'fased_read_buffer_deq']
    expected = {'readBuffer.clock': 'top.hostClock', 'readBuffer.reset': 'sim.fased_egress_reset',
        'top.fased_host_read_response.ready': 1,
        'sim.fased_host_responses.rReady': 'top.fased_host_read_response.ready',
        'sim.fased_host_responses.rValid': 'top.fased_host_read_response.valid',
        'sim.fased_host_responses.rLast': 'top.fased_host_read_response.bits.last',
        'readBuffer.enq.valid': 'top.fased_host_read_response.valid',
        'readBuffer.enq.bits.data': 'top.fased_host_read_response.bits.data',
        'readBuffer.enq.bits.last': 'top.fased_host_read_response.bits.last',
        'readBuffer.enqAddr': 'top.fased_host_read_response.bits.id',
        'readBuffer.deqAddr': 'top.fased_read_buffer_address',
        'top.fased_read_buffer_deq': 'readBuffer.deq'}
    for n in ('bReady', 'bValid'):
        expected['sim.fased_host_responses.' + n] = 'top.fased_host_write_responses.' + n
    for direction, name, typ in copied_ports:
        dest, src = ('sim.' + name, 'top.' + name) if direction == 'in' else ('top.' + name, 'sim.' + name)
        expected[dest] = src
    assert wrapper_wiring(actual[NEW]) == expected, 'production read-buffer wiring differs'
    raw_before = json.loads((directory / 'post-fame-fased-ingress-deadlock-all.json').read_text())
    raw_after = json.loads((directory / 'post-fame-fased-read-buffer-all.json').read_text())
    assert transfer(raw_before, OLD, NEW, copied) == raw_after, 'annotation transfer differs'
    return dict(existing_modules_retained=len(prior), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(raw_after), payload_bits=65, queues=16, beats_per_id=8)


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log,
                           stderr=subprocess.STDOUT, check=True)
    exported, native = out / 'helper.mlir', out / 'helper.sv'
    run([export_tool, candidate, exported], 'export')
    isolated = modules(exported.read_text().replace('firrtl.module public @', 'firrtl.module @'))
    assert isolated == {HELPER: modules(candidate.read_text())[HELPER]}, 'export rebuilt queue'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    header, body = module(clean(golden.read_text()), 'MultiQueue')
    # Keep the complete recorded module, including its actual sequential
    # pointer/full updates and synchronous RAM. Only its symbol is renamed.
    (out / 'oracle.sv').write_text('module SFCMultiQueue(' + header + ');' + body + 'endmodule\n')
    assert 'ram_data_io_deq_bits_MPORT_addr' in body and 'ram_data_MPORT_addr' in body
    checks = []
    for name in ('enqPtrs', 'deqPtrs', 'maybe_full'):
        for i in range(16):
            checks.append(f'if(candidate.{name}_{i} !== oracle.{name}_{i})'
                          f'$fatal(1,"{name}_{i} differs cycle=%0d",cycles);')
    tb = r'''module check;
reg clock=0,reset=0,enq_valid=0,deq_ready=0,enq_bits_last=0;
reg [63:0] enq_bits_data=0;
reg [3:0] enqAddr=0,deqAddr=0;
wire enq_ready,deq_valid,deq_bits_last,empty;
wire [63:0] deq_bits_data;
wire sfc_ready,sfc_valid,sfc_last;
wire [63:0] sfc_data;
integer cycles=0,payloadChecks=0,collisions=0,fullCycles=0,resetWrites=0;
integer switches=0,simultaneous=0,wraps=0,undefinedPayloadSkips=0,i,j;
reg comparePayload=0,payloadInitialized=0,collision;
reg [3:0] previous=0;
reg [31:0] rng=32'h19c7;
GGFASEDReadBuffer16x8 candidate(.*);
SFCMultiQueue oracle(.clock(clock),.reset(reset),.io_enq_ready(sfc_ready),
 .io_enq_valid(enq_valid),.io_enq_bits_data(enq_bits_data),.io_enq_bits_last(enq_bits_last),
 .io_deq_ready(deq_ready),.io_deq_valid(sfc_valid),.io_deq_bits_data(sfc_data),
 .io_deq_bits_last(sfc_last),.io_enqAddr(enqAddr),.io_deqAddr(deqAddr));
task step;
begin
 #1;
 if(enq_ready!==sfc_ready || deq_valid!==sfc_valid || empty!==oracle.empty)
   $fatal(1,"handshake/prefetch mismatch cycle=%0d",cycles);
 if(payloadInitialized && comparePayload && deq_valid) begin
   if(deq_bits_data!==sfc_data || deq_bits_last!==sfc_last)
     $fatal(1,"payload mismatch cycle=%0d",cycles);
   payloadChecks++;
 end
 if(payloadInitialized && !comparePayload && deq_valid)undefinedPayloadSkips++;
 @STATE_CHECKS@
 if(candidate.deqAddrReg!==oracle.deqAddrReg)$fatal(1,"delayed ID differs");
 if(deqAddr!=previous)switches++;
 if(!enq_ready)fullCycles++;
 if(enq_valid&&enq_ready&&deq_ready&&deq_valid)simultaneous++;
 if(reset&&enq_valid&&enq_ready)resetWrites++;
 if(deq_ready&&deq_valid&&oracle._GEN_101==3'h7)wraps++;
 collision=enq_valid&&enq_ready&&
   oracle.ram_data_MPORT_addr==oracle.ram_data_io_deq_bits_MPORT_addr;
 collisions+=collision;
 clock=1;#1;clock=0;#1;cycles++;
 // Payload has undefined RUW behavior on this edge. Compare all control
 // regardless, and resume payload comparison on the next defined read.
 comparePayload=!collision;
 previous=deqAddr;
end endtask
initial begin
 reset=1;step();step();reset=0;
 // Initialize every payload location before comparing RAM contents. This
 // does not claim equivalence for unspecified power-on memory state.
 for(i=0;i<16;i++)begin
   enqAddr=i;deqAddr=i;
   for(j=0;j<8;j++)begin
     enq_valid=1;enq_bits_data={32'hfeed0000+i,32'h01230000+j};
     enq_bits_last=(j==7);step();
   end
 end
 // Force full queues, drain and wrap before injecting arbitrary ID changes.
 payloadInitialized=1;enq_valid=0;deq_ready=1;
 for(i=0;i<16;i++)begin deqAddr=i;enqAddr=i;for(j=0;j<12;j++)step();end
 for(i=0;i<40000;i++)begin
   rng=rng^(rng<<13);rng=rng^(rng>>17);rng=rng^(rng<<5);
   enqAddr=rng[3:0];deqAddr=(i%3==0)?previous:rng[7:4];
   if(i%5==0)enqAddr=previous;
   enq_valid=rng[8];deq_ready=rng[9];reset=(i%137==0);
   enq_bits_data={rng,~rng};enq_bits_last=rng[10];step();
 end
 if(payloadChecks<1000||fullCycles<16||resetWrites<20||switches<1000||
    simultaneous<100||wraps<16||collisions<1)$fatal(1,"missing coverage");
 $display("PASS cycles=%0d payload_checks=%0d read_write_collisions=%0d undefined_payload_skips=%0d full=%0d reset_writes=%0d ID_switches=%0d simultaneous=%0d wraps=%0d",
   cycles,payloadChecks,collisions,undefinedPayloadSkips,fullCycles,resetWrites,switches,simultaneous,wraps);
 $finish;
end endmodule
'''.replace('@STATE_CHECKS@', '\n'.join(checks))
    (out / 'check.sv').write_text(tb)
    run(['verilator', '--binary', '-j', '4', '--timing', '-Wno-fatal', '--top-module', 'check',
         '--Mdir', out / 'obj', '-o', 'check', out / 'check.sv', native, out / 'oracle.sv'], 'verilator')
    run([out / 'obj/check'], 'simulation')
    log = (out / 'simulation.log').read_text()
    assert 'PASS cycles=' in log
    coverage = {n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)}
    return dict(coverage, production_helper_cloned_verbatim=True, actual_sfc_sequential_rtl=True,
                initialized_payload_memory=True, undefined_RUW_payload_excluded=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    reference_boundary(a.golden)
    candidate = a.candidate_dir / 'post-fame-fased-read-buffer.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  rtl_differential=differential(candidate, a.golden, a.export_tool, a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS NIC production read storage, wrapper/annotation transfer and actual SFC sequential RTL differential')


if __name__ == '__main__':
    main()
