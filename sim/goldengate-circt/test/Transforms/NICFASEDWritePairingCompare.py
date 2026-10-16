#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute production NIC AW/W pairing against the SFC LatencyPipe counter cone.

Model edges are targetFire-qualified, matching the Scala gated-clock contract.
The recorded enable-register shim's delta-cycle behavior is excluded. Target B
retirement and runtime maximum remain external ports until subsequent passes.
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

HELPER = 'GGFASEDWritePairing'
OLD, NEW = 'GGFASEDTimingAWQueueWrapper', 'GGFASEDWritePairingWrapper'


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-timing-aw-queue.mlir').read_text())
    text = (directory/'post-fame-fased-write-pairing.mlir').read_text()
    after, reference = modules(text), modules(baseline.read_text())
    assert before and all(after.get(n) == body for n, body in before.items())
    assert set(after)-set(before) == {HELPER, NEW}
    assert after[HELPER] == reference[HELPER]
    copied = [p for p in native_ports(before[OLD]) if p[1] != 'fased_write_pair_complete']
    assert native_ports(after[NEW]) == copied + [p for p in native_ports(reference[NEW])
        if p[1] in {'fased_target_b_fire', 'fased_write_max_reqs', 'fased_pending_writes'}]
    expected = {'pairing.clock': 'top.hostClock',
                'pairing.reset': 'sim.fased_model_reset',
                'pairing.targetFire': 'sim.fased_tfire',
                'pairing.awFire': frozenset({'sim.fased_timing_requests.aw.ready',
                                           'sim.fased_timing_requests.aw.valid'}),
                'pairing.wLastFire': frozenset({'sim.fased_timing_requests.w.ready',
                                              'sim.fased_timing_requests.w.valid',
                                              'sim.fased_timing_requests.w.bits.last'}),
                'pairing.bFire': 'top.fased_target_b_fire',
                'pairing.writeMaxReqs': 'top.fased_write_max_reqs',
                'sim.fased_write_pair_complete': 'pairing.pairComplete',
                'top.fased_pending_writes': 'pairing.pending'}
    for direction, name, typ in copied:
        dest, src = ('sim.'+name, 'top.'+name) if direction == 'in' else ('top.'+name, 'sim.'+name)
        expected[dest] = src
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-timing-aw-queue-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-write-pairing-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert len(re.findall(' = firrtl.regreset ', after[HELPER])) == 2
    assert ' = firrtl.reg ' not in after[HELPER] and ' = firrtl.mem ' not in after[HELPER]
    return dict(existing_modules_retained=len(before), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(new), counter_width=4, counters=2)


