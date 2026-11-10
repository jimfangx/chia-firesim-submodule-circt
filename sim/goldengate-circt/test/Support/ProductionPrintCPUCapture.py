#!/usr/bin/env python3
"""Execute frozen production Print queue and actual CPU read/AR/R transport.

Extracts unchanged emitted module bodies, joins their production interfaces,
then applies deterministic DMA bursts, response stalls, and interleaved TracerV
reads. The target model is replaced by explicit initialized Print inputs. This
is a supporting composition reproducer, not Linux, formal or full SFC parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


DRIVER = r'''
#include "VPrintCPU.h"
#include <verilated.h>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <cstring>
VPrintCPU d;
#ifndef TARGET_CYCLES
#define TARGET_CYCLES 100000
#endif
#ifndef PRINT_PERIOD
#define PRINT_PERIOD 1
#endif
#ifndef PRINT_STREAM_INDEX
#define PRINT_STREAM_INDEX 1
#endif
#ifndef MAX_SDK_BEATS
#define MAX_SDK_BEATS 0
#endif
#ifndef EXTRA_PHYSICAL_READS
#define EXTRA_PHYSICAL_READS 0
#endif
#ifndef PRINT_COUNT_ADDRESS
#define PRINT_COUNT_ADDRESS 668
#endif
struct Request{unsigned stream,remaining,id,lane,size;};
std::deque<Request> requests;
uint64_t logical=0,host=0,traceWritten=0,traceRead=0,printRead=0;
unsigned reserved[2]={0,0},issued=0;
unsigned peekIssued=0,peekReceived=0;
unsigned requestedLane=0;
unsigned headRequests=0,headResponses=0,acknowledgements=0;
bool ackActive=false,ackRetryGood=false,ackAWDone=false,ackWDone=false,ackBad=false;
unsigned ackAge=0,ackMode=0,ackID=0,ackPulses=0,ackTransactions=0,rejectedPartialACKs=0,totalACKPulses=0;
unsigned ackModes[3]={0,0,0};
uint32_t packed[16]={0};
bool running=false, arPending=false;
FILE *stream;
void step(){
 d.hValid=running;
 d.hBits_synthesizedPrintf_clockBridge_clocks_0_globalReset=logical<50;
 auto count=logical<50?0:logical-50;
 d.hBits_synthesizedPrintf_feature_domain_0_printf_wire_enable=logical>=50&&count%PRINT_PERIOD==0;
 d.hBits_synthesizedPrintf_feature_domain_0_printf_wire_args_0=count;
 d.hBits_synthesizedPrintf_feature_domain_0_printf_wire_args_1=count%16;
 d.trace_valid=running&&traceWritten<4096;
 for(unsigned w=0;w<16;++w)d.trace_bits[w]=0x92000000u|(w<<20)|traceWritten;
 d.dma_r_ready=host%23<15;
 bool headBudgetIssued=false;
#ifdef EXPLICIT_ACKS
 const unsigned budget=
#ifdef COALESCED_PEEKS
   1+
#else
   16+
#endif
   EXTRA_PHYSICAL_READS;
 headBudgetIssued=headRequests>=budget;
 if(headBudgetIssued&&headResponses>=budget&&requests.empty()&&!arPending&&!ackActive){
   ackActive=true;ackAWDone=ackWDone=false;ackAge=ackPulses=0;
   ackMode=ackTransactions%3;ackID=ackTransactions%4096;++ackTransactions;++ackModes[ackMode];
#ifdef PARTIAL_ACK_CONTROLS
   ackBad=!ackRetryGood;
#else
   ackBad=false;
#endif
 }
 d.ack_aw_valid=ackActive&&!ackAWDone&&ackAge>=(ackMode==1?5:0);
 d.ack_w_valid=ackActive&&!ackWDone&&ackAge>=(ackMode==0?5:0);
 d.ack_aw_bits_addr=ackAWDone?0x7fffffff:PRINT_COUNT_ADDRESS;
 d.ack_aw_bits_id=ackAWDone?4095:ackID;
 // Changing the live bus after W handshake catches adapters that forward
 // current WSTRB instead of retaining it alongside the accepted WDATA.
 d.ack_w_bits_data=ackWDone?0xdeadbeef:1;
 d.ack_w_bits_strb=ackWDone?0:(ackBad?7:15);
 d.ack_b_ready=ackActive&&ackAWDone&&ackWDone&&ackAge>=10&&host%7<5;
#endif
 if(!arPending && host>30000 && requests.size()<2&&!headBudgetIssued){
   unsigned desired=(issued%5==4)?0:1;
   unsigned counts[2]={d.traceCount,d.streamCount};
   if(counts[desired]<=reserved[desired])desired=1-desired;
   unsigned avail=counts[desired]>reserved[desired]?counts[desired]-reserved[desired]:0;
   if(avail){
     const unsigned lengths[6]={1,2,7,16,64,256};
     unsigned n=std::min(avail,lengths[issued%6]);
     d.dma_ar_bits_addr=static_cast<uint64_t>(desired?PRINT_STREAM_INDEX:0)<<19;
     d.dma_ar_bits_id=issued%4;d.dma_ar_bits_len=n-1;d.dma_ar_bits_size=6;
     requestedLane=0;
#ifdef NARROW_PEEKS
     if(desired==1){
       requestedLane=peekIssued%16;
#ifdef EXPLICIT_ACKS
#ifdef COALESCED_PEEKS
       requestedLane=0;
#else
       requestedLane=headRequests<16?headRequests:0;
#endif
#endif
       d.dma_ar_bits_id=32;d.dma_ar_bits_size=2;d.dma_ar_bits_len=0;d.dma_ar_bits_addr+=requestedLane*4;
#ifdef NORMALIZED_PEEKS
       d.dma_ar_bits_size=6;
#endif
#ifdef ALIGNED_PEEKS
       d.dma_ar_bits_addr&=~uint64_t(63);
#endif
     }
#endif
     arPending=true;
   }
 }
 d.dma_ar_valid=arPending;
 d.hostClock=0;d.eval();
 if(d.dma_ar_valid&&d.dma_ar_ready){
   unsigned s=(d.dma_ar_bits_addr>>19)==PRINT_STREAM_INDEX?1:0,n=d.dma_ar_bits_len+1;
   unsigned kind=d.dma_ar_bits_size;
#ifdef NARROW_PEEKS
   if(s==1){
     kind=2;++peekIssued;
#ifdef EXPLICIT_ACKS
#ifdef COALESCED_PEEKS
     kind=headRequests?4:3;
#else
     if(headRequests>=16)kind=4;
#endif
     ++headRequests;
#endif
   }
#endif
   requests.push_back({s,n,d.dma_ar_bits_id,requestedLane,kind});
   reserved[s]+=n;++issued;arPending=false;
 }
 if(d.dma_r_valid&&d.dma_r_ready){
   if(requests.empty())throw std::runtime_error("response without accepted AR");
   auto &r=requests.front();
   if(d.dma_r_bits_id!=r.id || d.dma_r_bits_resp || d.dma_r_bits_last!=(r.remaining==1))
     throw std::runtime_error("CPU burst ID/last/response mismatch");
   if(r.stream==1){
     ++headResponses;
     if(r.size==2){
       if(r.lane!=peekReceived%16)throw std::runtime_error("SDK word response order differs");
       packed[peekReceived%16]=d.dma_r_bits_data[r.lane];++peekReceived;
       if(peekReceived%16==0){std::fwrite(packed,64,1,stream);++printRead;}
     }else if(r.size==3){
       for(unsigned w=0;w<16;++w)std::fwrite(&d.dma_r_bits_data[w],4,1,stream);++printRead;peekReceived+=16;
     }else if(r.size!=4){for(unsigned w=0;w<16;++w)std::fwrite(&d.dma_r_bits_data[w],4,1,stream);++printRead;}
   }
   else{
     for(unsigned w=0;w<16;++w)if(d.dma_r_bits_data[w]!=(0x92000000u|(w<<20)|traceRead))
       throw std::runtime_error("interleaved Tracer data corruption");
     ++traceRead;
   }
   --reserved[r.stream];if(!--r.remaining)requests.pop_front();
 }
 bool fire=d.hValid&&d.hReady;
 bool tfire=d.trace_valid&&d.trace_ready;
 bool ack=false;
#ifdef EXPLICIT_ACKS
 bool awFire=d.ack_aw_valid&&d.ack_aw_ready,wFire=d.ack_w_valid&&d.ack_w_ready;
 if(d.observed_ack){++ackPulses;++totalACKPulses;}
 bool bFire=d.ack_b_valid&&d.ack_b_ready;
 if(bFire){
   if(!ackActive||!ackAWDone||!ackWDone||d.ack_b_bits_id!=ackID||d.ack_b_bits_resp)
     throw std::runtime_error("actual count MCR AXI B response is unbound or erroneous");
   if(ackBad){
     if(ackPulses)throw std::runtime_error("partial WSTRB incorrectly consumed a source head");
     ++rejectedPartialACKs;ackRetryGood=true;
   }else{
     if(ackPulses!=1){
       std::fprintf(stderr,"ACK_ROUTE_FAILURE observed=%u expected=1 retained_count=%u\n",ackPulses,d.streamCount);
       std::fflush(stream);throw std::runtime_error("full-strobe ACK did not consume exactly one head through actual MCR");
     }
     ack=true;ackRetryGood=false;
   }
   ackActive=false;
 }
 ackAWDone|=awFire;ackWDone|=wFire;
#endif
 d.hostClock=1;d.eval();d.hostClock=0;d.eval();
 if(ack){headRequests=headResponses=0;++acknowledgements;}
 if(ackActive)++ackAge;
 logical+=fire;traceWritten+=tfire;++host;
 if(host>static_cast<uint64_t>(TARGET_CYCLES)*4+1000000)throw std::runtime_error("production CPU stream watchdog");
}
void wr(unsigned word,uint32_t value){
 d.ctrl_aw_bits_addr=4*word;d.ctrl_aw_valid=1;d.ctrl_w_valid=1;d.ctrl_w_bits_data=value;
 bool a=false,w=false;
 for(unsigned n=0;n<100&&(!a||!w);++n){
   d.hostClock=0;d.eval();bool aa=d.ctrl_aw_valid&&d.ctrl_aw_ready,ww=d.ctrl_w_valid&&d.ctrl_w_ready;
   step();a|=aa;w|=ww;d.ctrl_aw_valid=!a;d.ctrl_w_valid=!w;
 }
 if(!a||!w)throw std::runtime_error("Print AW/W timeout");
 for(unsigned n=0;n<100;++n){d.hostClock=0;d.eval();bool b=d.ctrl_b_valid;step();if(b)return;}
 throw std::runtime_error("Print B timeout");
}
void drain(){
 unsigned stable=0;
 while(stable<100){step();
   bool empty=!d.streamCount&&!d.traceCount&&requests.empty()&&!arPending;
   stable=empty?stable+1:0;
 }
}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 std::string root=argv[1];stream=std::fopen((root+"/run.bin").c_str(),"wb");
 d.hostClock=0;d.hostReset=1;d.ctrl_b_ready=1;d.ctrl_r_ready=1;
 d.ctrl_aw_bits_size=2;d.ctrl_aw_bits_burst=1;d.ctrl_w_bits_last=1;d.ctrl_w_bits_strb=15;
 for(unsigned n=0;n<5;++n)step();d.hostReset=0;
 wr(0,0);wr(1,0);wr(2,UINT32_MAX);wr(3,UINT32_MAX);wr(4,1);
 running=true;while(logical<TARGET_CYCLES && (!MAX_SDK_BEATS || printRead<MAX_SDK_BEATS))step();
 if(MAX_SDK_BEATS&&printRead>=MAX_SDK_BEATS){
   std::fclose(stream);stream=std::fopen((root+"/flush.bin").c_str(),"wb");std::fclose(stream);
   std::printf("BOUNDED_CAPTURE logical=%llu host=%llu print_beats=%llu queue_head_count=%u SDK_peeks=%u\n",
     (unsigned long long)logical,(unsigned long long)host,(unsigned long long)printRead,d.streamCount,peekReceived);
   return 0;
 }
 running=false;drain();
 std::fclose(stream);stream=std::fopen((root+"/flush.bin").c_str(),"wb");wr(5,1);drain();std::fclose(stream);
 if(traceRead!=traceWritten)throw std::runtime_error("Tracer data not fully drained");
 std::printf("COMPLETE logical=%llu host=%llu print_beats=%llu trace_beats=%llu bursts=%u SDK_peeks=%u incomplete_words=%u explicit_ACKs=%u hardware_ACKs=%u OCL_transactions=%u rejected_partial=%u AWbeforeW=%u WbeforeAW=%u simultaneous=%u\n",
  (unsigned long long)logical,(unsigned long long)host,(unsigned long long)printRead,
  (unsigned long long)traceRead,issued,peekReceived,peekReceived%16,acknowledgements,totalACKPulses,
  ackTransactions,rejectedPartialACKs,ackModes[0],ackModes[1],ackModes[2]);
}
'''


def ports(header):
    header=re.sub(r'//[^\n]*','',header)
    result = []
    direction, width = None, ''
    for entry in header[header.index('(')+1:header.rindex(')')].split(','):
        match = re.fullmatch(r'\s*(?:(input|output|inout)\s+)?(\[[^]]+\]\s*)?(\w+)\s*', entry)
        if not match:
            raise ValueError('Unsupported production port: '+entry)
        if match[1]:
            direction, width = match[1], match[2] or ''
        result.append((direction, width, match[3]))
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo',type=Path,required=True);p.add_argument('--rtl',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--narrow-peeks',action='store_true',help='Replay simif_f2.cc BAR4 lane reads: size2,len0, successive4byteaddresses')
    p.add_argument('--normalized-peeks',action='store_true',help='Replay pinnedAWSBFM PCIID0x20, size6-normalizedDWORDpeeks (impliesnarrow-peeks)')
    p.add_argument('--aligned-peeks',action='store_true',help='Hypothesiscontrol: physical64BalignedPCIreads, originalhostDWORDlane steering retained; impliesnormalized-peeks')
    p.add_argument('--max-sdk-beats',type=int,default=0,help='Boundrepeatedheadcapture without draining indefinitely (0=normalcompleteexecution)')
    p.add_argument('--explicit-acks',action='store_true',help='ActualnewCountBankOCLwrite1headACKafterhost64Bcopy; neverinjectedintolegacyRTL')
    p.add_argument('--coalesced-peeks',action='store_true',help='Onephysical64BPCItransactionperhost64Bcopy; explicitACKonly')
    p.add_argument('--extra-physical-reads',type=int,default=0,help='0..2extraspeculative/repeatedphysicalreadsperhosthead beforeACK')
    p.add_argument('--partial-ack-controls',action='store_true',help='RejectpartialWSTRB7attherealMCRthenretryfull15; actualAXIAW/W/Borderingrotates')
    p.add_argument('--cycles',type=int,default=100000);p.add_argument('--period',type=int,default=1)
    p.add_argument('--print-stream-index',type=int,help='Default last actualCPU source, e.g.1singleRocket or4quadRocket')
    p.add_argument('--sfc-cpu-rtl',type=Path,help='Use actualimmutableSFC CPUManagedStreamEngine forlegal64Btransportoracle')
    p.add_argument('--jobs',type=int,default=1,help='BoundedindependentVerilatorC++buildparallelism1..4')
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    if a.aligned_peeks:a.normalized_peeks=True
    if a.coalesced_peeks:a.normalized_peeks=True
    if a.explicit_acks:a.normalized_peeks=True
    if a.normalized_peeks:a.narrow_peeks=True
    if a.sfc_cpu_rtl and a.narrow_peeks:p.error('SFCassertcontract is64Bbeats; narrowextensionhasseparateSDKoracle')
    if a.cycles<51 or a.period<1:p.error('cycles must exceed50 andperiod mustpositive')
    if not 1<=a.jobs<=4:p.error('jobs must be1..4')
    if a.max_sdk_beats<0 or (a.aligned_peeks and not a.max_sdk_beats and not a.explicit_acks):p.error('unalteredheadhypothesisrequiresboundedpositive max-sdk-beats')
    if not 0<=a.extra_physical_reads<=2 or ((a.coalesced_peeks or a.extra_physical_reads) and not a.explicit_acks):p.error('coalesced/extra-readACKprotocolcontrolsrequireexplicit-acks and0..2extras')
    if a.partial_ack_controls and not a.explicit_acks:p.error('partialACKcontrolsrequireexplicit-acks')
    rtl=a.rtl.read_text();original=hashlib.sha256(a.rtl.read_bytes()).hexdigest()
    modules={m[1]:m[0] for m in re.finditer(r'\bmodule\s+(\w+)\s*\(.*?\bendmodule\b',rtl,re.S)}
    indices={int(index) for index in re.findall(r'\bstream_(\d+)_valid\b',modules['GGCPUStreamRead'].split(';',1)[0])}
    print_index=max(indices) if a.print_stream_index is None else a.print_stream_index
    if print_index not in indices or print_index==0:raise ValueError('SelectedPrintstream isabsent orreservedTracer0')
    required=['GGPrintBridgeHostQueued','GGCPUStreamRead','GGCPUStreamAWQueue2','GGCPUStreamRQueue2','GGTracerVStreamQueue6144']
    selected={}
    def select(name,source=modules):
        if name in selected:
            if selected[name]!=source[name]:raise ValueError('Native/SFCmodulecollision: '+name)
            return
        body=source[name];selected[name]=body
        for child in re.findall(r'^\s*(\w+)\s+\w+\s*\(',body,re.M):
            if child in source and child!=name:select(child,source)
    for name in required:select(name)
    if a.explicit_acks:
        select('GGCPUStreamCountBank')
        select('GGCPUStreamMCRFile')
        if 'sdkReadAck' not in modules['GGCPUStreamRead'].split(';',1)[0]:
            raise ValueError('ActualexplicitACKprofileportsabsent; refusingtofabricateACKforlegacyRTL')
    live_strobe=None;route_checks={}
    if a.explicit_acks:
        def actual_instance(kind):
            matches=list(re.finditer(r'\b'+re.escape(kind)+r'\s+(\w+)\s*\((.*?)\);',modules['FPGATop'],re.S))
            if len(matches)!=1:raise ValueError('ProductionFPGAToprequiresoneactual'+kind)
            return dict(re.findall(r'\.(\w+)\s*\(([^()]*)\)',matches[0][2]))
        bank_live,mcr_live,read_live=(actual_instance(kind) for kind in ('GGCPUStreamCountBank','GGCPUStreamMCRFile','GGCPUStreamRead'))
        route_checks['mcr_strobe_live']=bank_live['mcr_wstrb'].strip()==mcr_live['mcr_wstrb'].strip()
        for name in bank_live:
            if re.fullmatch(r'mcr_write_\d+_(bits|valid)',name):
                route_checks[name]=bank_live[name].strip()==mcr_live[name].strip()
            elif name.startswith('sdkReadAck'):
                route_checks[name]=bank_live[name].strip()==read_live[name].strip()
        if not all(value for key,value in route_checks.items() if key!='mcr_strobe_live'):
            raise ValueError('ActualproductionMCRdata/valid/SDKACKrouteismisbound')
        if not route_checks['mcr_strobe_live']:
            candidate=bank_live['mcr_wstrb'].strip()
            if re.fullmatch(r"4'[hbd]0|4'h0",candidate) is None:raise ValueError('UnsupportedmisboundproductionWSTRBroute')
            live_strobe=candidate
    count_address=668
    if a.explicit_acks:
        header_path=a.rtl.parent/'FireSim-generated.const.h'
        match=re.search(r'CPUManagedStreams::StreamParameters\(std::string\("PRINTBRIDGEMODULE_0_to_cpu_stream"\),\s*([0-9]+)(?:ULL|U)?,\s*([0-9]+)(?:ULL|U)?',header_path.read_text())
        if not match or int(match[1])>>19!=print_index:raise ValueError('ActualPrintstreamDMA/countallocationmissingormisbound')
        count_address=int(match[2])
    sfc_original=None
    if a.sfc_cpu_rtl:
        sfc_text=a.sfc_cpu_rtl.read_text();sfc_original=hashlib.sha256(a.sfc_cpu_rtl.read_bytes()).hexdigest()
        sfc_modules={m[1]:m[0] for m in re.finditer(r'\bmodule\s+(\w+)\s*\(.*?\bendmodule\b',sfc_text,re.S)}
        select('CPUManagedStreamEngine',sfc_modules)
    definitions='\n\n'.join(selected.values())
    header=modules['GGPrintBridgeHostQueued'].split(';',1)[0]
    base=ports(header)
    external=[x for x in base if x[2] not in {'streamReady','streamValid','streamData'}]
    if a.sfc_cpu_rtl:external=[(d,'[13:0] ' if n=='streamCount' else w,n) for d,w,n in external]
    extra=[('input','','trace_valid'),('output','','trace_ready'),('input','[511:0] ','trace_bits'),
           ('output','[13:0] ' if a.sfc_cpu_rtl else '[12:0] ','traceCount')]
    if a.explicit_acks:
        extra += [('input','','ack_aw_valid'),('output','','ack_aw_ready'),('input','[31:0] ','ack_aw_bits_addr'),
                  ('input','[11:0] ','ack_aw_bits_id'),('input','','ack_w_valid'),('output','','ack_w_ready'),
                  ('input','[31:0] ','ack_w_bits_data'),('input','[3:0] ','ack_w_bits_strb'),
                  ('input','','ack_b_ready'),('output','','ack_b_valid'),('output','[11:0] ','ack_b_bits_id'),
                  ('output','[1:0] ','ack_b_bits_resp'),('output','','observed_ack')]
    for channel,fields in [('ar',[('id',16),('addr',64),('len',8),('size',3)]),
                           ('r',[('id',16),('data',512),('resp',2),('last',1)])]:
        extra += [('input' if channel=='ar' else 'output','','dma_'+channel+'_valid'),
                  ('output' if channel=='ar' else 'input','','dma_'+channel+'_ready')]
        extra += [('input' if channel=='ar' else 'output',f'[{w-1}:0] ' if w>1 else '',
                   'dma_'+channel+'_bits_'+n) for n,w in fields]
    wrapper='module PrintCPU(\n'+',\n'.join(f'  {d} {w}{n}' for d,w,n in external+extra)+'\n);\n'
    wire_ports={}
    instances=[]
    def instance(kind,name,mapping):
        nonlocal wrapper
        ps=ports(selected[kind].split(';',1)[0]);entries=[]
        for direction,width,port in ps:
            signal=mapping[port]
            if re.fullmatch(r'[A-Za-z_]\w*',signal) and signal not in {x[2] for x in external+extra}:
                wire_ports[signal]=width
            entries.append('  .'+port+'('+signal+')')
        instances.append(kind+' '+name+'(\n'+',\n'.join(entries)+'\n);')
    mapping={n:({'streamReady':'print_ready','streamValid':'print_valid','streamData':'print_bits'}.get(n,n)) for _,_,n in base}
    if a.sfc_cpu_rtl:mapping['streamCount']='printSourceCount'
    instance('GGPrintBridgeHostQueued','print',mapping)
    qports=ports(modules['GGTracerVStreamQueue6144'].split(';',1)[0])
    instance('GGTracerVStreamQueue6144','tracer',{n:{'clock':'hostClock','reset':'hostReset',
        'enq_valid':'trace_valid','enq_ready':'trace_ready','enq_bits':'trace_bits',
        'deq_valid':'tracer_valid','deq_ready':'tracer_ready','deq_bits':'tracer_bits',
        'count':'traceSourceCount' if a.sfc_cpu_rtl else 'traceCount'}[n] for _,_,n in qports})
    mapping={'clock':'hostClock','reset':'hostReset'}
    for direction,width,name in ports(modules['GGCPUStreamAWQueue2'].split(';',1)[0]):
        if name in mapping:continue
        mapping[name]=name.replace('enq_','dma_ar_').replace('deq_','read_ar_')
    instance('GGCPUStreamAWQueue2','arQueue',mapping)
    mapping={'clock':'hostClock','reset':'hostReset'}
    for direction,width,name in ports(modules['GGCPUStreamRQueue2'].split(';',1)[0]):
        if name in mapping:continue
        mapping[name]=name.replace('enq_','read_r_').replace('deq_','dma_r_')
    instance('GGCPUStreamRQueue2','rQueue',mapping)
    mapping={'clock':'hostClock','reset':'hostReset'}
    for direction,width,name in ports(modules['GGCPUStreamRead'].split(';',1)[0]):
        if name in mapping:continue
        if name.startswith('sdkReadAck'):
            mapping[name]=name.replace('sdkReadAck','sdk_ack') if a.explicit_acks else "'0";continue
        if name.startswith('stream_0_'):mapping[name]=name.replace('stream_0_','tracer_')
        elif name.startswith(f'stream_{print_index}_'):mapping[name]=name.replace(f'stream_{print_index}_','print_')
        elif name.startswith('stream_'):
            mapping[name]=('512\'h0' if name.endswith('_bits') else '1\'h0') if direction=='input' else 'unused_'+name
        else:mapping[name]='read_'+name
    if not a.sfc_cpu_rtl:
        instance('GGCPUStreamRead','cpuStreamRead',mapping)
    else:
        mapping={'clock':'hostClock','reset':'hostReset'}
        for direction,width,name in ports(selected['CPUManagedStreamEngine'].split(';',1)[0]):
            if name in mapping:continue
            if name.startswith('auto_cpu_managed_axi4_in_ar_') or name.startswith('auto_cpu_managed_axi4_in_r_'):
                mapping[name]='read_'+name.removeprefix('auto_cpu_managed_axi4_in_')
            elif name.startswith('streamsToHostCPU_0_'):mapping[name]=name.replace('streamsToHostCPU_0_','tracer_')
            elif name.startswith('streamsToHostCPU_1_'):mapping[name]=name.replace('streamsToHostCPU_1_','print_')
            else:mapping[name]=("'0" if direction=='input' else 'oracle_unused_'+name)
        instance('CPUManagedStreamEngine','oracle',mapping)
    if a.explicit_acks:
        mapping={'clock':'hostClock','reset':'hostReset','mcr_wstrb':live_strobe or 'ack_mcr_wstrb'}
        for direction,width,name in ports(selected['GGCPUStreamCountBank'].split(';',1)[0]):
            if name in mapping:continue
            if name.startswith('sdkReadAck'):mapping[name]=name.replace('sdkReadAck','sdk_ack')
            elif re.fullmatch(r'count_\d+',name):
                index=int(name.split('_')[-1]);mapping[name]='traceCount' if index==0 else 'streamCount' if index==print_index else "13'h0"
            elif name.startswith('mcr_'):mapping[name]='ack_'+name
            elif direction=='input':mapping[name]="'0"
            else:mapping[name]='ack_unused_'+name
        instance('GGCPUStreamCountBank','actualCountBank',mapping)
        mapping={'clock':'hostClock','reset':'hostReset'}
        exposed={item[2] for item in extra}
        for direction,width,name in ports(selected['GGCPUStreamMCRFile'].split(';',1)[0]):
            if name in mapping:continue
            candidate=name.replace('nasti_','ack_')
            if name.startswith('mcr_'):mapping[name]='ack_'+name
            elif candidate in exposed:mapping[name]=candidate
            elif direction=='input':
                mapping[name]="3'h2" if name.endswith('_size') else "2'h1" if name.endswith('_burst') else "1'h1" if name in ('nasti_w_bits_last','nasti_r_ready') else "'0"
            else:mapping[name]='ack_unused_'+name
        instance('GGCPUStreamMCRFile','actualCountMCR',mapping)
    wrapper+='\n'.join('wire '+width+name+';' for name,width in wire_ports.items())+'\n'
    wrapper+='\n'.join(instances)+'\nendmodule\n'
    if a.explicit_acks:
        if 'sdk_ack' in wire_ports:signal=f'sdk_ack[{print_index}]'
        else:signal=f'sdk_ack_{print_index}'
        wrapper=wrapper.replace('endmodule\n','assign observed_ack = '+signal+';\nendmodule\n')
    if a.sfc_cpu_rtl:
        count_names=['TRACERVBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q_io_count',
                     'PRINTBRIDGEMODULE_0_to_cpu_stream_outgoingQueueIO_q_io_count']
        if any(name not in selected['CPUManagedStreamEngine'] for name in count_names):
            raise ValueError('ActualSFCsourcequeuecount observationnames changed')
        observe=("assign read_r_bits_resp = 2'h0;\n"
                 'assign traceCount = {1\'b0,traceSourceCount} + {1\'b0,oracle.'+count_names[0]+'};\n'
                 'assign streamCount = {1\'b0,printSourceCount} + {1\'b0,oracle.'+count_names[1]+'};\n')
        wrapper=wrapper.replace('endmodule\n',observe+'endmodule\n')
    source=out/'production.sv';source.write_text(definitions+'\n'+wrapper)
    driver=out/'driver.cpp';driver.write_text(DRIVER)
    repo=a.repo.resolve();verilator=repo/'.conda-env/bin/verilator'
    command=[str(verilator),'--cc','--exe','--build','-j',str(a.jobs),'-Wno-fatal','--top-module','PrintCPU',
        '--prefix','VPrintCPU','--Mdir',str(out/'obj'),'-CFLAGS',
        f'-DTARGET_CYCLES={a.cycles} -DPRINT_PERIOD={a.period} -DPRINT_STREAM_INDEX={print_index}'
        +f' -DMAX_SDK_BEATS={a.max_sdk_beats}'+(' -DNARROW_PEEKS' if a.narrow_peeks else '')
        +(' -DNORMALIZED_PEEKS' if a.normalized_peeks else '')+(' -DALIGNED_PEEKS' if a.aligned_peeks else '')
        +(' -DEXPLICIT_ACKS' if a.explicit_acks else '')+(' -DCOALESCED_PEEKS' if a.coalesced_peeks else '')
        +(' -DPARTIAL_ACK_CONTROLS' if a.partial_ack_controls else '')
        +f' -DEXTRA_PHYSICAL_READS={a.extra_physical_reads} -DPRINT_COUNT_ADDRESS={count_address}',
        str(source),str(driver)]
    with(out/'build.log').open('w')as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
    executed=subprocess.run([str(out/'obj/VPrintCPU'),str(out)],text=True,capture_output=True,timeout=120)
    (out/'stdout.log').write_text(executed.stdout);(out/'stderr.log').write_text(executed.stderr)
    incomplete=[]
    for name in ('run.bin','flush.bin'):
        if not (out/name).exists():
            incomplete.append(name);(out/name).write_bytes(b'')
    decoded=[];cycle=0
    data=(out/'run.bin').read_bytes()+(out/'flush.bin').read_bytes()
    for off in range(0,len(data),16):
        token=int.from_bytes(data[off:off+16],'little')
        if token&1:
            decoded.append(dict(cycle=cycle,value=(token>>2)&((1<<64)-1),wrap=(token>>66)&15,enabled=bool(token&2)))
            cycle+=1
        else:cycle+=(token&65534)>>1
    expected=[dict(cycle=n+50,value=n,wrap=n%16,enabled=True) for n in range(0,max(0,a.cycles-50),a.period)]
    decoded_match=decoded==expected
    (out/'decoded.json').write_text(json.dumps(dict(actual=decoded,expected_count=len(expected),match=decoded_match),indent=2)+'\n')
    result=dict(passed=executed.returncode==0 and decoded_match and (out/'flush.bin').stat().st_size>=64
                and all(route_checks.values()),returncode=executed.returncode,
        scope='Actual frozen production Print+queue and '+('unchanged SFC CPUManagedStreamEngine' if a.sfc_cpu_rtl else 'nativeCPURead')+' +actualAR/R buffers under initialized target inputs/interleavedTracerDMA; no fulltransform/Linux/formalclaim',
        source=str(a.rtl.resolve()),source_sha256=original,modules=list(selected),build_command=command,
        stdout=executed.stdout,stderr=executed.stderr,
        incomplete_captures=incomplete,
        narrow_peeks=a.narrow_peeks,normalized_peeks=a.normalized_peeks,aligned_peeks_hypothesis=a.aligned_peeks,
        bounded_capture=a.max_sdk_beats,period=a.period,target_cycles=a.cycles,print_stream_index=print_index,decoded_records=len(decoded),
        explicit_acks=a.explicit_acks,coalesced_peeks=a.coalesced_peeks,extra_physical_reads=a.extra_physical_reads,
        actual_count_mcr=a.explicit_acks,count_address=count_address,partial_ack_controls=a.partial_ack_controls,
        actual_production_route_checks=route_checks,
        expected_records=len(expected),decoded_match=decoded_match,first_records=decoded[:4],
        normal_bytes=(out/'run.bin').stat().st_size,flush_bytes=(out/'flush.bin').stat().st_size,
        unchanged=hashlib.sha256(a.rtl.read_bytes()).hexdigest()==original)
    if a.aligned_peeks:
        normal=(out/'run.bin').read_bytes()
        result['retained_head_signature']=(len(normal)>=128 and all(normal[i:i+64]==normal[:64] for i in range(0,len(normal),64))
            and len(decoded)>=2 and all(row['value']==0 for row in decoded)
            and all(decoded[i]['cycle']-decoded[i-1]['cycle']==65585 for i in range(1,len(decoded))))
        result['physical_address_alignment_confirmed_on_hardware']=False
    if a.sfc_cpu_rtl:
        result['sfc_cpu_rtl']=dict(path=str(a.sfc_cpu_rtl.resolve()),sha256=sfc_original)
        result['unchanged']&=hashlib.sha256(a.sfc_cpu_rtl.read_bytes()).hexdigest()==sfc_original
    if a.narrow_peeks:
        protocol=[repo/'sims/firesim/sim/midas/src/main/cc/simif_f2.cc',
            repo/'sims/firesim/platforms/f2/aws-fpga-firesim-f2/hdk/common/verif/models/sh_bfm/sh_bfm.sv',
            repo/'sims/firesim/platforms/f2/aws-fpga-firesim-f2/hdk/docs/AWS_Shell_Interface_Specification.md']
        result['host_protocol_sources']=[dict(path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for path in protocol]
        result['host_protocol_scope']='F2SDK32-bitBAR4reads; PCIID0x20,len0,lane(addrmod64)/4; size2orBFM-normalized6; independentextensionoracleoutsideSFC64Bbulk-equivalenceclaim'
        if a.explicit_acks:
            result['host_protocol_scope']='F2 software-owned stream head: physical PCI reads have no dequeue side effects; actual CountBank OCLwrite1 ACK after complete host64Bcopy; aligned/coalesced/redundant reads; separate from unchanged full-widthDMA/SFC oracle'
    result['passed']&=result['unchanged'];(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2));return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
