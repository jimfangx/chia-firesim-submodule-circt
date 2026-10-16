#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute native NIC AR admission and final-R retirement against recorded SFC.

Clone the production counter helper, final-R AndPrimOps and actual wrapper
connections after checking eight observation hops and shared AR feedback.
The RTL differential closes each implementation's own AR ready back to its
accepted request. Runtime maximum MMIO, completion queues and clock-gate
shim delta-cycle behavior remain outside this counter/handshake boundary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICFASEDTimingCycleCompare import oracle_cone
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer

HELPER = 'GGFASEDReadAdmission'
OLD, NEW = 'GGFASEDWriteAdmissionWrapper', 'GGFASEDReadAdmissionWrapper'
OBSERVED = [OLD, 'GGFASEDWriteRetirementWrapper', 'GGFASEDWritePairingWrapper',
            'GGFASEDTimingAWQueueWrapper', 'GGFASEDWriteLatencyWrapper',
            'GGFASEDReadLatencyWrapper', 'GGFASEDTimingCycleWrapper',
            'GGFASEDResponseReleaserWrapper']


def composition(directory, baseline):
    before_text = (directory/'post-fame-fased-write-admission.mlir').read_text()
    text = (directory/'post-fame-fased-read-admission.mlir').read_text()
    before, after, reference = modules(before_text), modules(text), modules(baseline.read_text())
    assert set(after)-set(before) == {HELPER, NEW}
    assert all(after[n] == body for n, body in before.items() if n not in OBSERVED)
    compared = 0
    for j, name in enumerate(OBSERVED):
        assert native_ports(after[name]) == native_ports(before[name])+[('out', 'fased_accepted_r_last_fire', '!firrtl.uint<1>')]
        expected = wiring(before_text, name, aggregate=True)
        expected['top.fased_accepted_r_last_fire'] = ('sim.fased_accepted_r_last_fire' if j < 7 else
            frozenset({'releaser.r.ready', 'releaser.r.valid', 'releaser.r.bits.last'}))
        assert wiring(text, name, aggregate=True) == expected, name
        compared += len(expected)
    copied = native_ports(before[OLD])
    new_ports = native_ports(after[NEW])
    assert [(d, n) for d, n, _ in new_ports] == [(d, n) for d, n, _ in copied]+[
        ('in', 'fased_read_max_reqs'), ('out', 'fased_pending_reads')]
    ref = {n: (d, t) for d, n, t in native_ports(reference[NEW])}
    for d, n, t in new_ports:
        if n in {'fased_timing_requests', 'fased_read_max_reqs', 'fased_pending_reads'}:
            assert (d, t) == ref[n]
        else: assert (d, n, t) in copied
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n
                for d, n, _ in copied if n != 'fased_timing_requests'}
    for ch in ('aw', 'w'):
        expected[f'top.fased_timing_requests.{ch}'] = f'sim.fased_timing_requests.{ch}'
    for f in ('valid', 'bits'):
        expected[f'top.fased_timing_requests.ar.{f}'] = f'sim.fased_timing_requests.ar.{f}'
    for side in ('sim', 'top'):
        expected[f'{side}.fased_timing_requests.ar.ready'] = 'counter.arReady'
    expected.update({'counter.clock': 'top.hostClock', 'counter.reset': 'sim.fased_model_reset',
                     'counter.targetFire': 'sim.fased_tfire', 'counter.arValid': 'sim.fased_timing_requests.ar.valid',
                     'counter.rLastFire': 'sim.fased_accepted_r_last_fire', 'counter.readMaxReqs': 'top.fased_read_max_reqs',
                     'top.fased_pending_reads': 'counter.pending'})
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-write-admission-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-read-admission-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after[HELPER] == reference[HELPER]
    return dict(unchanged_modules=len(before)-len(OBSERVED), observation_hops=len(OBSERVED),
                prior_and_observation_connections_compared=compared, wrapper_connections_compared=len(expected),
                annotation_records_retained=len(new), request_payload_and_AW_W_passthrough_preserved=True)


