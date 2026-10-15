#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute production statistics RTL against extracted, unmodified SFC cones.

Inputs isolate decoded MMIO and accepted target handshakes. Full response
queues, control transport, platform integration and NIC workloads are external.
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
from PrintRocketResponsesCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer
from PrintRocketFASEDStatisticsCompare import COUNTERS, EVENTS, HIERARCHY

HELPER, OLD, NEW = 'GGFASEDStatistics', HIERARCHY[0], 'GGFASEDStatisticsWrapper'


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-response-errors.mlir').read_text())
    text = (directory/'post-fame-fased-statistics.mlir').read_text()
    after, reference = modules(text), modules(baseline.read_text())
    assert set(after)-set(before) == {NEW}
    assert all(after[n] == body for n, body in before.items() if n not in HIERARCHY)
    observation = 'fased_accepted_r_fire'
    for n in HIERARCHY:
        ports = native_ports(after[n]); assert ports[:-1] == native_ports(before[n])
        assert ports[-1] == ('out', observation, '!firrtl.uint<1>')
        expected = wiring(before[n], n, aggregate=True)
        expected['top.'+observation] = ('sim.'+observation if n != HIERARCHY[-1] else
                                      frozenset({'releaser.r.ready', 'releaser.r.valid'}))
        assert wiring(after[n], n, aggregate=True) == expected
    copied = native_ports(before[OLD]); assert native_ports(after[NEW])[:-1] == copied
    assert native_ports(after[NEW])[-1] == native_ports(reference[NEW])[-1]
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n for d,n,_ in copied}
    expected.update({'statistics.clock':'top.hostClock', 'statistics.hostReset':'top.hostReset',
                     'statistics.modelReset':'sim.fased_model_reset', 'statistics.targetFire':'sim.fased_tfire',
                     'statistics.rFire':'sim.'+observation, 'top.fased_statistics_mcr':'statistics.mcr'})
    for ch in ('aw','ar','w'):
        expected['statistics.'+ch+'Fire'] = frozenset({'sim.fased_timing_requests.'+ch+'.'+f for f in ('ready','valid')})
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-response-errors-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-statistics-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after[HELPER] == reference[HELPER]
    assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = false', after[HELPER]) == list(zip(COUNTERS,map(str,range(56,69,4))))
    return dict(unchanged_modules=len(before)-len(HIERARCHY), observation_modules=len(HIERARCHY),
                wrapper_connections_compared=len(expected), copied_ports=len(copied), annotation_records_retained=len(new))


