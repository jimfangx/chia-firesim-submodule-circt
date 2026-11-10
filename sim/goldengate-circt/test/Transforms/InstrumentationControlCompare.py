#!/usr/bin/env python3
# See LICENSE for license details.
"""Cycle compare the actual mixed CSV/Print/CPU network with unchanged SFC.
External independent target tokens are explicit. This is a selected simulator
composition differential, not Jasper, original platform or Linux acceptance.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
from AutoCounterControlCompare import DRIVER as CSV_DRIVER
from PrintRocketCPUStreamPortCompare import leaves
INIT=''.join(f'd->cpu_managed_axi4_{n}=0;'for n,(direction,width)in leaves().items()if direction=='in' and width<=64)
INIT+='for(unsigned i=0;i<16;++i)d->cpu_managed_axi4_w_bits_data[i]=0;d->cpu_managed_axi4_w_bits_strb=UINT64_MAX;d->cpu_managed_axi4_w_bits_last=1;'
PRINT_INPUTS=r'''
 d->p_reset_valid=running&&cycle%5!=0;d->p_record0_enable_valid=running&&cycle%7!=0;d->p_record0_argument_valid=running&&cycle%11!=0;
 d->p_record1_enable_valid=running&&cycle%13!=0;d->p_record1_argument_valid=running&&cycle%17!=0;
 d->p_reset_bits=cycle%233>=219;d->p_record0_enable_bits=cycle%3!=0;d->p_record0_argument_bits=(cycle*7+3)&31;
 d->p_record1_enable_bits=cycle%7<4;d->p_record1_argument_bits=(cycle*5+1)&7;
 d->cpu_managed_axi4_ar_valid=dmaEnabled;d->cpu_managed_axi4_ar_bits_addr=0;d->cpu_managed_axi4_ar_bits_id=42;
 d->cpu_managed_axi4_ar_bits_len=3;d->cpu_managed_axi4_ar_bits_size=6;d->cpu_managed_axi4_aw_bits_size=6;
 d->cpu_managed_axi4_r_ready=dmaEnabled&&cycle%19<13;
'''
PRINT_TRACE=r'''
 bool pf=d->p_reset_ready&&d->p_reset_valid;
 if(pf!=(bool(d->p_record0_enable_ready&&d->p_record0_enable_valid))||pf!=(bool(d->p_record0_argument_ready&&d->p_record0_argument_valid))||pf!=(bool(d->p_record1_enable_ready&&d->p_record1_enable_valid))||pf!=(bool(d->p_record1_argument_ready&&d->p_record1_argument_valid)))throw std::runtime_error("Print accepted partial record bag");
 printAccepted+=pf;dmaBeats+=d->cpu_managed_axi4_r_valid&&d->cpu_managed_axi4_r_ready;
 std::printf(" p=%u%u%u%u%u cpu=%u:%u:%u:%u",unsigned(d->p_reset_ready),unsigned(d->p_record0_enable_ready),unsigned(d->p_record0_argument_ready),unsigned(d->p_record1_enable_ready),unsigned(d->p_record1_argument_ready),unsigned(d->cpu_managed_axi4_ar_ready),unsigned(d->cpu_managed_axi4_r_valid),unsigned(d->cpu_managed_axi4_aw_ready),unsigned(d->cpu_managed_axi4_b_valid));
 if(d->cpu_managed_axi4_r_valid){std::printf(" dma=%u:%u:%u",unsigned(d->cpu_managed_axi4_r_bits_id),unsigned(d->cpu_managed_axi4_r_bits_last),unsigned(d->cpu_managed_axi4_r_bits_resp));for(unsigned i=0;i<16;++i)std::printf(":%08x",d->cpu_managed_axi4_r_bits_data[i]);}
'''
DRIVER=CSV_DRIVER.replace('bool running=false;','bool running=false,dmaEnabled=false;unsigned printAccepted=0,dmaBeats=0;')
DRIVER=DRIVER.replace(' CLOCK=0;d->eval();\n Result r',PRINT_INPUTS+' CLOCK=0;d->eval();\n Result r')
DRIVER=DRIVER.replace(' std::puts("");CLOCK=1;',PRINT_TRACE+' std::puts("");CLOCK=1;')
DRIVER=DRIVER.replace('d=new VWidget;RESET=1;', 'd=new VWidget;'+INIT+'RESET=1;')
DRIVER=DRIVER.replace('for(unsigned round=0;round<3;++round){for(bus=0;', 'for(unsigned round=0;round<3;++round){bus=2;busWrite(0,5,1);busWrite(4,0,2);busWrite(8,300,3);busWrite(12,0,4);busWrite(16,1,5);for(bus=0;')
DRIVER=DRIVER.replace('running=true;bus=0;', 'running=true;dmaEnabled=true;bus=0;')
DRIVER=DRIVER.replace('bus=0;pipelinedReads(round);errorTraffic();','bus=0;pipelinedReads(round);errorTraffic();bus=2;for(unsigned word=0;word<6;++word)(void)busRead(4*word,70+word);bus=0;(void)busRead(160,77);')
DRIVER=DRIVER.replace('running=false;RESET=1;', 'bus=2;busWrite(20,1,9);running=false;for(unsigned n=0;n<93;++n)step();dmaEnabled=false;RESET=1;')
DRIVER=DRIVER.replace(' std::fprintf(stderr,"PASS cycles=', ' if(printAccepted<1000||dmaBeats<20)throw std::runtime_error("insufficient enabled Print/DMA exercise");std::fprintf(stderr,"PRINT accepted=%u dmaBeats=%u\\n",printAccepted,dmaBeats);\n std::fprintf(stderr,"PASS cycles=')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('native',type=Path);p.add_argument('oracle',type=Path);p.add_argument('evidence',type=Path);p.add_argument('--firtool',required=True,type=Path);p.add_argument('--verilator',required=True,type=Path);a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True);commands=[]
 def run(cmd,name,stdout=None):
  cmd=list(map(str,cmd));commands.append(cmd);(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
  with(out/(name+'.log')).open('w')as log:
   if stdout:
    with stdout.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
   else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
 native=a.native.resolve();sfc=a.oracle.resolve()/'bound.fir';reference=out/'sfc.sv'
 expected=json.loads(Path(__file__).with_name('InstrumentationControl.anno.json').read_text());oracle=[x for x in json.loads((a.oracle/'input.anno.json').read_text())if not x['class'].endswith('$ConstructorHints')]
 assert expected==oracle,'actual unchanged SFC constructor/channel inputs differ'
 actual=json.loads(native.with_name(native.stem+'-all.json').read_text());outputs=[x for x in actual if x['class'].endswith('OutputFileAnnotation')]
 retained=[x for x in actual if x not in outputs]
 assert len(retained)==len(expected)and len(outputs)==2,'retained/input collateral inventories differ'
 assert [x['widgetConstructorKey']for x in retained if 'widgetConstructorKey'in x]==[x['widgetConstructorKey']for x in expected if 'widgetConstructorKey'in x],'constructor ordering changed'
 bridges=[x for x in retained if 'widgetConstructorKey'in x];bridge_index=0;normalized=[]
 sinks={global_name:b['target']+'.'+local.replace('_argument','.argument').replace('_enable','.enable')if b['widgetClass'].endswith('PrintBridgeModule') else b['target']+'.'+local for b in bridges for local,global_name in b['channelMapping'].items()}
 prefix='~GGSelectedPrintCPUStreamPlatformWrapper'
 for item in expected:
  row=json.loads(json.dumps(item))
  if 'widgetConstructorKey'in row:row['target']=bridges[bridge_index]['target'];bridge_index+=1
  else:
   row['clock']=row['clock'].replace('~Top|Top>',prefix+'|GGSelectedPrintCPUStreamPlatformWrapper>')
   row['sources']=[x.replace('~Top|Top>printToken_',prefix+'|GGAutoCounterBridgeHostWrapper>printToken_').replace('~Top|',prefix+'|')for x in row['sources']]
   row['sinks']=[sinks[row['globalName']]]
  normalized.append(row)
 assert retained==normalized,'complete channel/clock/reset/payload annotation transfer differs'
 run([a.firtool.resolve(),sfc,'--disable-all-randomization','--disable-annotation-unknown','-o',reference],'sfc-lower')
 traces=[]
 for kind,rtl,top,clock,reset,domains in [('native',native,'GGSelectedPrintCPUStreamPlatformWrapper','hostClock','hostReset','d->domain0=0;d->domain1=0;'),('sfc',reference,'LazyModule','clock','reset','')]:
  cpp=out/(kind+'.cpp');driver=DRIVER.replace('@CLOCK@',clock).replace('@RESET@',reset).replace('@DOMAININPUTS@',domains);cpp.write_text(driver);obj=out/(kind+'-obj')
  run([a.verilator.resolve(),'--cc','--exe','--build','-j','1','-Wno-fatal','--assert','--top-module',top,'--prefix','VWidget','--Mdir',obj,rtl,cpp],kind+'-build')
  trace=out/(kind+'.trace');run([obj/'VWidget',f"+verilator+seed+{101 if kind=='native' else 202}"],kind+'-run',trace);traces.append(trace)
 lines=[[line for line in trace.read_text().splitlines()if line and line[0].isdigit()]for trace in traces]
 for i,(n,s)in enumerate(zip(*lines)):
  assert n==s,f'cycle {i}: {n} != {s}'
 assert len(lines[0])==len(lines[1]),'cycle count differs'
 receipt=dict(passed=True,full_feature_acceptance=False,scope='actual enabled CSV/Print widgets + shared global control + buffered CPU stream AXI',cycles=len(lines[0]),retained_annotations=len(retained),checks=['actual complete constructor order and shared allocator','two independent CSV sampling/reset/trigger schedules','Print signed5/unsigned3 ordered records, reset, inclusive ROI and narrow flush','complete Print payload beats and DMA IDs/last under AR/R backpressure','independent AW/W control timing and duplicate outstanding IDs','unmapped high-address burst DECERR'],assumptions=['externally driven post-FAME target token schedules; generated clock/model composition not proved','selected catalog is exactly two CSV, one enabled Print and one CPU engine; original platform bridge integration remains required'],metrics={kind:(out/(kind+'-run.log')).read_text().strip()for kind in ['native','sfc']},sha256={str(x.resolve()):hashlib.sha256(x.read_bytes()).hexdigest()for x in [native,sfc,*traces,Path(__file__),out/'commands.json']})
 (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
