#!/usr/bin/env python3
# See LICENSE for license details.
"""Match source CSV/Print/trigger/FAME plus shared control/CPU RTL to SFC.

This supporting case uses caller-supplied rational clock tokens. FPGA shell,
Linux and formal feature acceptance remain separate obligations.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from AutoCounterPipelineCompare import DRIVER
from InstrumentationControlCompare import INIT

DMA_INPUTS = r'''
 d->cpu_managed_axi4_ar_valid=dmaEnabled&&cycle%8<5;
 d->cpu_managed_axi4_ar_bits_addr=0;d->cpu_managed_axi4_ar_bits_id=42;
 d->cpu_managed_axi4_ar_bits_len=0;d->cpu_managed_axi4_ar_bits_size=6;
 d->cpu_managed_axi4_aw_bits_size=6;
 d->cpu_managed_axi4_r_ready=dmaEnabled&&cycle%193<180;
'''
DMA_TRACE = r'''
 dmaBeats+=d->cpu_managed_axi4_r_valid&&d->cpu_managed_axi4_r_ready;
 std::printf(" cpu=%u:%u:%u:%u",unsigned(d->cpu_managed_axi4_ar_ready),unsigned(d->cpu_managed_axi4_r_valid),unsigned(d->cpu_managed_axi4_aw_ready),unsigned(d->cpu_managed_axi4_b_valid));
 if(d->cpu_managed_axi4_r_valid){std::printf(" dma=%u:%u:%u",unsigned(d->cpu_managed_axi4_r_bits_id),unsigned(d->cpu_managed_axi4_r_bits_last),unsigned(d->cpu_managed_axi4_r_bits_resp));for(unsigned i=0;i<16;++i)std::printf(":%08x",d->cpu_managed_axi4_r_bits_data[i]);}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('native','oracle','evidence'):p.add_argument(name,type=Path)
    p.add_argument('--firtool',type=Path,required=True)
    p.add_argument('--verilator',type=Path,required=True)
    p.add_argument('--direct-print-input',action='store_true',help='exercise the source input fanout and direct Print Decoupled loopback')
    a=p.parse_args();native=a.native.resolve();sfc=a.oracle.resolve();out=a.evidence.resolve()
    out.mkdir(parents=True,exist_ok=True);commands=[];files=[]
    def run(cmd,name,trace=None):
        cmd=list(map(str,cmd));commands.append(cmd)
        (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        with(out/(name+'.log')).open('w')as log:
            if trace:
                with trace.open('w')as data:subprocess.run(cmd,stdout=data,stderr=log,check=True)
            else:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    stem='post-instrumentation-control'
    na=json.loads((native/(stem+'-all.json')).read_text())
    sa=json.loads((sfc/'sfc-fame.json').read_text())
    na=[x for x in na if not x['class'].endswith('OutputFileAnnotation')]
    classes={x['class']for x in na}
    supplemental={'firrtl.options.TargetDirAnnotation','firrtl.transforms.CombinationalPath',
        'midas.InternalXDCAnnotation','firrtl.transforms.BlackBoxInlineAnno',
        'midas.passes.EnabledInstrumentationPipelineOracle$ConstructorHints'}
    assert {x['class']for x in sa}-classes==supplemental
    bridges=[x for x in na if x['class']=='firesim.lib.bridgeutils.BridgeIOAnnotation']
    assert [b['widgetClass'].rsplit('.',1)[-1]for b in bridges]==['AutoCounterBridgeModule','AutoCounterBridgeModule','PrintBridgeModule']
    names={name for b in bridges for name in b['channelMapping'].values()}
    wrappers=['GGSelectedPrintCPUStreamPlatformWrapper','GGPrintBridgeHostWrapper','GGAutoCounterBridgeHostWrapper']
    def norm(x):
        if isinstance(x,str):
            for wrapper in wrappers:
                x=x.replace('~'+wrapper+'|'+wrapper+'>','~FAMETop|FAMETop>')
                x=x.replace('~'+wrapper,'~FAMETop')
                x=x.replace('|'+wrapper+'>','|FAMETop>')
            return x
        if isinstance(x,list):return [norm(v)for v in x]
        if isinstance(x,dict):
            y={k:norm(v)for k,v in x.items()}
            # PrintPort is a concrete constructor field; Scala's optional JSON
            # serialization hint does not change the ordered ports or format.
            if y.get('class')=='midas.widgets.PrintPort':y.pop('class')
            if y.get('class')=='firesim.lib.bridgeutils.BridgeIOAnnotation':y['target']='BOUNDHOST'
            if y.get('class')=='midas.passes.fame.FAMEChannelConnectionAnnotation'and y['globalName']in names:y.pop('sinks',None)
            return y
        return x
    branch_map={}
    if a.direct_print_input:
        # ChannelExcision numbers the two fanout branches in statement order.
        # SFC orders the promoted Print connect first; CIRCT orders the retained
        # model connect first. Derive a bijection by each branch's actual sink
        # role, and prove both remain members of the same source fanout. Do not
        # normalize arbitrary channels or constructor/clock/reset identities.
        fanout_class='midas.passes.fame.FAMEChannelFanoutAnnotation'
        channel_class='midas.passes.fame.FAMEChannelConnectionAnnotation'
        for records in [na,sa]:
            assert [x for x in records if x['class']==fanout_class and x['channelNames']==['topEvent','topEvent_1']]
        native_branches={x['globalName']:norm(x['sinks'][0]).rsplit('>',1)[1]for x in na if x['class']==channel_class and x['globalName']in ['topEvent','topEvent_1']}
        sfc_branches={x['globalName']:x['sinks'][0].rsplit('>',1)[1]for x in sa if x['class']==channel_class and x['globalName']in ['topEvent','topEvent_1']}
        assert native_branches=={'topEvent':'Top_topEvent_sink.bits','topEvent_1':'topEvent_1_sink.bits'}
        assert sfc_branches=={'topEvent':'topEvent_sink.bits','topEvent_1':'Top_topEvent_sink.bits'}
        branch_map={'topEvent':'topEvent_1','topEvent_1':'topEvent'}
    def native_norm(x):
        x=norm(x)
        if not a.direct_print_input:return x
        if isinstance(x,list):return [native_norm(v)for v in x]
        if isinstance(x,dict):
            x={k:native_norm(v)for k,v in x.items()}
            if x.get('class')=='midas.passes.fame.FAMEChannelConnectionAnnotation'and x.get('globalName')in branch_map:x['globalName']=branch_map[x['globalName']]
            if x.get('class')=='midas.passes.fame.FAMEChannelFanoutAnnotation'and x.get('channelNames')==['topEvent','topEvent_1']:x['channelNames']=['topEvent','topEvent_1']
            return x
        if isinstance(x,str):return x.replace('>topEvent_1_sink.bits','>topEvent_sink.bits')
        return x
    nn=sorted(json.dumps(native_norm(x),sort_keys=True)for x in na)
    sn=sorted(json.dumps(norm(x),sort_keys=True)for x in sa if x['class']in classes)
    (out/'native-normalized.json').write_text(json.dumps(nn,indent=2)+'\n')
    (out/'sfc-normalized.json').write_text(json.dumps(sn,indent=2)+'\n')
    assert nn==sn,'retained FAME/channel/constructor contracts differ'
    assert [b['clockInfo']for b in bridges]==[dict(name='base',multiplier=1,divisor=1),dict(name='slow',multiplier=1,divisor=3),dict(name='base',multiplier=1,divisor=1)]
    sinks={name:b['target']+'.'+(local.replace('_args_0','.args_0').replace('_enable','.enable')if b['widgetClass'].endswith('PrintBridgeModule')else local)for b in bridges for local,name in b['channelMapping'].items()}
    for x in na:
        if x['class'].endswith('FAMEChannelConnectionAnnotation')and x['globalName']in sinks:
            assert x['sinks']==[sinks[x['globalName']]]
            assert len(x['sources'])==1 and '|FAMETop>'in norm(x['sources'][0])and x['sources'][0].endswith('.bits')
    ir=(native/(stem+'.mlir')).read_text()
    gate_info=re.findall(r'goldengate\.fameClockGate = "([^"]+)", goldengate\.generatedClockConstraint = \{mfmr = (\d+) : i64, name = "([^"]+)"\}',ir)
    assert sorted(gate_info)==[('clock0','1','base'),('clock1','3','slow')]
    assert 'totalCredits' in ir and 'totalDebits' in ir and 'trigger_sync' in ir,'dynamic trigger hardware missing'
    reference=out/'sfc.sv'
    run([a.firtool.resolve(),sfc/'bound.fir','--disable-all-randomization','--disable-annotation-unknown','-o',reference],'sfc-syntax')
    traces=[]
    channels=['clocks','reset0','reset1','event0','event1','value0','value1','topEvent']
    if a.direct_print_input:channels.append('directPrint')
    for kind in ['native','sfc']:
        prefix=''if kind=='native'else'channels_'
        def channel(n):
            if n=='directPrint':return prefix+('topEvent_1_sink_'if kind=='native'else'topEvent_sink_')
            return prefix+'Top_'+n+'_sink_'
        def output(i):return prefix+f'Top_observed{i}_source_'
        inputs=[];initial=[];checks=[];fires=[]
        for i,n in enumerate(channels):
            name=channel(n)
            bits=[name+'bits_0',name+'bits_1']if i==0 else[name+'bits']
            if kind=='sfc'and i==0:bits=[name+'bits___05F0',name+'bits___05F1']
            inputs.append(f'd->{name}valid=pending[{i}];');initial.append(f'd->{name}valid=0;')
            for j,bit in enumerate(bits):
                inputs.append(f'd->{bit}=(payload[{i}]>>{j})&1;'if i==0 else f'd->{bit}=payload[{i}];');initial.append(f'd->{bit}=0;')
            fires.append(f'bool(d->{name}valid&&d->{name}ready)')
            checks.append(f'std::printf(" %u",unsigned(d->{name}ready));')
        output_checks=[]
        for i in range(2):
            name=output(i);initial.append(f'd->{name}ready=0;')
            output_checks.append(f'std::printf(" %u",unsigned(d->{name}valid));if(cycle>=600&&d->{name}valid){{std::printf(" o{i}=%llu",(unsigned long long)d->{name}bits);if(d->{name}ready)++observed[{i}];}}')
        mapping=dict(CLOCK='hostClock'if kind=='native'else'clock',RESET='hostReset'if kind=='native'else'reset',
            EXTRAINPUTS=''if kind=='native'else'd->channels_hostClock=CLOCK;d->channels_hostReset=RESET;',
            DRIVEINPUTS='\n'.join(inputs),INITINPUTS='\n'.join(initial),FIRES=','.join(fires),
            TRACEINPUTS='\n'.join(checks),TRACEOUTPUTS='\n'.join(output_checks),OUT0=output(0),OUT1=output(1))
        source=DRIVER.replace('#define C(name) ((bus==0)?d->@CTRL0@##name:d->@CTRL1@##name)','#define C(name) d->ctrl_##name')
        source=source.replace('C(aw_bits_addr)=addr;','C(aw_bits_addr)=addr+(bus==0?0:bus==1?128:192);').replace('C(ar_bits_addr)=addr;','C(ar_bits_addr)=addr+(bus==0?0:bus==1?128:192);')
        source=source.replace('int saved=bus;for(bus=0;bus<2;++bus){std::printf','std::printf').replace('unsigned(C(r_bits_last)));}bus=saved;','unsigned(C(r_bits_last)));')
        source=source.replace('bool pending[8]={};','unsigned dmaBeats=0;bool dmaEnabled=false;\nbool pending[8]={};')
        if a.direct_print_input:source=source.replace('[8]','[9]').replace('i<8','i<9')
        source=source.replace(' CLOCK=0;d->eval();\n Result r',DMA_INPUTS+' CLOCK=0;d->eval();\n Result r')
        source=source.replace(' std::puts("");CLOCK=1;',DMA_TRACE+' std::puts("");CLOCK=1;')
        source=source.replace('d=new VBound;','d=new VBound;'+INIT)
        source=source.replace('for(unsigned round=0;round<3;++round){','for(unsigned round=0;round<3;++round){bus=2;busWrite(0,0,1);busWrite(4,0,2);busWrite(8,5000,3);busWrite(12,0,4);busWrite(16,1,5);dmaEnabled=true;')
        source=source.replace(' if(samples[0]<10',' bus=2;busWrite(20,1,9);for(unsigned n=0;n<800;++n)step();bus=0;(void)busRead(224,77);\n if(dmaBeats<10)throw std::runtime_error("insufficient source-derived Print DMA exercise");std::fprintf(stderr,"PRINT dmaBeats=%u\\n",dmaBeats);\n if(samples[0]<10')
        for key,value in mapping.items():source=source.replace('@'+key+'@',value)
        assert not re.search(r'@[A-Z0-9_]+@',source)
        cpp=out/(kind+'.cpp');cpp.write_text(source);obj=out/(kind+'-obj')
        rtl=native/(stem+'.sv')if kind=='native'else reference
        run([a.verilator.resolve(),'--cc','--exe','--build','-j','1','-Wno-fatal','--assert','--top-module','GGSelectedPrintCPUStreamPlatformWrapper'if kind=='native'else'LazyModule','--prefix','VBound','--Mdir',obj,rtl,sfc/'AbstractClockGate.v',cpp],kind+'-build')
        trace=out/(kind+'.trace');run([obj/'VBound',f"+verilator+seed+{101 if kind=='native'else 202}"],kind+'-run',trace)
        traces.append(trace);files += [rtl,cpp,trace]
    lines=[[x for x in trace.read_text().splitlines()if x and x[0].isdigit()]for trace in traces]
    assert len(lines[0])==len(lines[1]),'cycle count differs'
    for i,(n,s)in enumerate(zip(*lines)):assert n==s,f'cycle {i}: {n} != {s}'
    files += [native/(stem+'.mlir'),native/(stem+'-all.json'),sfc/'input.fir',sfc/'input.anno.json',sfc/'bound.fir',sfc/'sfc-fame.json',sfc/'AbstractClockGate.v',out/'commands.json',Path(__file__)]
    receipt=dict(passed=True,full_feature_acceptance=False,
        scope='same source -> enabled CSV + Print + dynamic trigger/reset/clock wiring -> complete FAME -> shared control/CPU/driver boundary',
        cycles=len(lines[0]),retained_annotations=len(na),direct_print_input=a.direct_print_input,fanout_branch_bijection=branch_map,checks=['complete ordered constructors and channel contracts','two target domains with rational1/1,1/3 clock ownership','independent stable target input tokens and stalled outputs','real target-derived counters and print records','dynamic reset-masked credit/debit trigger sources','shared MMIO sampling and Print DMA payload/order/IDs/last with backpressure','source-derived reset and narrow Print flush'],
        assumptions=['caller supplies rational clock tokens; clock generator phase is separate','independently seeded unreset memory; compare only live valid payloads after specified target resets','no FPGA shell or original platform bridge integration inferred'],
        remaining=['bounded Linux-capable F2 representative','separate complete LEC and SEC','checked enabled CSV/Print Linux exercise and clean poweroff'],
        metrics={kind:{'summary':[line for line in (out/(kind+'-run.log')).read_text().splitlines()if line.startswith(('PASS ','PRINT '))], 'target_printf_lines':sum(line.startswith('top=')for line in (out/(kind+'-run.log')).read_text().splitlines())}for kind in ['native','sfc']},sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest()for f in files})
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('PASS',receipt['cycles'],'cycles; enabled source/FAME/shared-control/CPU differential')

if __name__=='__main__':main()