def oracle(path, width=4):
    assert width in (4, 5)
    text = clean(path.read_text())
    header, counter = module(text, 'SatUpDownCounter_1')
    ceq = {k: ' '.join(v.split()) for k, v in equations(counter).items()}
    assert ceq['io_value'] == 'value' and ceq['io_full'] == 'value >= io_max'
    assert ceq['io_empty'] == f"value == {width}'h0" and ceq['_value_T_1'] == f"value + {width}'h1" and ceq['_value_T_3'] == f"value - {width}'h1"
    assert re.search(rf'reg \[{width-1}:0\] value;', counter)
    assert re.search(rf"if \(reset\) begin\s*value <= {width}'h0;\s*end else if \(io_inc & ~io_dec & ~io_full\) begin\s*value <= _value_T_1;\s*end else if \(~io_inc & io_dec & ~io_empty\) begin\s*value <= _value_T_3;", counter)
    pipe = module(text, 'LatencyPipe')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(pipe).items()}
    identity_name = re.search(r'(\w+) nastiReqIden \(', pipe).group(1)
    identity = equations(module(text, identity_name)[1])
    assert eq['tNasti_io_tNasti_ar_ready'] == 'nastiReqIden_io_in_ar_ready'
    assert identity['io_in_ar_ready'] == 'io_out_ar_ready'
    assert eq['tNasti_io_tNasti_r_valid'] == 'xactionRelease_io_r_valid'
    assert eq['xactionRelease_io_r_ready'] == 'tNasti_io_tNasti_r_ready'
    release = equations(module(text, 'AXI4Releaser')[1])
    assert release['io_r_valid'] == 'currentRead_q_io_deq_valid'
    expected = {'SatUpDownCounter_clock': 'clock', 'SatUpDownCounter_reset': 'reset',
                'SatUpDownCounter_io_inc': 'tNasti_io_tNasti_ar_ready & tNasti_io_tNasti_ar_valid',
                'SatUpDownCounter_io_dec': '_T_1 & tNasti_io_tNasti_r_bits_last',
                'SatUpDownCounter_io_max': 'tNasti_io_mmReg_readMaxReqs',
                'nastiReqIden_io_out_ar_ready': '~SatUpDownCounter_io_full',
                '_T_1': 'tNasti_io_tNasti_r_ready & tNasti_io_tNasti_r_valid'}
    for name, expression in expected.items(): assert eq[name] == expression, name
    shell = '''module SFCReadAdmission(input clock, reset,
input tNasti_io_tNasti_ar_valid,tNasti_io_tNasti_r_ready,tNasti_io_tNasti_r_valid,tNasti_io_tNasti_r_bits_last,
input [3:0] tNasti_io_mmReg_readMaxReqs,
output nastiReqIden_io_out_ar_ready,output [3:0] pendingValue,output pendingFull);
wire tNasti_io_tNasti_ar_ready=nastiReqIden_io_out_ar_ready;
wire SatUpDownCounter_clock,SatUpDownCounter_reset,SatUpDownCounter_io_inc,SatUpDownCounter_io_dec,SatUpDownCounter_io_full,SatUpDownCounter_io_empty;
wire [3:0] SatUpDownCounter_io_max,SatUpDownCounter_io_value;
'''
    for name in expected:
        pattern = r'\bwire\s+'+name+r'\s*=.*?;' if name == '_T_1' else r'\bassign\s+'+name+r'\s*=.*?;'
        matches = re.findall(pattern, pipe, re.S)
        assert len(matches) == 1; shell += matches[0]+'\n'
    instance = re.findall(r'SatUpDownCounter_1 SatUpDownCounter \(.*?\);', pipe, re.S)
    assert len(instance) == 1; shell += instance[0]+'\n'
    shell += 'assign pendingValue=SatUpDownCounter_io_value;assign pendingFull=SatUpDownCounter_io_full;\nendmodule\n'
    shell = shell.replace('[3:0]', f'[{width-1}:0]')
    _, gate = oracle_cone(path)
    return 'module SatUpDownCounter_1('+header+');'+counter+'endmodule\n'+shell, gate


