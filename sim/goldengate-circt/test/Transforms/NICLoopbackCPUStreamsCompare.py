#!/usr/bin/env python3
# See LICENSE for license details.
"""Actual CLI loopback CPU stream RTL vs unchanged SFC queues and disabled
packet adapters. Reset precedes traffic; no unspecified RAM payload is compared.
This is a supporting stream boundary, not full NIC/Linux/formal acceptance.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
DRIVER=r'''
#include "VStreams.h"
#include "verilated.h"
#include <cstdio>
#include <stdexcept>
#include <cstdint>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);try{auto*d=new VStreams;
unsigned counts[2]={0,0},accepted[2]={0,0},full[2]={0,0},resets=0;
bool pending[2]={false,false};
for(unsigned cycle=0;cycle<22000;++cycle){bool reset=cycle<4||(cycle>=7000&&cycle<7004)||(cycle>=15000&&cycle<15004);
d->@CLOCK@=0;d->@RESET@=reset;
bool valid[2]={!reset&&(pending[0]||cycle%13!=2),!reset&&(pending[1]||cycle%7>1)};
@FAULT@
@INPUTS@
d->eval();
if(reset){counts[0]=counts[1]=0;pending[0]=pending[1]=false;++resets;}
for(unsigned w=0;w<2;++w){bool ready=w?d->nic1_from_host_stream_ready:d->nic0_from_host_stream_ready;
bool outvalid=w?d->nic1_to_host_stream_valid:d->nic0_to_host_stream_valid;
unsigned inCount=w?d->nic1_from_host_stream_count:d->nic0_from_host_stream_count;
unsigned outCount=w?d->nic1_to_host_stream_count:d->nic0_to_host_stream_count;
if(!reset){if(ready!=(counts[w]<3072)||outvalid||inCount!=counts[w]||outCount)throw std::runtime_error("disabled stream handshake/occupancy differs");
 if(valid[w]&&ready){++counts[w];++accepted[w];}if(!ready)++full[w];pending[w]=valid[w]&&!ready;}
if(cycle>0)std::printf("%u %u %u %u %u %u\n",cycle,w,ready,outvalid,inCount,outCount);}
d->@CLOCK@=1;d->eval();}
for(unsigned w=0;w<2;++w)if(accepted[w]!=9216||full[w]<5000)throw std::runtime_error("missing capacity/backpressure/reset exercise");
std::fprintf(stderr,"PASS cycles=22000 accepted=%u/%u full=%u/%u resetCycles=%u\n",accepted[0],accepted[1],full[0],full[1],resets);d->final();delete d;return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
'''

def controlled_rtl(text):
 """Mutate copied native output ports; preserve the production RTL artifact."""
 start=text.index('module GGNICBridgeControlWrapper(');end=text.index('\nendmodule',start)
 top=text[start:end].replace('module GGNICBridgeControlWrapper(',
     'module GGNICBridgeControlWrapper(\n  input [2:0] verification_fault,',1)
 declarations=[];assignments=[]
 for port,width,mode,expression in [
   ('nic0_to_host_stream_bits',512,1,"512'hffffffff"),
   ('nic0_from_host_stream_count',12,2,"raw_nic0_from_host_stream_count ^ 12'h1"),
   ('nic0_from_host_stream_ready',1,3,'~raw_nic0_from_host_stream_ready'),
   ('nic0_to_host_stream_valid',1,4,"1'b1"),
   ('nic1_from_host_stream_count',12,5,'raw_nic0_from_host_stream_count'),
   ('nic1_to_host_stream_count',12,6,"12'h1")]:
  # Only the inner instance output is redirected. Public port identity remains
  # unchanged so the same independently checked stream driver observes it.
  top,n=re.subn(r'(\.'+re.escape(port)+r'\s*\()'+re.escape(port)+r'(\))',
               r'\g<1>raw_'+port+r'\2',top)
  assert n==1,('unexpected emitted output forwarding',port,n)
  declarations.append('  wire '+(f'[{width-1}:0] 'if width>1 else '')+'raw_'+port+';')
  assignments.append(f'  assign {port} = verification_fault == 3\'d{mode} ? {expression} : raw_{port};')
 return text[:start]+top+'\n'+'\n'.join(declarations+assignments)+text[end:]

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('native_rtl',type=Path);p.add_argument('sfc_fir',type=Path);p.add_argument('output',type=Path);p.add_argument('--firtool',type=Path,required=True);p.add_argument('--verilator',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);commands=[]
 def run(cmd,name,trace=None):
  cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  with(out/(name+'.log')).open('w')as log:
   if trace:
    with trace.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
   else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 run([a.firtool.resolve(),a.sfc_fir.resolve(),'--disable-all-randomization','-o',out/'sfc.sv'],'sfc-lower')
 traces=[]
 controlled=out/'native-controls.sv';controlled.write_text(controlled_rtl(a.native_rtl.read_text()))
 controls=[]
 for kind,rtl,top,clock,reset in [('native',a.native_rtl.resolve(),'GGNICBridgeControlWrapper','hostClock','hostReset'),('sfc',out/'sfc.sv','LoopbackCPUStreams','clock','reset'),('controls',controlled,'GGNICBridgeControlWrapper','hostClock','hostReset')]:
  text=rtl.read_text()
  if kind in ('native','controls'):
   # Actual emitted top may be renamed by the local MMIO transport.
   assert re.search(r'^module GGNICBridgeControlWrapper\(',text,re.M)
   top='GGNICBridgeControlWrapper'
  inputs=[]
  for w in range(2):
   inputs.extend([f'd->nic{w}_from_host_stream_valid=valid[{w}];',f'd->nic{w}_to_host_stream_ready=(cycle%{w*11+17})>3;'])
   for k in range(16):inputs.append(f'd->nic{w}_from_host_stream_bits[{k}]=0x9e3779b9u*(accepted[{w}]+{k+1})^{w};')
  fault='d->verification_fault=cycle>=32&&argc>1?std::atoi(argv[1]):0;'if kind=='controls'else ''
  driver=DRIVER.replace('@CLOCK@',clock).replace('@RESET@',reset).replace('@INPUTS@','\n'.join(inputs)).replace('@FAULT@',fault);cpp=out/(kind+'.cpp');cpp.write_text(driver)
  obj=out/(kind+'-obj');run([a.verilator.resolve(),'--cc','--exe','--build','-j','1','-Wno-fatal','--top-module',top,'--prefix','VStreams','--Mdir',obj,rtl,cpp],kind+'-build')
  if kind=='controls':
   for mode in range(1,7):
    cmd=list(map(str,[obj/'VStreams',str(mode),'+verilator+seed+101','+verilator+rand+reset+2']));commands.append(cmd)
    completed=subprocess.run(cmd,capture_output=True);(out/f'control-{mode}.log').write_bytes(completed.stderr)
    if mode==1:
     assert completed.returncode==0 and completed.stdout==traces[0].read_bytes(),'invalid payload perturbation affected defined behavior'
    else:
     assert completed.returncode==1 and b'FAIL disabled stream handshake/occupancy differs' in completed.stderr,('control failed for unrelated reason',mode,completed.returncode)
    controls.append(dict(mode=mode,returncode=completed.returncode,expected='pass'if mode==1 else 'reject',diagnostic=completed.stderr.decode().strip()))
   (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
   continue
  trace=out/(kind+'.trace');run([obj/'VStreams','+verilator+seed+101','+verilator+rand+reset+2'],kind+'-run',trace);traces.append(trace)
  repeat=out/(kind+'-seed202.trace');run([obj/'VStreams','+verilator+seed+202','+verilator+rand+reset+2'],kind+'-seed202-run',repeat)
  assert repeat.read_bytes()==trace.read_bytes(),'occupancy depends on unspecified initial RAM'
 assert traces[0].read_bytes()==traces[1].read_bytes(),'actual SFC/native CPU queue trace differs'
 result=dict(passed=True,full_feature_acceptance=False,scope='actual CLI disabled CPU queues/counts/catalog and unchanged SFC loopback packet adapter ties',cycles=22000,widgets=2,queue_depth=3072,checks=['each incoming queue fills all 3072 entries after each reset','independent incoming valid and outgoing ready schedules','incoming occupancy full backpressure and empty outgoing valid/count','distinct random initial RAM seeds'],assumptions=['host reset before traffic; comparisons start after the first host reset edge'],remaining=['full enabled NIC header/driver/platform assembly','complete LEC and SEC','enabled F2 Linux packet exercise'],metrics={k:(out/(k+'-run.log')).read_text().strip()for k in ['native','sfc']})
 result['controls']=controls
 result['checks']+=['stable producer valid and payload while stalled','invalid outgoing payload perturbation passes','wrong ready, incoming count, outgoing valid, widget count binding and outgoing count reject']
 result['sha256']={str(f):hashlib.sha256(f.read_bytes()).hexdigest()for f in [a.native_rtl.resolve(),a.sfc_fir.resolve(),Path(__file__).resolve(),out/'sfc.sv',controlled,*traces,out/'commands.json']}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
