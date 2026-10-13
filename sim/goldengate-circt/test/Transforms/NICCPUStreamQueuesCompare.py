#!/usr/bin/env python3
"""Compare native 3072x512 FIFO RTL with the SFC NIC CPU stream queues.

Standalone compiler-unit Verilator simulation; no FireSim manager execution.
Both directions receive independent stalls/resets. A transaction scoreboard
supplements direct SFC ready/valid/count/payload comparisons.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--golden', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    run([args.fixture.resolve(), out / 'queues.mlir'], 'fixture')
    run(['firtool', out / 'queues.mlir', '--format=mlir', '--disable-all-randomization',
         '-o', out / 'native.sv'], 'firtool')
    source = args.golden.read_text()
    def module(name):
        found = re.search(r'^module ' + name + r'\(.*?^endmodule', source, re.M | re.S)
        if not found:
            raise ValueError('missing golden module ' + name)
        return found[0]
    engine = module('CPUManagedStreamEngine')
    for name, stream in [('Queue_52', 'to_cpu_stream_outgoingQueueIO_q'),
                         ('Queue_54', 'from_cpu_stream_incomingQueueIO_q')]:
        assert name + ' SIMPLENICBRIDGEMODULE_0_' + stream in engine
        body = module(name)
        assert 'reg [511:0] ram [0:3071]' in body and 'output [11:0]  io_count' in body
    (out / 'sfc.sv').write_text(module('Queue_52') + '\n' + module('Queue_54') + '\n')
    (out / 'check.sv').write_text(r'''
module check;
  reg clock=0;
  reg [1:0] reset=3, enq_valid=0, deq_ready=0;
  reg [511:0] enq_bits[0:1];
  wire [1:0] nready,nvalid,sready,svalid;
  wire [511:0] nbits[0:1],sbits[0:1];
  wire [11:0] ncount[0:1],scount[0:1];
  for(genvar g=0;g<2;g=g+1) begin : q
    GGNICCPUStreamQueue3072 nativeq(.clock(clock),.reset(reset[g]),.enq_ready(nready[g]),
      .enq_valid(enq_valid[g]),.enq_bits(enq_bits[g]),.deq_ready(deq_ready[g]),
      .deq_valid(nvalid[g]),.deq_bits(nbits[g]),.count(ncount[g]));
  end
  Queue_54 incoming(.clock(clock),.reset(reset[0]),.io_enq_ready(sready[0]),.io_enq_valid(enq_valid[0]),
    .io_enq_bits(enq_bits[0]),.io_deq_ready(deq_ready[0]),.io_deq_valid(svalid[0]),.io_deq_bits(sbits[0]),.io_count(scount[0]));
  Queue_52 outgoing(.clock(clock),.reset(reset[1]),.io_enq_ready(sready[1]),.io_enq_valid(enq_valid[1]),
    .io_enq_bits(enq_bits[1]),.io_deq_ready(deq_ready[1]),.io_deq_valid(svalid[1]),.io_deq_bits(sbits[1]),.io_count(scount[1]));
  // Independent ring scoreboards contain only accepted transactions.
  reg [511:0] expected[0:1][0:3071];
  integer head[0:1],tail[0:1],count[0:1];
  integer fullCycles=0,emptyCycles=0,duals=0,resets=0,pushes=0,pops=0,wraps=0,resetWrites=0;
  integer fullPops=0,emptyPushes=0;
  reg push,pop;
  initial begin
    for(integer j=0;j<2;j=j+1)begin head[j]=0;tail[j]=0;count[j]=0;enq_bits[j]=0;end
    #2;clock=1;#2;clock=0;reset=0;
    for(integer cycle=0;cycle<40000;cycle=cycle+1)begin
      for(integer j=0;j<2;j=j+1)begin
        for(integer word=0;word<16;word=word+1)enq_bits[j][word*32+:32]=$urandom;
        if(j==0)begin
          enq_valid[j]=cycle<3101 || (cycle>=6201 && cycle<21001);
          deq_ready[j]=(cycle>=3100 && cycle<6201) || (cycle>=7801 && cycle<25001);
        end else begin
          enq_valid[j]=(cycle>=3101 && cycle<6301) || (cycle>=9401 && cycle<24001);
          deq_ready[j]=cycle<3101 || (cycle>=6201 && cycle<9401) || (cycle>=11001 && cycle<25001);
        end
        reset[j]=0;
        if(cycle>=25001)begin
          enq_valid[j]=($urandom%4)!=0;deq_ready[j]=($urandom%4)!=0;
          reset[j]=cycle%(j==0?113:127)==0;
        end
      end
      #2;
      for(integer j=0;j<2;j=j+1)begin
        if(nready[j] !== sready[j] || nready[j] !== (count[j]<3072))$fatal(1,"ready %d/%d",cycle,j);
        if(nvalid[j] !== svalid[j] || nvalid[j] !== (count[j]>0))$fatal(1,"valid %d/%d",cycle,j);
        if(ncount[j] !== scount[j] || ncount[j] !== 12'(count[j]))$fatal(1,"count %d/%d",cycle,j);
        // Invalid payload can retain RAM data; direct SFC comparison checks it too.
        if(nbits[j] !== sbits[j])$fatal(1,"RAM lookahead/reset payload %d/%d",cycle,j);
        if(nvalid[j] && nbits[j] !== expected[j][head[j]])$fatal(1,"ordering %d/%d",cycle,j);
        push=nready[j]&&enq_valid[j];pop=nvalid[j]&&deq_ready[j];
        fullCycles+=count[j]==3072;emptyCycles+=count[j]==0;duals+=push&&pop;
        fullPops+=(count[j]==3072)&&pop&&!push;emptyPushes+=(count[j]==0)&&push&&!pop;
        resets+=reset[j];resetWrites+=reset[j]&&push;pushes+=push;pops+=pop;
        if(push)begin expected[j][tail[j]]=enq_bits[j];wraps+=tail[j]==3071;tail[j]=(tail[j]+1)%3072;end
        if(pop)begin wraps+=head[j]==3071;head[j]=(head[j]+1)%3072;end
        count[j]+=integer'(push)-integer'(pop);
        if(reset[j])begin head[j]=0;tail[j]=0;count[j]=0;end
      end
      clock=1;#2;clock=0;
    end
    if(fullCycles<20 || emptyCycles<20 || duals<20000 || resets<200 || pushes<35000 || pops<35000 ||
      wraps<16 || resetWrites<100 || fullPops<2 || emptyPushes<100)$fatal(1,"coverage");
    $display("PASS NIC CPU SFC Queue_52/Queue_54: 40000 cycles per direction, full=%0d empty=%0d dual=%0d reset=%0d push=%0d pop=%0d wrap=%0d resetWrite=%0d fullPop=%0d emptyPush=%0d",
      fullCycles,emptyCycles,duals,resets,pushes,pops,wraps,resetWrites,fullPops,emptyPushes);
    $finish;
  end
endmodule
''')
    run(['verilator','--binary','--timing','--top-module','check','-Wno-fatal','--Mdir',out/'obj',
         '-o','check','-j','2',out/'native.sv',out/'sfc.sv',out/'check.sv'],'verilator-build')
    run([out/'obj/check'],'behavior')
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result = dict(fixture=str(args.fixture.resolve()),fixture_sha256=digest(args.fixture),
                  native_ir_sha256=digest(out/'queues.mlir'),native_rtl_sha256=digest(out/'native.sv'),
                  golden=str(args.golden.resolve()),
                  golden_sha256=hashlib.sha256(args.golden.read_bytes()).hexdigest(),
                  modules=['Queue_52','Queue_54'],cycles_per_direction=40000,result='matched',
                  behavior=(out/'behavior.log').read_text())
    (out/'comparison.json').write_text(json.dumps(result,indent=2)+'\n')
    print(result['behavior'],end='')


if __name__ == '__main__':
    main()
