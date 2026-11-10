#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual bound native CLI RTL with unchanged SFC widget+binding RTL.

Both widgets receive independently stalled trigger/reset/event tokens and
independent AXI traffic. Schedules model two domains; retained clock ratios are
checked by the native unit, not proved by this local-interface differential.
"""
import argparse,copy,hashlib,json,subprocess
from pathlib import Path
DRIVER=r'''
#include "VWidget.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define C(name) ((bus==0)?d->@CTRL0@##name:d->@CTRL1@##name)
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
 int saved=bus;for(bus=0;bus<2;++bus){std::printf(" %u %u %u %u %u",unsigned(C(aw_ready)),unsigned(C(w_ready)),unsigned(C(b_valid)),unsigned(C(ar_ready)),unsigned(C(r_valid)));if(C(b_valid))std::printf(" b=%u:%u",unsigned(C(b_bits_id)),unsigned(C(b_bits_resp)));if(C(r_valid))std::printf(" r=%u:%u:%u:%u",unsigned(C(r_bits_id)),unsigned(C(r_bits_data)),unsigned(C(r_bits_resp)),unsigned(C(r_bits_last)));}bus=saved;
 std::puts("");CLOCK=1;d->eval();++cycle;return r;
}
void busWrite(unsigned addr,uint32_t data,unsigned seed){C(aw_bits_addr)=addr;C(aw_bits_id)=seed&4095;C(w_bits_data)=data;bool aw=false,w=false,b=false;for(unsigned n=0;!b&&n<100;++n){C(aw_valid)=!aw&&n>=seed%3;C(w_valid)=!w&&n>=(seed+1)%4;C(b_ready)=n%4!=0;auto r=step();aw|=r.aw;w|=r.w;b=r.b;}if(!aw||!w||!b)throw std::runtime_error("write timeout");C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;}
uint32_t busRead(unsigned addr,unsigned seed){C(ar_bits_addr)=addr;C(ar_bits_id)=seed&4095;bool ar=false;for(unsigned n=0;n<100;++n){C(ar_valid)=!ar;C(r_ready)=n>2&&n%3!=0;auto r=step();ar|=r.ar;if(r.r){C(ar_valid)=0;C(r_ready)=0;++reads;return r.data;}}throw std::runtime_error("read timeout");}
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);d=new VWidget;RESET=1;for(bus=0;bus<2;++bus){C(aw_bits_len)=0;C(ar_bits_len)=0;C(w_bits_strb)=15;C(w_bits_last)=1;}bus=0;for(unsigned n=0;n<4;++n)step();RESET=0;
 for(unsigned round=0;round<3;++round){for(bus=0;bus<2;++bus){busWrite(32,round==0?0:round==1?3+bus:17+bus,round+bus+1);busWrite(36,0,round+5);busWrite(40,1,round+bus+9);}running=true;bus=0;for(unsigned n=0;n<211;++n)step();
 for(unsigned n=0;n<13;++n)for(bus=0;bus<2;++bus){if(busRead(44,n+30)){++samples[bus];for(unsigned word=0;word<8;++word)(void)busRead(word*4,n*8+word);busWrite(48,1,n+50);}}
 running=false;RESET=1;bus=0;for(unsigned n=0;n<3;++n)step();RESET=0;for(unsigned n=0;n<4;++n)step();}
 if(samples[0]<10||samples[1]<10||reads<300||accepted[0]<100||accepted[1]<100||accepted[0]==accepted[1])throw std::runtime_error("insufficient independent widget exercise");
 std::fprintf(stderr,"PASS cycles=%llu reads=%u samples=%u/%u accepted=%u/%u\n",(unsigned long long)cycle,reads,samples[0],samples[1],accepted[0],accepted[1]);d->final();delete d;return 0;}
'''
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('native_rtl',type=Path);p.add_argument('sfc_dir',type=Path);p.add_argument('evidence',type=Path);p.add_argument('--firtool',type=Path,required=True);p.add_argument('--verilator',type=Path,required=True);a=p.parse_args()
 out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);commands=[]
 def run(cmd,name,stdout=None):
  cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  with (out/(name+'.log')).open('w')as log:
   if stdout:
    with stdout.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
   else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 native=a.native_rtl.resolve();sfc=a.sfc_dir.resolve()/'bound.fir';sfc_rtl=out/'sfc.sv'
 fixture=Path(__file__).with_name('AutoCounterBridgeBinding.anno.json').resolve()
 reference=sfc.with_name('input.anno.json');emitted=native.with_name(native.stem+'-all.json')
 inputs=json.loads(fixture.read_text());sfc_annotations=json.loads(reference.read_text())
 meaningful=[a for a in sfc_annotations if not a['class'].endswith('$ConstructorHints')]
 assert meaningful==inputs,'fixture constructor/channel metadata differs from actual serialized SFC annotations'
 actual=json.loads(emitted.read_text());assert len(actual)==len(inputs),'bound annotation inventory changed'
 bridges=[a for a in actual if a['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation']
 assert len(bridges)==2 and len({a['target']for a in bridges})==2
 prefix='~GGAutoCounterBridgeHostWrapper';sinks={}
 for bridge in bridges:
  assert bridge['target'].startswith(prefix+'|')and bridge['target'].endswith('>hBits')
  for local,global_name in bridge['channelMapping'].items():sinks[global_name]=bridge['target']+'.'+local
 expected=copy.deepcopy(inputs);index=0
 for annotation in expected:
  if annotation['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation':annotation['target']=bridges[index]['target'];index+=1
  else:
   annotation['clock']=annotation['clock'].replace('~Top|Top>',prefix+'|GGAutoCounterBridgeHostWrapper>')
   annotation['sources']=[source.replace('~Top|',prefix+'|')for source in annotation['sources']]
   annotation['sinks']=[sinks[annotation['globalName']]]
 assert actual==expected,'bound constructors/clock ownership/channel source/sink mapping mismatch'

 run([a.firtool.resolve(),sfc,'--disable-all-randomization','--disable-annotation-unknown','-o',sfc_rtl],'sfc-lower')
 traces=[]
 for kind,rtl,top,mapping in [('native',native,'GGAutoCounterBridgeHostWrapper',dict(CTRL0='autocounter_0_ctrl_',CTRL1='autocounter_1_ctrl_',CLOCK='hostClock',RESET='hostReset',DOMAININPUTS='d->domain0=0;d->domain1=0;')),('sfc',sfc_rtl,'LazyModule',dict(CTRL0='ctrl0_',CTRL1='ctrl1_',CLOCK='clock',RESET='reset',DOMAININPUTS=''))]:
  driver=DRIVER
  for k,v in mapping.items():driver=driver.replace('@'+k+'@',v)
  cpp=out/(kind+'.cpp');cpp.write_text(driver);obj=out/(kind+'-obj')
  run([a.verilator.resolve(),'--cc','--exe','--build','-j','2','-Wno-fatal','--assert','--top-module',top,'--prefix','VWidget','--Mdir',obj,rtl,cpp],kind+'-build')
  trace=out/(kind+'.trace');run([obj/'VWidget'],kind+'-run',trace);traces.append(trace)
 n,s=[t.read_text().splitlines()for t in traces];assert n==s,'native/SFC complete cycle trace mismatch'
 result=dict(passed=True,scope='two_actual_csv_widgets_and_unchanged_SFC_HostPortIOConnectChannels2Port',full_feature_acceptance=False,cycles=len(n),checks=['actual compiler-emitted bound RTL syntax','all10 actual serialized SFC constructor/channel annotations match fixture; emitted annotation retargeting preserves contracts','eight trigger/globalReset/Accumulate64/Identity64 payload leaves joined atomically','two independent widget/MMIO identities and stalls','retained domain1/1 and1/3 ownership checked in binding unit','trigger/global and host reset, period0/3/17/4/18, queue-full drops','independent AXI AW/W timing and R/B backpressure, exact response IDs and CSV data'],assumptions=['externally supplied token schedules; no rational-clock-generator phase proof','constructor + post-FAME tokens boundary; target CSV synthesis is not implemented','local indexed AXI32/id12 ports; global allocation/header/F2 platform assembly remains separate'],remaining=['production AutoCounter target synthesis','global MMIO allocation and driver/header composition','complete enabled Print integration','representative semantic, LEC, SEC, F2 Linux checked CSV and prints'])
 files=[native,sfc,fixture,reference,emitted,*traces,out/'commands.json',Path(__file__).resolve()];result['sha256']={str(f):hashlib.sha256(f.read_bytes()).hexdigest()for f in files};result['run_metrics']={kind:(out/(kind+'-run.log')).read_text().strip()for kind in ['native','sfc']};(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
