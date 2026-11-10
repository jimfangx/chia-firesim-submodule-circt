// See LICENSE for license details.
// Public-port differential driver for FAMETransform-derived virtual controllers.
#include "VTop.h"
#include "verilated.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#ifndef THREADS
#define THREADS 4
#endif
#if THREADS == 2
#define INPUTS(suffix) &top.in0_##suffix, &top.in1_##suffix
#define OUTPUTS(suffix) &top.out0_##suffix, &top.out1_##suffix
#else
#define INPUTS(suffix) &top.in0_##suffix, &top.in1_##suffix, &top.in2_##suffix, &top.in3_##suffix
#define OUTPUTS(suffix) &top.out0_##suffix, &top.out1_##suffix, &top.out2_##suffix, &top.out3_##suffix
#endif
int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv); VTop top;
  const std::string control=argc>1?argv[1]:"";
  std::array<CData*,THREADS> valid{INPUTS(valid)},ready{INPUTS(ready)},bits{INPUTS(bits)};
  std::array<CData*,THREADS> outReady{OUTPUTS(ready)},outValid{OUTPUTS(valid)},outBits{OUTPUTS(bits)};
  std::array<bool,THREADS> inputFired{},outputFired{},enabled{};
  auto member=[](unsigned slot){return slot==0?1u:slot==1?0u:5-slot;};
  auto edge=[&]{top.hostClock=1;top.eval();top.hostClock=0;top.eval();};
  top.hostClock=0;top.hostReset=1;
  for(unsigned i=0;i<THREADS;++i){*valid[i]=*outReady[i]=1;*bits[i]=128;}
  for(unsigned i=0;i<THREADS;++i)edge();
  top.hostReset=0;
  uint64_t checksum=0;unsigned transactions=0,outputTokens=0,stalls=0,resetTokens=0;
  for(unsigned cycle=0;cycle<16384;++cycle){
    unsigned selected=member(cycle%THREADS);
    if(cycle==4096){
      top.hostReset=1;
      for(unsigned i=0;i<THREADS;++i){*valid[i]=*outReady[i]=1;*bits[i]=128;}
      for(unsigned i=0;i<THREADS;++i)edge();
      inputFired.fill(false);outputFired.fill(false);enabled.fill(false);
    }
    const bool reset=false;top.hostReset=0;
    for(unsigned i=0;i<THREADS;++i){
      *valid[i]=cycle<64 || (cycle*3+i*7)%11!=0;
      *outReady[i]=cycle<64 || (cycle/(i+1)+i)%7!=0;
      *bits[i]=cycle<64 || cycle%97<THREADS ? 128 : ((cycle/THREADS*17+i*29)%127);
    }
    top.eval();
    bool finishing=(outputFired[selected] || *outReady[selected] && !outputFired[selected]) && *valid[selected];
    for(unsigned i=0;i<THREADS;++i){
      bool active=i==selected;
      unsigned r=*ready[i] ^ (control=="--corrupt-ready" && cycle==0 && i==0);
      unsigned v=*outValid[i] ^ (control=="--corrupt-valid" && cycle==0 && i==selected);
      if(r!=(active && finishing && !inputFired[selected]) || v!=(active && !outputFired[selected])){
        std::cerr<<"dynamic FAME control mismatch "<<cycle<<' '<<i<<'\n';return 1;
      }
      checksum=checksum*131+r;checksum=checksum*131+v;
      // Target state is reset by ordinary target-reset input transactions. No
      // uninitialized bank data is compared before this bounded startup phase.
      unsigned payload=*outBits[i];
      if(control=="--different-invalid-payload" && !v)payload^=0xa5;
      if(cycle>=8 && cycle<64 && v && payload!=0){
        std::cerr<<"ordinary target reset did not define state "<<cycle<<' '<<i<<'\n';return 1;
      }
      if(cycle>=64 && v){
        if(control=="--corrupt-defined-payload")payload^=1;
        checksum=checksum*131+payload;++outputTokens;
      }
    }
    if(finishing){++transactions;if(*bits[selected]&128)++resetTokens;}else++stalls;
    if(reset){inputFired[selected]=outputFired[selected]=enabled[selected]=false;}
    else{
      enabled[selected]=finishing || enabled[selected];
      inputFired[selected]=finishing ? false : inputFired[selected] || (finishing && !inputFired[selected] && *valid[selected]);
      outputFired[selected]=finishing ? false : outputFired[selected] || (*outReady[selected] && !outputFired[selected]);
    }
    edge();
  }
  if(transactions<1000 || outputTokens<1000 || stalls==0 || resetTokens==0)return 3;
  std::cout<<"DYNAMIC_FAME "<<THREADS<<' '<<transactions<<' '<<outputTokens<<' '<<stalls<<' '<<resetTokens<<' '<<checksum<<'\n';top.final();
}