def oracle(path, width=4):
    assert width in (4, 5)
    text = clean(path.read_text())
    header, counter = module(text, 'SatUpDownCounter_1')
    eq = {k: ' '.join(v.split()) for k, v in equations(counter).items()}
    for name, expression in {'io_value': 'value', 'io_full': 'value >= io_max',
                            'io_empty': f"value == {width}'h0", '_value_T_1': f"value + {width}'h1",
                            '_value_T_3': f"value - {width}'h1"}.items():
        assert eq[name] == expression
    assert re.search(rf'reg \[{width-1}:0\] value;', counter)
    assert re.search(rf"if \(reset\) begin\s*value <= {width}'h0;\s*end else if \(io_inc & ~io_dec & ~io_full\) begin\s*value <= _value_T_1;\s*end else if \(~io_inc & io_dec & ~io_empty\) begin\s*value <= _value_T_3;", counter)
    pipe = module(text, 'LatencyPipe')[1]
    eq = {k: ' '.join(v.split()) for k, v in equations(pipe).items()}
    expected = {'_T_5': 'tNasti_io_tNasti_w_ready & tNasti_io_tNasti_w_valid',
                '_newWReq_T_3': 'SatUpDownCounter_2_io_value < SatUpDownCounter_1_io_value & SatUpDownCounter_2_io_inc',
                '_newWReq_T_4': 'SatUpDownCounter_2_io_value > SatUpDownCounter_1_io_value & SatUpDownCounter_1_io_inc | _newWReq_T_3',
                '_newWReq_T_5': 'SatUpDownCounter_2_io_inc & SatUpDownCounter_1_io_inc',
                'newWReq': '_newWReq_T_4 | _newWReq_T_5',
                'awQueue_io_deq_ready': '_newWReq_T_4 | _newWReq_T_5',
                'writePipe_io_enq_valid': '_newWReq_T_4 | _newWReq_T_5'}
    for i in (1, 2):
        prefix = f'SatUpDownCounter_{i}_'
        expected.update({prefix+'clock': 'clock', prefix+'reset': 'reset',
                         prefix+'io_dec': 'tNasti_io_tNasti_b_ready & tNasti_io_tNasti_b_valid',
                         prefix+'io_max': 'tNasti_io_mmReg_writeMaxReqs'})
    expected['SatUpDownCounter_1_io_inc'] = 'tNasti_io_tNasti_aw_ready & tNasti_io_tNasti_aw_valid'
    expected['SatUpDownCounter_2_io_inc'] = '_T_5 & tNasti_io_tNasti_w_bits_last'
    for name, expression in expected.items(): assert eq[name] == expression, name
    # Execute the original instances and equations verbatim, including the raw
    # AW/W/B handshakes. Only their surrounding input/output shell is generated.
    shell = '''module SFCPairing(input clock, reset,
input tNasti_io_tNasti_aw_ready, tNasti_io_tNasti_aw_valid,
input tNasti_io_tNasti_w_ready, tNasti_io_tNasti_w_valid, tNasti_io_tNasti_w_bits_last,
input tNasti_io_tNasti_b_ready, tNasti_io_tNasti_b_valid,
input [3:0] tNasti_io_mmReg_writeMaxReqs,
output pairComplete, output [3:0] awValue, wValue, output awFull, wFull);
'''
    for i in (1, 2):
        prefix = f'SatUpDownCounter_{i}_'
        shell += 'wire '+','.join(prefix+n for n in ('clock', 'reset', 'io_inc', 'io_dec', 'io_full', 'io_empty'))+';\n'
        shell += 'wire [3:0] '+prefix+'io_max,'+prefix+'io_value;\n'
    for name in ('_T_5', '_newWReq_T_3', '_newWReq_T_4', '_newWReq_T_5', 'newWReq'):
        declaration = re.findall(r'\bwire\s+'+name+r'\s*=.*?;', pipe, re.S)
        assert len(declaration) == 1; shell += declaration[0]+'\n'
    for i in (1, 2):
        prefix = f'SatUpDownCounter_{i}_'
        for name in ('clock', 'reset', 'io_inc', 'io_dec', 'io_max'):
            assignment = re.findall(r'\bassign\s+'+prefix+name+r'\s*=.*?;', pipe, re.S)
            assert len(assignment) == 1; shell += assignment[0]+'\n'
        instance = re.findall(r'SatUpDownCounter_1 SatUpDownCounter_'+str(i)+r' \(.*?\);', pipe, re.S)
        assert len(instance) == 1; shell += instance[0]+'\n'
    shell += '''assign pairComplete=newWReq;
assign awValue=SatUpDownCounter_1_io_value; assign wValue=SatUpDownCounter_2_io_value;
assign awFull=SatUpDownCounter_1_io_full; assign wFull=SatUpDownCounter_2_io_full;
endmodule
'''
    shell = shell.replace('[3:0]', f'[{width-1}:0]')
    _, gate = oracle_cone(path)
    return 'module SatUpDownCounter_1('+header+');'+counter+'endmodule\n'+shell, gate


