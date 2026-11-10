#!/usr/bin/env python3
"""Supporting two-widget emitted-RTL comparison; not F2 feature acceptance.

The queue-boundary fixture loops decoded tokens back into each widget's encoder.
Compare the real native wrapper with two independent compositions of immutable
SFC adapters and CPU queues, plus an end-to-end accepted-packet scoreboard.
This fixture is distinct from SimpleNIC's compile-time/runtime loopback APIs.
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
    commands = []

    def run(command, name):
        command = list(map(str, command))
        commands.append(command)
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    run([args.fixture.resolve(), out / 'native.mlir'], 'fixture')
    run(['firtool', out / 'native.mlir', '--format=mlir', '--disable-all-randomization',
         '--disable-annotation-unknown',
         '-o', out / 'native.sv'], 'firtool')
    source = args.golden.read_text()
    names = ['BigTokenToNICTokenAdapter', 'NICTokenToBigTokenAdapter', 'Queue_52', 'Queue_54']
    bodies = []
    for name in names:
        found = re.search(r'^module ' + name + r'\(.*?^endmodule', source, re.M | re.S)
        if not found:
            raise ValueError('missing immutable SFC module ' + name)
        bodies.append(found[0])
    for body in bodies[2:]:
        assert 'reg [511:0] ram [0:3071]' in body
    (out / 'sfc.sv').write_text('\n\n'.join(bodies) + '\n')
    reference = r'''
module SFCNormalNICPath(input clock,reset,input in_valid,input [511:0] in_bits,
 output in_ready,output out_valid,output [511:0] out_bits,input out_ready,
 output [11:0] in_count,out_count);
 wire qvalid,qready,dvalid,dready,evalid,eready;
 wire [511:0] qbits,ebits;
 wire [65:0] dbits;
 Queue_54 incoming(.clock(clock),.reset(reset),.io_enq_valid(in_valid),.io_enq_ready(in_ready),
   .io_enq_bits(in_bits),.io_deq_valid(qvalid),.io_deq_ready(qready),.io_deq_bits(qbits),.io_count(in_count));
 BigTokenToNICTokenAdapter decoder(.clock(clock),.reset(reset),.io_pcie_in_valid(qvalid),
   .io_pcie_in_ready(qready),.io_pcie_in_bits(qbits),.io_htnt_valid(dvalid),.io_htnt_ready(dready),
   .io_htnt_bits_data_in_data(dbits[63:0]),.io_htnt_bits_data_in_last(dbits[64]),.io_htnt_bits_data_in_valid(dbits[65]));
 NICTokenToBigTokenAdapter encoder(.clock(clock),.reset(reset),.io_ntht_valid(dvalid),.io_ntht_ready(dready),
   .io_ntht_bits_data_out_data(dbits[63:0]),.io_ntht_bits_data_out_last(dbits[64]),.io_ntht_bits_data_out_valid(dbits[65]),
   .io_ntht_bits_data_in_ready(1'b1),.io_pcie_out_valid(evalid),.io_pcie_out_ready(eready),.io_pcie_out_bits(ebits));
 Queue_52 outgoing(.clock(clock),.reset(reset),.io_enq_valid(evalid),.io_enq_ready(eready),
   .io_enq_bits(ebits),.io_deq_valid(out_valid),.io_deq_ready(out_ready),.io_deq_bits(out_bits),.io_count(out_count));
endmodule
module check;
 reg clock=0,reset=1;
 reg [1:0] in_valid=0,out_ready=0,hold_input=0,push_input=0;
 reg [511:0] in_bits[0:1];
 wire [1:0] nr,nv,sr,sv;
 wire [511:0] nb[0:1],sb[0:1];
 wire [11:0] nic[0:1],noc[0:1],sic[0:1],soc[0:1];
 GGNICCPUStreamQueuesWrapper native_path(.hostClock(clock),.hostReset(reset),
'''
    ports = []
    for i in range(2):
        ports += [f'.nic{i}_from_host_stream_valid(in_valid[{i}])',
                  f'.nic{i}_from_host_stream_ready(nr[{i}])',
                  f'.nic{i}_from_host_stream_bits(in_bits[{i}])',
                  f'.nic{i}_to_host_stream_valid(nv[{i}])',
                  f'.nic{i}_to_host_stream_ready(out_ready[{i}])',
                  f'.nic{i}_to_host_stream_bits(nb[{i}])',
                  f'.nic{i}_from_host_stream_count(nic[{i}])',
                  f'.nic{i}_to_host_stream_count(noc[{i}])']
    reference += ',\n'.join(ports) + ');\n'
    reference += r'''
 for(genvar i=0;i<2;i=i+1) begin
   SFCNormalNICPath reference_path(.clock(clock),.reset(reset),.in_valid(in_valid[i]),
     .in_ready(sr[i]),.in_bits(in_bits[i]),.out_valid(sv[i]),.out_ready(out_ready[i]),
     .out_bits(sb[i]),.in_count(sic[i]),.out_count(soc[i]));
 end
 reg [511:0] expected[0:1][0:8191];
 integer head[0:1],tail[0:1],count[0:1],sequence_number[0:1],sent[0:1],received[0:1];
 integer full_cycles[0:1],stalls[0:1],reset_cycles=0;
 reg [511:0] packet;
 initial begin
   for(integer i=0;i<2;i=i+1) begin
     head[i]=0;tail[i]=0;count[i]=0;sequence_number[i]=0;sent[i]=0;received[i]=0;full_cycles[i]=0;stalls[i]=0;in_bits[i]=0;
   end
   #2;clock=1;#2;clock=0;reset=0;
   for(integer cycle=0;cycle<48000;cycle=cycle+1) begin
     reset=(cycle==35000||cycle==35001);
     in_valid[0]=hold_input[0]||cycle<32000?1:($urandom%5)!=0;
     out_ready[0]=cycle<30000?0:($urandom%4)!=0;
     in_valid[1]=hold_input[1]||($urandom%3)!=0;
     out_ready[1]=cycle%29<19;
     #2;
     for(integer i=0;i<2;i=i+1) begin
       if(nr[i]!==sr[i]||nv[i]!==sv[i]||nic[i]!==sic[i]||noc[i]!==soc[i])
         $fatal(1,"widget %0d handshake/count cycle %0d",i,cycle);
       if(nv[i]&&(nb[i]!==sb[i]))$fatal(1,"widget %0d SFC payload cycle %0d",i,cycle);
       if(!reset)begin
         if(nv[i]&&out_ready[i])begin
           if(count[i]==0||nb[i]!==expected[i][head[i]])$fatal(1,"widget %0d independent packet scoreboard cycle %0d",i,cycle);
           head[i]=(head[i]+1)%8192;count[i]--;received[i]++;
         end
         if(in_valid[i]&&nr[i])begin
           if(count[i]>=8192)$fatal(1,"scoreboard capacity");
           packet=in_bits[i];packet[42:0]=43'(sequence_number[i]);
           for(integer j=0;j<7;j=j+1)packet[44+j*3]=1'b1;
           expected[i][tail[i]]=packet;tail[i]=(tail[i]+1)%8192;count[i]++;sequence_number[i]++;sent[i]++;
         end
       end else begin head[i]=0;tail[i]=0;count[i]=0;sequence_number[i]=0;end
       full_cycles[i]+=!nr[i];stalls[i]+=nv[i]&&!out_ready[i];
     end
     hold_input=in_valid&~nr&{2{!reset}};push_input=in_valid&nr;
     reset_cycles+=reset;clock=1;#2;clock=0;
     for(integer i=0;i<2;i=i+1)if(push_input[i]||reset)
       for(integer j=0;j<16;j=j+1)in_bits[i][j*32+:32]=$urandom;
   end
   if(full_cycles[0]<1000||stalls[0]<20000||received[0]<1000||received[1]<5000||sent[1]<5000||reset_cycles!=2)
     $fatal(1,"insufficient saturation/stall/reset coverage");
   $display("PASS two-widget SFC NIC packet/CPU pipeline: 48000 cycles sent=%0d/%0d received=%0d/%0d full=%0d/%0d stalls=%0d/%0d reset=%0d",
     sent[0],sent[1],received[0],received[1],full_cycles[0],full_cycles[1],stalls[0],stalls[1],reset_cycles);$finish;
 end
endmodule
'''
    (out / 'check.sv').write_text(reference)
    run(['verilator', '--binary', '--timing', '--top-module', 'check', '-Wno-fatal',
         '--Mdir', out / 'obj', '-o', 'check', '-j', '2',
         out / 'native.sv', out / 'sfc.sv', out / 'check.sv'], 'verilator-build')
    run([out / 'obj/check'], 'behavior')
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    result = dict(passed=True, scope='Supporting normal-mode packet/CPU queues only; no actual NIC loopback API or F2 feature acceptance',
                  cycles=48000, widgets=2, modules=names, commands=commands,
                  sha256={str(p.resolve()): sha(p) for p in [args.fixture, args.golden, out / 'native.mlir', out / 'native.sv', out / 'sfc.sv', out / 'check.sv']},
                  behavior=(out / 'behavior.log').read_text())
    (out / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print(result['behavior'], end='')


if __name__ == '__main__':
    main()
