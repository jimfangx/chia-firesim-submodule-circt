#!/usr/bin/env python3
"""Matched actual SFC/native repeated-definition FAME boundary RTL.

Run after the oracle and native CLI emitted the same prepared input. No target
memory initialization or reset assumptions are added. The independent ledger
compares only written memory and enabled synchronous read results.
"""
from pathlib import Path
import hashlib,json,re,subprocess,sys
if len(sys.argv)!=2:raise SystemExit('usage: RepeatedAliasesCompare.py evidence-directory')
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
for i,(prefix,group,width) in enumerate([('h__','data',8),('h__','right',8),('m__','request',16),('m__','read',4),('m1__','request',16),('m1__','read',4),('x__','data',8),('x__','salt',8),('x1__','data',8),('x1__','salt',8)]):
 ports += [('output','ir'+str(i),1,prefix+group+'_sink_ready'),('input','iv'+str(i),1,prefix+group+'_sink_valid'),('input','id'+str(i),width,prefix+group+'_sink_bits')]
for i,(prefix,group) in enumerate([('h__','out'),('h__','otherOut'),('m__','asynchronous'),('m__','synchronous'),('m1__','asynchronous'),('m1__','synchronous'),('x__','out'),('x1__','out')]):
 ports += [('input','orr'+str(i),1,prefix+group+'_source_ready'),('output','ov'+str(i),1,prefix+group+'_source_valid'),('output','od'+str(i),8,prefix+group+'_source_bits')]
header='module CompareTop(\n'+',\n'.join(d+' '+('['+str(n-1)+':0] ' if n>1 else '')+a for d,a,n,b in ports)+',\n'+',\n'.join(f'output mem_step{j}, output reg [31:0] mem_edges{j}, output reg [15:0] captured_request{j}, output reg [3:0] captured_read{j}' for j in range(2))+'\n);\n'
def wrapper(mode):
 mapping={'od2':'raw_od2','ir2':'raw_ir2'}
 if mode=='binding':mapping['id2']='id4'
 body='wire [7:0] raw_od2;wire raw_ir2;\nTop target(\n'+',\n'.join('.'+b+'('+mapping.get(a,a)+')' for d,a,n,b in ports)+'\n);\n'
 for j,inst in enumerate(['m_','m1_']):body+=f'assign mem_step{j}=target.{inst}.clock_buffer.CE;\ninitial mem_edges{j}=0; always @(posedge target.{inst}.clock_buffer.O) begin mem_edges{j} <= mem_edges{j}+1; captured_request{j} <= id{2+2*j}; captured_read{j} <= id{3+2*j}; end\n'
 return header+body+'assign od2=raw_od2 ^ '+{'normal':"8'b0",'invalid':"(ov2 ? 8'b0 : 8'hff)",'payload':"8'h1",'ready':"8'b0",'binding':"8'b0"}[mode]+';\nassign ir2='+('~raw_ir2' if mode=='ready' else 'raw_ir2')+';\nendmodule\n'
code=r'''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VCheck d;unsigned seq[10]={},seen[8]={},clockIndex=0,payload[10]={},ledger[2][16]={},writes[2]={},masked[2]={},disabled[2]={},asyncChecks[2]={},syncChecks[2]={},comboChecks[2]={},collisions[2]={},edges[2]={},initialEdges[2]={},syncValue[2]={};bool pending[10]={},known[2][16]={},syncKnown[2]={};
for(unsigned cycle=0;cycle<16384;++cycle){bool reset=cycle<4||(cycle>=5400&&cycle<5404)||(cycle>=11000&&cycle<11004);bool warm=cycle<128||(cycle>=5400&&cycle<5528)||(cycle>=11000&&cycle<11128);d.clk=0;d.rst=reset;if(reset)clockIndex=0;d.cv=!reset;d.c0=clockIndex%2==0;d.c1=clockIndex%3==0;
'''
for i in range(10):
 if i<2:payload=f'(warm?128:((seq[{i}]*13+{i*7})%127))'
 elif i in (2,4):
  j=(i-2)//2;payload=f'((seq[{i}]<64||seq[{i}]%7!={3+j}?0x8000:0)|(seq[{i}]<64||seq[{i}]%5!={2+j}?0x4000:0)|((seq[{i}]%16)<<8)|((seq[{i}]*{29+2*j}+{11+73*j})%256))'
 elif i in (3,5):payload=f'((seq[{i}]*7+{(i-3)*3})%16)'
 else:payload=f'(seq[{i}]*17+{i*11})%256'
 code+=f'if(reset)pending[{i}]=false;if(!reset&&!pending[{i}]&&(warm||(cycle+{i*3})%11!=4)){{pending[{i}]=true;payload[{i}]={payload};}}d.iv{i}=pending[{i}];d.id{i}=payload[{i}];\n'
