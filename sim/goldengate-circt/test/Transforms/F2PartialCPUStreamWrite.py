#!/usr/bin/env python3
"""Execute an actual production-emitted CPU write helper under legal WC fragments.

--expect-old-bug retains the original doubled-token failure with assertions off.
The normal mode enables assertions and requires exactly one assembled stream beat.
No SFC oracle, input RTL or previously archived result is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

DRIVER = r'''
#include "VGGCPUStreamWrite.h"
#include "verilated.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
static void edge(VGGCPUStreamWrite &d){d.clock=0;d.eval();d.clock=1;d.eval();}
int main(int argc,char **argv){
 Verilated::commandArgs(argc,argv);bool old=argc>1&&std::string(argv[1])=="old";
 VGGCPUStreamWrite d;d.reset=1;d.aw_valid=0;d.w_valid=0;d.b_ready=1;d.stream_ready=1;edge(d);d.reset=0;
 d.aw_bits_addr=0;d.aw_bits_id=9;d.aw_bits_len=0;d.aw_bits_size=6;
 unsigned emitted=0,physical=0;
 for(unsigned line=0;line<(old?1:128);line++){
  unsigned parts=line%3==0?2:line%3==1?16:64;
  for(unsigned part=0;part<parts;part++){
   unsigned bytes=64/parts,low=part*bytes;uint64_t mask=(~uint64_t(0)>>(64-bytes))<<low;
   for(unsigned word=0;word<16;word++)d.w_bits_data[word]=0xdeadbeef;
   for(unsigned byte=low;byte<low+bytes;byte++){
    unsigned word=byte/4,shift=8*(byte%4);unsigned value=(line*17+byte*7+3)&255;
    d.w_bits_data[word]=(d.w_bits_data[word]&~(255u<<shift))|(value<<shift);
   }
   d.aw_bits_addr=line*64+low;d.aw_valid=1;d.w_valid=1;d.w_bits_strb=mask;
   if(!old){
    // An incomplete physical fragment does not depend on stream readiness.
    d.stream_ready=0;d.clock=0;d.eval();
    bool final=part+1==parts;
    assert(bool(d.stream_valid)==final);
    assert(bool(d.w_ready)==!final);
    if(final){assert(!d.aw_ready&&!d.b_valid);}
    d.stream_ready=1;
   }
   d.clock=0;d.eval();assert(d.aw_ready&&d.w_ready&&d.b_valid&&d.b_bits_resp==0&&d.b_bits_id==9);
   if(d.stream_valid&&d.stream_ready){
    emitted++;
    if(!old)for(unsigned byte=0;byte<64;byte++)assert(((d.stream_bits[byte/4]>>(8*(byte%4)))&255)==((line*17+byte*7+3)&255));
   }
   physical++;edge(d);d.aw_valid=0;d.w_valid=0;edge(d);
  }
 }
 assert(emitted==(old?2:128));
 std::printf("physical_writes=%u logical_beats=%u expected=%s\n",physical,emitted,old?"old-doubling-reproduced":"exactly-once-byte-assembly");
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rtl',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--verilator',default='verilator')
    p.add_argument('--expect-old-bug',action='store_true')
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    rtl=a.rtl.resolve();source=rtl.read_bytes()
    match=re.search(r'^module GGCPUStreamWrite\(.*?^endmodule\n',source.decode(),re.M|re.S)
    if not match:raise ValueError('actual emitted CPU write helper missing')
    driver=DRIVER
    if 'stream_0_ready' in match.group():
        # The fixture has two independently assembled sinks; exercise sink0
        # here. The SSA unit separately interleaves both sinks byte by byte.
        for field in ('ready','valid','bits'):
            driver=driver.replace('d.stream_'+field,'d.stream_0_'+field)
    (out/'actual.sv').write_text(match.group());(out/'driver.cpp').write_text(driver)
    argv=[a.verilator,'--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','GGCPUStreamWrite','--Mdir',str(out/'obj'),str(out/'actual.sv'),str(out/'driver.cpp')]
    if not a.expect_old_bug:argv.append('--assert')
    with (out/'build.log').open('w') as log:subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,check=True)
    run=[str(out/'obj/VGGCPUStreamWrite')]+(['old'] if a.expect_old_bug else [])
    result=subprocess.run(run,capture_output=True,text=True,timeout=60)
    (out/'stdout.log').write_text(result.stdout);(out/'stderr.log').write_text(result.stderr)
    receipt=dict(passed=result.returncode==0,expected_old_failure_reproduced=a.expect_old_bug,
        rtl=dict(path=str(rtl),sha256=hashlib.sha256(source).hexdigest()),build=argv,run=run,
        returncode=result.returncode,stdout=result.stdout,stderr=result.stderr,
        scope='Actual emitted CPU write helper, sink0; legal two-half/sixteen-DWORD/64-byte-lane fragments and completion stalls. Synthetic PCIS input, no physical FPGA acceptance.')
    (out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2));return result.returncode
if __name__=='__main__':raise SystemExit(main())
