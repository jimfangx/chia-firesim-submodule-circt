#!/usr/bin/env python3
"""Compare native actual internal transport primitive against unchanged SFC RTL."""
from pathlib import Path
import subprocess,json,hashlib,sys
if len(sys.argv)!=2:raise SystemExit('usage: InternalReadyValidCompare.py evidence-directory')
w=Path(sys.argv[1]).resolve();r=Path(__file__).resolve().parents[6];v=r/'.conda-env/bin/verilator';ft=r/'.conda-env/riscv-tools/bin/firtool';commands=[]
def run(cmd,log):
 commands.append(list(map(str,cmd)))
 with (w/log).open('w') as f:subprocess.run(list(map(str,cmd)),cwd=w,stdout=f,stderr=subprocess.STDOUT,check=True)
run([ft,w/'sfc-queue.fir','--disable-all-randomization','--disable-annotation-unknown','-o',w/'sfc-queue.sv'],'queue-sfc-firtool.log')
ports=[('input','clock',1),('input','reset',1),('output','io_enq_target_ready',1),('input','io_enq_target_valid',1),('input','io_enq_target_bits',8),('output','io_enq_fwd_hReady',1),('input','io_enq_fwd_hValid',1),('input','io_enq_rev_hReady',1),('output','io_enq_rev_hValid',1),('input','io_deq_target_ready',1),('output','io_deq_target_valid',1),('output','io_deq_target_bits',8),('input','io_deq_fwd_hReady',1),('output','io_deq_fwd_hValid',1),('output','io_deq_rev_hReady',1),('input','io_deq_rev_hValid',1),('input','io_targetReset_bits',1),('input','io_targetReset_valid',1),('output','io_targetReset_ready',1)]
code='''#include "VCheck.h"
#include "verilated.h"
#include <cstdio>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VCheck d;bool fp=false,rp=false;unsigned fs=0,rs=0,seen=0;
for(unsigned cycle=0;cycle<24000;++cycle){d.clock=0;d.reset=cycle<4||(cycle>=7931&&cycle<7935)||(cycle>=18021&&cycle<18025);if(d.reset){fp=false;rp=false;}if(!d.reset&&!fp&&cycle%11!=4)fp=true;if(!d.reset&&!rp&&cycle%13!=8)rp=true;d.io_enq_fwd_hValid=fp;d.io_enq_target_valid=fs%5!=2;d.io_enq_target_bits=(fs*19+73)&255;d.io_deq_rev_hValid=rp;d.io_deq_target_ready=rs%7!=3;d.io_enq_rev_hReady=!d.reset&&(cycle%9!=2);d.io_deq_fwd_hReady=!d.reset&&(cycle%7!=1);d.io_targetReset_valid=1;d.io_targetReset_bits=0;d.eval();
if(cycle>0){std::printf("%u %u %u %u %u %u %u",cycle,d.io_enq_target_ready,d.io_enq_fwd_hReady,d.io_enq_rev_hValid,d.io_deq_fwd_hValid,d.io_deq_rev_hReady,d.io_targetReset_ready);if(d.io_deq_fwd_hValid){std::printf(" %u",d.io_deq_target_valid);if(d.io_deq_target_valid)std::printf(" %u",d.io_deq_target_bits);}std::printf("\\n");}
if(fp&&d.io_enq_fwd_hReady){++fs;fp=false;}if(rp&&d.io_deq_rev_hReady){++rs;rp=false;}if(d.io_deq_fwd_hValid&&d.io_deq_fwd_hReady&&d.io_deq_target_valid)++seen;d.clock=1;d.eval();d.clock=0;d.eval();}
std::fprintf(stderr,"forward=%u reverse=%u defined_deliveries=%u\\n",fs,rs,seen);return fs>8000&&rs>8000&&seen>1000?0:2;}
'''
(w/'queue-driver.cpp').write_text(code)
traces={}
for name,module,source in [('native','GGFAMEReadyValid8',w/'native-transport/post-fame-model-graph.sv'),('sfc','ReadyValidChannel',w/'sfc-queue.sv')]:
 wrapper=w/('queue-'+name+'-wrapper.sv');wrapper.write_text('module QueueCompare(\n'+',\n'.join(d+' '+('['+str(n-1)+':0] ' if n>1 else '')+p for d,p,n in ports)+'\n);\n'+module+' target('+','.join('.'+p+'('+p+')' for d,p,n in ports)+');\nendmodule\n')
 run([v,'--cc','--exe','--build','-j','1','-Wno-fatal','--top-module','QueueCompare','--prefix','VCheck','--Mdir',w/('obj-queue-'+name),source,wrapper,w/'queue-driver.cpp'],'queue-'+name+'-build.log')
 cmd=[str(w/('obj-queue-'+name)/'VCheck')];commands.append(cmd)
 with (w/('queue-'+name+'-trace.txt')).open('w') as f,(w/('queue-'+name+'-metrics.txt')).open('w') as e:subprocess.run(cmd,cwd=w,stdout=f,stderr=e,check=True)
 traces[name]=(w/('queue-'+name+'-trace.txt')).read_bytes()
assert traces['native']==traces['sfc'],'internal ready-valid transport differs from actual SFC queue'
(w/'primitive-differential-result.json').write_text(json.dumps({'passed':True,'cycles':24000,'trace_sha256':hashlib.sha256(traces['native']).hexdigest(),'commands':commands,'metrics':{n:(w/('queue-'+n+'-metrics.txt')).read_text() for n in traces},'contract':'Actual internal-link queue with targetReset.valid=1 and bits=0, exactly as both native/SFC SimWrapper bind it. Strict host valid/ready/target-ready, target-valid when host-valid, packet data when both valid. Persistent input tokens and separate forward/reverse stalls, three host resets. Payload slots remain uninitialized and are ignored until valid; no RAM clearing or arbitrary invalid-data equality.','scope':'Exact unchanged production ReadyValidChannel primitive only; SFC full internal SimWrapper cannot elaborate, no graph transport equivalence, formal, platform or Linux claim.'},indent=2)+'\n')
print('PASS unchanged internal ReadyValidChannel queue primitive: 24000 host cycles')
