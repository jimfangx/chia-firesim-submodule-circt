#!/usr/bin/env python3
# See LICENSE for license details.
"""Executable SFC/native CSV-widget differential, independent of CHIA acceptance.

Scala AutoCounterBridgeOracle produces sfc_dir. The native fixture or actual CLI
emits a rooted native MLIR widget. Both use the unchanged Scala constructor.
This deliberately does not claim target synthesis/binding or feature acceptance.
"""
import argparse, hashlib, json, re, subprocess
from pathlib import Path

DRIVER = r'''
#include "VWidget.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define C(name) d->@CTRL@##name
#define CLOCK d->@CLOCK@
#define RESET d->@RESET@
#define HV d->@HV@
#define HR d->@HR@
#define FV d->@FV@
#define FR d->@FR@
#define TRIGGER d->@TRIGGER@
#define GLOBALRESET d->@GLOBALRESET@
#define EVENT0 d->@EVENT0@
#define EVENT1 d->@EVENT1@
struct Result { bool aw,w,b,ar,r; uint32_t data; };
VWidget *d; uint64_t cycle=0; bool running=false; unsigned reads=0,samples=0;
Result step() {
  HV=running&&(cycle%5!=0); FR=(cycle%7!=0);
  TRIGGER=(cycle%97<79);GLOBALRESET=(cycle%131>=119);
  EVENT0=(cycle%7==0)?UINT64_MAX:(cycle*11+3);
  EVENT1=(cycle<<40)^(UINT64_MAX-cycle*17);
  CLOCK=0;d->eval();
  Result r{bool(C(aw_ready)&&C(aw_valid)),bool(C(w_ready)&&C(w_valid)),
      bool(C(b_valid)&&C(b_ready)),bool(C(ar_ready)&&C(ar_valid)),
      bool(C(r_valid)&&C(r_ready)),uint32_t(C(r_bits_data))};
  std::printf("%llu %u %u %u %u %u %u %u",(unsigned long long)cycle,
      unsigned(HR),unsigned(FV),unsigned(C(aw_ready)),unsigned(C(w_ready)),
      unsigned(C(b_valid)),unsigned(C(ar_ready)),unsigned(C(r_valid)));
  if(C(b_valid))std::printf(" b=%u:%u",unsigned(C(b_bits_id)),unsigned(C(b_bits_resp)));
  if(C(r_valid))std::printf(" r=%u:%u:%u:%u",unsigned(C(r_bits_id)),unsigned(C(r_bits_data)),unsigned(C(r_bits_resp)),unsigned(C(r_bits_last)));
  std::puts("");CLOCK=1;d->eval();++cycle;return r;
}
void busWrite(unsigned addr,uint32_t data,unsigned seed) {
  C(aw_bits_addr)=addr;C(aw_bits_id)=seed&4095;C(w_bits_data)=data;
  bool aw=false,w=false,b=false;
  for(unsigned n=0;!b&&n<100;++n) {
    C(aw_valid)=!aw&&(n>=seed%3);C(w_valid)=!w&&(n>=(seed+1)%4);
    C(b_ready)=n%4!=0;auto r=step();aw|=r.aw;w|=r.w;b=r.b;
  }
  if(!aw||!w||!b)throw std::runtime_error("write timeout");
  C(aw_valid)=0;C(w_valid)=0;C(b_ready)=0;
}
uint32_t busRead(unsigned addr,unsigned seed) {
  C(ar_bits_addr)=addr;C(ar_bits_id)=seed&4095;bool ar=false;
  for(unsigned n=0;n<100;++n) {
    C(ar_valid)=!ar;C(r_ready)=n>2&&n%3!=0;auto r=step();ar|=r.ar;
    if(r.r){C(ar_valid)=0;C(r_ready)=0;++reads;return r.data;}
  }
  throw std::runtime_error("read timeout");
}
int main(int argc,char **argv) {
  Verilated::commandArgs(argc,argv);d=new VWidget;
  RESET=1;C(aw_bits_len)=0;C(ar_bits_len)=0;C(w_bits_strb)=15;C(w_bits_last)=1;
  for(unsigned n=0;n<4;++n)step();RESET=0;
  for(unsigned round=0;round<3;++round) {
    busWrite(32,round==0?0:round==1?3:17,round+1);busWrite(36,0,round+5);
    busWrite(40,1,round+9);running=true;
    // Leave a full queue while target tokens continue: SFC drops excess
    // samples rather than backpressuring the target. Then drain via real MMIO.
    for(unsigned n=0;n<211;++n)step();
    for(unsigned n=0;n<13;++n) {
      if(busRead(44,n+30)) {
        ++samples;
        for(unsigned word=0;word<8;++word)(void)busRead(word*4,n*8+word);
        busWrite(48,1,n+50);
      }
    }
    // Host reset clears control, queue pointers, periodic and accumulated
    // state; global target reset above gates events without clearing cycles.
    running=false;RESET=1;for(unsigned n=0;n<3;++n)step();RESET=0;
    for(unsigned n=0;n<4;++n)step();
  }
  if(samples<20||reads<180)throw std::runtime_error("insufficient sample exercise");
  std::fprintf(stderr,"PASS cycles=%llu reads=%u samples=%u\n",(unsigned long long)cycle,reads,samples);
  d->final();delete d;return 0;
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('native_mlir',type=Path);p.add_argument('sfc_dir',type=Path);p.add_argument('evidence',type=Path)
    p.add_argument('--native-rtl',type=Path,help='use exact compiler-emitted native RTL for execution')
    p.add_argument('--firtool',required=True,type=Path);p.add_argument('--verilator',required=True,type=Path)
    a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    commands=[]
    def run(cmd,name):
        commands.append([str(x)for x in cmd]);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with (out/(name+'.log')).open('w')as log:
            subprocess.run([str(x)for x in cmd],stdout=log,stderr=subprocess.STDOUT,check=True)
    native=a.native_mlir.resolve();sfc=a.sfc_dir.resolve();firtool=a.firtool.resolve();verilator=a.verilator.resolve()
    # rawAnnotations remain in the compiler snapshot for semantic checks.
    # The final RTL lowering boundary consumes them separately, as the actual
    # compiler emitter does; FIRRTL's generic target resolver cannot interpret
    # this explicitly unbound constructor-only diagnostic circuit.
    lower_text,count=re.subn(r'(?m)^(\s*firrtl.circuit "[^"]+") attributes \{rawAnnotations = .*\} \{$',r'\1 {',native.read_text())
    assert count==1,'expected retained rawAnnotations on native diagnostic circuit'
    lower_native=out/'native-lower-input.mlir';lower_native.write_text(lower_text)
    paths=[]
    for kind,inp,top in [('native',lower_native,'GGAutoCounterBridgeHost'),('sfc',sfc/'autocounter.fir','AutoCounterBridgeModule')]:
        sv=out/(kind+'.sv')
        if kind=='native' and a.native_rtl:
            sv.write_bytes(a.native_rtl.resolve().read_bytes())
        else:
            run([firtool,inp,'--disable-all-randomization','--disable-annotation-unknown','-o',sv],kind+'-lower')
        mapping=dict(CTRL='ctrl_',CLOCK='hostClock',RESET='hostReset',HV='hValid',HR='hReady',FV='fromHostValid',FR='fromHostReady',TRIGGER='hBits_trigger',GLOBALRESET='hBits_globalReset',EVENT0='hBits_event0',EVENT1='hBits_event1')
        if kind=='sfc':mapping=dict(CTRL='io_ctrl_',CLOCK='clock',RESET='reset',HV='hPort_toHost_hValid',HR='hPort_toHost_hReady',FV='hPort_fromHost_hValid',FR='hPort_fromHost_hReady',TRIGGER='hPort_hBits_trigger',GLOBALRESET='hPort_hBits_globalReset',EVENT0='hPort_hBits_event0',EVENT1='hPort_hBits_event1')
        driver=DRIVER
        for k,v in mapping.items():driver=driver.replace('@'+k+'@',v)
        cpp=out/(kind+'.cpp');cpp.write_text(driver);obj=out/(kind+'-obj')
        run([verilator,'--cc','--exe','--build','-j','4','-Wno-fatal','--assert','--top-module',top,'--prefix','VWidget','--Mdir',obj,sv,cpp],kind+'-build')
        trace=out/(kind+'.trace');paths.append(trace)
        commands.append([str(obj/'VWidget')]);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with trace.open('w')as traceout,(out/(kind+'-run.log')).open('w')as log:
            subprocess.run([str(obj/'VWidget')],stdout=traceout,stderr=log,check=True)
    native_lines,sfc_lines=[p.read_text().splitlines()for p in paths]
    assert native_lines==sfc_lines,'cycle-accurate widget output mismatch; compare native.trace and sfc.trace'
    registry=['autocounter_low_local_cycle','autocounter_high_local_cycle','autocounter_low_count','autocounter_high_count','autocounter_low_value','autocounter_high_value','cycles_low','cycles_high','readrate_low','readrate_high','init_done','countersready','readdone']
    abi=''.join(f'{n}={4*i}\n'for i,n in enumerate(registry))
    assert (sfc/'autocounter-abi.txt').read_text()==abi
    result=dict(passed=True,scope='focused_csv_autocounter_widget_axi32_cycle_differential',full_feature_acceptance=False,cycles=len(native_lines),checks=['actual unchanged Scala AutoCounterBridgeModule constructor','native local F2 AXI32/id12 RTL syntax and assertions','13 exact MMIO register offsets and permissions','Accumulate64 rollover and Identity64','accepted-token stalls; trigger/global reset; host reset','period0/3/17 delayed sampling and queue-full sample drops','independent AW/W arrival, R/B response backpressure, ID preservation'],assumptions=['pre-MIDAS AutoCounterFirrtlAnnotation preserves op type with HasSerializationHints; standalone synthesized BridgeIOAnnotation+AutoCounterParameters export lacks nested op hints, so supplemental test hints preserve actual singleton types','constructor-only boundary; no target-token/global-platform binding claimed'],normalizations=['retain raw annotations in compiler snapshot; consume them from a separate copy at RTL lowering'],remaining=['production CSV target synthesis and host-token binding','global widget allocation and driver/header assembly','enabled Print+CSV representative LEC/SEC and F2 Linux checked output'])
    result['sha256']={str(p):hashlib.sha256(p.read_bytes()).hexdigest()for p in [native,sfc/'autocounter.fir',sfc/'input.anno.json',*paths,out/'commands.json',*([a.native_rtl.resolve()] if a.native_rtl else [])]}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
