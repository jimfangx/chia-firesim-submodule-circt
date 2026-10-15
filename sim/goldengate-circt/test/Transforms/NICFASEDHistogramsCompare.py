#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute the production occupancy bank against unmodified SFC update cones.

The probe isolates pre-edge read/AW occupancy and decoded MMIO. It does not
prove the pending-counter producers, control transport, NIC runtime or platform.
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

NAMES = [g+'OutstandingHistogram_'+str(i) for g in ('write', 'read') for i in range(5)]
HELPER, OLD, NEW = 'GGFASEDHistograms', 'GGFASEDStatisticsWrapper', 'GGFASEDHistogramsWrapper'
BOUNDS = (0, 2, 4, 8)


def composition(directory, baseline):
    before = modules((directory/'post-fame-fased-statistics.mlir').read_text())
    text = (directory/'post-fame-fased-histograms.mlir').read_text()
    after, reference = modules(text), modules(baseline.read_text())
    assert set(after)-set(before) == {NEW} and set(before) <= set(after)
    assert all(after[n] == body for n, body in before.items()), 'attachment changed existing hardware'
    copied = native_ports(before[OLD]); added = native_ports(after[NEW])
    assert added[:-1] == copied and added[-1] == native_ports(reference[NEW])[-1]
    expected = {('sim.' if d == 'in' else 'top.')+n: ('top.' if d == 'in' else 'sim.')+n for d,n,_ in copied}
    expected.update({'histograms.clock':'top.hostClock', 'histograms.hostReset':'top.hostReset',
                     'histograms.modelReset':'sim.fased_model_reset', 'histograms.targetFire':'sim.fased_tfire',
                     'histograms.pendingReads':'sim.fased_pending_reads.value',
                     'histograms.pendingAW':'sim.fased_pending_writes.awValue',
                     'top.fased_histograms_mcr':'histograms.mcr'})
    assert wiring(text, NEW, aggregate=True) == expected
    old = json.loads((directory/'post-fame-fased-statistics-all.json').read_text())
    new = json.loads((directory/'post-fame-fased-histograms-all.json').read_text())
    assert transfer(old, OLD, NEW, {p[1] for p in copied}) == new
    assert after[HELPER] == reference[HELPER]
    assert re.findall(r'name = "(\w+)", offset = (\d+) : i32, readable = true, writeable = false', after[HELPER]) == list(zip(NAMES,map(str,range(16,53,4))))
    return dict(unchanged_modules=len(before), copied_ports=len(copied),
                wrapper_connections_compared=len(expected), annotation_records_retained=len(new))