def differential(candidate, cone, gate, export_tool, out, retirement=False, admission=None, width=4):
    out = out.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'write-pairing.mlir', out/'write-pairing.sv'
    run([export_tool, candidate, exported], 'export')
    exported_text = exported.read_text().replace('firrtl.module public @', 'firrtl.module @')
    isolated = modules(exported_text)
    assert isolated[HELPER] == modules(candidate.read_text())[HELPER], 'export rebuilt production helper'
    assert set(isolated) == ({HELPER, 'GGFASEDWriteRetirementProbe'} if retirement else {HELPER})
    if retirement:
        expected = {'pairing.'+n: 'top.'+n for n in
                    ('clock', 'reset', 'targetFire', 'awFire', 'wLastFire', 'writeMaxReqs')}
        expected.update({'pairing.bFire': frozenset({'top.bReady', 'top.bValid'}),
                         'top.pairComplete': 'pairing.pairComplete', 'top.pending': 'pairing.pending'})
        assert wiring(exported_text, 'GGFASEDWriteRetirementProbe', aggregate=True) == expected
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    extra_native = []
    if admission is not None:
        assert retirement, 'closed admission requires production B retirement'
        boundary, admission_tool = admission
        admission_mlir, admission_sv = out/'write-admission.mlir', out/'write-admission.sv'
        run([admission_tool, boundary, admission_mlir], 'export-admission')
        admission_text = admission_mlir.read_text().replace('firrtl.module public @', 'firrtl.module @')
        assert set(modules(admission_text)) == {'GGFASEDWriteAdmissionProbe'}
        assert wiring(admission_text, 'GGFASEDWriteAdmissionProbe', aggregate=True) == {
            'top.awReady': ('not', 'top.awFull'), 'top.wReady': ('not', 'top.wFull')}
        run(['firtool', admission_mlir, '--format=mlir', '--disable-all-randomization', '-o', admission_sv], 'firtool-admission')
        extra_native.append(admission_sv)
    (out/'oracle.sv').write_text(cone)
    (out/'recorded-clock-gate.sv').write_text(gate)
    tb = r'''module check;
reg clock=0,reset=0,targetFire=0;
reg av=0,ar=0,wv=0,wr=0,last=0,bv=0,br=0;
reg [3:0] writeMaxReqs=15;
wire awFire=av&ar,wLastFire=wv&wr&last,bFire=bv&br;
wire pairComplete,pending_awFull,pending_wFull;
wire [3:0] pending_awValue,pending_wValue;
wire sPair,sAwFull,sWFull;
wire [3:0] sAw,sW;
// Qualified edges; recorded enable-register shim delta-cycle glitches excluded.
wire modelClock=clock & targetFire;
GGFASEDWritePairing candidate(.*);
SFCPairing oracle(.clock(modelClock),.reset(reset),
 .tNasti_io_tNasti_aw_ready(ar),.tNasti_io_tNasti_aw_valid(av),
 .tNasti_io_tNasti_w_ready(wr),.tNasti_io_tNasti_w_valid(wv),.tNasti_io_tNasti_w_bits_last(last),
 .tNasti_io_tNasti_b_ready(br),.tNasti_io_tNasti_b_valid(bv),
 .tNasti_io_mmReg_writeMaxReqs(writeMaxReqs),.pairComplete(sPair),
 .awValue(sAw),.wValue(sW),.awFull(sAwFull),.wFull(sWFull));
integer aw=0,w=0,cycles=0,stalls=0,stalledResets=0,resets=0;
integer awAhead=0,wAhead=0,both=0,saturation=0,emptyRetire=0,incDec=0,lowered=0;
reg [15:0] limits=0,awStates=0,wStates=0;
reg [63:0] rng=64'h203abc;
task compare;
reg paired;
begin
 paired=(w>aw && awFire)||(aw>w && wLastFire)||(awFire&&wLastFire);
 if(pairComplete!==sPair || pairComplete!==paired ||
    pending_awValue!==sAw || pending_wValue!==sW ||
    pending_awValue!==4'(aw) || pending_wValue!==4'(w) ||
    pending_awFull!==sAwFull || pending_wFull!==sWFull ||
    pending_awFull!==(aw>=writeMaxReqs) || pending_wFull!==(w>=writeMaxReqs))
  $fatal(1,"pairing/count/full mismatch cycle=%0d aw=%0d w=%0d max=%0d",cycles,aw,w,writeMaxReqs);
 if(candidate.pendingAW!==oracle.SatUpDownCounter_1.value ||
    candidate.pendingW!==oracle.SatUpDownCounter_2.value)
  $fatal(1,"counter register mismatch cycle=%0d",cycles);
end endtask
function integer nextCount(input integer value,input bit inc);
begin
 nextCount=value;
 if(targetFire)begin
  if(reset)nextCount=0;
  else if(inc&&!bFire&&value<writeMaxReqs)nextCount=value+1;
  else if(!inc&&bFire&&value>0)nextCount=value-1;
 end
end endfunction
task step;
begin
 #2;if(cycles>0)compare();
 limits[writeMaxReqs]=1;awStates[aw]=1;wStates[w]=1;
 stalls+=!targetFire;stalledResets+=reset&&!targetFire;resets+=reset&&targetFire;
 awAhead+=aw>w&&wLastFire&&!awFire;wAhead+=w>aw&&awFire&&!wLastFire;
 both+=awFire&&wLastFire;
 saturation+=targetFire&&!reset&&!bFire&&((awFire&&aw>=writeMaxReqs)||(wLastFire&&w>=writeMaxReqs));
 emptyRetire+=targetFire&&!reset&&bFire&&((!awFire&&aw==0)||(!wLastFire&&w==0));
 incDec+=targetFire&&!reset&&bFire&&(awFire||wLastFire);
 lowered+=aw>writeMaxReqs||w>writeMaxReqs;
 aw=nextCount(aw,awFire);w=nextCount(w,wLastFire);
 clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
initial begin
 targetFire=1;reset=1;step();reset=0;
 ar=1;av=1; // AW ahead then last-W catches up.
 for(integer i=0;i<16;i++)step();av=0;wr=1;wv=1;last=1;
 for(integer i=0;i<16;i++)step();
 targetFire=0;reset=1;step();targetFire=1;step();reset=0;
 for(integer i=0;i<16;i++)step();wv=0;av=1;
 for(integer i=0;i<16;i++)step();wv=1;br=1;bv=1;step();
 for(integer i=0;i<16;i++)begin writeMaxReqs=4'(i);step();end
 av=0;wv=0;for(integer i=0;i<17;i++)step();
 for(integer i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  targetFire=rng[0];reset=i%113==0;writeMaxReqs=rng[7:4];
  av=rng[8];ar=rng[9];wv=rng[10];wr=rng[11];last=rng[12];bv=rng[13];br=rng[14];
  step();
 end
 if(stalls<1000||stalledResets<100||resets<100||awAhead<100||wAhead<100||both<100||
    saturation<100||emptyRetire<100||incDec<100||lowered<100||
    limits!=16'hffff||awStates!=16'hffff||wStates!=16'hffff)
  $fatal(1,"missing pairing coverage");
 $display("PASS cycles=%0d stalls=%0d stalled_resets=%0d resets=%0d aw_ahead=%0d w_ahead=%0d simultaneous_pairs=%0d saturation=%0d empty_retirements=%0d simultaneous_inc_dec=%0d lowered_limits=%0d",
 cycles,stalls,stalledResets,resets,awAhead,wAhead,both,saturation,emptyRetire,incDec,lowered);
 $finish;
end endmodule
'''
    if retirement:
        tb = tb.replace('GGFASEDWritePairing candidate(.*);',
                        'GGFASEDWriteRetirementProbe candidate(.bReady(br),.bValid(bv),.*);')
        tb = tb.replace('candidate.pendingAW', 'candidate.pairing.pendingAW')
        tb = tb.replace('candidate.pendingW', 'candidate.pairing.pendingW')
        tb = tb.replace('task step;', '''integer acceptedB=0,stalledB=0,bBackpressure=0,bReadyOnly=0;
reg [15:0] bContexts=0;
task step;''')
        tb = tb.replace('limits[writeMaxReqs]=1;', '''bContexts[{reset,targetFire,br,bv}]=1;
 acceptedB+=bFire&&targetFire;stalledB+=bFire&&!targetFire;
 bBackpressure+=bv&&!br;bReadyOnly+=br&&!bv;
 limits[writeMaxReqs]=1;''')
        tb = tb.replace(' $finish;', ''' if(bContexts!=16'hffff||acceptedB<100||stalledB<100||bBackpressure<100||bReadyOnly<100)
  $fatal(1,"missing B retirement coverage");
 $display("accepted_b=%0d stalled_b=%0d valid_backpressure=%0d ready_without_valid=%0d b_contexts=%0d",
 acceptedB,stalledB,bBackpressure,bReadyOnly,bContexts);
 $finish;''')
    if admission is not None:
        tb = tb.replace('reg av=0,ar=0,wv=0,wr=0,last=0,bv=0,br=0;',
                        'reg av=0,wv=0,last=0,bv=0,br=0; wire ar,wr,sAr,sWr;')
        tb = tb.replace('SFCPairing oracle(', '''GGFASEDWriteAdmissionProbe admission(.awFull(pending_awFull),.wFull(pending_wFull),.awReady(ar),.wReady(wr));
SFCAdmission admissionOracle(.SatUpDownCounter_1_io_full(sAwFull),.SatUpDownCounter_2_io_full(sWFull),
 .nastiReqIden_io_out_aw_ready(sAr),.nastiReqIden_io_out_w_ready(sWr));
SFCPairing oracle(''')
        tb = tb.replace('.tNasti_io_tNasti_aw_ready(ar)', '.tNasti_io_tNasti_aw_ready(sAr)')
        tb = tb.replace('.tNasti_io_tNasti_w_ready(wr)', '.tNasti_io_tNasti_w_ready(sWr)')
        tb = tb.replace(' paired=(w>aw', ''' if(ar!==sAr || wr!==sWr || ar!==(aw<writeMaxReqs) || wr!==(w<writeMaxReqs))
  $fatal(1,"closed admission readiness mismatch cycle=%0d",cycles);
 paired=(w>aw''')
        tb = tb.replace('ar=1;av=1;', 'av=1;').replace('av=0;wr=1;wv=1;', 'av=0;wv=1;')
        tb = tb.replace('av=rng[8];ar=rng[9];wv=rng[10];wr=rng[11];', 'av=rng[8];wv=rng[10];')
        # Accepted increments cannot saturate a full counter in the closed loop;
        # require blocked requests instead, independently for AW and W.
        tb = tb.replace(' saturation+=targetFire', ' blockedAW+=av&&!ar;blockedW+=wv&&!wr;zeroMax+=writeMaxReqs==0;\n saturation+=targetFire')
        tb = tb.replace('reg [15:0] limits=0', 'integer blockedAW=0,blockedW=0,zeroMax=0;\nreg [15:0] limits=0')
        tb = tb.replace('saturation<100||', 'saturation!=0||blockedAW<100||blockedW<100||zeroMax<100||')
        tb = tb.replace(' $finish;', ''' $display("blocked_aw=%0d blocked_w=%0d zero_max=%0d",blockedAW,blockedW,zeroMax);
 $finish;''')
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
         '--top-module', 'check', '--Mdir', out/'obj', '-o', 'check',
         out/'check.sv', native, *extra_native, out/'oracle.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    log = (out/'simulation.log').read_text(); assert 'PASS cycles=' in log
    return dict({n: int(v) for n, v in re.findall(r'(\w+)=(\d+)', log)},
                production_helper_cloned_verbatim=True, original_sfc_counters_and_pairing_executed=True,
                production_retirement_handshake_and_counter_bindings_executed=retirement,
                production_admission_operations_and_sfc_ready_assignments_executed=admission is not None,
                admission_feedback_closed=admission is not None,
                original_aw_w_b_handshake_equations_executed=True,
                limit_and_state_values_exercised=1 << width, counter_width=width,
                outputs_and_registers_compared_before_and_after_every_edge=True,
                recorded_clock_gate_delta_cycle_behavior_excluded=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir', 'baseline', 'golden', 'immutable', 'export-tool', 'output-dir', 'report-json'):
        p.add_argument('--'+name, type=Path, required=True)
    a = p.parse_args()
    cone, gate = oracle(a.golden)
    assert tuple(' '.join(x.split()) for x in (cone, gate)) == tuple(
        ' '.join(x.split()) for x in oracle(a.immutable)), 'immutable SFC pairing/gate differs'
    candidate = a.candidate_dir/'post-fame-fased-write-pairing.mlir'
    result = composition(a.candidate_dir, a.baseline)
    result.update(golden=str(a.golden.resolve()), immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['SatUpDownCounter_1', 'LatencyPipe', 'FASEDMemoryTimingModel', 'AbstractClockGate'],
                  rtl_differential=differential(candidate, cone, gate, a.export_tool, a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result, indent=2)+'\n')
    print('PASS NIC pending AW/W counters, pre-edge completion and boundary composition')


if __name__ == '__main__': main()
