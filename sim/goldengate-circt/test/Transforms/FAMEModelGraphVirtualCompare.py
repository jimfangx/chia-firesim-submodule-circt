#!/usr/bin/env python3
"""Actual unchanged SFC/native hubless scalar virtual-model boundary RTL."""
from pathlib import Path
import hashlib,json,re,subprocess,sys
w=Path(sys.argv[1]).resolve();w.mkdir(parents=True,exist_ok=True);binary=Path(sys.argv[2]).resolve();r=Path(__file__).resolve().parents[6];s=Path(__file__).resolve().parent
jar=Path('/scratch/jfx/f2-sfc-goldens/toolchain/firesim-main.jar');java=r/'.conda-env/lib/jvm/bin/java';v=r/'.conda-env/bin/verilator';commands=[]
def run(cmd,log,check=True):
 commands.append(list(map(str,cmd)))
 with (w/log).open('w') as f:p=subprocess.run(list(map(str,cmd)),cwd=w,stdout=f,stderr=subprocess.STDOUT,check=check)
 return p.returncode
cp='/home/ubuntu/.cache/coursier/v1/https/repo1.maven.org/maven2/org/scala-lang/'
compiler=Path(cp+'scala-compiler/2.13.10/scala-compiler-2.13.10.jar');reflect=Path(cp+'scala-reflect/2.13.10/scala-reflect-2.13.10.jar');(w/'classes').mkdir(exist_ok=True)
run([java,'-XX:ActiveProcessorCount=1','-cp',str(compiler)+':'+str(reflect)+':'+str(jar),'scala.tools.nsc.Main','-usejavacp','-d',w/'classes',s/'FAMEModelGraphVirtualOracle.scala'],'scalac.log')
run([java,'-XX:ActiveProcessorCount=1','-cp',str(w/'classes')+':'+str(jar),'midas.passes.fame.FAMEModelGraphVirtualOracle',w,s/'FAMEModelGraphVirtual.fir',s/'FAMEModelGraphVirtual.anno.json'],'oracle.log')
run([binary,s/'FAMEModelGraphVirtual.fir','--annotation-file',s/'FAMEModelGraphVirtual.anno.json','--output-dir',w/'native','--rewrite-fame-model-graph-boundary'],'native.log')
assert run([binary,s/'FAMEModelGraphVirtual.fir','--annotation-file',s/'FAMEModelGraphVirtual.anno.json','--output-dir',w/'unsupported-transport','--rewrite-fame-model-graph'],'unsupported-transport.log',False)!=0
assert 'simulator transport requires an explicit target clock hub' in (w/'unsupported-transport.log').read_text()
a=json.loads((w/'sfc-fame.json').read_text());b=json.loads((w/'native/post-fame-model-graph-boundary-all.json').read_text());assert [x for x in a if not x['class'].endswith('BlackBoxInlineAnno')]==b,'all retained annotation rows differ'
(w/'AbstractClockGate.v').write_text(next(x['text'] for x in a if x['class'].endswith('BlackBoxInlineAnno')))
ports=[('input','clk',1,'hostClock'),('input','rst',1,'hostReset')]
for i in range(2):
 ports += [('output',f'ir{i}',1,f'w{i}__data_sink_ready'),('input',f'iv{i}',1,f'w{i}__data_sink_valid'),('input',f'id{i}',8,f'w{i}__data_sink_bits'),('input',f'orr{i}',1,f'w{i}__out_source_ready'),('output',f'ov{i}',1,f'w{i}__out_source_valid'),('output',f'od{i}',8,f'w{i}__out_source_bits')]
head='module CompareTop(\n'+',\n'.join(d+' '+(f'[{n-1}:0] ' if n>1 else '')+p for d,p,n,_ in ports)+',\noutput [31:0] edges0, edges1, output [7:0] captured0, captured1\n);\n'
def wrapper(sfc,mode='normal'):
 mapping=[]
 for _,p,_,n in ports:
  if mode=='binding' and p=='id0':p='id1'
  if mode=='ready' and p=='orr0':p='orr1'
  mapping.append('.'+n+'('+p+')')
 t=head+'Top target(\n'+',\n'.join(mapping)+'\n);\n'
 for j in range(2):
  inst=f'w{j}__' if sfc else f'w{j}_'
  t+=f'reg [31:0] edge{j}=0; reg [7:0] value{j};always @(posedge target.{inst}.clock_buffer.O) begin edge{j}<=edge{j}+1;value{j}<=target.{inst}.data_sink_bits;end\nassign edges{j}=edge{j};assign captured{j}=value{j};\n'
 return t+'endmodule\n'
code=r'''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VCheck d;unsigned seq[2]={},outputs[2]={},held[2]={},edges[2]={},initial[2]={},state[2]={},checks[2]={},resets[2]={};bool pending[2]={},known[2]={};
for(unsigned cycle=0;cycle<16384;++cycle){bool reset=cycle<4||(cycle>=5400&&cycle<5404)||(cycle>=11000&&cycle<11004);d.clk=0;d.rst=reset;
'''
for i in range(2):
 code+=f'if(reset)pending[{i}]=false;if(!reset&&!pending[{i}]&&(cycle+{i*3})%11!=4){{pending[{i}]=true;held[{i}]=(seq[{i}]%17==0?128:0)|((seq[{i}]*{31+i*6}+{i*11})%127);}}d.iv{i}=pending[{i}];d.id{i}=held[{i}];d.orr{i}=!reset&&(cycle+{i*2})%7!=2&&(cycle/71+{i})%5!=3;\n'
