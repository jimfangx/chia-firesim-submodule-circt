#!/usr/bin/env python3
"""Compare real emitted grouped FAME graph RTL with the unchanged SFC clock gate.

Input directory contains the frozen SFC/native boundary products documented in
FAMEModelGraphSharedGrouped.md. This is bounded supporting evidence, not formal
proof or full feature acceptance.
"""
from pathlib import Path
import json,subprocess,hashlib,sys
if len(sys.argv) != 2: raise SystemExit('usage: SharedGroupedRTL.py evidence-directory')
w=Path(sys.argv[1]).resolve();r=Path(__file__).resolve().parents[6];v=r/'.conda-env/bin/verilator';commands=[]
rows=json.loads((w/'sfc-fame.json').read_text());(w/'AbstractClockGate.v').write_text(next(x['text'] for x in rows if x['class'].endswith('BlackBoxInlineAnno') and x['name']=='AbstractClockGate.v'))
# Same module/port ABI on independently emitted complete Top boundaries.
ports=[('input','clk',1,'hostClock'),('input','rst',1,'hostReset'),('output','cr',1,'h__tick_sink_ready'),('input','cv',1,'h__tick_sink_valid'),('input','c0',1,'h__tick_sink_bits_c0'),('input','c1',1,'h__tick_sink_bits_c1')]
for i,(prefix,group,bundle) in enumerate([('h__','data',False),('h__','right',False),('w0__','w1_input',True),('w1__','w1_input',True)]):
 ports += [('output','ir'+str(i),1,prefix+group+'_sink_ready'),('input','iv'+str(i),1,prefix+group+'_sink_valid')]
 for field in (['_a','_b'] if bundle else ['']):ports.append(('input','id'+str(i)+('b' if field=='_b' else 'a'),8,prefix+group+'_sink_bits'+('_'+field if field else '')))
for i,(prefix,group,bundle) in enumerate([('h__','out',False),('h__','otherOut',False),('w0__','w1_output',True),('w1__','w1_output',True)]):
 ports += [('input','orr'+str(i),1,prefix+group+'_source_ready'),('output','ov'+str(i),1,prefix+group+'_source_valid')]
 for field in (['_a','_b'] if bundle else ['']):ports.append(('output','od'+str(i)+('b' if field=='_b' else 'a'),8,prefix+group+'_source_bits'+('_'+field if field else '')))
wrapper='module CompareTop(\n'+',\n'.join(d+' '+('['+str(n-1)+':0] ' if n>1 else '')+a for d,a,n,b in ports)+'\n);\nTop target(\n'+',\n'.join('.'+b+'('+a+')' for d,a,n,b in ports)+'\n);\nendmodule\n';(w/'wrapper.sv').write_text(wrapper)
code='''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv); VCheck d; unsigned seq[4]={0,0,0,0},seen[4]={0,0,0,0},clockIndex=0;
for(unsigned cycle=0;cycle<8192;++cycle){bool reset=cycle<4||(cycle>=2011&&cycle<2015)||(cycle>=6201&&cycle<6205); d.clk=0; d.rst=reset; if(reset)clockIndex=0;d.cv=!reset; d.c0=clockIndex%2==0;d.c1=clockIndex%3==0;
'''
for i in range(4):
 code+=f'd.iv{i}=!reset&&((cycle+{i*3})%9!=4);d.id{i}a=(seq[{i}]==0||seq[{i}]%17==9)?128:((seq[{i}]*13+{i*7})%127);d.orr{i}=!reset&&((cycle+{i*2})%7!=2);\n'
 if i>=2:code+=f'd.id{i}b=(seq[{i}]*19+{i*3})%256;\n'
code+='d.eval(); std::printf("%u %u",cycle,d.cr);\n'
for i in range(4):
 code+=f'std::printf(" %u %u",d.ir{i},d.ov{i});if(d.ov{i}){{std::printf(" %u",d.od{i}a);'
 if i>=2:code+=f'std::printf(" %u",d.od{i}b);'
 code+='}\n'
 code+=f'if(d.iv{i}&&d.ir{i})++seq[{i}];if(d.ov{i}&&d.orr{i})++seen[{i}];\n'
code+='std::printf("\\n");if(d.cv&&d.cr)++clockIndex; d.clk=1;d.eval();d.clk=0;d.eval();}\n'
code+='for(unsigned i=0;i<4;++i){std::fprintf(stderr,"stream%u sent%u received%u\\n",i,seq[i],seen[i]);if(seq[i]<250||seen[i]<250)return 2;}return 0;}\n';(w/'driver.cpp').write_text(code)
def run(name,file):
 cmd=list(map(str,[v,'--cc','--exe','--build','-j','2','-Wno-fatal','--top-module','CompareTop','--prefix','VCheck','--Mdir',w/('obj-'+name),file,w/'AbstractClockGate.v',w/'wrapper.sv',w/'driver.cpp']));commands.append(cmd)
 with (w/(name+'-verilator-build.log')).open('w') as f:subprocess.run(cmd,cwd=w,stdout=f,stderr=subprocess.STDOUT,check=True)
 cmd=[str(w/('obj-'+name)/'VCheck')];commands.append(cmd)
 with (w/(name+'-trace.txt')).open('w') as f,(w/(name+'-metrics.txt')).open('w') as e:subprocess.run(cmd,cwd=w,stdout=f,stderr=e,check=True)
# Four workers total for two tiny, independent RTL builds; no manager build overlap.
for name,file in [('native-cli',w/'native-cli/post-fame-model-graph-boundary.sv'),('sfc',w/'sfc.sv')]:run(name,file)
a=(w/'native-cli-trace.txt').read_bytes();b=(w/'sfc-trace.txt').read_bytes()
assert a==b,'actual CLI and unchanged SFC RTL traces differ'
(w/'rtl-differential-result.json').write_text(json.dumps({'passed':True,'cycles':8192,'scope':'Complete grouped graph boundary actual emitted RTL, unchanged production registered AbstractClockGate implementation; not a Jasper proof or transported Linux representative.','defined_comparison':'All source valid/sink ready and clock ready strict, output payload when source valid (including stalls).','initial_state':'Verilator two-state default initializes unreset target registers to common concrete zero; a bounded common initial-state sample, not an initialization requirement or inferred formal relation. No memory is in this fixture.','commands':commands,'trace_sha256':hashlib.sha256(a).hexdigest(),'metrics':{n:(w/(n+'-metrics.txt')).read_text() for n in ['native-cli','sfc']},'input_hashes':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [w/'native-cli/post-fame-model-graph-boundary.sv',w/'sfc.sv',w/'AbstractClockGate.v',w/'wrapper.sv',w/'driver.cpp']}},indent=2)+'\n')
print('PASS 8192 cycles of independently emitted actual CLI/SFC complete grouped graph RTL with real registered SFC clock gates')
