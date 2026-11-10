#!/usr/bin/env python3
"""Compare actual two-widget native decoded NIC wrapper to archived SFC state.

This is a supporting component comparison, not an F2 Linux NIC receipt.
The original six-register SFC clocked cone is instantiated independently twice;
only its existing decoder and done sample inputs become test pins.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from PrintControlResponsesCompare import clean, module
from NICMMIOBankCompare import SIGNALS


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('compose-tool', 'golden', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    commands = []
    def run(cmd, name):
        commands.append(list(map(str, cmd)))
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(commands[-1], stdout=log, stderr=subprocess.STDOUT, check=True)
    run([a.compose_tool, out / 'wrapper.mlir'], 'compose')
    run(['firtool', out / 'wrapper.mlir', '--format=mlir', '--disable-all-randomization',
         '--disable-annotation-unknown', '-o', out / 'native.sv'], 'firtool')
    _, body = module(clean(a.golden.read_text()), 'SimpleNICBridgeModule')
    inputs = [('clock', 1), ('reset', 1), ('nic_done', 1), ('mcr_wstrb', 4)]
    for i in range(6): inputs += [(f'mcr_write_{i}_valid', 1), (f'mcr_write_{i}_bits', 32), (f'mcr_read_{i}_ready', 1)]
    payloads = SIGNALS + [(f'mcr_read_{i}_bits', 32) for i in range(6)]
    controls = [(f'mcr_read_{i}_valid', 1) for i in range(6)] + [(f'mcr_write_{i}_ready', 1) for i in range(6)]
    decl = 'module SFCNICRegisters(' + ','.join(f'{d} [{w-1}:0] {n}' for d, ps in [('input', inputs), ('output', payloads)] for n, w in ps) + ');'
    lines = [l for l in body.splitlines() if re.match(r'\s*reg (?:\[31:0\] )?(?:macAddrRegUpper|macAddrRegLower|rlimitSettings|pauseThreshold|pauseTimes| done)\s*;', l)
             or 'wire [63:0] _hPort_hBits_nic_macAddr_T =' in l or 'wire [31:0] _GEN_5 =' in l
             or re.match(r'\s*assign hPort_hBits_nic_(?:macAddr|rlimit_|pauser_)', l)
             or re.match(r'\s*assign crFile_io_mcr_read_[0-5]_bits =', l)]
    assert sum(l.strip().startswith('reg ') for l in lines) == 6
    assert sum(l.strip().startswith('assign ') for l in lines) == 13
    start = body.index('  always @(posedge clock)')
    clocked = body[start:body.index('\n`ifdef RANDOMIZE', start)]
    assert sum(f'~crFile_io_mcr_read_{i}_ready' in clocked for i in range(5)) == 5
    ref = decl + '\n' + '\n'.join(lines) + '\n' + clocked + '\nendmodule\n'
    ref = ref.replace('crFile_io_', '').replace('hPort_hBits_', '').replace("{{31'd0}, _T}", "{{31'd0}, nic_done}")
    (out / 'sfc-registers.sv').write_text(ref)
    test = ['module check;', 'reg clock=0,reset=0;']
    for j in range(2):
        test += [f'reg [{w-1}:0] w{j}_{n}=0;' for n, w in inputs if n not in ('clock', 'reset')]
        test += [f'wire [{w-1}:0] {pre}{j}_{n};' for pre in ('n','s') for n,w in payloads]
        test += [f'wire w{j}_{n};' for n,w in controls]
        test += [f'reg [31:0] expected{j}_{i}=0;' for i in range(6)]
        bindings = [f'.{n}({n if n in ("clock","reset") else ("s" if (n,w) in payloads else "w") + str(j) + "_" + n})' for n,w in inputs+payloads]
        test.append(f'SFCNICRegisters s{j}(' + ','.join(bindings) + ');')
    bindings = ['.hostClock(clock)', '.hostReset(reset)']
    for j in range(2):
        bindings += [f'.done{j}_input(w{j}_nic_done)']
        bindings += [f'.observed{j}_{n[4:]}(n{j}_{n})' for n,w in SIGNALS]
        bindings += [f'.nicBridge_{j}_{n}(w{j}_{n})' for n,w in inputs+controls if n.startswith('mcr_')]
        bindings += [f'.nicBridge_{j}_{n}(n{j}_{n})' for n,w in payloads if n.startswith('mcr_')]
    test += ['GGNICMMIOWrapper native(' + ','.join(bindings) + ');', 'integer cycles=0,independentWrites=0,resetWrites=0,zeroStrobeWrites=0;',
             'task tick;begin clock=0;#2;clock=1;#2;clock=0;#2;cycles++;']
    for j in range(2):
        for i in range(6):
            value = f'w{j}_mcr_write_{i}_bits' if i < 5 else f'(w{j}_mcr_write_5_bits & 1)'
            test += [f'if(w{j}_mcr_write_{i}_valid) expected{j}_{i}={value};' + (f'else expected{j}_{i}=w{j}_nic_done;' if i==5 else '')]
            expected_read = f"expected{j}_{i}" if i==5 else "32'd0"
            test += [f'if(n{j}_mcr_read_{i}_bits!=={expected_read})$fatal(1,"independent state cycle=%0d widget={j} reg={i}",cycles);']
        expected_signals = [f'{{expected{j}_0[15:0],expected{j}_1}}',
                            f'expected{j}_2[23:16]', f'expected{j}_2[15:8]', f'expected{j}_2[7:0]',
                            f'expected{j}_3[15:0]', f'expected{j}_4[15:0]', f'expected{j}_4[31:16]']
        for (n,w), expected in zip(SIGNALS,expected_signals):
            test += [f'if(n{j}_{n}!=={expected})$fatal(1,"independent config widget={j} {n}");']
        for n,w in payloads:
            test += [f'if(n{j}_{n}!==s{j}_{n})$fatal(1,"SFC cycle=%0d widget={j} {n} native=%h SFC=%h",cycles,n{j}_{n},s{j}_{n});']
        for n,w in controls: test += [f'if(w{j}_{n}!==1)$fatal(1,"constant handshake {j} {n}");']
    test += ['independentWrites+=(w0_mcr_write_0_valid&&!w1_mcr_write_0_valid);',
             'resetWrites+=(reset&&w0_mcr_write_0_valid);zeroStrobeWrites+=(!w0_mcr_wstrb&&w0_mcr_write_0_valid);end endtask', 'task data;begin']
    for j in range(2):
        for i in range(6): test += [f'w{j}_mcr_write_{i}_bits=$urandom;']
    test += ['end endtask','initial begin reset=1;data();']
    for j in range(2):
        for i in range(6): test += [f'w{j}_mcr_write_{i}_valid=1;']
    test += ['tick();for(integer mask=0;mask<64;mask++)for(integer strobes=0;strobes<16;strobes++)for(integer mode=0;mode<4;mode++)begin',
             'reset=mode[0];w0_nic_done=mode[1];w1_nic_done=!mode[1];w0_mcr_wstrb=strobes;w1_mcr_wstrb=~strobes;data();']
    for j in range(2):
        for i in range(6): test += [f'w{j}_mcr_write_{i}_valid={"!" if j else ""}mask[{i}];w{j}_mcr_read_{i}_ready=reset;']
    test += ['tick();end','for(integer trial=0;trial<20000;trial++)begin reset=$urandom;data();']
    for j in range(2):
        test += [f'w{j}_nic_done=$urandom;w{j}_mcr_wstrb=$urandom;']
        for i in range(6): test += [f'w{j}_mcr_write_{i}_valid={"trial%256<128?0:" if j else ""}$urandom;w{j}_mcr_read_{i}_ready={"$urandom" if i==5 else "reset&&($urandom&1)"};']
    test += ['tick();end', 'if(independentWrites<1000||resetWrites<1000||zeroStrobeWrites<100)$fatal(1,"coverage");',
             '$display("PASS two-widget SFC MMIO cycles=%0d independentWrites=%0d resetWrites=%0d zeroStrobeWrites=%0d",cycles,independentWrites,resetWrites,zeroStrobeWrites);$finish;end endmodule']
    (out / 'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-j','2','-o','check',out/'check.sv',out/'native.sv',out/'sfc-registers.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    def artifact(path): return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report = dict(result='matched',golden=artifact(a.golden),native=artifact(out/'native.sv'),
                  compose_tool=artifact(a.compose_tool),comparison_script=artifact(Path(__file__)),commands=commands,
                  simulation=(out/'simulation.log').read_text(),cycles=24097,widgets=2,
                  scope='Two independent actual native decoded MMIO instances versus original archived SFC register cones and independent state scoreboard. Host reset and strobes do not mask register writes. Initial values unspecified until first write.',
                  incomplete=['Nasti/global allocation and headers','compile-time and runtime loopback','full NIC F2 Linux representative','LEC and SEC'])
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report['simulation'])
if __name__=='__main__': main()
