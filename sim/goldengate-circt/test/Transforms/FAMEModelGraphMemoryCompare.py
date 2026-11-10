#!/usr/bin/env python3
"""Matched actual SFC/native memory+combinational FAME boundary RTL.

Run after the oracle and native CLI emitted the same prepared input. No target
memory initialization or reset assumptions are added. The independent ledger
compares only written memory and enabled synchronous read results.
"""
from pathlib import Path
import hashlib,json,re,subprocess,sys
if len(sys.argv)!=2:raise SystemExit('usage: MemoryCompare.py evidence-directory')
w=Path(sys.argv[1]).resolve();r=Path(__file__).resolve().parents[6];v=r/'.conda-env/bin/verilator';ft=r/'.conda-env/riscv-tools/bin/firtool';cmds=[]
def run(cmd,log):
 cmds.append(list(map(str,cmd)))
 with (w/log).open('w') as f:subprocess.run(list(map(str,cmd)),cwd=w,stdout=f,stderr=subprocess.STDOUT,check=True)
# sfc.sv is emitted by the immutable Scala VerilogCompiler, not firtool.
assert (w/'sfc.sv').is_file()
run([ft,w/'sfc-wrapper.fir','--disable-all-randomization','--disable-annotation-unknown','-o',w/'sfc-transport.sv'],'sfc-wrapper-firtool.log')
a=json.loads((w/'sfc-fame.json').read_text());b=json.loads((w/'native-cli/post-fame-model-graph-boundary-all.json').read_text())
excluded={'midas.InternalXDCAnnotation','firrtl.transforms.BlackBoxInlineAnno','midas.passes.fame.FAMEChannelFanoutAnnotation'}
assert [x for x in a if x['class'] not in excluded]==b,'complete retained annotations differ'
(w/'AbstractClockGate.v').write_text(next(x['text'] for x in a if x['class'].endswith('BlackBoxInlineAnno')))
mlir=(w/'native-cli/post-fame-model-graph-boundary.mlir').read_text();clocks=[]
for row in a:
 if row['class']!='midas.InternalXDCAnnotation':continue
 gate=row['argumentList'][0].split('>')[1].split('.')[0];name=re.search(r'create_generated_clock -name (\w+)',row['formatString'])[1];mfmr=int(re.search(r'set_multicycle_path (\d+) -setup',row['formatString'])[1]);line=next(x for x in mlir.splitlines() if 'firrtl.instance '+gate+' ' in x)
 assert f'mfmr = {mfmr} : i64, name = "{name}"' in line,'generated clock identity/MFMR differs'
 clocks.append({'gate':gate+'.O','name':name,'mfmr':mfmr})
assert len(clocks)==2
ports=[('input','clk',1,'hostClock'),('input','rst',1,'hostReset'),('output','cr',1,'h__tick_sink_ready'),('input','cv',1,'h__tick_sink_valid'),('input','c0',1,'h__tick_sink_bits_c0'),('input','c1',1,'h__tick_sink_bits_c1')]
for i,(prefix,group,width) in enumerate([('h__','data',8),('h__','right',8),('m__','request',16),('m__','read',4),('x__','data',8),('x__','salt',8)]):
 ports += [('output','ir'+str(i),1,prefix+group+'_sink_ready'),('input','iv'+str(i),1,prefix+group+'_sink_valid'),('input','id'+str(i),width,prefix+group+'_sink_bits')]
for i,(prefix,group) in enumerate([('h__','out'),('h__','otherOut'),('m__','asynchronous'),('m__','synchronous'),('x__','out')]):
 ports += [('input','orr'+str(i),1,prefix+group+'_source_ready'),('output','ov'+str(i),1,prefix+group+'_source_valid'),('output','od'+str(i),8,prefix+group+'_source_bits')]
header='module CompareTop(\n'+',\n'.join(d+' '+('['+str(n-1)+':0] ' if n>1 else '')+a for d,a,n,b in ports)+',\noutput mem_step, output reg [31:0] mem_edges, output reg [15:0] captured_request, output reg [3:0] captured_read\n);\n'
def wrapper(mode):
 body='wire [7:0] raw_od2;wire raw_ir2;\nTop target(\n'+',\n'.join('.'+b+'('+({'od2':'raw_od2','ir2':'raw_ir2'}.get(a,a))+')' for d,a,n,b in ports)+'\n);\n'
 return header+body+'assign mem_step=target.m_.clock_buffer.CE;\ninitial mem_edges=0; always @(posedge target.m_.clock_buffer.O) begin mem_edges <= mem_edges+1; captured_request <= id2; captured_read <= id3; end\nassign od2=raw_od2 ^ '+{'normal':"8'b0",'invalid':"(ov2 ? 8'b0 : 8'hff)",'payload':"8'h1",'ready':"8'b0"}[mode]+';\nassign ir2='+('~raw_ir2' if mode=='ready' else 'raw_ir2')+';\nendmodule\n'