for i in range(8):code+=f'd.orr{i}=!reset&&(warm||(cycle+{i*2})%7!=2);\n'
code+='d.eval();if(cycle>0){std::printf("%u %u",cycle,d.cr);\n'
for j in range(2):code+=f'std::printf(" %u %u",d.mem_step{j},d.mem_edges{j}-initialEdges[{j}]);\n'
for i in range(10):code+=f'std::printf(" %u",d.ir{i});\n'
for i in range(8):
 condition='!warm' if i<2 else (f'known[{(i-2)//2}][d.id{3+2*((i-2)//2)}]' if i<6 and i%2==0 else f'syncKnown[{(i-3)//2}]' if i<6 else 'true')
 code+=f'std::printf(" %u",d.ov{i});if(d.ov{i}&&({condition}))std::printf(" %u",d.od{i});\n'
code+='std::printf("\\n");}\n'
for j in range(2):
 code+=f'if(d.ov{2+j*2}&&known[{j}][d.id{3+j*2}]){{++asyncChecks[{j}];if(d.od{2+j*2}!=ledger[{j}][d.id{3+j*2}]){{std::fprintf(stderr,"async ledger mismatch instance{j} cycle%u observed%u expected%u\\n",cycle,d.od{2+j*2},ledger[{j}][d.id{3+j*2}]);return 10;}}}}\n'
 code+=f'if(d.ov{3+j*2}&&syncKnown[{j}]){{++syncChecks[{j}];if(d.od{3+j*2}!=syncValue[{j}]){{std::fprintf(stderr,"sync ledger mismatch instance{j} cycle%u observed%u expected%u\\n",cycle,d.od{3+j*2},syncValue[{j}]);return 11;}}}}\n'
 code+=f'if(d.ov{6+j}){{++comboChecks[{j}];if(d.od{6+j}!=(d.id{6+j*2}^d.id{7+j*2})){{std::fprintf(stderr,"combinational xor mismatch instance{j}\\n");return 12;}}}}\n'
for i in range(10):code+=f'if(d.iv{i}&&d.ir{i}){{++seq[{i}];pending[{i}]=false;}}\n'
for i in range(8):code+=f'if(d.ov{i}&&d.orr{i})++seen[{i}];\n'
code+='if(d.cv&&d.cr)++clockIndex;d.clk=1;d.eval();\n'
for j in range(2):
 code+=f'if(d.mem_edges{j}!=edges[{j}]){{if(d.mem_edges{j}!=edges[{j}]+1)return 13;edges[{j}]=d.mem_edges{j};unsigned read=d.captured_read{j},request=d.captured_request{j};syncKnown[{j}]=known[{j}][read];syncValue[{j}]=ledger[{j}][read];unsigned addr=(request>>8)&15;if(request&0x8000){{if(request&0x4000){{collisions[{j}]+=addr==read;known[{j}][addr]=true;ledger[{j}][addr]=request&255;++writes[{j}];}}else ++masked[{j}];}}else ++disabled[{j}];}}\n'