def oracle(path):
    raw = module(path.read_text(),'FASEDMemoryTimingModel')[1]
    assert not re.search(r'\$(?:fatal|finish|stop)\b',raw), 'SFC FASED termination policy changed'
    host = clean(raw); pipe = module(clean(path.read_text()),'LatencyPipe')[1]
    heq, eq = equations(host), equations(pipe)
    assert heq['model_clock'] == 'gate_O' and heq['gate_I'] == 'clock' and heq['model_reset'] == 'hPort_hBits_reset'
    instance, = re.findall(r'LatencyPipe model\s*\((.*?)\);', host, re.S)
    assert '.clock(model_clock)' in instance and '.reset(model_reset)' in instance
    assert set(re.split(r'\s*&\s*', heq['gate_CE'])) == {
        'hPort_toHost_hValid','hPort_fromHost_hReady','ingress_io_nastiInputs_hReady',
        'writeEgress_io_resp_hValid','readEgress_io_resp_hValid','tResetReady'}
    pins = ['input clock,reset']+[f'input tNasti_io_tNasti_{ch}_ready,tNasti_io_tNasti_{ch}_valid' for ch in ('w','r','aw','ar')]
    pins += [f'output [31:0] crFile_io_mcr_read_{14+i}_bits' for i in range(4)]
    cone = 'module SFCStatistics('+','.join(pins)+');\n'
    updates, diagnostic = [], []
    for i,(name,guard,ch) in enumerate(zip(COUNTERS,['_T_5','_T_1','SatUpDownCounter_1_io_inc','SatUpDownCounter_io_inc'],('w','r','aw','ar'))):
        declaration, = re.findall(r'\breg \[31:0\] '+name+r';',pipe)
        increment = '_'+name+'_T_1'
        assert eq[increment] == name+" + 32'h1" and eq[guard] == f'tNasti_io_tNasti_{ch}_ready & tNasti_io_tNasti_{ch}_valid'
        cone += declaration+'\n'
        for symbol in (increment,guard):
            assignment, = re.findall(r'\b(?:wire(?:\s*\[[^]]+\])?|assign)\s+'+symbol+r'\s*=\s*.*?;',pipe,re.S)
            if assignment.startswith('assign'):
                declaration, = re.findall(r'\bwire(?:\s*\[[^]]+\])?\s+'+symbol+r';',pipe)
                cone += declaration+'\n'
            cone += assignment+'\n'
        update, = re.findall(r'if \(reset\) begin\s*'+name+r" <= 32'h0;\s*end else if \("+guard+r'\) begin\s*'+name+r' <= '+increment+r';\s*end',pipe)
        begin = pipe.rfind('always @(posedge clock)', 0, pipe.index(update))
        assert begin >= 0 and 'always @' not in pipe[begin+len('always @(posedge clock)'):pipe.index(update)]
        updates.append(update)
        assert eq['tNasti_io_mmReg_'+name] == name
        assert heq[f'crFile_io_mcr_read_{14+i}_bits'] == 'model_tNasti_io_mmReg_'+name
        cone += f'assign crFile_io_mcr_read_{14+i}_bits = {name};\n'
        diag, = re.findall(r'if \(~reset & ~\(~crFile_io_mcr_write_'+str(14+i)+r'_valid\)\) begin\s*\$fwrite\(.*?Register '+name+r'.*?\);\s*end',host,re.S)
        begin = host.rfind('always @(posedge clock)', 0, host.index(diag))
        assert begin >= 0 and 'always @' not in host[begin+len('always @(posedge clock)'):host.index(diag)]
        diagnostic.append(diag)
    cone += 'always @(posedge clock) begin\n'+'\n'.join(updates)+'\nend\nendmodule\n'
    cone += 'module SFCDiagnostics(input clock,reset,'+','.join(f'input crFile_io_mcr_write_{14+i}_valid' for i in range(4))+');\n'
    cone += 'always @(posedge clock) begin\n'+'\n'.join(diagnostic)+'\nend\nendmodule\n'
    return cone


def allocation(candidate, golden):
    regions = re.findall(r'\{name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64}',candidate.read_text())
    assert regions and len(set(regions)) == 1
    size,slave,base = map(int,regions[0]); header = golden.with_suffix('.const.h')
    widget, = re.findall(r'registry.add_widget\(new FASEDMemoryTimingModel\((.*?)\n  0,',header.read_text(),re.S)
    tables = re.findall(r'std::vector<std::pair<std::string, uint32_t>>\{(.*?)\n    }',widget,re.S)
    assert len(tables)==2
    for i,name in enumerate(COUNTERS):
        addresses = re.findall(r'R"ESC\('+name+r'\)ESC", (\d+)',header.read_text())
        assert len(addresses)==1 and int(addresses[0])==base+56+4*i and 60+4*i<=size
        assert f'R"ESC({name})ESC"' in tables[0] and f'R"ESC({name})ESC"' not in tables[1]
    return dict(golden=str(header),sha256=hashlib.sha256(header.read_bytes()).hexdigest(),base=base,size=size,slave=slave,
                offsets=list(range(56,69,4)),addresses=list(range(base+56,base+69,4)),read_only=True)


