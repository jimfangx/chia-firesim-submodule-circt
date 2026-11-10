#!/usr/bin/env python3
"""Real complete FAME boundary + unchanged transport RTL differential.

This is bounded supporting evidence. Full unchanged SFC SimWrapper cannot
elaborate internal RV links; its actual failure is retained by the Scala oracle.
"""
from pathlib import Path
import hashlib,json,re,subprocess,sys
if len(sys.argv)!=2:raise SystemExit('usage: ReadyValidCompare.py evidence-directory')
w=Path(sys.argv[1]).resolve();r=Path(__file__).resolve().parents[6];v=r/'.conda-env/bin/verilator';ft=r/'.conda-env/riscv-tools/bin/firtool';cmds=[]
def run(cmd,log):
 cmds.append(list(map(str,cmd)))
 with (w/log).open('w') as f:subprocess.run(list(map(str,cmd)),cwd=w,stdout=f,stderr=subprocess.STDOUT,check=True)
run([ft,w/'sfc-fame.fir','--disable-all-randomization','--disable-annotation-unknown','-o',w/'sfc.sv'],'sfc-firtool.log')
run([ft,w/'sfc-queue.fir','--disable-all-randomization','--disable-annotation-unknown','-o',w/'sfc-queue.sv'],'primitive-firtool.log')
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
for i,(prefix,group,width) in enumerate([('h__','data',8),('h__','second',8),('h__','back',1),('w__','link_fwd',8),('w__','enable',8)]):
 ports += [('output','ir'+str(i),1,prefix+group+'_sink_ready'),('input','iv'+str(i),1,prefix+group+'_sink_valid')]
 if i==3:ports.append(('input','tv',1,prefix+group+'_sink_bits_valid'));field='_bits'
 else:field=''
 ports.append(('input','id'+str(i),width,prefix+group+'_sink_bits'+field))
for i,(prefix,group,width) in enumerate([('h__','link_fwd',8),('h__','observed',8),('w__','back',1),('w__','observed',8)]):
 ports += [('input','orr'+str(i),1,prefix+group+'_source_ready'),('output','ov'+str(i),1,prefix+group+'_source_valid')]
 if i==0:ports.append(('output','otv',1,prefix+group+'_source_bits_valid'));field='_bits'
 else:field=''
 ports.append(('output','od'+str(i),width,prefix+group+'_source_bits'+field))
header='module CompareTop(\n'+',\n'.join(d+' '+('['+str(n-1)+':0] ' if n>1 else '')+a for d,a,n,b in ports)+'\n);\n'
def wrapper(mode):
 decl='wire [7:0] raw_od0; wire raw_ir0;\n'
 body='Top target(\n'+',\n'.join('.'+b+'('+({'od0':'raw_od0','ir0':'raw_ir0'}.get(a,a))+')' for d,a,n,b in ports)+'\n);\n'
 return header+decl+body+'assign od0=raw_od0 ^ '+{'normal':"8'b0",'invalid':"(otv ? 8'b0 : 8'hff)",'payload':"8'h1",'ready':"8'b0"}[mode]+';\nassign ir0='+('~raw_ir0' if mode=='ready' else 'raw_ir0')+';\nendmodule\n'
code='''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VCheck d;unsigned seq[5]={},seen[4]={},clockIndex=0,payload[5]={};bool pending[5]={};
for(unsigned cycle=0;cycle<12288;++cycle){bool reset=cycle<4||(cycle>=4000&&cycle<4004)||(cycle>=9000&&cycle<9004);bool warm=cycle<128||(cycle>=4000&&cycle<4128)||(cycle>=9000&&cycle<9128);d.clk=0;d.rst=reset;if(reset)clockIndex=0;d.cv=!reset;d.c0=clockIndex%2==0;d.c1=clockIndex%3==0;d.tv=(seq[3]%4)!=1;
'''
for i in range(5):code+=f'if(reset)pending[{i}]=false;if(!reset&&!pending[{i}]&&(warm||(cycle+{i*3})%11!=4)){{pending[{i}]=true;payload[{i}]='+('((seq[2]%5)!=2)' if i==2 else f'(warm?128:((seq[{i}]*13+{i*7})%127))')+f';}}d.iv{i}=pending[{i}];d.id{i}=payload[{i}];\n'
for i in range(4):code+=f'd.orr{i}=!reset&&(warm||(cycle+{i*2})%7!=2);\n'
code+='d.eval();if(cycle>0){std::printf("%u %u",cycle,d.cr);\n'
for i in range(5):code+=f'std::printf(" %u",d.ir{i});\n'
for i in range(4):
 code+=f'std::printf(" %u",d.ov{i});'
 if i==0:code+='if(d.ov0){std::printf(" %u",d.otv);if(d.otv&&!warm)std::printf(" %u",d.od0);}'
 else:code+=f'if(d.ov{i}&&!warm)std::printf(" %u",d.od{i});'
 code+='\n'
