#!/usr/bin/env python3
"""Execute complete matched SFC SimWrapper/native Pipe graph RTL.

The supplied directory contains actual Scala-emitted sfc.sv, unchanged
SimWrapper sfc-wrapper.fir, and native/post-fame-model-graph.sv on the same
FAMEModelGraphTransport input. No target state is forced or initialized.
"""
from pathlib import Path
import hashlib,json,re,subprocess,sys
w=Path(sys.argv[1]).resolve();r=Path(__file__).resolve().parents[6]
v=r/'.conda-env/bin/verilator';ft=r/'.conda-env/riscv-tools/bin/firtool';commands=[]
def run(cmd,log):
 commands.append(list(map(str,cmd)))
 with (w/log).open('w') as f:subprocess.run(list(map(str,cmd)),cwd=w,stdout=f,stderr=subprocess.STDOUT,check=True)
run([ft,w/'sfc-wrapper.fir','--disable-all-randomization','--disable-annotation-unknown','-o',w/'sfc-transport.sv'],'wrapper-export.log')
a=json.loads((w/'sfc-fame.json').read_text())
(w/'AbstractClockGate.v').write_text(next(x['text'] for x in a if x['class'].endswith('BlackBoxInlineAnno')))
# Named-port adapter links the actual Scala-emitted target to its own unchanged
# SimWrapper. Its two clock vector lanes retain index0=c0 and index1=c1.
box=(w/'sfc-wrapper.fir').read_text().split('  extmodule TargetBox :\n')[1].split('    defname')[0]
ports=[]
for direction,name,typ in re.findall(r'^    (input|output) (\w+) : (UInt<\d+>|Clock)$',box,re.M):
 width=int(re.search(r'\d+',typ)[0]) if typ!='Clock' else 1
 ports.append((direction,name,width))
assert len(ports)==30
(w/'TargetBox.sv').write_text('module TargetBox(\n'+',\n'.join(d+' '+(f'[{n-1}:0] ' if n>1 else '')+p for d,p,n in ports)+'\n);\nTop target(\n'+',\n'.join('.'+({'h__tick_sink_bits__0':'h__tick_sink_bits_c0','h__tick_sink_bits__1':'h__tick_sink_bits_c1'}.get(p,p))+'('+p+')' for d,p,n in ports)+'\n);\nendmodule\n')
ports=[('input','clk',1,'hostClock'),('input','rst',1,'hostReset'),('output','cr',1,'h__tick_sink_ready'),('input','cv',1,'h__tick_sink_valid'),('input','c0',1,'h__tick_sink_bits_0'),('input','c1',1,'h__tick_sink_bits_1')]
for i,g in enumerate(['data','right']):ports += [('output',f'ir{i}',1,'h__'+g+'_sink_ready'),('input',f'iv{i}',1,'h__'+g+'_sink_valid'),('input',f'id{i}',8,'h__'+g+'_sink_bits')]
for i,g in enumerate(['h__out','w0__out','w1__out']):ports += [('input',f'orr{i}',1,g+'_source_ready'),('output',f'ov{i}',1,g+'_source_valid'),('output',f'od{i}',8,g+'_source_bits')]
head='module CompareTop(\n'+',\n'.join(d+' '+(f'[{n-1}:0] ' if n>1 else '')+p for d,p,n,_ in ports)+',\noutput [3:0] initialized, output [31:0] edges0, edges1, edges2, edges3\n);\n'
def wrapper(sfc=False,mode='normal'):
 mapping=[]
 for _,p,_,n in ports:
  if sfc:n={'hostClock':'clock','hostReset':'reset'}.get(n,'channelPorts_'+n)
  if mode=='lanes' and p in ('c0','c1'):p='c1' if p=='c0' else 'c0'
  if mode=='ready' and p=='orr1':p='orr2'
  mapping.append('.'+n+'('+p+')')
 body=head+('SimWrapper' if sfc else 'GGFAMEPipeWrapper')+' dut(\n'+',\n'.join(mapping)+'\n);\n'
 base='dut.target.target' if sfc else 'dut.target_FAMETop'
 for j,(inst,gate,data) in enumerate([('h','c0','data'),('h','c1','right'),('w0','clock','data'),('w1','clock','data')]):
  inst=inst+'__' if sfc else inst+'_'
  path=base+'.'+inst
  body+=f'reg init{j}=0; reg [31:0] edge{j}=0; always @(posedge {path}.{gate}_buffer.O) begin edge{j}<=edge{j}+1; if({path}.{data}_sink_bits[7])init{j}<=1; end\nassign initialized[{j}]=init{j};assign edges{j}=edge{j};\n'
 return body+'endmodule\n'
