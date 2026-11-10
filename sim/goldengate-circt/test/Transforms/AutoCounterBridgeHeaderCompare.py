#!/usr/bin/env python3
# See LICENSE for license details.
"""Compile and compare native/SFC constructors using the unchanged driver ABI.

Only constructor behavior is intercepted to print its received metadata. This
checks the actual C++ types, static struct checks, AddressMap permissions, widget
indices, counter order/widths/addresses and rational clocks. No MMIO or feature
runtime claim is made by this constructor boundary test.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

DRIVER = r'''
#include "bridges/autocounter.h"
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <cstdlib>
widget_t::widget_t(simif_t &s,const void *k):simif(s),kind(k){}
widget_t::~widget_t()=default;
simif_t::simif_t(const TargetConfig &c):config(c){}
simif_t::~simif_t()=default;
CPUManagedStreamIO &simif_t::get_cpu_managed_stream_io(){std::abort();}
FPGAManagedStreamIO &simif_t::get_fpga_managed_stream_io(){std::abort();}
int simif_t::run(simulation_t &){std::abort();}
AddressMap::AddressMap(const std::vector<std::pair<std::string,uint32_t>> &r,const std::vector<std::pair<std::string,uint32_t>> &w):r_registers(r.begin(),r.end()),w_registers(w.begin(),w.end()){}
char autocounter_t::KIND;
autocounter_t::autocounter_t(simif_t &s,AddressMap &&a,const AUTOCOUNTERBRIDGEMODULE_struct &m,unsigned i,const std::vector<std::string>&args,const std::vector<Counter>&c,const ClockInfo &k):bridge_driver_t(s,&KIND),mmio_addrs(m),addr_map(std::move(a)),counters(c),clock_info(k){
 std::cout<<"widget "<<i<<" args="<<args.size()<<" clock="<<std::quoted(k.domain_name)<<":"<<k.multiplier<<":"<<k.divisor<<"\n";
 for(auto &r:addr_map.r_registers)std::cout<<"read "<<std::quoted(r.first)<<" "<<r.second<<"\n";
 for(auto &r:addr_map.w_registers)std::cout<<"write "<<std::quoted(r.first)<<" "<<r.second<<"\n";
 std::cout<<"fixed "<<m.cycles_low<<" "<<m.cycles_high<<" "<<m.readrate_low<<" "<<m.readrate_high<<" "<<m.init_done<<" "<<m.countersready<<" "<<m.readdone<<"\n";
 for(auto &x:c)std::cout<<"counter "<<std::quoted(x.type)<<" "<<std::quoted(x.event_label)<<" "<<std::quoted(x.event_msg)<<" "<<x.bit_width<<" "<<x.accumulator_width<<" "<<x.event_addr_hi<<" "<<x.event_addr_lo<<"\n";
}
autocounter_t::~autocounter_t()=default;
void autocounter_t::init(){}void autocounter_t::tick(){}void autocounter_t::finish(){}
#define GET_SUBSTRUCT_CHECKS
@HEADERS@
#undef GET_SUBSTRUCT_CHECKS
struct DummySim:simif_t{DummySim(const TargetConfig &c):simif_t(c){}void write(size_t,uint32_t)override{}uint32_t read(size_t)override{return 0;}};
struct Registry{void add_widget(autocounter_t*w){delete w;}};
int main(){TargetConfig cfg{};DummySim simif(cfg);Registry registry;std::vector<std::string>args{"checked-abi"};
#define GET_BRIDGE_CONSTRUCTOR
@HEADERS@
#undef GET_BRIDGE_CONSTRUCTOR
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('native',type=Path)
    p.add_argument('sfc',type=Path,nargs='+')
    p.add_argument('--driver-include',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    commands=[];files=[];traces=[]
    for kind,headers in [('native',[a.native]),('sfc',a.sfc)]:
        source=out/(kind+'.cpp');binary=out/kind;trace=out/(kind+'.txt')
        include='\n'.join('#include '+json.dumps(str(h.resolve())) for h in headers)
        source.write_text(DRIVER.replace('@HEADERS@',include));files += headers+[source]
        cmd=['c++','-std=c++20','-Wall','-Werror','-I',str(a.driver_include.resolve()),str(source),'-o',str(binary)]
        commands.append(cmd)
        with (out/(kind+'-build.log')).open('w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
        commands.append([str(binary)])
        trace.write_bytes(subprocess.check_output([str(binary)]));traces.append(trace);files.append(trace)
    assert traces[0].read_bytes()==traces[1].read_bytes(),'actual C++ constructor metadata differs'
    # A corrupted live event address must change this comparison, even with all
    # struct checks, names, permissions and clocks otherwise intact.
    mutated=out/'corrupt.h';text=a.native.read_text();needle='.event_addr_hi = 4U';assert needle in text
    mutated.write_text(text.replace(needle,'.event_addr_hi = 8U',1))
    source=out/'corrupt.cpp';source.write_text(DRIVER.replace('@HEADERS@','#include '+json.dumps(str(mutated))))
    cmd=['c++','-std=c++20','-Wall','-Werror','-I',str(a.driver_include.resolve()),str(source),'-o',str(out/'corrupt')];commands.append(cmd)
    with (out/'corrupt-build.log').open('w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
    commands.append([str(out/'corrupt')]);assert subprocess.check_output([str(out/'corrupt')])!=traces[1].read_bytes(),'event address corruption control did not fail'
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    files += [Path(__file__),out/'commands.json',mutated,source]
    files += [a.driver_include / p for p in ['bridges/autocounter.h','core/address_map.h','core/clock_info.h','core/widget.h','core/simif.h']]
    receipt=dict(passed=True,full_feature_acceptance=False,scope='actual constructor C++ syntax and semantic ABI; explicit fixture allocation only',widgets=sum(line.startswith("widget ") for line in traces[0].read_text().splitlines()),semantic_rows=len(traces[0].read_text().splitlines()),corruption_control=True,sha256={str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest() for f in files})
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS actual driver header ABI',receipt['widgets'],'widgets',receipt['semantic_rows'],'rows')

if __name__=='__main__':main()