def differential(candidate, cone, export_tool, out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name,check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=check)
    exported,native = out/'statistics.mlir',out/'statistics.sv'
    run([export_tool,candidate,exported],'export')
    isolated=modules(exported.read_text().replace('firrtl.module public @','firrtl.module @'))
    assert isolated[HELPER]==modules(candidate.read_text())[HELPER], 'production bank rebuilt'
    body=modules(candidate.read_text())[NEW]
    mutations=[('host-reset',body.replace('firrtl.strictconnect %statistics_hostReset, %hostReset','firrtl.strictconnect %statistics_hostReset, %sim_fased_model_reset'),'wrong host clock/reset'),
               ('target-fire',body.replace('firrtl.strictconnect %statistics_targetFire, %sim_fased_tfire','firrtl.strictconnect %statistics_targetFire, %hostReset'),'wrong target reset/fire'),
               ('accepted-r',body.replace('firrtl.strictconnect %statistics_rFire, %sim_fased_accepted_r_fire','firrtl.strictconnect %statistics_rFire, %sim_fased_tfire'),'wrong accepted-R observation')]
    for name,changed,diagnostic in mutations:
        assert changed!=body
        bad=out/(name+'.mlir');bad.write_text(candidate.read_text().replace(body,changed))
        assert run([export_tool,bad,out/(name+'-probe.mlir')],name,False).returncode!=0
        assert diagnostic in (out/(name+'.log')).read_text()
    run(['firtool',exported,'--format=mlir','--disable-all-randomization','-o',native],'firtool')
    (out/'oracle.sv').write_text(cone)
    tb='''module check;
reg clock=0,hostReset=0,modelReset=0,targetFire=0;
reg [7:0] handshakes=0;
reg [3:0] writes=0,mcr_wstrb=0,readReady=0;
reg [31:0] writeBits=0;
wire awFire=handshakes[4]&handshakes[5],arFire=handshakes[6]&handshakes[7];
wire wFire=handshakes[0]&handshakes[1],rFire=handshakes[2]&handshakes[3];
reg oracleOnly=0,nativeOnly=0;
wire targetClock=clock&targetFire;
wire diagnosticReset=hostReset|nativeOnly,candidateReset=hostReset|oracleOnly;
'''
    for i in range(4):
        tb+=f'wire [31:0] n{i},s{i}; wire rv{i},wr{i};\n'
    ports=['.clock(clock)','.hostReset(candidateReset)','.modelReset(modelReset)','.targetFire(targetFire)']+[f'.{e}({e})' for e in EVENTS]+['.mcr_wstrb(mcr_wstrb)']
    for i in range(4):
        ports += [f'.mcr_read_{i}_bits(n{i})',f'.mcr_read_{i}_valid(rv{i})',f'.mcr_read_{i}_ready(readReady[{i}])',
                  f'.mcr_write_{i}_ready(wr{i})',f'.mcr_write_{i}_valid(writes[{i}])',f'.mcr_write_{i}_bits(writeBits)']
    tb+='GGFASEDStatisticsProbe candidate('+','.join(ports)+');\n'
    ports=['.clock(targetClock)','.reset(modelReset)']
    for i,ch in enumerate(('w','r','aw','ar')):
        ports += [f'.tNasti_io_tNasti_{ch}_ready(handshakes[{2*i}])',f'.tNasti_io_tNasti_{ch}_valid(handshakes[{2*i+1}])',f'.crFile_io_mcr_read_{14+i}_bits(s{i})']
    tb+='SFCStatistics oracle('+','.join(ports)+');\n'
    tb+='SFCDiagnostics diagnostics(.clock(clock),.reset(diagnosticReset),'+','.join(f'.crFile_io_mcr_write_{14+i}_valid(writes[{i}])' for i in range(4))+');\n'
    tb+='''reg [31:0] expected[4];reg [63:0] rng=64'h212abcd;
integer cycles=0,wraps=0,stalledResets=0,hostOnlyResets=0,blockedBeats=0,nonfinalBeats=0,badWrite=0;
reg last=0;
task compare;begin
'''
    for i,name in enumerate(COUNTERS):
        tb+=f'if(n{i}!==s{i}||n{i}!==expected[{i}]||candidate.statistics.{name}!==oracle.{name}||!rv{i}||!wr{i}) $fatal(1,"counter {name} mismatch cycle=%0d",cycles);\n'
    tb+='end endtask\ntask seed(input [31:0] value);begin\n'
    for i,name in enumerate(COUNTERS):
        tb+=f'candidate.statistics.{name}=value;oracle.{name}=value;expected[{i}]=value;\n'
    tb+='''end endtask
task step;begin
 #2;if(cycles>0)compare();
 stalledResets+=modelReset&&!targetFire;hostOnlyResets+=hostReset&&!modelReset;
 blockedBeats+=handshakes[3]&&!handshakes[2];nonfinalBeats+=rFire&&targetFire&&!last&&!modelReset;
'''
    for i,event in enumerate(EVENTS):
        tb+=f'wraps+=targetFire&&!modelReset&&{event}&&expected[{i}]==32\'hffffffff;\nif(targetFire)expected[{i}]=modelReset?0:expected[{i}]+{event};\n'
    tb+='''clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
initial begin
 oracleOnly=$test$plusargs("oracle-only");nativeOnly=$test$plusargs("native-only");
 for(integer i=0;i<4;i++)expected[i]=0;
 if($value$plusargs("bad-write=%d",badWrite))begin
  modelReset=1;targetFire=1;hostReset=1;writes=badWrite[3:0];
  repeat(2)begin #2;clock=1;#2;clock=0;end
  modelReset=0;hostReset=0;handshakes=255;
  for(integer i=0;i<3;i++)begin
   targetFire=i==1;modelReset=i!=1;#2;clock=1;#2;clock=0;
  end
  targetFire=1;modelReset=0;writes=0;repeat(2)begin #2;clock=1;#2;clock=0;end
'''
    for i in range(4): tb+=f'if(n{i}!==3||s{i}!==3)$fatal(1,"illegal write changed counter or prevented recovery");\n'
    tb+='''hostReset=1;writes=badWrite[3:0];targetFire=0;modelReset=1;
  repeat(2)begin #2;clock=1;#2;clock=0;end
'''
    for i in range(4): tb+=f'if(n{i}!==3||s{i}!==3)$fatal(1,"stalled reset changed counter");\n'
    tb+='''targetFire=1;#2;clock=1;#2;clock=0;
'''
    for i in range(4): tb+=f'if(n{i}!==0||s{i}!==0)$fatal(1,"target reset did not clear counter");\n'
    tb+='''$display("PASS illegal-write mask=%0d continued",badWrite);$finish;
 end else begin
 modelReset=1;targetFire=1;step();
 for(integer i=0;i<2048;i++)begin
  handshakes=i[7:0];hostReset=i[8];modelReset=i[9];targetFire=i[10];last=i[0];
  seed(i[0]?32'hffffffff:32'hfffffffe);writes=hostReset?i[3:0]:0;step();
 end
 for(integer i=0;i<32768;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  handshakes=rng[7:0];hostReset=rng[8];modelReset=i%31==0;targetFire=rng[9];last=rng[10];
  readReady=rng[14:11];mcr_wstrb=rng[18:15];writeBits=rng[50:19];writes=hostReset?rng[54:51]:0;
  if(i%97==0)seed(32'hffffffff);step();
 end
 if(!wraps||!stalledResets||!hostOnlyResets||!blockedBeats||!nonfinalBeats)$fatal(1,"missing coverage");
 $display("PASS cycles=%0d wraps=%0d stalled_resets=%0d host_only_resets=%0d blocked_beats=%0d nonfinal_beats=%0d",cycles,wraps,stalledResets,hostOnlyResets,blockedBeats,nonfinalBeats);$finish;
 end
end endmodule
'''
    (out/'check.sv').write_text(tb)
    run(['verilator','--binary','--assert','-j','4','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'oracle.sv'],'verilator')
    run([out/'obj/check'],'simulation');log=(out/'simulation.log').read_text();assert 'PASS cycles=' in log
    for mask in (*[1<<i for i in range(4)],15):
        for mode in ('native','oracle','joint'):
            flags=[] if mode=='joint' else ['+'+mode+'-only']
            run([out/'obj/check',*flags,f'+bad-write={mask}'],f'{mode}-write-{mask}')
            diag=(out/f'{mode}-write-{mask}.log').read_text();assert f'PASS illegal-write mask={mask} continued' in diag
            for i,name in enumerate(COUNTERS):
                assert diag.count(f'Assertion failed: Register {name} is read only') == (3*(2 if mode=='joint' else 1) if mask&(1<<i) else 0)
    return dict({n:int(v) for n,v in re.findall(r'(\w+)=(\d+)',log)},production_bank_cloned_verbatim=True,
                validated_bindings_cloned=9, valid_IR_mutations_rejected=len(mutations), illegal_write_runs=15,
                nonfatal_diagnostics_and_reset_suppression_matched=True, counter_continuation_and_target_reset_matched=True,
                scope='decoded MMIO and observed target handshakes; independent host/target edges')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir','baseline','golden','immutable','export-tool','output-dir','report-json'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();cone=oracle(a.golden)
    assert ' '.join(cone.split())==' '.join(oracle(a.immutable).split())
    candidate=a.candidate_dir/'post-fame-fased-statistics.mlir'
    result=composition(a.candidate_dir,a.baseline)
    result.update(golden=str(a.golden.resolve()),immutable=str(a.immutable.resolve()),golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),allocated_register_ABI=allocation(candidate,a.golden),
                  rtl_differential=differential(candidate,cone,a.export_tool,a.output_dir),limitation=__doc__)
    a.report_json.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS NIC FASED statistics, target reset/fire, four read-only MMIO lanes, nonfatal diagnostics and accepted-R hierarchy')


if __name__=='__main__':main()