code+='std::printf("\\n");}\n'
for i in range(5):code+=f'if(d.iv{i}&&d.ir{i}){{++seq[{i}];pending[{i}]=false;}}\n'
for i in range(4):code+=f'if(d.ov{i}&&d.orr{i})++seen[{i}];\n'
code+='if(d.cv&&d.cr)++clockIndex;d.clk=1;d.eval();d.clk=0;d.eval();}\nfor(unsigned i=0;i<5;++i){std::fprintf(stderr,"input%u=%u\\n",i,seq[i]);if(seq[i]<1000)return 2;}for(unsigned i=0;i<4;++i){std::fprintf(stderr,"output%u=%u\\n",i,seen[i]);if(seen[i]<1000)return 3;}return 0;}\n'
(w/'graph-driver.cpp').write_text(code)
def simulate(name,files,source,driver,top='CompareTop',args=()):
 run([v,'--cc','--exe','--build','-j','1','-Wno-fatal','--top-module',top,'--prefix','VCheck','--Mdir',w/('obj-'+name),*files,source,driver],name+'-build.log')
 cmd=[str(w/('obj-'+name)/'VCheck'),*args];cmds.append(cmd)
 with (w/(name+'-trace.txt')).open('w') as f,(w/(name+'-metrics.txt')).open('w') as e:subprocess.run(cmd,cwd=w,stdout=f,stderr=e,check=True)
 return (w/(name+'-trace.txt')).read_bytes()
traces={}
for name,mode,sv in [('native','normal',w/'native-cli/post-fame-model-graph-boundary.sv'),('sfc','normal',w/'sfc.sv'),('invalid','invalid',w/'native-cli/post-fame-model-graph-boundary.sv'),('payload','payload',w/'native-cli/post-fame-model-graph-boundary.sv'),('ready','ready',w/'native-cli/post-fame-model-graph-boundary.sv')]:
 source=w/(name+'-wrapper.sv');source.write_text(wrapper(mode));traces[name]=simulate(name,[sv,w/'AbstractClockGate.v'],source,w/'graph-driver.cpp')
assert traces['native']==traces['sfc'],'complete graph boundary RTL mismatch'
assert traces['invalid']==traces['sfc'],'invalid RV payload was compared'
assert traces['payload']!=traces['sfc'],'valid RV payload corruption escaped'
assert traces['ready']!=traces['sfc'],'sink ready corruption escaped'
(w/'rtl-differential-result.json').write_text(json.dumps({'passed':True,'cycles':12288,'retained_annotations':len(b),'clock_contracts':clocks,'trace_sha256':hashlib.sha256(traces['native']).hexdigest(),'comparison':'All host valid/ready and target-valid bits are strict after first host reset edge; payloads checked when host-valid and target-valid where RV, after explicit target reset warmup. Independent input/output stalls, rational 1/2 and 1/3 tokens held until accepted, three host resets.','initial_state':'No initialization added. Target reset tokens drive each target register to known zero during each 128-cycle warmup; warmup output payloads excluded. Actual SFC registered AbstractClockGate is used verbatim. No memory in graph.','negative_controls':{'invalid_rv_payload_ignored':True,'valid_rv_payload_corruption_rejected':True,'ready_corruption_rejected':True},'scope':'Complete unchanged SFC FAMETransform and native CLI boundary RTL only. SFC SimWrapper fails internal RV lookup; transport primitive comparison is separate. No platform ABI, formal proof, F2 or feature acceptance.','commands':cmds,'metrics':{n:(w/(n+'-metrics.txt')).read_text() for n in ['native','sfc']}},indent=2)+'\n')
print('PASS complete internal-RV model boundary: 12288 cycles, strict controls/defined payload, corruption controls')