code=r'''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VCheck d;unsigned seq[2]={},out[3]={},ticks=0,clockIndex=0,held[2]={},base[4]={};bool pending[2]={},clockPending=false;
for(unsigned cycle=0;cycle<32768;++cycle){bool reset=cycle<4||(cycle>=11000&&cycle<11004)||(cycle>=23000&&cycle<23004);d.clk=0;d.rst=reset;if(reset){clockIndex=0;clockPending=false;}else if(!clockPending&&cycle%13!=7)clockPending=true;d.cv=clockPending;d.c0=clockIndex%2==0;d.c1=clockIndex%3==0;
'''
for i in range(2):code+=f'if(reset)pending[{i}]=false;if(!reset&&!pending[{i}]&&(cycle+{i*3})%11!=4){{pending[{i}]=true;held[{i}]=(seq[{i}]%17==0?128:0)|((seq[{i}]*{31+i*6}+{i*11})%127);}}d.iv{i}=pending[{i}];d.id{i}=held[{i}];\n'
for i in range(3):code+=f'd.orr{i}=!reset&&(cycle+{i*2})%7!=2&&(cycle/71+{i})%5!=3;\n'
code+='d.eval();if(cycle==1024&&d.initialized!=15){std::fprintf(stderr,"target reset coverage absent %u\\n",d.initialized);return 5;}if(cycle>0){std::printf("%u %u %u %u",cycle,d.cr,d.ir0,d.ir1);\n'
for i in range(3):code+=f'std::printf(" %u",d.ov{i});if(cycle>1024&&d.ov{i})std::printf(" %u",d.od{i});\n'
code+='for(unsigned j=0;j<4;++j){unsigned n=j==0?d.edges0:j==1?d.edges1:j==2?d.edges2:d.edges3;std::printf(" %u",n-base[j]);}std::printf("\\n");}\n'
for i in range(2):code+=f'if(d.iv{i}&&d.ir{i}){{++seq[{i}];pending[{i}]=false;}}\n'
for i in range(3):code+=f'if(d.ov{i}&&d.orr{i})++out[{i}];\n'
code+='if(d.cv&&d.cr){++clockIndex;++ticks;clockPending=false;}d.clk=1;d.eval();d.clk=0;d.eval();if(cycle==0){base[0]=d.edges0;base[1]=d.edges1;base[2]=d.edges2;base[3]=d.edges3;}}\n'
code+='std::fprintf(stderr,"inputs=%u,%u outputs=%u,%u,%u ticks=%u initialized=%u edges=%u,%u,%u,%u\\n",seq[0],seq[1],out[0],out[1],out[2],ticks,d.initialized,d.edges0,d.edges1,d.edges2,d.edges3);return seq[0]>1000&&seq[1]>1000&&out[0]>1000&&out[1]>1000&&out[2]>1000&&ticks>1000&&d.initialized==15?0:6;}\n'
(w/'driver.cpp').write_text(code)
def simulate(name,sfc,mode='normal',latency=False):
 source=w/(name+'-wrapper.sv');source.write_text(wrapper(sfc,mode));sv=w/'native/post-fame-model-graph.sv'
 if latency:
  t=sv.read_text();assert t.count('initializing <= reset;')==1;t=t.replace('initializing <= reset;',"initializing <= 1'b0;");sv=w/'latency-mutated.sv';sv.write_text(t)
 files=[w/'sfc-transport.sv',w/'TargetBox.sv',w/'sfc.sv'] if sfc else [sv]
 run([v,'--cc','--exe','--build','-j','1','-Wno-fatal','--x-initial','unique','--x-assign','unique','--top-module','CompareTop','--prefix','VCheck','--Mdir',w/('obj-'+name),*files,w/'AbstractClockGate.v',source,w/'driver.cpp'],name+'-build.log')
 cmd=[w/('obj-'+name)/'VCheck','+verilator+rand+reset+2','+verilator+seed+'+('73' if sfc else '11')];commands.append(list(map(str,cmd)))
 with (w/(name+'-trace.txt')).open('w') as f,(w/(name+'-metrics.txt')).open('w') as e:p=subprocess.run(list(map(str,cmd)),cwd=w,stdout=f,stderr=e)
 return p.returncode,(w/(name+'-trace.txt')).read_bytes()
traces={}
for name,sfc,mode,latency in [('native',False,'normal',False),('sfc',True,'normal',False),('lanes',False,'lanes',False),('ready',False,'ready',False),('latency',False,'normal',True)]:traces[name]=simulate(name,sfc,mode,latency)
assert traces['native'][0]==traces['sfc'][0]==0,'actual wrapper run or reset/activity coverage failed'
assert traces['native'][1]==traces['sfc'][1],'complete transported wrapper runtime differs'
for name in ['lanes','ready','latency']:assert traces[name]!=traces['native'],name+' fault escaped'
result={'passed':True,'cycles':32768,'graph':{'definitions':2,'instances':3,'hub_domains':2,'pipe_queues':7,'latency1':1,'internal_atomic_fanout_branches':2},'scope':'Actual unchanged SFC FAMETransform/VerilogCompiler target + unchanged SimWrapper against native complete Pipe wrapper. Prepared low-FIRRTL graph; supporting RTL comparison, not platform/F2/Linux or formal acceptance.','comparison':'All external ready/valid controls and all actual target-clock edge schedules strictly compared after first reset edge; all valid output payloads strictly compared after1024 cycles. Independent held clock tokens with ratios1/2,1/3, independent producer/consumer stalls, three host resets. Read-only monitors require each target register domain to receive its own explicit target reset by cycle1024.','initial_state':'Independent two-state Verilator startup seeds11/73; no target register force/initialization. First registered-gate startup edge offset normalized in read-only counters. Output payloads before the four explicit target-reset cover events drain are omitted; controls and subsequent live payloads are strict.','negative_controls':{'clock_lane_swap':True,'independent_output_ready_crosswire':True,'latency1_initial_token_removed':True},'metrics':{x:(w/(x+'-metrics.txt')).read_text() for x in traces},'commands':commands,'trace_sha256':hashlib.sha256(traces['native'][1]).hexdigest()}
(w/'rtl-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS actual complete transported model graph RTL:32768 cycles, independent controls/payloads/gated clocks and three faults')