def differential(candidate, cone, gate, export_tool, out, width=4):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'read-admission.mlir', out/'read-admission.sv'
    run([export_tool, candidate, exported], 'export')
    text = exported.read_text().replace('firrtl.module public @', 'firrtl.module @')
    isolated = modules(text)
    assert set(isolated) == {HELPER, 'GGFASEDReadAdmissionProbe'}
    assert isolated[HELPER] == modules(candidate.read_text())[HELPER], 'export rebuilt production counter'
    expected = {'counter.'+n: 'top.'+n for n in ('clock', 'reset', 'targetFire', 'arValid', 'readMaxReqs')}
    expected.update({'counter.rLastFire': frozenset({'top.rReady', 'top.rValid', 'top.rLast'}),
                     'top.arReady': 'counter.arReady', 'top.pending': 'counter.pending'})
    assert wiring(text, 'GGFASEDReadAdmissionProbe', aggregate=True) == expected
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    (out/'oracle.sv').write_text(cone)
    (out/'recorded-clock-gate.sv').write_text(gate)
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0,arValid=0,rReady=0,rValid=0,rLast=0;
reg [3:0] readMaxReqs=15;
wire arReady,pending_full,sReady,sFull;
wire [3:0] pending_value,sValue;
wire inc=arValid&arReady,dec=rReady&rValid&rLast;
// Qualified model edges; recorded gate enable-register delta behavior excluded.
wire modelClock=clock&targetFire;
GGFASEDReadAdmissionProbe candidate(.*);
SFCReadAdmission oracle(.clock(modelClock),.reset(reset),
 .tNasti_io_tNasti_ar_valid(arValid),.tNasti_io_tNasti_r_ready(rReady),
 .tNasti_io_tNasti_r_valid(rValid),.tNasti_io_tNasti_r_bits_last(rLast),
 .tNasti_io_mmReg_readMaxReqs(readMaxReqs),.nastiReqIden_io_out_ar_ready(sReady),
 .pendingValue(sValue),.pendingFull(sFull));
integer pending=0,cycles=0,stalls=0,stalledResets=0,resets=0;
integer acceptedAR=0,acceptedR=0,nonfinalR=0,blockedAR=0,emptyRetire=0,incDec=0,lowered=0,zeroMax=0,backpressure=0,readyOnly=0;
reg [15:0] limits=0,states=0;
reg [31:0] contexts=0;
reg [63:0] rng=64'h206abc;
task compare;
begin
 if(arReady!==sReady || arReady!==(pending<readMaxReqs) ||
    pending_value!==sValue || pending_value!==4'(pending) ||
    pending_full!==sFull || pending_full!==(pending>=readMaxReqs))
  $fatal(1,"read admission/count/full mismatch cycle=%0d pending=%0d max=%0d",cycles,pending,readMaxReqs);
 if(candidate.counter.pendingReads!==oracle.SatUpDownCounter.value)
  $fatal(1,"pending-read register mismatch cycle=%0d",cycles);
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 limits[readMaxReqs]=1;states[pending]=1;contexts[{reset,targetFire,rReady,rValid,rLast}]=1;
 stalls+=!targetFire;stalledResets+=reset&&!targetFire;resets+=reset&&targetFire;
 acceptedAR+=inc&&targetFire;acceptedR+=dec&&targetFire;nonfinalR+=rReady&&rValid&&!rLast;
 blockedAR+=arValid&&!arReady;emptyRetire+=targetFire&&!reset&&dec&&!inc&&pending==0;
 incDec+=targetFire&&!reset&&inc&&dec;lowered+=pending>readMaxReqs;zeroMax+=readMaxReqs==0;
 backpressure+=rValid&&!rReady;readyOnly+=rReady&&!rValid;
 if(targetFire)begin
  if(reset)pending=0;
  else if(inc&&!dec&&pending<readMaxReqs)pending++;
  else if(!inc&&dec&&pending>0)pending--;
 end
 clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
