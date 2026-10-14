#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute the NIC production read queue against original SFC Queue_16 RTL.

Compare targetFire-qualified model edges, as in the Scala gated-clock contract.
The archived clock-gate shim's delta-cycle behavior remains excluded. SFC
optimizes away read len; an independent metadata scoreboard checks that field.
Write completion, MMIO and complete NIC integration remain later boundaries.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
from NICMMIOBankCompare import modules
from NICFASEDTimingCycleCompare import oracle_cone
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer

HELPER = 'GGFASEDReadLatency10'
OLD, NEW = 'GGFASEDTimingCycleWrapper', 'GGFASEDReadLatencyWrapper'


def composition(directory, baseline):
    before_text = (directory/'post-fame-fased-timing-cycle.mlir').read_text()
    text = (directory/'post-fame-fased-read-latency.mlir').read_text()
    before, after = modules(before_text), modules(text)
    assert before and all(after.get(n) == b for n, b in before.items())
    assert set(after)-set(before) == {HELPER, NEW}
    assert after[HELPER] == modules(baseline.read_text())[HELPER]
    copied = [p for p in native_ports(before[OLD])
              if p[1] not in {'fased_next_read', 'fased_read_release_cycle'}]
    assert native_ports(after[NEW]) == copied
    expected = {'readLatency.clock': 'top.hostClock',
                'readLatency.reset': 'sim.fased_model_reset',
                'readLatency.targetFire': 'sim.fased_tfire',
                'readLatency.tCycle': 'sim.fased_timing_cycle',
                'readLatency.enq.valid': frozenset(['sim.fased_timing_requests.ar.ready',
                                                  'sim.fased_timing_requests.ar.valid']),
                'readLatency.enq.bits.releaseCycle': 'sim.fased_read_release_cycle',
                'readLatency.enq.bits.id': 'sim.fased_timing_requests.ar.bits.id',
                'readLatency.enq.bits.len': 'sim.fased_timing_requests.ar.bits.len',
                'sim.fased_next_read': 'readLatency.deq'}
    for direction, name, typ in copied:
        dest, src = ('sim.'+name, 'top.'+name) if direction == 'in' else ('top.'+name, 'sim.'+name)
        expected[dest] = src
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-timing-cycle-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-read-latency-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert len(re.findall(' = firrtl.regreset ', after[HELPER])) == 3
    memories = [line for line in after[HELPER].splitlines() if ' = firrtl.mem ' in line]
    assert len(memories) == 1 and all(term in memories[0] for term in
        ['depth = 10 : i64', 'readLatency = 0 : i32', 'writeLatency = 1 : i32', 'uint<76>'])
    return dict(existing_modules_retained=len(before), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(new), queue_registers=3, queue_depth=10)


def oracle(path):
    text = clean(path.read_text())
    header, body = module(text, 'Queue_16')
    assert 'len' not in header
    assert re.search(r'ram_releaseCycle \[0:9\]', body)
    assert re.search(r'ram_xaction_id \[0:9\]', body)
    pipe = module(text, 'LatencyPipe')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(pipe).items()}
    assert re.search(r'Queue_16 readPipe \(', pipe)
    assert eq['readDone'] == 'readPipe_io_deq_bits_releaseCycle <= tCycle'
    assert eq['xactionRelease_io_nextRead_valid'] == 'readPipe_io_deq_valid & readDone'
    assert eq['readPipe_io_deq_ready'] == 'rResp_ready & readDone'
    assert eq['readPipe_io_enq_valid'] == 'nastiReqIden_io_out_ar_ready & nastiReqIden_io_out_ar_valid'
    assert eq['readPipe_clock'] == 'clock' and eq['readPipe_reset'] == 'reset'
    assert eq['_T_16'] == '~reset'
    assert re.search(r'if \(_T_16 & ~\(readPipe_io_enq_ready \| ~_readPipe_io_enq_valid_T\)\)', pipe)
    # Preserve the original queue module verbatim (comments removed only),
    # and the original release/retirement equations from its parent.
    declarations = [re.findall(r'\bwire\s+'+name+r' = [^;]+;', pipe)[0]
                    for name in ['readDone']]
    assigns = [re.findall(r'\bassign '+name+r' = [^;]+;', pipe)[0]
               for name in ['xactionRelease_io_nextRead_valid', 'readPipe_io_deq_ready']]
    _, gate = oracle_cone(path)  # Also pin clock/reset/targetFire and release arithmetic.
    return 'module Queue_16('+header+');'+body+'endmodule\n', '\n'.join(declarations+assigns), gate


