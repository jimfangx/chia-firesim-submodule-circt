#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the production NIC timer with the actual SFC LatencyPipe RTL cone.

Retain the original counter declaration/update and deadline arithmetic. Pin
the archived clock-gate binding and compare at targetFire-qualified edges.
The SFC posedge enable-register shim can glitch when CE changes between host
pulses; its delta-cycle behavior is outside this counter/arithmetic contract.
Completion queues, MMIO latency programming and full NIC platform integration
are also outside this boundary.
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
from PrintSelectedFASEDTimingCycleCompare import transfer

HELPER = 'GGFASEDTimingCycle'
OLD, NEW = 'GGFASEDResponseReleaserWrapper', 'GGFASEDTimingCycleWrapper'


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-response-releaser.mlir').read_text())
    after = modules((directory/'post-fame-fased-timing-cycle.mlir').read_text())
    assert before and all(after.get(n)==b for n,b in before.items()), 'prior module changed'
    assert set(after)-set(before)=={HELPER,NEW}
    reference = modules(baseline.read_text())
    assert after[HELPER]==reference[HELPER], 'shared timing helper changed'
    copied = native_ports(before[OLD])
    assert native_ports(after[NEW])==copied+native_ports(reference[NEW])[-5:]
    expected = {'timer.clock':'top.hostClock', 'timer.reset':'sim.fased_model_reset',
                'timer.targetFire':'sim.fased_tfire', 'top.fased_timing_cycle':'timer.tCycle',
                'timer.readLatency':'top.fased_read_latency',
                'timer.writeLatency':'top.fased_write_latency',
                'top.fased_read_release_cycle':'timer.readReleaseCycle',
                'top.fased_write_release_cycle':'timer.writeReleaseCycle'}
    for direction,name,typ in copied:
        dest,src = ('sim.'+name,'top.'+name) if direction=='in' else ('top.'+name,'sim.'+name)
        expected[dest]=src
    assert wrapper_wiring(after[NEW])==expected, 'timer/reset/fire/latency binding differs'
    old = json.loads((directory/'post-fame-fased-response-releaser-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-timing-cycle-all.json').read_text())
    assert transfer(old,OLD,NEW,{p[1] for p in copied})==new
    assert len(re.findall(' = firrtl.regreset ',after[HELPER]))==1
    assert ' = firrtl.reg ' not in after[HELPER] and 'firrtl.mem ' not in after[HELPER]
    return dict(existing_modules_retained=len(before), wrapper_connections_compared=len(expected),
                annotation_records_retained=len(new), cycle_registers=1)


def oracle_cone(path):
    text=clean(path.read_text())
    header,body=module(text,'LatencyPipe')
    eq={k:' '.join(v.split()) for k,v in equations(body).items()}
    assert eq['_tCycle_T_1']=="tCycle + 64'h1"
    names=['tCycle','_tCycle_T_1','_GEN_12','_GEN_13',
           '_writePipe_io_enq_bits_releaseCycle_T_1', '_readPipe_io_enq_bits_releaseCycle_T_1',
           'writePipe_io_enq_bits_releaseCycle', 'readPipe_io_enq_bits_releaseCycle']
    declarations=[]
    for name in names:
        matches=re.findall(r'^\s*(?:reg|wire) \[63:0\] '+re.escape(name)+r'(?: = [^;]+)?;',body,re.M)
        assert len(matches)==1, 'missing or duplicated SFC declaration: '+name
        declarations.append(matches[0].strip())
    assigns=[]
    for kind,extension in [('write','_GEN_12'),('read','_GEN_13')]:
        assert re.search(r'input\s+\[31:0\] tNasti_io_mmReg_'+kind+r'Latency\s*[,\n]',header)
        assert eq[extension]=="{{32'd0}, tNasti_io_mmReg_"+kind+'Latency}'
        add='_'+kind+'Pipe_io_enq_bits_releaseCycle_T_1'
        assert eq[add]==extension+' + tCycle'
        assert eq[kind+'Pipe_io_enq_bits_releaseCycle']==add+" - 64'h1"
        match=re.findall(r'^\s*assign '+kind+r'Pipe_io_enq_bits_releaseCycle = [^;]+;',body,re.M)
        assert len(match)==1
        assigns.append(match[0].strip())
    # Extract the original first register update from the multi-register block.
    # The final end only closes the surrounding always block; its if/else and
    # all state expressions remain verbatim from the oracle.
    update=re.search(r"always @\(posedge clock\) begin\s*if \(reset\) begin\s*"
                     r"tCycle <= 64'h0;\s*end else begin\s*tCycle <= _tCycle_T_1;\s*end",body)
    assert update, 'SFC counter update changed'
    host=equations(module(text,'FASEDMemoryTimingModel')[1])
    assert host['gate_I']=='clock' and host['model_clock']=='gate_O'
    assert host['model_reset']=='hPort_hBits_reset'
    assert ' '.join(host['gate_CE'].split())==' '.join(host['targetFire'].split())
    gate_header,gate_body=module(text,'AbstractClockGate')
    assert re.search(r'always @\(posedge I\)\s*enable <= CE;',gate_body)
    assert equations(gate_body)['O']=='(I & enable)'
    cone='\n'.join(declarations+assigns)+ '\n'+update[0]+'\nend\n'
    gate='module AbstractClockGate('+gate_header+');'+gate_body+'endmodule\n'
    return cone,gate