code+='d.eval();if(cycle==128&&(!known[0]||!known[1]))return 10;if(cycle>0){std::printf("%u",cycle);\n'
for i in range(2):
 code+=f'std::printf(" %u %u %u",d.ir{i},d.ov{i},d.edges{i}-initial[{i}]);if(cycle>128&&d.ov{i})std::printf(" %u",d.od{i});\n'
code+='std::printf("\\n");}\n'
for i in range(2):
 code+=f'if(d.ov{i}&&known[{i}]){{++checks[{i}];if(d.od{i}!=((state[{i}]+(d.id{i}&127))&255)){{std::fprintf(stderr,"ledger mismatch instance{i} cycle%u observed%u expected%u\\n",cycle,d.od{i},(state[{i}]+(d.id{i}&127))&255);return 7;}}}}if(d.iv{i}&&d.ir{i}){{++seq[{i}];pending[{i}]=false;}}if(d.ov{i}&&d.orr{i})++outputs[{i}];\n'
code+='d.clk=1;d.eval();\n'
for i in range(2):code+=f'if(d.edges{i}!=edges[{i}]){{if(d.edges{i}!=edges[{i}]+1)return 8;edges[{i}]=d.edges{i};if(d.captured{i}&128){{state[{i}]=0;known[{i}]=true;++resets[{i}];}}else state[{i}]=(state[{i}]+(d.captured{i}&127))&255;}}\n'
code+='d.clk=0;d.eval();if(cycle==0){initial[0]=d.edges0;initial[1]=d.edges1;}}\n'
code+='for(unsigned i=0;i<2;++i){std::fprintf(stderr,"instance%u inputs=%u outputs=%u checks=%u resets=%u\\n",i,seq[i],outputs[i],checks[i],resets[i]);if(seq[i]<1000||outputs[i]<1000||checks[i]<1000||resets[i]<100)return 9;}return 0;}\n'
(w/'driver.cpp').write_text(code)
def simulate(name,sfc,mode='normal'):
 source=w/(name+'-wrapper.sv');source.write_text(wrapper(sfc,mode));sv=w/'sfc.sv' if sfc else w/'native/post-fame-model-graph-boundary.sv'
 run([v,'--cc','--exe','--build','-j','1','-Wno-fatal','--x-initial','unique','--x-assign','unique','--top-module','CompareTop','--prefix','VCheck','--Mdir',w/('obj-'+name),sv,w/'AbstractClockGate.v',source,w/'driver.cpp'],name+'-build.log')
 cmd=[w/('obj-'+name)/'VCheck','+verilator+rand+reset+2','+verilator+seed+'+('73' if sfc else '11')];commands.append(list(map(str,cmd)))
 with (w/(name+'-trace.txt')).open('w') as f,(w/(name+'-metrics.txt')).open('w') as e:p=subprocess.run(list(map(str,cmd)),stdout=f,stderr=e)
 return p.returncode,(w/(name+'-trace.txt')).read_bytes()
traces={}
for name,sfc,mode in [('native',False,'normal'),('sfc',True,'normal'),('binding',False,'binding'),('ready',False,'ready')]:traces[name]=simulate(name,sfc,mode)
assert traces['native'][0]==traces['sfc'][0]==0,'actual RTL/independent ledger check failed'
assert traces['native'][1]==traces['sfc'][1],'complete virtual graph boundary RTL mismatch'
assert traces['binding']!=traces['native'] and traces['ready']!=traces['native'],'independence fault escaped'
result={'passed':True,'cycles':16384,'retained_annotations':len(b),'compiler_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'oracle_jar_sha256':hashlib.sha256(jar.read_bytes()).hexdigest(),'scope':'Actual unchanged SFC FAMETransform+VerilogCompiler and native boundary. One shared scalar definition, two independent virtual-clock instances, no hub. SFC SimWrapper requires one clock channel; native full transport remains an atomic rejection. Supporting bounded two-state evidence, not Jasper or Linux/F2 feature acceptance.','checks':'Complete retained annotation rows; all ready/valid and actual clock-edge schedules after first reset edge; all valid output payloads after128 cycles; independent per-instance persistent state ledger after observed target reset; three host resets never clear target state. Each domain receives over100 target resets and over1000 live payload checks.','initial_state':'Independent Verilator seeds11/73, no register initialization/force. Read-only monitor tracks actual gated edges/captured input. First registered-gate startup edge count offset normalized; payload checks begin after explicit target reset observed in each instance.','negative_controls':{'independent_instance_input_crosswire':True,'independent_output_ready_crosswire':True},'metrics':{x:(w/(x+'-metrics.txt')).read_text() for x in traces},'commands':commands,'trace_sha256':hashlib.sha256(traces['native'][1]).hexdigest()}
(w/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS hubless repeated scalar virtual models:16384 actual SFC/native RTL cycles, independent persistent ledgers and wiring faults')