def differential(candidate, queue, release, gate, export_tool, out):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'read-latency.mlir', out/'read-latency.sv'
    run([export_tool, candidate, exported], 'export')
    assert modules(exported.read_text().replace('firrtl.module public @', 'firrtl.module @')) == {
        HELPER: modules(candidate.read_text())[HELPER]}, 'export rebuilt production helper'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    native_text = clean(native.read_text())
    for name in ['enq_ptr_value', 'deq_ptr_value', 'maybe_full']:
        assert re.search(r'\breg(?:\s+\[3:0\])?\s+'+name+r'\s*;', native_text)
    assert re.search(r'\breg\s+\[75:0\]\s+Memory\[0:9\];', module(native_text, 'ram_10x76')[1])
    memory = re.findall(r'\bram_10x76\s+(\w+)\s*\(', module(native_text, HELPER)[1])
    assert len(memory) == 1
    oracle_text = queue + '''module SFCReadLatency(input clock,reset,
input [63:0] tCycle,deadline,input [3:0] id,input valid,rResp_ready,
output readPipe_io_enq_ready,xactionRelease_io_nextRead_valid,
output [3:0] readId);
wire readPipe_io_deq_ready,readPipe_io_deq_valid;
wire [63:0] readPipe_io_deq_bits_releaseCycle;
Queue_16 readPipe(.clock(clock),.reset(reset),
 .io_enq_ready(readPipe_io_enq_ready),.io_enq_valid(valid),
 .io_enq_bits_releaseCycle(deadline),.io_enq_bits_xaction_id(id),
 .io_deq_ready(readPipe_io_deq_ready),.io_deq_valid(readPipe_io_deq_valid),
 .io_deq_bits_releaseCycle(readPipe_io_deq_bits_releaseCycle),.io_deq_bits_xaction_id(readId));
''' + release + '\nendmodule\n'
    (out/'oracle.sv').write_text(oracle_text)
    (out/'recorded-clock-gate.sv').write_text(gate)
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0;
reg [63:0] tCycle=0,enq_bits_releaseCycle=0;
reg [3:0] enq_bits_id=0;
reg [7:0] enq_bits_len=0;
reg enq_valid=0,deq_ready=0;
wire enq_ready,deq_valid;
wire [3:0] deq_bits_id;
wire [7:0] deq_bits_len;
wire sReady,sValid;
wire [3:0] sId;
// Qualified model edges; archived enable-register shim glitches are excluded.
wire modelClock=clock & targetFire;
GGFASEDReadLatency10 candidate(.*);
SFCReadLatency oracle(.clock(modelClock),.reset(reset),.tCycle(tCycle),
 .deadline(enq_bits_releaseCycle),.id(enq_bits_id),.valid(enq_valid),
 .rResp_ready(deq_ready),.readPipe_io_enq_ready(sReady),
 .xactionRelease_io_nextRead_valid(sValid),.readId(sId));
reg [75:0] metadata[0:9];
reg written[0:9];
integer ep=0,dp=0,count=0;
integer cycles=0,flows=0,fullPops=0,stalls=0,stalledResets=0,resetWrites=0;
integer wraps=0,simultaneous=0,blockedHead=0,equalDeadline=0,maxDeadline=0;
reg push,pop;
reg [75:0] head;
reg [63:0] rng=64'h200abc;
task compare;
begin
 head=count==0?{enq_bits_releaseCycle,enq_bits_id,enq_bits_len}:metadata[dp];
 if(enq_ready!==sReady || deq_valid!==sValid || deq_bits_id!==sId || deq_bits_len!==head[7:0])
  $fatal(1,"read queue cycle=%0d count=%0d native=%b,%b,%h,%h SFC=%b,%b,%h len=%h",
         cycles,count,enq_ready,deq_valid,deq_bits_id,deq_bits_len,sReady,sValid,sId,head[7:0]);
 if(candidate.enq_ptr_value!==oracle.readPipe.enq_ptr_value ||
    candidate.deq_ptr_value!==oracle.readPipe.deq_ptr_value ||
    candidate.maybe_full!==oracle.readPipe.maybe_full ||
    candidate.enq_ptr_value!==4'(ep) || candidate.deq_ptr_value!==4'(dp))
  $fatal(1,"queue pointer/full mismatch at %0d",cycles);
 for(integer k=0;k<10;k++)if(written[k])begin
  if(candidate.@MEMORY@.Memory[k]!==metadata[k] || oracle.readPipe.ram_releaseCycle[k]!==metadata[k][75:12] ||
     oracle.readPipe.ram_xaction_id[k]!==metadata[k][11:8])
   $fatal(1,"queue RAM write/reset/stall mismatch cycle=%0d slot=%0d",cycles,k);
 end
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 push=enq_ready&&enq_valid&&!(count==0&&deq_ready&&head[75:12]<=tCycle);
 pop=count>0&&deq_valid&&deq_ready;
 stalls+=!targetFire;stalledResets+=reset&&!targetFire;
 flows+=targetFire&&count==0&&enq_valid&&deq_ready&&head[75:12]<=tCycle;
 fullPops+=targetFire&&count==10&&pop;
 resetWrites+=targetFire&&reset&&push;
 wraps+=targetFire&&!reset&&((push&&ep==9)||(pop&&dp==9));
 simultaneous+=targetFire&&!reset&&push&&pop;
 blockedHead+=count>0&&head[75:12]>tCycle;
 equalDeadline+=head[75:12]==tCycle;maxDeadline+=head[75:12]==64'hffffffffffffffff;
 if(targetFire)begin
  // Queue RAM writes are independent of reset, including a reset+enqueue edge.
  if(push)begin metadata[ep]={enq_bits_releaseCycle,enq_bits_id,enq_bits_len};written[ep]=1;end
  if(reset)begin ep=0;dp=0;count=0;end
  else begin
   if(push)ep=(ep+1)%10;if(pop)dp=(dp+1)%10;
   count=count+integer'(push)-integer'(pop);
  end
 end
 clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
