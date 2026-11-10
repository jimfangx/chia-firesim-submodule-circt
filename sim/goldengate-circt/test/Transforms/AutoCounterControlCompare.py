#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the complete emitted two-CSV AXI network with unchanged SFC
HasWidgets.genCtrlIO, NastiRecursiveInterconnect and actual CSV widgets.
Externally supplied independent token schedules are explicit; this does not
constitute feature-enabled Linux/formal acceptance or full platform support.
"""
import argparse, hashlib, json, subprocess
from pathlib import Path
DRIVER=r'''

#include "VWidget.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define C(name) d->ctrl_##name
#define CLOCK d->@CLOCK@
#define RESET d->@RESET@
VWidget*d;int bus=0;uint64_t cycle=0;bool running=false;unsigned reads=0,samples[2]={0,0},accepted[2]={0,0};
struct Result{bool aw,w,b,ar,r;uint32_t data;};
Result step(){
 @DOMAININPUTS@
 d->w0_trigger_valid=running&&cycle%5!=0;d->w0_globalReset_valid=running&&cycle%7!=0;
 d->w0_event0_valid=running&&cycle%11!=0;d->w0_event1_valid=running&&cycle%13!=0;
 d->w1_trigger_valid=running&&cycle%3==0;d->w1_globalReset_valid=running&&cycle%17!=0;
 d->w1_event0_valid=running&&cycle%19!=0;d->w1_event1_valid=running&&cycle%23!=0;
 d->w0_trigger_bits=cycle%97<79;d->w0_globalReset_bits=cycle%131>=119;
 d->w0_event0_bits=cycle%7==0?UINT64_MAX:cycle*11+3;d->w0_event1_bits=(cycle<<40)^(UINT64_MAX-cycle*17);
 d->w1_trigger_bits=cycle%67<53;d->w1_globalReset_bits=cycle%149>=137;
 d->w1_event0_bits=cycle%9==0?UINT64_MAX:cycle*13+7;d->w1_event1_bits=(cycle<<37)^(UINT64_MAX-cycle*31);
 CLOCK=0;d->eval();
 Result r{bool(C(aw_ready)&&C(aw_valid)),bool(C(w_ready)&&C(w_valid)),bool(C(b_valid)&&C(b_ready)),bool(C(ar_ready)&&C(ar_valid)),bool(C(r_valid)&&C(r_ready)),uint32_t(C(r_bits_data))};
 if(d->w0_trigger_ready!=d->w0_globalReset_ready||d->w0_trigger_ready!=d->w0_event0_ready||d->w0_trigger_ready!=d->w0_event1_ready)throw std::runtime_error("non-atomic widget0 ready");
 if(d->w1_trigger_ready!=d->w1_globalReset_ready||d->w1_trigger_ready!=d->w1_event0_ready||d->w1_trigger_ready!=d->w1_event1_ready)throw std::runtime_error("non-atomic widget1 ready");
 if(d->w0_trigger_ready&&(!d->w0_trigger_valid||!d->w0_globalReset_valid||!d->w0_event0_valid||!d->w0_event1_valid))throw std::runtime_error("widget0 partial payload consumed");
 if(d->w1_trigger_ready&&(!d->w1_trigger_valid||!d->w1_globalReset_valid||!d->w1_event0_valid||!d->w1_event1_valid))throw std::runtime_error("widget1 partial payload consumed");
 accepted[0]+=d->w0_trigger_ready;accepted[1]+=d->w1_trigger_ready;
 std::printf("%llu %u %u",(unsigned long long)cycle,unsigned(d->w0_trigger_ready),unsigned(d->w1_trigger_ready));
 std::printf(" %u %u %u %u %u",unsigned(C(aw_ready)),unsigned(C(w_ready)),unsigned(C(b_valid)),unsigned(C(ar_ready)),unsigned(C(r_valid)));if(C(b_valid))std::printf(" b=%u:%u",unsigned(C(b_bits_id)),unsigned(C(b_bits_resp)));if(C(r_valid))std::printf(" r=%u:%u:%u:%u",unsigned(C(r_bits_id)),unsigned(C(r_bits_data)),unsigned(C(r_bits_resp)),unsigned(C(r_bits_last)));
 std::puts("");CLOCK=1;d->eval();++cycle;return r;
}
void busWrite(unsigned addr,uint32_t data,unsigned seed){C(aw_bits_addr)=addr+64*bus;C(aw_bits_id)=seed&4095;C(w_bits_data)=data;bool aw=false,w=false,b=false;for(unsigned n=0;!b&&n<100;++n){C(aw_valid)=!aw&&n>=seed%3;C(w_valid)=!w&&n>=(seed+1)%4;C(b_ready)=n%4!=0;auto r=step();aw|=r.aw;w|=r.w;b=r.b;}if(!aw||!w||!b)throw std::runtime_error("write timeout");C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;}
uint32_t busRead(unsigned addr,unsigned seed){C(ar_bits_addr)=addr+64*bus;C(ar_bits_id)=seed&4095;bool ar=false;for(unsigned n=0;n<100;++n){C(ar_valid)=!ar;C(r_ready)=n>2&&n%3!=0;auto r=step();ar|=r.ar;if(r.r){C(ar_valid)=0;C(r_ready)=0;++reads;return r.data;}}throw std::runtime_error("read timeout");}

void pipelinedReads(unsigned seed){
 unsigned sent=0,received=0;const unsigned ids[8]={7,11,7,11,31,31,12,8};
 for(unsigned n=0;n<300&&received<8;++n){C(ar_valid)=sent<8;C(ar_bits_addr)=(sent%2)*64+44;C(ar_bits_id)=ids[sent%8];C(r_ready)=n>13&&n%5!=0;auto r=step();sent+=r.ar;received+=r.r;}
 if(sent!=8||received!=8)throw std::runtime_error("pipelined duplicate-ID read timeout");C(ar_valid)=0;C(r_ready)=0;
}
void errorTraffic(){
 bus=0;C(ar_bits_addr)=0x80000000u;C(ar_bits_id)=4095;C(ar_bits_len)=3;bool ar=false;unsigned received=0;
 for(unsigned n=0;n<100&&received<4;++n){C(ar_valid)=!ar;C(r_ready)=n>5&&n%4!=0;CLOCK=0;d->eval();if(C(r_valid)&&(C(r_bits_resp)!=3||C(r_bits_id)!=4095||C(r_bits_data)!=0||C(r_bits_last)!=(received==3)))throw std::runtime_error("decode error read contract");auto r=step();ar|=r.ar;received+=r.r;}
 if(!ar||received!=4)throw std::runtime_error("decode error burst read timeout");C(ar_valid)=0;C(r_ready)=0;C(ar_bits_len)=0;
 C(aw_bits_addr)=0x80000080u;C(aw_bits_id)=4094;C(aw_bits_len)=3;bool aw=false,b=false;unsigned words=0;
 for(unsigned n=0;n<100&&!b;++n){C(aw_valid)=!aw;C(w_valid)=words<4&&n>2;C(w_bits_last)=words==3;C(w_bits_data)=words*17;C(b_ready)=n>11&&n%3!=0;CLOCK=0;d->eval();if(C(b_valid)&&(C(b_bits_resp)!=3||C(b_bits_id)!=4094))throw std::runtime_error("decode error write contract");auto r=step();aw|=r.aw;words+=r.w;b=r.b;}
 if(!aw||words!=4||!b)throw std::runtime_error("decode error burst write timeout");C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;C(w_bits_last)=1;C(aw_bits_len)=0;
}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);Verilated::randReset(2);d=new VWidget;RESET=1;CLOCK=0;C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;C(ar_valid)=0;C(r_ready)=0;C(aw_bits_addr)=0;C(aw_bits_len)=0;C(aw_bits_size)=0;C(aw_bits_burst)=0;C(aw_bits_lock)=0;C(aw_bits_cache)=0;C(aw_bits_prot)=0;C(aw_bits_qos)=0;C(aw_bits_region)=0;C(aw_bits_id)=0;C(aw_bits_user)=0;C(ar_bits_addr)=0;C(ar_bits_len)=0;C(ar_bits_size)=0;C(ar_bits_burst)=0;C(ar_bits_lock)=0;C(ar_bits_cache)=0;C(ar_bits_prot)=0;C(ar_bits_qos)=0;C(ar_bits_region)=0;C(ar_bits_id)=0;C(ar_bits_user)=0;C(w_bits_data)=0;C(w_bits_id)=0;C(w_bits_user)=0;C(w_bits_strb)=15;C(w_bits_last)=1;bus=0;d->w0_trigger_valid=0;d->w0_trigger_bits=0;d->w0_globalReset_valid=0;d->w0_globalReset_bits=0;d->w0_event0_valid=0;d->w0_event0_bits=0;d->w0_event1_valid=0;d->w0_event1_bits=0;d->w1_trigger_valid=0;d->w1_trigger_bits=0;d->w1_globalReset_valid=0;d->w1_globalReset_bits=0;d->w1_event0_valid=0;d->w1_event0_bits=0;d->w1_event1_valid=0;d->w1_event1_bits=0;@DOMAININPUTS@
 d->eval();CLOCK=1;d->eval();for(unsigned n=0;n<4;++n)step();RESET=0;
 for(unsigned round=0;round<3;++round){for(bus=0;bus<2;++bus){busWrite(32,round==0?0:round==1?3+bus:17+bus,round+bus+1);busWrite(36,0,round+5);busWrite(40,1,round+bus+9);}running=true;bus=0;for(unsigned n=0;n<211;++n)step();
 for(unsigned n=0;n<13;++n)for(bus=0;bus<2;++bus){if(busRead(44,n+30)){++samples[bus];for(unsigned word=0;word<8;++word)(void)busRead(word*4,n*8+word);busWrite(48,1,n+50);}}
 bus=0;pipelinedReads(round);errorTraffic();
 running=false;RESET=1;bus=0;for(unsigned n=0;n<3;++n)step();RESET=0;for(unsigned n=0;n<4;++n)step();}
 if(samples[0]<10||samples[1]<10||reads<300||accepted[0]<100||accepted[1]<100||accepted[0]==accepted[1])throw std::runtime_error("insufficient independent widget exercise");
 std::fprintf(stderr,"PASS cycles=%llu reads=%u samples=%u/%u accepted=%u/%u\n",(unsigned long long)cycle,reads,samples[0],samples[1],accepted[0],accepted[1]);d->final();delete d;return 0;}
'''

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('native_rtl',type=Path);p.add_argument('sfc_dir',type=Path);p.add_argument('evidence',type=Path)
 p.add_argument('--firtool',type=Path,required=True);p.add_argument('--verilator',type=Path,required=True)
 a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);commands=[]
 def run(cmd,name,stdout=None):
  cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  with (out/(name+'.log')).open('w')as log:
   if stdout:
    with stdout.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
   else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 native=a.native_rtl.resolve();sfc=a.sfc_dir.resolve()/'bound.fir';sfc_rtl=out/'sfc.sv'
 fixture=Path(__file__).with_name('AutoCounterBridgeBinding.anno.json')
 inputs=json.loads(fixture.read_text());reference=json.loads((a.sfc_dir/'input.anno.json').read_text())
 assert [x for x in reference if not x['class'].endswith('$ConstructorHints')]==inputs,'actual serialized SFC metadata differs from matched fixture'
 emitted=native.with_name(native.stem+'-all.json');actual=json.loads(emitted.read_text())
 assert len(actual)==len(inputs),'complete control changed retained annotation count'
 bridges=[x for x in actual if x['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation']
 assert len(bridges)==2 and [b['widgetConstructorKey'] for b in bridges]==[x['widgetConstructorKey'] for x in inputs if x['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation']
 sinks={global_name:b['target']+'.'+local for b in bridges for local,global_name in b['channelMapping'].items()}
 prefix='~GGControlMasterWrapper';expected=[];bridge_index=0
 for annotation in inputs:
  annotation=json.loads(json.dumps(annotation))
  if annotation['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation':
   annotation['target']=bridges[bridge_index]['target'];bridge_index+=1
  else:
   annotation['clock']=annotation['clock'].replace('~Top|Top>',prefix+'|GGControlMasterWrapper>')
   annotation['sources']=[source.replace('~Top|',prefix+'|') for source in annotation['sources']]
   annotation['sinks']=[sinks[annotation['globalName']]]
  expected.append(annotation)
 assert actual==expected,'complete-control constructor/clock/channel retargeting changed semantics'
 run([a.firtool.resolve(),sfc,'--disable-all-randomization','--disable-annotation-unknown','-o',sfc_rtl],'sfc-lower')
 traces=[]
 for kind,rtl,top,mapping in [('native',native,'GGControlMasterWrapper',dict(CLOCK='hostClock',RESET='hostReset',DOMAININPUTS='d->domain0=0;d->domain1=0;')),('sfc',sfc_rtl,'LazyModule',dict(CLOCK='clock',RESET='reset',DOMAININPUTS=''))]:
  driver=DRIVER
  for k,v in mapping.items():driver=driver.replace('@'+k+'@',v)
  cpp=out/(kind+'.cpp');cpp.write_text(driver);obj=out/(kind+'-obj')
  run([a.verilator.resolve(),'--cc','--exe','--build','-j','1','-Wno-fatal','--assert','--top-module',top,'--prefix','VWidget','--Mdir',obj,rtl,cpp],kind+'-build')
  trace=out/(kind+'.trace');run([obj/'VWidget',f"+verilator+seed+{101 if kind == 'native' else 202}"],kind+'-run',trace);traces.append(trace)
 lines=[[line for line in path.read_text().splitlines()if line and line[0].isdigit()]for path in traces]
 if lines[0]!=lines[1]:
  for i,(n,s)in enumerate(zip(*lines)):
   if n!=s:raise AssertionError(f'native/SFC cycle {i} mismatch: {n} != {s}')
  raise AssertionError(f'native/SFC trace lengths differ {list(map(len,lines))}')
 result=dict(passed=True,full_feature_acceptance=False,scope='complete two-CSV global control, actual unchanged SFC HasWidgets + interconnect + widgets',cycles=len(lines[0]),exact_retained_annotations=len(actual),checks=['single global AXI32/id12 port replaces all independent local controls','complete constructor-ordered banks and actual HasWidgets allocation','independent CSV Accumulate64/Identity64 payloads, trigger/global reset, host reset, depth-two sampling backpressure','AXI independent AW/W timing, read/write IDs, response backpressure','pipelined reads with reused/duplicate outstanding IDs','high-address unmapped burst read/write DECERR and response IDs'],assumptions=['two actual CSV banks only; complete mixed-widget platform registration remains required','external rational-domain token schedules; phase-generator composition outside boundary'],remaining=['enabled Print production platform integration','separate Jasper LEC and SEC','feature-enabled F2 Linux checked CSV/prints and clean poweroff'],run_metrics={kind:(out/(kind+'-run.log')).read_text().strip()for kind in ['native','sfc']})
 files=[native,sfc,fixture,emitted,*traces,out/'commands.json',Path(__file__).resolve()]
 result['sha256']={str(f):hashlib.sha256(f.read_bytes()).hexdigest()for f in files}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
