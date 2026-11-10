#!/usr/bin/env python3
# See LICENSE for license details.
"""Compile actual mixed SFC/native driver constructors and compare their inputs.
No host MMIO or Linux runtime is inferred from this constructor boundary.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
from AutoCounterBridgeHeaderCompare import DRIVER as CSV_DRIVER
EXTRA=r'''
char synthesized_prints_t::KIND;
synthesized_prints_t::synthesized_prints_t(simif_t&s,StreamEngine&stream,const PRINTBRIDGEMODULE_struct&m,unsigned index,const std::vector<std::string>&args,const std::vector<Print>&records,unsigned bytes,unsigned mask,unsigned stream_index,unsigned depth,const ClockInfo&clock):streaming_bridge_driver_t(s,stream,&KIND),mmio_addrs(m),prints(records),token_bytes(bytes),idle_cycles_mask(mask),stream_idx(stream_index),stream_depth(depth),clock_info(clock),printno(index){
 std::cout<<"print "<<index<<" args="<<args.size()<<" clock="<<std::quoted(clock.domain_name)<<":"<<clock.multiplier<<":"<<clock.divisor<<" token="<<bytes<<":"<<mask<<" stream="<<stream_index<<":"<<depth<<"\n";
 std::cout<<"print-mmio "<<m.startCycleL<<" "<<m.startCycleH<<" "<<m.endCycleL<<" "<<m.endCycleH<<" "<<m.doneInit<<" "<<m.flushNarrowPacket<<"\n";
 for(auto&r:records){std::cout<<"record "<<r.print_offset<<" "<<std::quoted(r.format_string);for(auto w:r.argument_widths)std::cout<<" "<<w;std::cout<<"\n";}
}
synthesized_prints_t::~synthesized_prints_t()=default;
void synthesized_prints_t::init(){}void synthesized_prints_t::tick(){}void synthesized_prints_t::flush(){}
CPUManagedStreamWidget::CPUManagedStreamWidget(simif_t&,unsigned index,const std::vector<std::string>&args,std::vector<CPUManagedStreams::StreamParameters>&&from,std::vector<CPUManagedStreams::StreamParameters>&&to){
 std::cout<<"engine "<<index<<" args="<<args.size()<<"\n";
 for(auto*collection:{&from,&to}){std::cout<<(collection==&from?"incoming":"outgoing")<<" "<<collection->size()<<"\n";for(auto&s:*collection)std::cout<<"stream "<<std::quoted(s.stream_name)<<" "<<s.dma_addr<<" "<<s.count_addr<<" "<<s.fpga_buffer_size<<" "<<s.fpga_buffer_width_bytes<<"\n";}
}
'''
DRIVER=CSV_DRIVER.replace('#include "bridges/autocounter.h"','#include "bridges/autocounter.h"\n#include "bridges/synthesized_prints.h"\n#include "bridges/cpu_managed_stream.h"\n#define GET_INCLUDES\n@HEADERS@\n#undef GET_INCLUDES')
DRIVER=DRIVER.replace('#define GET_SUBSTRUCT_CHECKS',EXTRA+'\n#define GET_SUBSTRUCT_CHECKS')
DRIVER=DRIVER.replace('struct Registry{void add_widget(autocounter_t*w){delete w;}};','struct Registry{StreamEngine stream; StreamEngine*get_stream_engine(){return &stream;} void add_widget(autocounter_t*w){delete w;} void add_widget(synthesized_prints_t*w){delete w;} void add_widget(CPUManagedStreamWidget*w){delete w;}};')
DRIVER=DRIVER.replace('#define GET_BRIDGE_CONSTRUCTOR','#define GET_MANAGED_STREAM_CONSTRUCTOR\n#define GET_BRIDGE_CONSTRUCTOR').replace('#undef GET_BRIDGE_CONSTRUCTOR','#undef GET_BRIDGE_CONSTRUCTOR\n#undef GET_MANAGED_STREAM_CONSTRUCTOR')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('native',type=Path);p.add_argument('sfc',type=Path);p.add_argument('--driver-include',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 commands=[];traces=[];files=[]
 for kind,header in [('native',a.native),('sfc',a.sfc)]:
  source=out/(kind+'.cpp');binary=out/kind;trace=out/(kind+'.txt');source.write_text(DRIVER.replace('@HEADERS@','#include '+json.dumps(str(header.resolve()))))
  cmd=['c++','-std=c++20','-Wall','-Werror','-I',str(a.driver_include.resolve()),str(source),'-o',str(binary)];commands.append(cmd)
  with(out/(kind+'-build.log')).open('w')as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  commands.append([str(binary)]);trace.write_bytes(subprocess.check_output([str(binary)]));traces.append(trace);files += [source,header,trace]
 assert traces[0].read_bytes()==traces[1].read_bytes(),'actual mixed constructor arguments differ'
 lines=traces[0].read_text().splitlines();assert sum(x.startswith('widget ')for x in lines)==2 and sum(x.startswith('print ')for x in lines)==1 and sum(x.startswith('engine ')for x in lines)==1
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');files += [Path(__file__),out/'commands.json']
 receipt=dict(passed=True,full_feature_acceptance=False,scope='complete selected enabled CSV + Print + CPU actual constructor ABI',semantic_rows=len(lines),sha256={str(x.resolve()):hashlib.sha256(x.read_bytes()).hexdigest()for x in files})
 (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS actual mixed driver constructor ABI',len(lines),'rows')
if __name__=='__main__':main()