initial begin
 for(integer k=0;k<10;k++)written[k]=0;
 reset=1;targetFire=1;step();reset=0;
 enq_valid=1;deq_ready=1;step(); // Empty due flow does not write RAM.
 deq_ready=0;enq_bits_releaseCycle=64'hffffffffffffffff;step();
 reset=1;targetFire=0;step(); // Pending head survives stalled reset.
 targetFire=1;enq_bits_len=255;step();reset=0;
 for(integer round=0;round<8;round++)begin
  reset=1;enq_valid=0;step();reset=0;deq_ready=0;tCycle=0;
  for(integer k=0;k<10;k++)begin
   enq_valid=1;enq_bits_releaseCycle=64'(k+1);enq_bits_id=4'(k);enq_bits_len=8'(k*23);step();
  end
  if($test$plusargs("overflow"))begin
   // Deliberately violate the pinned SFC admission predicate on a model edge.
   #2;clock=1;#2;$fatal(1,"missing native overflow assertion");
  end
  targetFire=0;reset=1;step();reset=0;targetFire=1;enq_valid=0;
  deq_ready=1;tCycle=64'hffffffffffffffff;step(); // Full queue pop: no pipe bypass.
  enq_valid=1;enq_bits_releaseCycle=0;step(); // Simultaneous enqueue/dequeue.
  enq_valid=0;for(integer k=0;k<10;k++)step();
 end
 for(integer i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  tCycle=(i%17==0)?64'hffffffffffffffff:64'(i);
  enq_bits_releaseCycle=(i%11==0)?64'hffffffffffffffff:
                        (i%13==0)?0:(i%7==0)?tCycle:tCycle+64'(rng[3:1])-3;
  enq_bits_id=rng[7:4];enq_bits_len=rng[15:8];
  targetFire=rng[0];reset=i%113==0;deq_ready=rng[17];enq_valid=rng[18];
  // Preserve admission contract; full+valid is permitted only off model edges
  // or under reset, when the original overflow assertion is disabled.
  if(count==10&&targetFire&&!reset)enq_valid=0;
  step();
 end
 if(flows<10||fullPops<8||stalls<1000||stalledResets<100||resetWrites<10||
    wraps<20||simultaneous<100||blockedHead<100||equalDeadline<100||maxDeadline<100)
  $fatal(1,"missing queue coverage");
 $display("PASS cycles=%0d flows=%0d full_pops=%0d stalls=%0d stalled_resets=%0d reset_writes=%0d wraps=%0d simultaneous=%0d blocked_head=%0d equal_deadline=%0d max_deadline=%0d",
 cycles,flows,fullPops,stalls,stalledResets,resetWrites,wraps,simultaneous,blockedHead,equalDeadline,maxDeadline);
 $finish;
end endmodule
'''.replace('@MEMORY@', memory[0])
    (out/'check.sv').write_text(tb)
    run(['verilator', '--binary', '--assert', '-j', '4', '--timing', '-Wno-fatal',
         '--top-module', 'check', '--Mdir', out/'obj', '-o', 'check',
         out/'check.sv', native, out/'oracle.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    log = (out/'simulation.log').read_text(); assert 'PASS cycles=' in log
    with (out/'overflow.log').open('w') as overflow_log:
        rejected = subprocess.run([str(out/'obj/check'), '+overflow'], stdout=overflow_log,
                                  stderr=subprocess.STDOUT,
                                  preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    assert rejected.returncode != 0 and 'LBP read latency pipe would overflow.' in (out/'overflow.log').read_text()
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_helper_cloned_verbatim=True, original_sfc_queue_executed=True,
                original_sfc_release_and_retirement_equations_executed=True,
                pins_and_state_compared_before_and_after_every_edge=True,
                initialized_ram_compared_on_every_edge_including_reset=True,
                len_checked_against_independent_metadata_scoreboard=True,
                native_overflow_assertion_executed_and_rejected=True,
                recorded_clock_gate_delta_cycle_behavior_excluded=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    queue, release, gate = oracle(a.golden)
    immutable = oracle(a.immutable)
    assert tuple(' '.join(x.split()) for x in (queue, release, gate)) == tuple(
        ' '.join(x.split()) for x in immutable), 'immutable SFC queue/release/gate differs'
    candidate = a.candidate_dir/'post-fame-fased-read-latency.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['Queue_16', 'LatencyPipe', 'FASEDMemoryTimingModel', 'AbstractClockGate'],
                  rtl_differential=differential(candidate, queue, release, gate, a.export_tool, a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC production read completion, queue RAM/reset/retirement and boundary composition')


if __name__ == '__main__': main()