initial begin
 targetFire=1;reset=1;step();reset=0;arValid=1;
 for(integer i=0;i<17;i++)step();
 arValid=0;rReady=1;rValid=1;rLast=0;step(); // Non-final beats never retire.
 rLast=1;for(integer i=0;i<17;i++)step(); // Include empty retirement.
 for(integer i=0;i<128;i++)step(); // Directed underflow saturation; random traffic rarely empties.
 arValid=1;rValid=0;for(integer i=0;i<15;i++)step();
 for(integer i=0;i<16;i++)begin readMaxReqs=4'(i);step();end
 targetFire=0;reset=1;step();targetFire=1;step();reset=0;
 readMaxReqs=15;arValid=1;rValid=0;step();rValid=1;step(); // Simultaneous AR/final R.
 for(integer flags=0;flags<32;flags++)begin
  reset=flags[4];targetFire=flags[3];rReady=flags[2];rValid=flags[1];rLast=flags[0];step();
 end
 for(integer i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  targetFire=rng[0];reset=i%113==0;readMaxReqs=rng[7:4];arValid=rng[8];
  rReady=rng[9];rValid=rng[10];rLast=rng[11];step();
 end
 $display("coverage limits=%0d states=%0d contexts=%0d stalls=%0d stalled_resets=%0d resets=%0d accepted_ar=%0d accepted_final_r=%0d nonfinal_r=%0d blocked_ar=%0d empty_retirements=%0d simultaneous_inc_dec=%0d lowered_limits=%0d zero_max=%0d valid_backpressure=%0d ready_without_valid=%0d",
 limits,states,contexts,stalls,stalledResets,resets,acceptedAR,acceptedR,nonfinalR,blockedAR,emptyRetire,incDec,lowered,zeroMax,backpressure,readyOnly);
 if(stalls<1000||stalledResets<100||resets<100||acceptedAR<100||acceptedR<100||
    nonfinalR<100||blockedAR<100||emptyRetire<100||incDec<100||lowered<100||
    zeroMax<100||backpressure<100||readyOnly<100||limits!=16'hffff||states!=16'hffff||contexts!=32'hffffffff)
  $fatal(1,"missing read-admission/final-R coverage");
 $display("PASS cycles=%0d stalls=%0d stalled_resets=%0d resets=%0d accepted_ar=%0d accepted_final_r=%0d nonfinal_r=%0d blocked_ar=%0d empty_retirements=%0d simultaneous_inc_dec=%0d lowered_limits=%0d zero_max=%0d valid_backpressure=%0d ready_without_valid=%0d r_contexts=%0d",
 cycles,stalls,stalledResets,resets,acceptedAR,acceptedR,nonfinalR,blockedAR,emptyRetire,incDec,lowered,zeroMax,backpressure,readyOnly,contexts);
 $finish;
end endmodule
'''
    assert width in (4, 5)
    if width == 5:
        tb = tb.replace('[3:0]', '[4:0]').replace("4'(", "5'(")
        tb = tb.replace('readMaxReqs=15', 'readMaxReqs=31').replace('writeMaxReqs=15', 'writeMaxReqs=31')
        tb = tb.replace('i<17;', 'i<33;').replace('i<16;', 'i<32;').replace('i<15;', 'i<31;')
        tb = tb.replace('MaxReqs=rng[7:4]', 'MaxReqs=rng[8:4]')
        tb = tb.replace('reg [15:0] limits', 'reg [31:0] limits')
        for bitmap in ('limits', 'states', 'awStates', 'wStates'):
            tb = tb.replace(bitmap+"!=16'hffff", bitmap+"!=32'hffffffff")
    (out/'check.sv').write_text(tb)
    run(['verilator', '--binary', '--assert', '-j', '4', '--timing', '-Wno-fatal',
         '--top-module', 'check', '--Mdir', out/'obj', '-o', 'check', out/'check.sv', native, out/'oracle.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    log = (out/'simulation.log').read_text(); assert 'PASS cycles=' in log
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_helper_cloned_verbatim=True, production_final_R_operations_and_counter_bindings_executed=True,
                original_sfc_counter_AR_and_final_R_equations_executed=True, independent_AR_feedback_closed=True,
                limit_and_state_values_exercised=1 << width, counter_width=width, all_32_reset_fire_R_handshake_contexts_exercised=True,
                outputs_and_registers_compared_before_and_after_every_edge=True,
                recorded_clock_gate_delta_cycle_behavior_excluded=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args()
    cone, gate = oracle(a.golden)
    assert tuple(' '.join(x.split()) for x in (cone, gate)) == tuple(' '.join(x.split()) for x in oracle(a.immutable))
    candidate = a.candidate_dir/'post-fame-fased-read-admission.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['LatencyPipe', 'IdentityModule', 'AXI4Releaser', 'SatUpDownCounter_1', 'FASEDMemoryTimingModel'],
                  rtl_differential=differential(candidate, cone, gate, a.export_tool, a.output_dir), limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC closed AR admission, final-R retirement, pending reads and preserved boundary composition')


if __name__ == '__main__': main()
