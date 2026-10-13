#!/usr/bin/env python3
"""Standalone emitted-RTL differential against the archived SFC NIC adapters."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture', type=Path, required=True)
    p.add_argument('--golden', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    a = p.parse_args()
    out = a.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    run([a.fixture.resolve(), out / 'adapters.mlir'], 'fixture')
    run(['firtool', out / 'adapters.mlir', '--format=mlir', '--disable-all-randomization', '-o', out / 'native.sv'], 'firtool')
    names = ['BigTokenToNICTokenAdapter', 'NICTokenToBigTokenAdapter']
    source = a.golden.read_text()
    modules = []
    for name in names:
        match = re.search(r'^module ' + name + r'\(.*?^endmodule', source, re.M | re.S)
        if not match:
            raise ValueError('missing SFC ' + name)
        modules.append(match.group(0))
    (out / 'sfc.sv').write_text('\n\n'.join(modules) + '\n')
    (out / 'check.sv').write_text(r'''
module check;
  reg clock=0,reset=1,dvalid=0,dready=0,evalid=0,eready=0;
  reg [511:0] dbits=0;
  reg [65:0] ebits=0;
  wire dnready,dnvalid,dsready,dsvalid,enready,envalid,esready,esvalid;
  wire [65:0] dnbits,dsbits;
  wire [511:0] enbits,esbits;
  GGNICBigTokenDecoder dec(.clock(clock),.reset(reset),.in_ready(dnready),.in_valid(dvalid),.in_bits(dbits),
    .out_ready(dready),.out_valid(dnvalid),.out_bits(dnbits));
  BigTokenToNICTokenAdapter sdec(.clock(clock),.reset(reset),.io_htnt_ready(dready),.io_htnt_valid(dsvalid),
    .io_htnt_bits_data_in_data(dsbits[63:0]),.io_htnt_bits_data_in_last(dsbits[64]),.io_htnt_bits_data_in_valid(dsbits[65]),
    .io_pcie_in_ready(dsready),.io_pcie_in_valid(dvalid),.io_pcie_in_bits(dbits));
  GGNICBigTokenEncoder enc(.clock(clock),.reset(reset),.in_ready(enready),.in_valid(evalid),.in_bits(ebits),
    .out_ready(eready),.out_valid(envalid),.out_bits(enbits));
  NICTokenToBigTokenAdapter senc(.clock(clock),.reset(reset),.io_ntht_ready(esready),.io_ntht_valid(evalid),
    .io_ntht_bits_data_out_data(ebits[63:0]),.io_ntht_bits_data_out_last(ebits[64]),.io_ntht_bits_data_out_valid(ebits[65]),
    .io_ntht_bits_data_in_ready(1'b1),.io_pcie_out_ready(eready),.io_pcie_out_valid(esvalid),.io_pcie_out_bits(esbits));
  // Independent packet-format and transaction scoreboards. Incomplete packets
  // are abandoned on reset; buffer storage survives reset, as in the oracle.
  integer di=0,ei=0,decoded=0,encoded=0,ds=0,es=0,resets=0,bypasses=0,bubbles=0;
  reg [42:0] trace_count=0;
  reg [65:0] buffer[0:5];
  reg [511:0] expected;
  reg [65:0] expected_token,t;
  reg dconsume,econsume,packetconsume;
  task new_packet;
    for(integer j=0;j<16;j=j+1) dbits[j*32+:32]=$urandom;
  endtask
  task new_token;
    ebits={2'($urandom),$urandom,$urandom};
  endtask
  initial begin
    #2;clock=1;#2;clock=0;reset=0;new_packet();new_token();
    for(integer cycle=0;cycle<40000;cycle=cycle+1) begin
      // Directed seventh-token backpressure, then independent stalls/bubbles.
      dvalid=cycle<100 ? 1 : ($urandom%5)!=0;
      evalid=cycle<100 ? 1 : ($urandom%5)!=0;
      dready=cycle<30 ? 1 : cycle<60 ? 0 : ($urandom%3)!=0;
      eready=cycle<30 ? 0 : cycle<100 ? 1 : ($urandom%3)!=0;
      reset=cycle>=100 && cycle%113==0;
      #2;
      expected_token={dbits[43+di*3],dbits[45+di*3],dbits[64+di*64+:64]};
      if(dnready !== dsready || dnready !== (dready&&di==6) || dnvalid !== dsvalid || dnvalid !== dvalid)
        $fatal(1,"decoder handshake cycle %0d",cycle);
      if(dnbits !== dsbits || dnbits !== expected_token) $fatal(1,"decoder packet slice cycle %0d",cycle);
      if(enready !== esready || enready !== (ei!=6||eready) || envalid !== esvalid || envalid !== (ei==6&&evalid))
        $fatal(1,"encoder handshake cycle %0d",cycle);
      expected=0;expected[42:0]=trace_count;
      for(integer j=0;j<7;j=j+1) begin
        t=j==6?ebits:buffer[j];
        expected[64+j*64+:64]=t[63:0];
        expected[43+j*3+:3]={t[64],1'b1,t[65]};
      end
      if(envalid && (enbits !== esbits || enbits !== expected)) $fatal(1,"encoder packet layout/trace cycle %0d",cycle);
      dconsume=dvalid&&dready;econsume=evalid&&enready;packetconsume=dvalid&&dnready;
      ds+=dvalid&&!dready;es+=envalid&&!eready;bubbles+=!dvalid||!evalid;
      bypasses+=envalid&&eready;resets+=reset;
      if(evalid&&ei<6)buffer[ei]=ebits;
      if(dconsume)begin decoded++;di=(di+1)%7;end
      if(econsume)begin encoded++;ei=(ei+1)%7;end
      if(envalid&&eready)trace_count=trace_count+1;
      if(reset)begin di=0;ei=0;trace_count=0;end
      clock=1;#2;clock=0;
      // Producers hold payload across backpressure; an input packet stays
      // present until all seven decoder token transactions have completed.
      if(packetconsume || reset)new_packet();
      if(econsume||reset)new_token();
    end
    if(decoded<15000||encoded<15000||ds<1000||es<500||bypasses<2000||resets<300||bubbles<1000)$fatal(1,"coverage");
    $display("PASS SFC NIC packet adapters: 40000 cycles decoded=%0d encoded=%0d decode_stall=%0d seventh_stall=%0d bypass=%0d reset=%0d bubbles=%0d",
      decoded,encoded,ds,es,bypasses,resets,bubbles);$finish;
  end
endmodule
''')
    run(['verilator', '--binary', '--timing', '--top-module', 'check', '-Wno-fatal', '--Mdir', out / 'obj', '-o', 'check', '-j', '2',
         out / 'native.sv', out / 'sfc.sv', out / 'check.sv'], 'verilator-build')
    run([out / 'obj/check'], 'behavior')
    result = {'golden': str(a.golden.resolve()), 'modules': names, 'cycles': 40000,
              'result': 'matched', 'behavior': (out / 'behavior.log').read_text()}
    (out / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
    print(result['behavior'], end='')


if __name__ == '__main__':
    main()