def oracle(path):
    reference = clean(path.read_text())
    host, pipe = (module(reference, n)[1] for n in ('FASEDMemoryTimingModel', 'LatencyPipe'))
    heq, eq = equations(host), equations(pipe)
    assert not re.search(r'\$(?:fatal|finish|stop)\b',host), 'SFC termination policy changed'
    assert heq['model_clock'] == 'gate_O' and heq['gate_I'] == 'clock' and heq['model_reset'] == 'hPort_hBits_reset'
    instance, = re.findall(r'LatencyPipe model\s*\((.*?)\);', host, re.S)
    assert '.clock(model_clock)' in instance and '.reset(model_reset)' in instance
    assert set(re.split(r'\s*&\s*', heq['gate_CE'])) == {
        'hPort_toHost_hValid','hPort_fromHost_hReady','ingress_io_nastiInputs_hReady',
        'writeEgress_io_resp_hValid','readEgress_io_resp_hValid','tResetReady'}
    assert heq['targetFire'] == heq['gate_CE']
    kind, = re.findall(r'\b(MCRFile\w*) crFile',host)
    mcr = equations(module(reference, kind)[1])
    for i in (4,9):
        assert any(f"5'h{4+i:x} == rIndex ? 32'h0 : " in v for v in mcr.values())
    pins = ['input clock,reset', 'input [3:0] SatUpDownCounter_io_value,SatUpDownCounter_1_io_value']
    pins += [f'output [31:0] crFile_io_mcr_read_{4+i}_bits' for i in range(10)]
    cone = 'module SFCHistograms('+','.join(pins)+');\n'
    updates, diagnostics = [], []
    for group,count,terms in [('write','SatUpDownCounter_1_io_value',[51,56,61,59,64]),
                              ('read','SatUpDownCounter_io_value',[31,36,41,39,44])]:
        for symbol in [f'_T_{n}' for n in terms]:
            assignment, = re.findall(r'\bwire\s+'+symbol+r'\s*=\s*[^;]+;',pipe)
            cone += assignment+'\n'
        for term,bound in zip(terms[:3],BOUNDS):
            assert eq[f'_T_{term}'] == f"{count} <= 4'h{bound:x}"
        assert eq[f'_T_{terms[3]}'] == f'_T_{terms[0]} | _T_{terms[1]}'
        assert eq[f'_T_{terms[4]}'] == f'_T_{terms[0]} | _T_{terms[1]} | _T_{terms[2]}'
        guards = [f"{count} <= 4'h0", f"~({count} <= 4'h0) & {count} <= 4'h2",
                  f"~_T_{terms[3]} & {count} <= 4'h4", f"~_T_{terms[4]} & {count} <= 4'h8"]
        for bin in range(5):
            name = group+'OutstandingHistogram_'+str(bin); i=NAMES.index(name)
            if bin<4:
                declaration, = re.findall(r'\breg \[31:0\] '+name+r';',pipe)
                increment = '_'+name+'_T_1'
                assert eq[increment] == name+" + 32'h1"
                assignment, = re.findall(r'\bwire \[31:0\] '+increment+r'\s*=\s*[^;]+;',pipe)
                cone += declaration+'\n'+assignment+'\n'
                update, = re.findall(r'if \(reset\) begin\s*'+name+r" <= 32'h0;\s*end else if \("+
                                     re.escape(guards[bin])+r'\) begin\s*'+name+r' <= '+increment+r';\s*end',pipe)
                pos = pipe.index(update); begin=pipe.rfind('always @(posedge clock)',0,pos)
                assert begin>=0 and 'always @' not in pipe[begin+len('always @(posedge clock)'):pos]
                updates.append(update)
                assert eq['tNasti_io_mmReg_'+name] == name
                assert heq[f'crFile_io_mcr_read_{4+i}_bits'] == 'model_tNasti_io_mmReg_'+name
            else:
                assert not re.search(r'\breg \[31:0\] '+name+r';',pipe)
            cone += f'assign crFile_io_mcr_read_{4+i}_bits = '+(name if bin<4 else "32'h0")+';\n'
            diag, = re.findall(r'if \(~reset & ~\(~crFile_io_mcr_write_'+str(4+i)+r'_valid\)\) begin\s*\$fwrite\(.*?Register '+name+r'.*?\);\s*end',host,re.S)
            pos=host.index(diag);begin=host.rfind('always @(posedge clock)',0,pos)
            assert begin>=0 and 'always @' not in host[begin+len('always @(posedge clock)'):pos]
            diagnostics.append(diag)
    cone += 'always @(posedge clock) begin\n'+'\n'.join(updates)+'\nend\nendmodule\n'
    cone += 'module SFCDiagnostics(input clock,reset,'+','.join(f'input crFile_io_mcr_write_{4+i}_valid' for i in range(10))+');\n'
    return cone+'always @(posedge clock) begin\n'+'\n'.join(diagnostics)+'\nend\nendmodule\n'


def allocation(candidate, golden):
    regions = re.findall(r'\{name = "FASEDMemoryTimingModel_0", size = (\d+) : i64, slave = (\d+) : i32, start = (\d+) : i64}',candidate.read_text())
    assert regions and len(set(regions)) == 1
    size,slave,base=map(int,regions[0]);header=golden.with_suffix('.const.h')
    widget,=re.findall(r'registry.add_widget\(new FASEDMemoryTimingModel\((.*?)\n  0,',header.read_text(),re.S)
    tables=re.findall(r'std::vector<std::pair<std::string, uint32_t>>\{(.*?)\n    }',widget,re.S)
    assert len(tables)==2
    for i,name in enumerate(NAMES):
        addresses=re.findall(r'R"ESC\('+name+r'\)ESC", (\d+)',header.read_text())
        assert len(addresses)==1 and int(addresses[0])==base+16+4*i and 20+4*i<=size
        assert f'R"ESC({name})ESC"' in tables[0] and f'R"ESC({name})ESC"' not in tables[1]
    return dict(golden=str(header),sha256=hashlib.sha256(header.read_bytes()).hexdigest(),base=base,size=size,slave=slave,
                offsets=list(range(16,53,4)),addresses=list(range(base+16,base+53,4)),read_only=True)