code=r'''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VCheck d;unsigned seq[6]={},seen[5]={},clockIndex=0,payload[6]={},ledger[16]={},writes=0,masked=0,disabled=0,asyncChecks=0,syncChecks=0,comboChecks=0,collisions=0,edges=0,initialEdges=0;bool pending[6]={},known[16]={},syncKnown=false;unsigned syncValue=0;
for(unsigned cycle=0;cycle<16384;++cycle){bool reset=cycle<4||(cycle>=5400&&cycle<5404)||(cycle>=11000&&cycle<11004);bool warm=cycle<128||(cycle>=5400&&cycle<5528)||(cycle>=11000&&cycle<11128);d.clk=0;d.rst=reset;if(reset)clockIndex=0;d.cv=!reset;d.c0=clockIndex%2==0;d.c1=clockIndex%3==0;
'''
for i in range(6):
 payload='(warm?128:((seq[%d]*13+%d)%%127))'%(i,i*7) if i<2 else ('((seq[2]<64||seq[2]%7!=3?0x8000:0)|(seq[2]<64||seq[2]%5!=2?0x4000:0)|((seq[2]%16)<<8)|((seq[2]*29+11)%256))' if i==2 else ('((seq[3]*7)%16)' if i==3 else '(seq[%d]*17+%d)%%256'%(i,i*11)))
 code+=f'if(reset)pending[{i}]=false;if(!reset&&!pending[{i}]&&(warm||(cycle+{i*3})%11!=4)){{pending[{i}]=true;payload[{i}]={payload};}}d.iv{i}=pending[{i}];d.id{i}=payload[{i}];\n'
for i in range(5):code+=f'd.orr{i}=!reset&&(warm||(cycle+{i*2})%7!=2);\n'
code+=r'''d.eval();if(cycle>0){std::printf("%u %u %u %u",cycle,d.cr,d.mem_step,d.mem_edges-initialEdges);
'''
for i in range(6):code+=f'std::printf(" %u",d.ir{i});\n'
for i in range(5):
 condition= '!warm' if i<2 else ('known[d.id3]' if i==2 else 'syncKnown' if i==3 else 'true')
 code+=f'std::printf(" %u",d.ov{i});if(d.ov{i}&&({condition}))std::printf(" %u",d.od{i});\n'
