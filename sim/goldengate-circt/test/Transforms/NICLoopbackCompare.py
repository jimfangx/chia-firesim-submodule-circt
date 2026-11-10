#!/usr/bin/env python3
# See LICENSE for license details.
"""Differential of two complete loopback token paths, using actual SFC queues,
generator and HostPortIOConnectChannels2Port. Not MMIO/CPU/F2 acceptance.
Unwritten seed payloads and explicitly undriven SFC pauser fields are excluded;
all handshakes, packet-valid markers and written live flits remain checked.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
DRIVER=r'''
#include "VLoop.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <deque>
#include <stdexcept>
VLoop*d;
struct Flit {uint64_t data;unsigned keep,last,valid;};
std::deque<Flit> pending[2];unsigned seq[2]={0,0},seeds[2]={10,10},sent[2]={0,0},received[2]={0,0},seeded[2]={0,0},stalls[2]={0,0};
uint64_t bits(unsigned w,unsigned p){switch(w*15+p){@BITS@}throw std::runtime_error("bad bits");}
bool valid(unsigned w,unsigned p){switch(w*15+p){@VALID@}throw std::runtime_error("bad valid");}
bool ready(unsigned w,unsigned p){switch(w*15+p){@READY@}throw std::runtime_error("bad ready");}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);d=new VLoop;
for(unsigned cycle=0;cycle<12000;++cycle){bool reset=cycle<4||(cycle>=2111&&cycle<2115)||(cycle>=7901&&cycle<7905);
 d->@RESET@=reset;d->@CLOCK@=0;
 Flit payload[2];bool source[2][4],sink[2][11];
 for(unsigned w=0;w<2;++w){unsigned n=seq[w];payload[w]={0x9e3779b97f4a7c15ULL*(n+1)^(uint64_t(w)<<60),unsigned((n*37+w*19)&255),unsigned(n%7==6),unsigned(n%11!=3)};
  for(unsigned j=0;j<4;++j)source[w][j]=!reset&&(cycle%(17+w*12)>3||cycle%(4+w*3)==j);
  for(unsigned j=0;j<11;++j)sink[w][j]=!reset&&(cycle%(31+w*18)<19&&(cycle%7!=0||cycle%11!=j));
 }
 @INPUTS@
 d->eval();
 if(reset){for(unsigned w=0;w<2;++w){pending[w].clear();seeds[w]=10;}}
 else for(unsigned w=0;w<2;++w){bool push=true,pop=true;
  for(unsigned j=0;j<4;++j)push&=source[w][j]&&ready(w,j);
  for(unsigned j=4;j<15;++j)pop&=sink[w][j-4]&&valid(w,j);
  if(push){pending[w].push_back(payload[w]);++seq[w];++sent[w];}
  if(pop){if(seeds[w]){if(bits(w,7)!=0)throw std::runtime_error("seed contains live packet");--seeds[w];++seeded[w];}
    else{if(pending[w].empty())throw std::runtime_error("lost/reordered/unsolicited token");auto p=pending[w].front();pending[w].pop_front();
      if(bits(w,7)!=p.valid)throw std::runtime_error("packet valid marker mismatch");
      if(p.valid&&(bits(w,4)!=p.data||bits(w,5)!=p.keep||bits(w,6)!=p.last))throw std::runtime_error("written packet corruption/order/keep mismatch");++received[w];}}
  bool allSource=true;for(unsigned j=0;j<4;++j)allSource&=source[w][j];
  if((allSource&&!push)||(valid(w,7)&&!pop))++stalls[w];
 }
 std::printf("%u",cycle);
 for(unsigned w=0;w<2;++w){for(unsigned j=0;j<4;++j)std::printf(" %u",unsigned(ready(w,j)));
  for(unsigned j=4;j<15;++j)std::printf(" %u",unsigned(valid(w,j)));
  // Packet marker is defined on queued tokens. Seed/invalid packet flit
  // contents remain unspecified; queue RAM is never cleared for this check.
  if(valid(w,7)){std::printf(" v=%llu",(unsigned long long)bits(w,7));
   if(bits(w,7))for(unsigned j=4;j<7;++j)if(valid(w,j))std::printf(" p%u=%llu",j,(unsigned long long)bits(w,j));}
  for(unsigned j=8;j<12;++j)if(valid(w,j))std::printf(" c%u=%llu",j,(unsigned long long)bits(w,j));
 }
 std::printf(" d=%u:%u\n",unsigned(d->nic0_done),unsigned(d->nic1_done));
 d->@CLOCK@=1;d->eval();
}
std::fprintf(stderr,"METRICS sent=%u/%u received=%u/%u seeds=%u/%u stalls=%u/%u\n",sent[0],sent[1],received[0],received[1],seeded[0],seeded[1],stalls[0],stalls[1]);
for(unsigned w=0;w<2;++w)if(sent[w]<1000||received[w]<1000||seeded[w]!=30||stalls[w]<100)throw std::runtime_error("insufficient traffic/reset/backpressure");
if(sent[0]==sent[1])throw std::runtime_error("missing independent schedules");
std::fprintf(stderr,"PASS cycles=12000 sent=%u/%u received=%u/%u seeds=%u/%u stalls=%u/%u\n",sent[0],sent[1],received[0],received[1],seeded[0],seeded[1],stalls[0],stalls[1]);d->final();delete d;return 0;}
'''
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('native_mlir',type=Path);p.add_argument('sfc_fir',type=Path);p.add_argument('evidence',type=Path);p.add_argument('--firtool',type=Path,required=True);p.add_argument('--verilator',type=Path,required=True);p.add_argument('--native-rtl',type=Path,help='Use actual compiler-emitted RTL instead of lowering the native MLIR again');a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);commands=[]
 def run(cmd,name,stdout=None):
  cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  with(out/(name+'.log')).open('w')as log:
   if stdout:
    with stdout.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
   else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 traces=[]
 for kind,source,top,clock,reset in [('native',a.native_mlir.resolve(),'GGNICLoopbackTokenQueuesWrapper','hostClock','hostReset'),('sfc',a.sfc_fir.resolve(),'LoopbackFixture','clock','reset')]:
  rtl=out/(kind+'.sv')
  if kind=='native' and a.native_rtl:
   rtl.write_bytes(a.native_rtl.read_bytes())
  else:run([a.firtool.resolve(),source,'--disable-all-randomization','--disable-annotation-unknown','-o',rtl],kind+'-lower')
  values={f:''.join(f'case {w*15+j}:return d->w{w}_p{j}_{f.lower()};'for w in range(2)for j in range(15))for f in ['BITS','VALID','READY']}
  assignments=[]
  for w in range(2):
   for j,field in enumerate(['data','keep','last','valid']):assignments.append(f'd->w{w}_p{j}_bits=payload[{w}].{field};d->w{w}_p{j}_valid=source[{w}][{j}];')
   for j in range(4,15):assignments.append(f'd->w{w}_p{j}_ready=sink[{w}][{j-4}];')
  values.update(INPUTS='\n'.join(assignments),CLOCK=clock,RESET=reset);driver=DRIVER
  for key,val in values.items():driver=driver.replace('@'+key+'@',val)
  cpp=out/(kind+'.cpp');cpp.write_text(driver);obj=out/(kind+'-obj')
  run([a.verilator.resolve(),'--cc','--exe','--build','-j','2','-Wno-fatal','--top-module',top,'--prefix','VLoop','--Mdir',obj,rtl,cpp],kind+'-build')
  trace=out/(kind+'.trace');run([obj/'VLoop','+verilator+seed+1'],kind+'-run',trace);traces.append(trace)
 assert traces[0].read_bytes()==traces[1].read_bytes(),'NIC loopback defined cycle trace mismatch'
 # Repeat both implementations with distinct arbitrary initial RAM contents;
 # qualified traces still agree. Written payload/keep/order check stays active.
 for kind in ['native','sfc']:
  trace=out/(kind+'-seed99.trace');run([out/(kind+'-obj')/'VLoop','+verilator+seed+99','+verilator+rand+reset+2'],kind+'-seed99-run',trace)
  assert trace.read_bytes()==traces[0].read_bytes(),'defined behavior depends on arbitrary seed contents'
 result=dict(passed=True,full_feature_acceptance=False,cycles=12000,widgets=2,scope='two complete token queue/seed-forward scheduler/actual SFC HostPort binder paths',checks=['all30 pipe handshakes and both done predicates each cycle','ten invalid seed tokens after each host reset','written valid packet data/keep/last/order/valid with independent stalls','exact SFC loopback MAC/rate constants','distinct random uninitialized RAM seeds do not affect defined traces'],exclusions=['invalid/unwritten seed flit payloads','three pauser fields explicitly undriven by SFC compile-time loopback'],remaining=['loopback one-word MMIO/header/driver and allocated disabled CPU stream integration','runtime per-widget loopback traffic','enabled representative separate LEC/SEC and F2 Linux packet checks'],metrics={k:(out/(k+'-run.log')).read_text().strip()for k in ['native','sfc']})
 result['sha256']={str(f):hashlib.sha256(f.read_bytes()).hexdigest()for f in [a.native_mlir.resolve(),a.sfc_fir.resolve(),Path(__file__).resolve(),out/'native.sv',out/'sfc.sv',*traces,out/'commands.json']}
 result['native_rtl_source']=str(a.native_rtl.resolve()) if a.native_rtl else 'firtool lowering of supplied native MLIR'
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