def differential(candidate, cone, export_tool, out):
    out=out.resolve();out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name,check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=check)
    exported,native=out/'histograms.mlir',out/'histograms.sv'
    run([export_tool,candidate,exported],'export')
    isolated=modules(exported.read_text().replace('firrtl.module public @','firrtl.module @'))
    assert isolated[HELPER]==modules(candidate.read_text())[HELPER], 'production bank rebuilt'
    text=candidate.read_text();body=modules(text)[NEW]
    mutations=[('host-reset',body.replace('firrtl.strictconnect %histograms_hostReset, %hostReset','firrtl.strictconnect %histograms_hostReset, %sim_fased_model_reset'),'wrong host clock/reset'),
               ('target-fire',body.replace('firrtl.strictconnect %histograms_targetFire, %sim_fased_tfire','firrtl.strictconnect %histograms_targetFire, %hostReset'),'wrong target reset/fire'),
               ('pending-w',body.replace('firrtl.subfield %sim_fased_pending_writes[awValue]','firrtl.subfield %sim_fased_pending_writes[wValue]'),'wrong pre-edge read/AW occupancy')]
    for name,changed,diagnostic in mutations:
        assert changed!=body
        bad=out/(name+'.mlir');bad.write_text(text.replace(body,changed))
        assert run([export_tool,bad,out/(name+'-probe.mlir')],name,False).returncode!=0
        assert diagnostic in (out/(name+'.log')).read_text()
    run(['firtool',exported,'--format=mlir','--disable-all-randomization','-o',native],'firtool')
    (out/'oracle.sv').write_text(cone)
    tb='''module check;
reg clock=0,hostReset=0,modelReset=0,targetFire=0;
reg [3:0] pendingReads=0,pendingAW=0,mcr_wstrb=0;
reg [9:0] writes=0,readReady=0;
reg [31:0] writeBits=0;
reg oracleOnly=0,nativeOnly=0;
wire targetClock=clock&targetFire;
wire diagnosticReset=hostReset|nativeOnly,candidateReset=hostReset|oracleOnly;
'''
    for i in range(10):tb+=f'wire [31:0] n{i},s{i};wire rv{i},wr{i};\n'
    ports=['.clock(clock)','.hostReset(candidateReset)','.modelReset(modelReset)','.targetFire(targetFire)',
           '.pendingReads(pendingReads)','.pendingAW(pendingAW)','.mcr_wstrb(mcr_wstrb)']
    for i in range(10):
        ports += [f'.mcr_read_{i}_bits(n{i})',f'.mcr_read_{i}_valid(rv{i})',f'.mcr_read_{i}_ready(readReady[{i}])',
                  f'.mcr_write_{i}_ready(wr{i})',f'.mcr_write_{i}_valid(writes[{i}])',f'.mcr_write_{i}_bits(writeBits)']
    tb+='GGFASEDHistogramsProbe candidate('+','.join(ports)+');\n'
    ports=['.clock(targetClock)','.reset(modelReset)', '.SatUpDownCounter_io_value(pendingReads)', '.SatUpDownCounter_1_io_value(pendingAW)']
    ports += [f'.crFile_io_mcr_read_{4+i}_bits(s{i})' for i in range(10)]
    tb+='SFCHistograms oracle('+','.join(ports)+');\n'
    ports=['.clock(clock)','.reset(diagnosticReset)']+[f'.crFile_io_mcr_write_{4+i}_valid(writes[{i}])' for i in range(10)]
    tb+='SFCDiagnostics diagnostics('+','.join(ports)+');\n'
    tb+='''reg [31:0] expected[0:9];reg [63:0] rng=233;
integer cycles=0,wraps=0,stalledResets=0,hostOnlyResets=0,aboveLastBound=0,badWrite;
function integer bin(input [3:0] count);begin
 bin=count==0?0:count<=2?1:count<=4?2:count<=8?3:4;
end endfunction
task compare;begin
'''
    for i,name in enumerate(NAMES):
        tb+=f'if(n{i}!==s{i}||n{i}!==expected[{i}]||!rv{i}||!wr{i})$fatal(1,"{name} mismatch cycle=%0d",cycles);\n'
        if i%5<4:tb+=f'if(candidate.histograms.{name}!==oracle.{name})$fatal(1,"{name} state mismatch");\n'
    tb+='end endtask\ntask seed(input [31:0] value);begin\n'
    for i,name in enumerate(NAMES):
        if i%5<4:tb+=f'candidate.histograms.{name}=value;oracle.{name}=value;expected[{i}]=value;\n'
    tb+='''end endtask
task step;begin
 #2;if(cycles>0)compare();
 stalledResets+=modelReset&&!targetFire;hostOnlyResets+=hostReset&&!modelReset;
 aboveLastBound+=targetFire&&(pendingReads>8||pendingAW>8);
'''
    for i in range(10):
        if i%5<4:
            event=f"bin({'pendingAW' if i<5 else 'pendingReads'})=={i%5}"
            tb+=f"wraps+=targetFire&&!modelReset&&({event})&&expected[{i}]==32'hffffffff;\nif(targetFire)expected[{i}]=modelReset?0:expected[{i}]+({event});\n"
    tb+='''clock=1;#2;compare();clock=0;#2;cycles++;
end endtask
initial begin
 oracleOnly=$test$plusargs("oracle-only");nativeOnly=$test$plusargs("native-only");
 for(integer i=0;i<10;i++)expected[i]=0;
 if($value$plusargs("bad-write=%d",badWrite))begin
  modelReset=1;targetFire=1;hostReset=1;writes=badWrite[9:0];step();step();
  modelReset=0;hostReset=0;
  for(integer i=0;i<3;i++)begin
   targetFire=i==1;modelReset=i!=1;step();
  end
  targetFire=1;modelReset=0;writes=0;step();step();
  hostReset=1;writes=badWrite[9:0];targetFire=0;modelReset=1;step();step();
  targetFire=1;step();
  $display("PASS illegal-write mask=%0d continued",badWrite);$finish;
 end else begin
 modelReset=1;targetFire=1;step();
 for(integer i=0;i<8192;i++)begin
  pendingReads=i[3:0];pendingAW=i[7:4];hostReset=i[8];modelReset=i[9];targetFire=i[10];
  seed(i[11]?32'hffffffff:32'hfffffffe);writes=hostReset?i[9:0]:0;step();
 end
 for(integer i=0;i<32768;i++)begin
  rng=rng^(rng<<13);rng=rng^(rng>>7);rng=rng^(rng<<17);
  pendingReads=rng[3:0];pendingAW=rng[7:4];hostReset=rng[8];modelReset=i%31==0;targetFire=rng[9];
  readReady=rng[19:10];mcr_wstrb=rng[23:20];writeBits=rng[55:24];writes=hostReset?rng[19:10]:0;
  if(i%97==0)seed(32'hffffffff);step();
 end
 if(!wraps||!stalledResets||!hostOnlyResets||!aboveLastBound)$fatal(1,"missing coverage");
 $display("PASS cycles=%0d wraps=%0d stalled_resets=%0d host_only_resets=%0d above_last_bound=%0d",cycles,wraps,stalledResets,hostOnlyResets,aboveLastBound);$finish;
 end
end endmodule
'''
    (out/'check.sv').write_text(tb)
    run(['verilator','--binary','--assert','-j','4','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'oracle.sv'],'verilator')
    run([out/'obj/check'],'simulation');log=(out/'simulation.log').read_text();assert 'PASS cycles=' in log
    for mask in (*[1<<i for i in range(10)],1023):
        for mode in ('native','oracle','joint'):
            flags=[] if mode=='joint' else ['+'+mode+'-only']
            run([out/'obj/check',*flags,f'+bad-write={mask}'],f'{mode}-write-{mask}')
            diag=(out/f'{mode}-write-{mask}.log').read_text();assert f'PASS illegal-write mask={mask} continued' in diag
            for i,name in enumerate(NAMES):
                assert diag.count(f'Assertion failed: Register {name} is read only') == (3*(2 if mode=='joint' else 1) if mask&(1<<i) else 0)
    return dict({n:int(v) for n,v in re.findall(r'(\w+)=(\d+)',log)},production_bank_cloned_verbatim=True,
                validated_bindings_cloned=7,valid_IR_mutations_rejected=len(mutations),illegal_write_runs=33,
                nonfatal_diagnostics_and_reset_suppression_matched=True,
                scope='pre-edge occupancies and decoded MMIO; independent host/target edges')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir','baseline','golden','immutable','export-tool','output-dir','report-json'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();cone=oracle(a.golden)
    assert ' '.join(cone.split())==' '.join(oracle(a.immutable).split())
    candidate=a.candidate_dir/'post-fame-fased-histograms.mlir'
    result=composition(a.candidate_dir,a.baseline)
    result.update(golden=str(a.golden.resolve()),immutable=str(a.immutable.resolve()),golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),allocated_register_ABI=allocation(candidate,a.golden),
                  rtl_differential=differential(candidate,cone,a.export_tool,a.output_dir),limitation=__doc__)
    a.report_json.write_text(json.dumps(result,indent=2)+'\n')
    print('PASS production FASED occupancy histograms, zero fifth lanes, target reset/fire and nonfatal host diagnostics')


if __name__=='__main__':main()
