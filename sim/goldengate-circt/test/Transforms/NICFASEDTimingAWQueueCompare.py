#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute the production NIC accepted-AW ID queue against SFC Queue_14.

Compare targetFire-qualified model edges, as in the Scala gated-clock contract.
The archived clock-gate shim's delta-cycle behavior remains excluded.
The AW/W pairing pulse is external; pending counters, MMIO and complete NIC
integration remain later boundaries.
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

HELPER = 'GGFASEDTimingAWQueue10'
OLD, NEW = 'GGFASEDWriteLatencyWrapper', 'GGFASEDTimingAWQueueWrapper'


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-write-latency.mlir').read_text())
    text = (directory/'post-fame-fased-timing-aw-queue.mlir').read_text()
    after, reference = modules(text), modules(baseline.read_text())
    assert before and all(after.get(n) == b for n, b in before.items())
    assert set(after)-set(before) == {HELPER, NEW}
    assert after[HELPER] == reference[HELPER]
    copied = [p for p in native_ports(before[OLD]) if p[1] != 'fased_write_completion']
    assert native_ports(after[NEW]) == copied + [
        p for p in native_ports(reference[NEW])
        if p[1] in {'fased_write_pair_complete', 'fased_timing_aw_queue_ready'}]
    expected = {'timingAW.clock': 'top.hostClock',
                'timingAW.reset': 'sim.fased_model_reset',
                'timingAW.targetFire': 'sim.fased_tfire',
                'timingAW.enq.valid': frozenset({'sim.fased_timing_requests.aw.ready',
                                               'sim.fased_timing_requests.aw.valid'}),
                'timingAW.enq.bits.id': 'sim.fased_timing_requests.aw.bits.id',
                'timingAW.deq.ready': 'top.fased_write_pair_complete',
                'sim.fased_write_completion.valid': 'top.fased_write_pair_complete',
                'sim.fased_write_completion.bits.id': 'timingAW.deq.bits.id',
                'top.fased_timing_aw_queue_ready': 'timingAW.enq.ready'}
    for direction, name, typ in copied:
        dest, src = ('sim.'+name, 'top.'+name) if direction == 'in' else ('top.'+name, 'sim.'+name)
        expected[dest] = src
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-write-latency-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-timing-aw-queue-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert len(re.findall(' = firrtl.regreset ', after[HELPER])) == 3
    memories = [line for line in after[HELPER].splitlines() if ' = firrtl.mem ' in line]
    assert len(memories) == 1 and all(term in memories[0] for term in
        ['depth = 10 : i64', 'readLatency = 0 : i32', 'writeLatency = 1 : i32', 'uint<4>'])
    return dict(existing_modules_retained=len(before), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(new), queue_registers=3, queue_depth=10)


def oracle(path):
    text = clean(path.read_text())
    header, body = module(text, 'Queue_14')
    assert re.search(r'reg \[3:0\] ram_id \[0:9\];', body)
    pipe = module(text, 'LatencyPipe')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(pipe).items()}
    assert re.search(r'Queue_14 awQueue \(', pipe)
    assert eq['awQueue_io_enq_valid'] == 'nastiReqIden_io_out_aw_ready & nastiReqIden_io_out_aw_valid'
    assert eq['awQueue_io_enq_bits_id'] == 'nastiReqIden_io_out_aw_bits_id'
    assert eq['awQueue_io_deq_ready'] == eq['writePipe_io_enq_valid'] == eq['newWReq']
    assert eq['writePipe_io_enq_bits_xaction_id'] == 'awQueue_io_deq_bits_id'
    assert eq['awQueue_clock'] == 'clock' and eq['awQueue_reset'] == 'reset'
    assert eq['_T_16'] == '~reset'
    assert re.search(r'if \(_T_16 & ~\(awQueue_io_enq_ready \| ~_awQueue_io_enq_valid_T\)\)', pipe)
    _, gate = oracle_cone(path)  # Pin host model clock/reset/targetFire as well.
    # Execute the original queue verbatim; the pairing pulse is supplied by
    # the test and independently checked against the parent boundary above.
    return 'module Queue_14('+header+');'+body+'endmodule\n', gate


def differential(candidate, queue, gate, export_tool, out):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'timing-aw-queue.mlir', out/'timing-aw-queue.sv'
    run([export_tool, candidate, exported], 'export')
    assert modules(exported.read_text().replace('firrtl.module public @', 'firrtl.module @')) == {
        HELPER: modules(candidate.read_text())[HELPER]}, 'export rebuilt production helper'
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    native_text = clean(native.read_text())
    for name in ['enq_ptr_value', 'deq_ptr_value', 'maybe_full']:
        assert re.search(r'\breg(?:\s+\[3:0\])?\s+'+name+r'\s*;', native_text)
    assert re.search(r'\breg\s+\[3:0\]\s+Memory\[0:9\];', module(native_text, 'ram_10x4')[1])
    memory = re.findall(r'\bram_10x4\s+(\w+)\s*\(', module(native_text, HELPER)[1])
    assert len(memory) == 1
    (out/'oracle.sv').write_text(queue)
    (out/'recorded-clock-gate.sv').write_text(gate)
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0;
reg [3:0] enq_bits_id=0;
reg enq_valid=0,deq_ready=0;
wire enq_ready,deq_valid;
wire [3:0] deq_bits_id;
wire sReady,sValid;
wire [3:0] sId;
// Qualified model edges; archived enable-register shim glitches are excluded.
wire modelClock=clock & targetFire;
GGFASEDTimingAWQueue10 candidate(.*);
Queue_14 oracle(.clock(modelClock),.reset(reset),
 .io_enq_ready(sReady),.io_enq_valid(enq_valid),.io_enq_bits_id(enq_bits_id),
 .io_deq_ready(deq_ready),.io_deq_valid(sValid),.io_deq_bits_id(sId));