def differential(candidate, cone, gate, export_tool, out):
    out=out.resolve(); out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    exported,native=out/'timing-cycle.mlir',out/'timing-cycle.sv'
    run([export_tool,candidate,exported],'export')
    assert modules(exported.read_text().replace('firrtl.module public @','firrtl.module @'))=={
        HELPER:modules(candidate.read_text())[HELPER]}, 'export rebuilt production timer'
    run(['firtool',exported,'--format=mlir','--disable-all-randomization','-o',native],'firtool')
    registers=re.findall(r'\breg \[63:0\]\s+(\w+)\s*;',clean(native.read_text()))
    assert len(registers)==1, 'production timer state is not a single UInt64 register'
    oracle='''module SFCTimingCycle(input clock,reset,
input [31:0] tNasti_io_mmReg_readLatency,tNasti_io_mmReg_writeLatency,
output [63:0] cycle,readDeadline,writeDeadline);
'''+cone+'''assign cycle=tCycle;
assign readDeadline=readPipe_io_enq_bits_releaseCycle;
assign writeDeadline=writePipe_io_enq_bits_releaseCycle;
endmodule
'''
    (out/'oracle.sv').write_text(oracle)
    (out/'recorded-clock-gate.sv').write_text(gate)
    tb=r'''module check;
reg clock=0,reset=0,targetFire=0;
reg [31:0] readLatency=0,writeLatency=0;
// Scala withClock(gate.O) contract: one model edge per targetFire host edge.
// The archived posedge enable-register shim is checked separately; it is not
// substituted for this contract because its delta-cycle glitches add edges.
wire modelClock=clock & targetFire;
wire [63:0] tCycle,readReleaseCycle,writeReleaseCycle;
wire [63:0] sCycle,sRead,sWrite;
GGFASEDTimingCycle candidate(.*);
SFCTimingCycle oracle(.clock(modelClock),.reset(reset),
 .tNasti_io_mmReg_readLatency(readLatency),.tNasti_io_mmReg_writeLatency(writeLatency),
 .cycle(sCycle),.readDeadline(sRead),.writeDeadline(sWrite));
integer cycles=0,stalls=0,stalledReset=0,resets=0,increments=0,counterWrap=0;
integer zeroLatency=0,maxLatency=0,deadlineUnderflow=0,deadlineOverflow=0,latencyChangeStalled=0;
reg [31:0] oldRead=0,oldWrite=0;
reg [63:0] rng=64'h199abc;
reg [63:0] seeds[0:7];
reg [31:0] latencies[0:5];
task compare;
begin
 if(tCycle!==sCycle || readReleaseCycle!==sRead || writeReleaseCycle!==sWrite)
  $fatal(1,"timer cycle=%0d native=%h,%h,%h SFC=%h,%h,%h",cycles,
         tCycle,readReleaseCycle,writeReleaseCycle,sCycle,sRead,sWrite);
end endtask
task step;
begin
 #2;if(cycles>0)compare();
 stalls+=!targetFire;stalledReset+=reset&&!targetFire;resets+=reset&&targetFire;
 increments+=targetFire&&!reset;counterWrap+=targetFire&&!reset&&(tCycle==64'hffffffffffffffff);
 zeroLatency+=!readLatency||!writeLatency;
 maxLatency+=(readLatency==32'hffffffff)||(writeLatency==32'hffffffff);
 deadlineUnderflow+=tCycle==0&&(!readLatency||!writeLatency);
 deadlineOverflow+=tCycle==64'hffffffffffffffff&&(readLatency>1||writeLatency>1);
 latencyChangeStalled+=!targetFire&&(readLatency!=oldRead||writeLatency!=oldWrite);
 clock=1;#2;compare();clock=0;#2;cycles++;
 oldRead=readLatency;oldWrite=writeLatency;
end endtask
initial begin
 reset=1;targetFire=1;step();reset=0;
 seeds[0]=0;seeds[1]=1;seeds[2]=29;seeds[3]=30;
 seeds[4]=64'hffffffff;seeds[5]=64'h100000000;
 seeds[6]=64'hfffffffffffffffe;seeds[7]=64'hffffffffffffffff;
 latencies[0]=0;latencies[1]=1;latencies[2]=2;latencies[3]=30;
 latencies[4]=32'hfffffffe;latencies[5]=32'hffffffff;
 for(integer s=0;s<8;s++)for(integer r=0;r<6;r++)for(integer w=0;w<6;w++)
 for(integer flags=0;flags<4;flags++)begin
  // Deposit identical seeds while both clocks are low, then execute the
  // unmodified counter updates. This reaches UInt64 overflow in finite time.
  candidate.@STATE@=seeds[s];oracle.tCycle=seeds[s];
  readLatency=latencies[r];writeLatency=latencies[w];reset=flags[0];targetFire=flags[1];step();
 end
 for(integer i=0;i<40000;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  readLatency=rng[31:0];writeLatency=rng[63:32];targetFire=rng[0];reset=(i%137==0);
  step();
 end
 if(stalls<1000||stalledReset<100||resets<100||increments<1000||counterWrap<10||
    zeroLatency<100||maxLatency<100||deadlineUnderflow<10||deadlineOverflow<10||
    latencyChangeStalled<1000)$fatal(1,"missing timer coverage");
 $display("PASS cycles=%0d stalls=%0d stalled_reset=%0d resets=%0d increments=%0d counter_wrap=%0d zero_latency=%0d max_latency=%0d deadline_underflow=%0d deadline_overflow=%0d latency_change_stalled=%0d",
 cycles,stalls,stalledReset,resets,increments,counterWrap,zeroLatency,maxLatency,
 deadlineUnderflow,deadlineOverflow,latencyChangeStalled);
 $finish;
end endmodule
'''.replace('@STATE@',registers[0])
    (out/'check.sv').write_text(tb)
    run(['verilator','--binary','-j','4','--timing','-Wno-fatal','--top-module','check',
         '--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'oracle.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    log=(out/'simulation.log').read_text();assert 'PASS cycles=' in log
    return dict({n:int(v) for n,v in re.findall(r'(\w+)=(\d+)',log)},
                production_helper_cloned_verbatim=True, clock_gate_binding_verified=True,
                recorded_clock_gate_delta_cycle_behavior_excluded=True,
                original_sfc_counter_and_deadline_cone_executed=True,
                all_outputs_compared_before_and_after_every_edge=True,
                wraparound_state_seeded_with_both_clocks_low=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('candidate-dir','baseline','golden','immutable','export-tool','output-dir','report-json'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    cone,gate=oracle_cone(a.golden)
    assert (cone,gate)==oracle_cone(a.immutable), 'immutable SFC timing cone or clock gate differs'
    candidate=a.candidate_dir/'post-fame-fased-timing-cycle.mlir'
    result=composition(a.candidate_dir,a.baseline)
    result.update(golden=str(a.golden.resolve()),immutable=str(a.immutable.resolve()),
                  golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
                  SFC_modules=['LatencyPipe','FASEDMemoryTimingModel','AbstractClockGate'],
                  rtl_differential=differential(candidate,cone,gate,a.export_tool,a.output_dir),
                  limitation=__doc__)
    a.report_json.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS NIC production timer, boundary composition and gated SFC RTL cone differential')


if __name__=='__main__':main()