code+='d.clk=0;d.eval();if(cycle==0){initialEdges[0]=d.mem_edges0;initialEdges[1]=d.mem_edges1;}}\n'
code+=r'''for(unsigned i=0;i<10;++i){std::fprintf(stderr,"input%u=%u\n",i,seq[i]);if(seq[i]<1000)return 2;}for(unsigned i=0;i<8;++i){std::fprintf(stderr,"output%u=%u\n",i,seen[i]);if(seen[i]<1000)return 3;}
for(unsigned j=0;j<2;++j){std::fprintf(stderr,"instance%u writes=%u masked=%u disabled=%u collisions=%u async_checks=%u sync_checks=%u combo_checks=%u\n",j,writes[j],masked[j],disabled[j],collisions[j],asyncChecks[j],syncChecks[j],comboChecks[j]);if(writes[j]<1000||masked[j]<100||disabled[j]<100||collisions[j]<100||asyncChecks[j]<1000||syncChecks[j]<1000||comboChecks[j]<1000)return 4;}return 0;}
'''
(w/'driver.cpp').write_text(code)
def simulate(name,mode,sv,seed):
 source=w/(name+'-wrapper.sv');body=wrapper(mode)
 for inst in re.findall(r"\bMemory\s+(\w+)\s*\(",sv.read_text()):
  original='m1_' if inst.startswith('m1') else 'm_';body=body.replace('target.'+original+'.','target.'+inst+'.')
 source.write_text(body)
 run([v,'--cc','--exe','--build','-j','1','-Wno-fatal','--x-initial','unique','--x-assign','unique','--top-module','CompareTop','--prefix','VCheck','--Mdir',w/('obj-'+name),sv,w/'AbstractClockGate.v',source,w/'driver.cpp'],name+'-build.log')
 cmd=[str(w/('obj-'+name)/'VCheck'),'+verilator+rand+reset+2','+verilator+seed+'+str(seed)];cmds.append(cmd)
 with (w/(name+'-trace.txt')).open('w') as f,(w/(name+'-metrics.txt')).open('w') as e:result=subprocess.run(cmd,cwd=w,stdout=f,stderr=e)
 return result.returncode,(w/(name+'-trace.txt')).read_bytes()
traces={}
for name,mode,sv,seed in [('native','normal',w/'native-cli/post-fame-model-graph-boundary.sv',11),('sfc','normal',w/'sfc.sv',73),('invalid','invalid',w/'native-cli/post-fame-model-graph-boundary.sv',11),('payload','payload',w/'native-cli/post-fame-model-graph-boundary.sv',11),('ready','ready',w/'native-cli/post-fame-model-graph-boundary.sv',11),('binding','binding',w/'native-cli/post-fame-model-graph-boundary.sv',11)]:
 traces[name]=simulate(name,mode,sv,seed)
assert traces['native'][0]==traces['sfc'][0]==0,'ledger/runtime check failed'
assert traces['native'][1]==traces['sfc'][1],'complete memory boundary RTL mismatch'
assert traces['invalid']==traces['native'],'invalid memory payload was compared'
assert traces['payload']!=traces['native'],'valid memory corruption escaped'
assert traces['ready']!=traces['native'],'memory request ready corruption escaped'
assert traces['binding']!=traces['native'],'independent repeated-instance request crosswire escaped'
(w/'rtl-differential-result.json').write_text(json.dumps({'passed':True,'cycles':16384,'retained_annotations':len(b),'clock_contracts':clocks,'trace_sha256':hashlib.sha256(traces['native'][1]).hexdigest(),'comparison':'All host ready/valid, clock ready, actual target-memory clock enable and observed gated-clock edge count are strict after first host reset edge; two independent asynchronous memories checked only for their own written addresses; two independent synchronous outputs checked after enabled reads of their own written addresses, old RUW ledger; combinational XOR always checked; hub payload after target reset warmup. Independent stalls and rational1/2,1/3 held clock tokens. Three host resets do not clear the ledger or target memories.','initial_state':'Independent Verilator random startup seeds11/73. No memory initialization or clearing. The unchanged registered gate can deliver a startup edge from its unreset enable. The monitor observes actual gated-clock edges and normalizes only its initial counter offset after the first host reset edge; target memory state is never altered. Host resets preserve target memories. Two-state bounded RTL evidence, not X/four-state or formal proof.','negative_controls':{'invalid_payload_ignored':True,'valid_memory_corruption_rejected':True,'request_ready_corruption_rejected':True,'repeated_instance_request_crosswire_rejected':True},'scope':'Complete unchanged SFC FAMETransform/native model boundary RTL, actual SFC registered clock gate. Actual SFC SimWrapper and native full Pipe wrapper syntax checked separately; no claim of transported wrapper RTL equivalence, platform ABI, Jasper, F2 or feature acceptance.','commands':cmds,'metrics':{n:(w/(n+'-metrics.txt')).read_text() for n in ['native','sfc']}},indent=2)+'\n')
print('PASS complete repeated-definition memory/combinational FAME boundary:16384 cycles, independent old-RUW/write-mask ledger and corruption controls')