code+=r'''std::printf("\n");}
if(d.ov2&&known[d.id3]){++asyncChecks;if(d.od2!=ledger[d.id3]){std::fprintf(stderr,"async ledger mismatch cycle%u observed%u expected%u address%u\n",cycle,d.od2,ledger[d.id3],d.id3);return 10;}}
if(d.ov3&&syncKnown){++syncChecks;if(d.od3!=syncValue){std::fprintf(stderr,"sync ledger mismatch cycle%u observed%u expected%u\n",cycle,d.od3,syncValue);return 11;}}
if(d.ov4){++comboChecks;if(d.od4!=(d.id4^d.id5)){std::fprintf(stderr,"combinational xor mismatch\n");return 12;}}

'''
for i in range(6):code+=f'if(d.iv{i}&&d.ir{i}){{++seq[{i}];pending[{i}]=false;}}\n'
for i in range(5):code+=f'if(d.ov{i}&&d.orr{i})++seen[{i}];\n'
code+=r'''if(d.cv&&d.cr)++clockIndex;d.clk=1;d.eval();
if(d.mem_edges!=edges){if(d.mem_edges!=edges+1)return 13;edges=d.mem_edges;unsigned read=d.captured_read,request=d.captured_request;syncKnown=known[read];syncValue=ledger[read];unsigned addr=(request>>8)&15;if(request&0x8000){if(request&0x4000){collisions+=addr==read;known[addr]=true;ledger[addr]=request&255;++writes;}else ++masked;}else ++disabled;}
d.clk=0;d.eval();if(cycle==0)initialEdges=d.mem_edges;}
for(unsigned i=0;i<6;++i){std::fprintf(stderr,"input%u=%u\n",i,seq[i]);if(seq[i]<1000)return 2;}for(unsigned i=0;i<5;++i){std::fprintf(stderr,"output%u=%u\n",i,seen[i]);if(seen[i]<1000)return 3;}
std::fprintf(stderr,"writes=%u masked=%u disabled=%u collisions=%u async_checks=%u sync_checks=%u combo_checks=%u\n",writes,masked,disabled,collisions,asyncChecks,syncChecks,comboChecks);if(writes<1000||masked<100||disabled<100||collisions<100||asyncChecks<1000||syncChecks<1000||comboChecks<1000)return 4;return 0;}
'''
(w/'driver.cpp').write_text(code)
def simulate(name,mode,sv,seed):
 source=w/(name+'-wrapper.sv');monitor_instance=re.search(r"\bMemory\s+(\w+)\s*\(",sv.read_text())[1];source.write_text(wrapper(mode).replace("target.m_.", "target."+monitor_instance+"."))
 run([v,'--cc','--exe','--build','-j','1','-Wno-fatal','--x-initial','unique','--x-assign','unique','--top-module','CompareTop','--prefix','VCheck','--Mdir',w/('obj-'+name),sv,w/'AbstractClockGate.v',source,w/'driver.cpp'],name+'-build.log')
 cmd=[str(w/('obj-'+name)/'VCheck'),'+verilator+rand+reset+2','+verilator+seed+'+str(seed)];cmds.append(cmd)
 with (w/(name+'-trace.txt')).open('w') as f,(w/(name+'-metrics.txt')).open('w') as e:result=subprocess.run(cmd,cwd=w,stdout=f,stderr=e)
 return result.returncode,(w/(name+'-trace.txt')).read_bytes()
traces={}
for name,mode,sv,seed in [('native','normal',w/'native-cli/post-fame-model-graph-boundary.sv',11),('sfc','normal',w/'sfc.sv',73),('invalid','invalid',w/'native-cli/post-fame-model-graph-boundary.sv',11),('payload','payload',w/'native-cli/post-fame-model-graph-boundary.sv',11),('ready','ready',w/'native-cli/post-fame-model-graph-boundary.sv',11)]:
 traces[name]=simulate(name,mode,sv,seed)
assert traces['native'][0]==traces['sfc'][0]==0,'ledger/runtime check failed'
assert traces['native'][1]==traces['sfc'][1],'complete memory boundary RTL mismatch'
assert traces['invalid']==traces['native'],'invalid memory payload was compared'
assert traces['payload']!=traces['native'],'valid memory corruption escaped'
assert traces['ready']!=traces['native'],'memory request ready corruption escaped'
(w/'rtl-differential-result.json').write_text(json.dumps({'passed':True,'cycles':16384,'retained_annotations':len(b),'clock_contracts':clocks,'trace_sha256':hashlib.sha256(traces['native'][1]).hexdigest(),'comparison':'All host ready/valid, clock ready, actual target-memory clock enable and observed gated-clock edge count are strict after first host reset edge; asynchronous memory checked only for written addresses; synchronous output checked after enabled read of written address, old RUW ledger; combinational XOR always checked; hub payload after target reset warmup. Independent stalls and rational1/2,1/3 held clock tokens. Three host resets do not clear the ledger or target memories.','initial_state':'Independent Verilator random startup seeds11/73. No memory initialization or clearing. The unchanged registered gate can deliver a startup edge from its unreset enable. The monitor observes actual gated-clock edges and normalizes only its initial counter offset after the first host reset edge; target memory state is never altered. Host resets preserve target memories. Two-state bounded RTL evidence, not X/four-state or formal proof.','negative_controls':{'invalid_payload_ignored':True,'valid_memory_corruption_rejected':True,'request_ready_corruption_rejected':True},'scope':'Complete unchanged SFC FAMETransform/native model boundary RTL, actual SFC registered clock gate. Actual SFC SimWrapper and native full Pipe wrapper syntax checked separately; no claim of transported wrapper RTL equivalence, platform ABI, Jasper, F2 or feature acceptance.','commands':cmds,'metrics':{n:(w/(n+'-metrics.txt')).read_text() for n in ['native','sfc']}},indent=2)+'\n')
print('PASS complete memory/combinational FAME boundary:16384 cycles, independent old-RUW/write-mask ledger and corruption controls')
