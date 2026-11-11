#!/usr/bin/env python3
"""Simulate generated native assertion hardware against an independent token model."""
import argparse
from pathlib import Path
import subprocess

DRIVER = r'''
#include "VAssertion.h"
#include "verilated.h"
#include <deque>
#include <cstdio>
#include <stdexcept>
#include <cstdint>
int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv); VAssertion d;
  struct Token {unsigned bits; bool reset;}; std::deque<Token> q;
  bool enable=false,resume=false; uint64_t cycles=0;
  unsigned stalls=0,pops=0,resumes=0,masked=0,disabled=0;
  uint32_t rng=0x769a3b21;
  auto next=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
  for(unsigned tick=0;tick<6000;++tick) {
    d.hostClock=0;d.hostReset=tick<3||tick%503==0;
    unsigned r=next();d.hValid=(r&7)!=0;d.fromHostReady=(r>>3)&1;
    d.hBits_asserts=(r>>4)&7;d.hBits_globalReset=(r%13)==0;
    @DEFAULTS@
    d.mcr_wstrb=15;
    bool we=tick%67==3,wr=tick%29==11;unsigned en=(tick/67)%4!=0;
    d.mcr_write_5_valid=we;d.mcr_write_5_bits=en;
    d.mcr_write_4_valid=wr;d.mcr_write_4_bits=1;
    d.eval();
    bool fail=!q.empty()&&q.front().bits&&!q.front().reset;
    bool valid=!q.empty(),ready=q.size()<2;
    unsigned id=0;if(valid&&q.front().bits){while(!(q.front().bits&(1u<<id)))++id;}
    if(d.hReady!=ready||!d.fromHostValid||d.mcr_read_1_bits!=fail||
       d.mcr_read_2_bits!=uint32_t(cycles)||d.mcr_read_3_bits!=uint32_t(cycles>>32)||
       d.mcr_read_5_bits!=enable||(valid&&d.mcr_read_0_bits!=id)) {
      std::fprintf(stderr,"mismatch tick=%u queue=%zu enable=%u resume=%u fire=%u expected=%u cycle=%llu/%u\n",tick,q.size(),enable,resume,unsigned(d.mcr_read_1_bits),fail,(unsigned long long)cycles,unsigned(d.mcr_read_2_bits));return 1;
    }
    bool pop=valid&&(!fail||resume||!enable),push=d.hValid&&ready;
    if(valid&&fail&&enable&&!resume)++stalls;
    if(valid&&fail&&resume)++resumes;
    if(valid&&q.front().bits&&q.front().reset)++masked;
    if(valid&&fail&&!enable)++disabled;
    Token incoming{unsigned(d.hBits_asserts),bool(d.hBits_globalReset)};
    if(pop){q.pop_front();++cycles;++pops;}
    if(push)q.push_back(incoming);
    enable=we?en:enable;resume=wr;
    if(d.hostReset){q.clear();enable=false;resume=false;cycles=0;}
    d.hostClock=1;d.eval();
  }
  if(stalls<100||pops<100||resumes<5||masked<5||disabled<5)throw std::runtime_error("missing behavioral coverage");
  std::printf("PASS assertion RTL queue/backpressure, priority, global/host reset, enable/resume and cycles (%u stalls, %u pops, %u resumes, %u masked, %u disabled)\n",stalls,pops,resumes,masked,disabled);
}
'''

def main():
    p=argparse.ArgumentParser();p.add_argument('fixture',type=Path);p.add_argument('evidence',type=Path)
    p.add_argument('--firtool',required=True,type=Path);p.add_argument('--verilator',required=True,type=Path)
    p.add_argument('--cxx',default='c++');p.add_argument('--runtime-include',type=Path)
    a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
    def run(cmd,log):
        result=subprocess.run([str(x) for x in cmd],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
        (out/(log+'.log')).write_text(result.stdout)
        if result.returncode:raise RuntimeError(result.stdout)
        return result.stdout
    mlir=out/'assertion.mlir';sv=out/'assertion.sv';cpp=out/'driver.cpp';obj=out/'obj'
    header=out/'assertion.const.h'
    run([a.fixture.resolve(),mlir,header],'fixture')
    if a.runtime_include:
        check=out/'header.cpp'
        check.write_text('#include <cstddef>\n#include <cstdint>\n#include <vector>\n#include <string>\n#define GET_INCLUDES\n#include "assertion.const.h"\n#undef GET_INCLUDES\n#define GET_SUBSTRUCT_CHECKS\n#include "assertion.const.h"\n#undef GET_SUBSTRUCT_CHECKS\nstruct Registry { void add_widget(bridge_driver_t*); };\nvoid create(simif_t& simif, Registry& registry, const std::vector<std::string>& args) {\n#define GET_BRIDGE_CONSTRUCTOR\n#include "assertion.const.h"\n}\n')
        run([a.cxx,'-std=c++17','-fsyntax-only','-I',a.runtime_include.resolve(),check],'runtime-header')
    run([a.firtool.resolve(),mlir,'--disable-all-randomization','--disable-annotation-unknown','-o',sv],'firtool')
    defaults=[]
    for i in range(6):
        defaults += [f'd.mcr_read_{i}_ready=0;',f'd.mcr_write_{i}_valid=0;',f'd.mcr_write_{i}_bits=0;']
    cpp.write_text(DRIVER.replace('@DEFAULTS@','\n'.join(defaults)))
    run([a.verilator.resolve(),'--cc','--exe','--build','-j','4','-Wno-fatal','--assert','--top-module','GGAssertionBridgeBank','--prefix','VAssertion','--Mdir',obj,sv,cpp],'verilator')
    print(run([obj/'VAssertion'],'runtime'),end='')
if __name__=='__main__':main()
