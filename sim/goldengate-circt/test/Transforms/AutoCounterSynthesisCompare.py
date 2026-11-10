#!/usr/bin/env python3
# See LICENSE for license details.
"""Matched-input target CSV synthesis comparison against unchanged Scala.

This compares defined target outputs and normalized constructor/channel/trigger/
reset contracts, preserving exact positional constructor event order.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def ports(rtl):
 header=re.search(r'module Top\((.*?)\);',rtl,re.S).group(1);direction=None;width=1;result={}
 for part in header.split(','):
  part=re.sub(r'//[^\n]*','',part).strip();match=re.match(r'(input|output)\s*(?:wire\s*|logic\s*)?(?:\[(\d+):0\]\s*)?(\w+)$',part)
  if match:direction=match[1];width=int(match[2])+1 if match[2] else 1;name=match[3]
  else:assert re.fullmatch(r'\w+',part),part;name=part
  assert direction and name not in result;result[name]=(direction,width)
 return result

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('native_dir',type=Path);p.add_argument('sfc_dir',type=Path);p.add_argument('evidence',type=Path);p.add_argument('--firtool',type=Path,required=True);p.add_argument('--verilator',type=Path,required=True);a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);commands=[]
 def run(cmd,name,trace=None):
  cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  with (out/(name+'.log')).open('w')as log:
   if trace:
    with trace.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
   else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 native=a.native_dir.resolve();sfc=a.sfc_dir.resolve();native_anno=native/'post-autocounter-csv-all.json';sfc_anno=sfc/'post-autocounter.json'
 na=json.loads(native_anno.read_text());sa=json.loads(sfc_anno.read_text())
 bookkeeping={'firrtl.options.TargetDirAnnotation','midas.passes.AutoCounterSynthesisOracle$ConstructorHints'}
 sa=[x for x in sa if x['class']not in bookkeeping]
 nc,sc=na,sa
 bridges=lambda seq:[d for d in seq if d['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation']
 assert bridges(nc)==bridges(sc),'constructor order or typed field/label/operation/domain correspondence differs'
 sort=lambda seq:sorted(json.dumps(x,sort_keys=True)for x in seq)
 assert sort(nc)==sort(sc),'normalized complete annotation contracts differ'
 order_differences=[]
 for index,(n,s)in enumerate(zip(bridges(na),bridges(sa))):
  norder=[e['portName']for e in n['widgetConstructorKey']['eventMetadata']];sorder=[e['portName']for e in s['widgetConstructorKey']['eventMetadata']]
  if norder!=sorder:order_differences.append(dict(widget=index,native=norder,sfc=sorder))
 nr=native/'post-autocounter-csv.sv';sr=out/'sfc.sv';run([a.firtool.resolve(),sfc/'post-autocounter.fir','--disable-all-randomization','--disable-annotation-unknown','-o',sr],'sfc-lower')
 np,sp=ports(nr.read_text()),ports(sr.read_text());assert np==sp,'normalized emitted interfaces differ';assert len(np)==25
 outputs=sorted(n for n,(d,w)in np.items()if d=='output')
 checks=''.join('std::printf(" %llu",(unsigned long long)d->'+n+');'for n in outputs)
 driver=r'''#include "VTarget.h"
#include "verilated.h"
#include <cstdint>
#include <cstdio>
#include <stdexcept>
int main(int argc,char**argv){Verilated::commandArgs(argc,argv);VTarget*d=new VTarget;
 for(unsigned c=0;c<8192;++c){d->clock0=c%2;d->clock1=(c/3)%2;d->reset0=c%97>=89;d->reset1=c%113>=101;d->event0=(c*31+7)&255;d->event1=(c*47+11)&255;d->value0=(uint64_t(c)<<40)^(UINT64_MAX-c*17);d->value1=(uint64_t(c)<<35)^(UINT64_MAX-c*23);d->topEvent=c&15;d->autocounter_triggerEnable=c%3==0;d->eval();
 if(d->observed0!=d->value0||d->observed1!=d->value1)throw std::runtime_error("original target outputs changed");
 if(d->autocounter_clock0!=d->clock0||d->autocounter_clock1!=d->clock1)throw std::runtime_error("wrong exported source clocks");
 if(d->autocounter_triggerEnable_0!=1||d->autocounter_triggerEnable_1!=1||d->autocounter_globalResetCondition!=0||d->autocounter_globalResetCondition_0!=0)throw std::runtime_error("wrong default trigger/global reset");
 if(d->autocounter_topCount_0!=(d->reset0?0:d->topEvent))throw std::runtime_error("top counter reset gate");
 if(d->autocounter_left_leaf_leafCount_0!=(d->reset0?0:d->event0)||d->autocounter_left_leaf_coverCount!=(d->reset0?0:d->event0)||d->autocounter_left_leaf_leafValue!=(d->reset0?0:d->value0))throw std::runtime_error("left counter reset/data route");
 if(d->autocounter_right_leaf_leafCount_0!=(d->reset1?0:d->event1)||d->autocounter_right_leaf_coverCount!=(d->reset1?0:d->event1)||d->autocounter_right_leaf_leafValue!=(d->reset1?0:d->value1))throw std::runtime_error("right counter reset/data route");
 std::printf("%u",c);@OUTPUTS@std::puts("");}
 std::fprintf(stderr,"PASS 8192 samples; 7 event/reset payloads, 2 clocks, 2 trigger/reset domains, original outputs\n");d->final();delete d;return 0;}
'''.replace('@OUTPUTS@',checks)
 traces=[]
 for kind,rtl in [('native',nr),('sfc',sr)]:
  cpp=out/(kind+'.cpp');cpp.write_text(driver);obj=out/(kind+'-obj');run([a.verilator.resolve(),'--cc','--exe','--build','-j','2','-Wno-fatal','--assert','--top-module','Top','--prefix','VTarget','--Mdir',obj,rtl,cpp],kind+'-build');trace=out/(kind+'.trace');run([obj/'VTarget'],kind+'-run',trace);traces.append(trace)
 assert traces[0].read_bytes()==traces[1].read_bytes(),'observable target output differential mismatch'
 files=[nr,native_anno,sfc/'post-autocounter.fir',sfc_anno,*traces,out/'commands.json',Path(__file__).resolve()]
 result=dict(passed=True,scope='enabled_csv_target_synthesis_matched_unchanged_SFC_transform',full_feature_acceptance=False,samples=8192,defined_output_observations=8192*len(outputs),top_ports=len(np),normalized_annotations=len(na),generated_widgets=2,event_exports=7,normalized_interfaces=np,event_order_correspondence=order_differences,exact_constructor_order=True,normalizations=['ignore only SFC TargetDir and standalone serialization hint bookkeeping','annotation registry order is not semantic; preserve per-domain bridge order and every constructor event list position'],assumptions=['LowerTypes/ExpandWhens target input with positive UInt events, synchronous UInt1 resets and unique input Clock roots','all outputs in this fixture are defined; no RAM is initialized or compared and no zero-out policy is changed'],remaining=['production assembly through clock/trigger/global reset wiring, FAME tokenization, CSV hosts and full F2 platform','global MMIO allocation and driver/header emission remain subsequent stages','complete enabled Print integration','representative semantic/LEC/SEC/F2 Linux checked output'])
 result['sha256']={str(f):hashlib.sha256(f.read_bytes()).hexdigest()for f in files};(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