reg [3:0] metadata[0:9];
reg written[0:9];
integer ep=0,dp=0,count=0;
integer cycles=0,flows=0,fullPops=0,stalls=0,stalledResets=0,resetWrites=0;
integer wraps=0,simultaneous=0;
reg [15:0] ids=0;
reg push,pop;
reg [3:0] head;
reg [63:0] rng=64'h202abc;
task compare;
begin
 head=count==0?enq_bits_id:metadata[dp];
 if(enq_ready!==sReady || deq_valid!==sValid || deq_bits_id!==sId ||
    enq_ready!==(count!=10) || deq_valid!==(enq_valid||count!=0) || deq_bits_id!==head)
  $fatal(1,"AW queue cycle=%0d count=%0d native=%b,%b,%h SFC=%b,%b,%h",
         cycles,count,enq_ready,deq_valid,deq_bits_id,sReady,sValid,sId);
 if(candidate.enq_ptr_value!==oracle.enq_ptr_value ||
    candidate.deq_ptr_value!==oracle.deq_ptr_value ||
    candidate.maybe_full!==oracle.maybe_full ||
    candidate.enq_ptr_value!==4'(ep) || candidate.deq_ptr_value!==4'(dp))
  $fatal(1,"queue pointer/full mismatch at %0d",cycles);
 for(integer k=0;k<10;k++)if(written[k])begin
  if(candidate.@MEMORY@.Memory[k]!==metadata[k] || oracle.ram_id[k]!==metadata[k])
   $fatal(1,"queue RAM write/reset/stall mismatch cycle=%0d slot=%0d",cycles,k);
 end
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 push=enq_ready&&enq_valid&&!(count==0&&deq_ready);
 pop=count>0&&deq_valid&&deq_ready;
 stalls+=!targetFire;stalledResets+=reset&&!targetFire;
 flows+=targetFire&&count==0&&enq_valid&&deq_ready;
 fullPops+=targetFire&&count==10&&pop;
 resetWrites+=targetFire&&reset&&push;
 wraps+=targetFire&&!reset&&((push&&ep==9)||(pop&&dp==9));
 simultaneous+=targetFire&&!reset&&push&&pop;
 if(targetFire)begin
  if(enq_valid)ids[enq_bits_id]=1;
  // RAM writes occur even when queue pointer/full registers reset.
  if(push)begin metadata[ep]=enq_bits_id;written[ep]=1;end
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
 enq_valid=1;deq_ready=1;step(); // Empty flow does not write RAM.
 deq_ready=0;enq_bits_id=8;step();
 reset=1;targetFire=0;step(); // Pending head survives stalled reset.
 targetFire=1;enq_bits_id=15;step();reset=0;
 for(integer round=0;round<8;round++)begin
  reset=1;enq_valid=0;step();reset=0;deq_ready=0;
  for(integer k=0;k<10;k++)begin enq_valid=1;enq_bits_id=4'(k);step();end
  if($test$plusargs("overflow"))begin
   #2;clock=1;#2;$fatal(1,"missing native overflow assertion");
  end
  targetFire=0;reset=1;step();reset=0;targetFire=1;enq_valid=0;
  deq_ready=1;step(); // Full queue pop: no pipe bypass.
  enq_valid=1;step(); // Simultaneous enqueue/dequeue.
  enq_valid=0;for(integer k=0;k<10;k++)step();
 end
 for(integer i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  enq_bits_id=rng[7:4];
  targetFire=rng[0];reset=i%113==0;deq_ready=rng[17];enq_valid=rng[18];
  if(count==10&&targetFire&&!reset)enq_valid=0;
  step();
 end
 if(flows<100||fullPops<8||stalls<1000||stalledResets<100||resetWrites<10||
    wraps<20||simultaneous<100||ids!=16'hffff)
  $fatal(1,"missing queue coverage");
 $display("PASS cycles=%0d flows=%0d full_pops=%0d stalls=%0d stalled_resets=%0d reset_writes=%0d wraps=%0d simultaneous=%0d",
 cycles,flows,fullPops,stalls,stalledResets,resetWrites,wraps,simultaneous);
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
    assert rejected.returncode != 0 and 'AW queue in SplitTransaction timing model would overflow.' in (out/'overflow.log').read_text()
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_helper_cloned_verbatim=True, original_sfc_queue_executed=True,
                accepted_aw_and_oldest_id_bindings_pinned=True, all_sixteen_ids_exercised=True,
                pins_and_state_compared_before_and_after_every_edge=True,
                initialized_ram_compared_on_every_edge_including_reset=True,
                native_overflow_assertion_executed_and_rejected=True,
                recorded_clock_gate_delta_cycle_behavior_excluded=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    queue, gate = oracle(a.golden)
    assert tuple(' '.join(x.split()) for x in (queue, gate)) == tuple(
        ' '.join(x.split()) for x in oracle(a.immutable)), 'immutable SFC AW queue/gate differs'
    candidate = a.candidate_dir/'post-fame-fased-timing-aw-queue.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['Queue_14', 'LatencyPipe', 'FASEDMemoryTimingModel', 'AbstractClockGate'],
                  rtl_differential=differential(candidate, queue, gate, a.export_tool, a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC accepted AW ID queue, RAM/reset/flow and completion boundary composition')


if __name__ == '__main__': main()
