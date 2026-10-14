#!/usr/bin/env python3
"""Compare the production NIC decoded MMIO bank with the SFC register cone.

Extract original declarations, assignments and clocked updates from the
archived SimpleNICBridgeModule. Independent decoded MCR pins replace only
the Nasti decoder boundary; NIC done input replaces the existing !tFire wire.
Global control allocation, Nasti transport and NIC runtime remain separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from PrintControlResponsesCompare import clean, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import permission_checks, wiring


SIGNALS = [('nic_macAddr', 48), ('nic_rlimit_inc', 8), ('nic_rlimit_period', 8),
           ('nic_rlimit_size', 8), ('nic_pauser_threshold', 16),
           ('nic_pauser_quanta', 16), ('nic_pauser_refresh', 16)]
NAMES = ['macaddr_upper', 'macaddr_lower', 'rlimit_settings',
         'pause_threshold', 'pause_times', 'done']


def modules(text):
    return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S)}


def transfer(value, held):
    old, new = 'GGNICCPUStreamResponseBufferWrapper', 'GGNICMMIOWrapper'
    if isinstance(value, str):
        if value == '~' + old: return '~' + new
        if not value.startswith('~' + old + '|'): return value
        suffix = value[len(old) + 1:]
        prefix = '|' + old + '>'
        if suffix.startswith(prefix):
            port = re.split(r'[.\[]', suffix[len(prefix):])[0]
            if port not in held: suffix = '|' + new + '>' + suffix[len(prefix):]
        return '~' + new + suffix
    if isinstance(value, list): return [transfer(v, held) for v in value]
    if isinstance(value, dict): return {k: transfer(v, held) for k, v in value.items()}
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate', 'mmio-candidate', 'golden', 'header', 'compose-tool', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    run([a.compose_tool, a.candidate, out / 'attached.mlir', out / 'bank.mlir'], 'compose')
    before = modules(a.candidate.read_text()); after = modules(a.mmio_candidate.read_text())
    reproduced = modules((out / 'attached.mlir').read_text())
    assert after == reproduced, 'test attachment differs from compiler production output'
    assert set(after) - set(before) == {'GGNICMMIOBank', 'GGNICMMIOWrapper'}
    for name, body in before.items(): assert after[name] == body, 'prior module mutated: ' + name
    bank = after['GGNICMMIOBank']; top = after['GGNICMMIOWrapper']
    ports = native_ports(before['GGNICCPUStreamResponseBufferWrapper'])
    held = {n for n, _ in SIGNALS} | {'nic_done'}
    assert native_ports(top)[:-1] == [p for p in ports if p[1] not in held]
    assert native_ports(top)[-1][0:2] == ('out', 'nicBridge_mcr')
    expected = {('sim.' + n if d == 'in' else 'top.' + n):
                ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in ports if n not in held}
    expected.update({'nicRegisters.clock': 'top.hostClock', 'nicRegisters.reset': 'top.hostReset',
                     'nicRegisters.nic_done': 'sim.nic_done', 'top.nicBridge_mcr': 'nicRegisters.mcr'})
    expected.update({'sim.' + n: 'nicRegisters.' + n for n, _ in SIGNALS})
    assert wiring(top) == expected, 'production config/status wrapper wiring differs'
    def annotations(path): return json.loads(path.with_name(path.stem + '-all.json').read_text())
    records = annotations(a.mmio_candidate)
    assert records == transfer(annotations(a.candidate), held), 'recursive annotation transfer differs'
    registry = re.findall(r'name = "([^"]+)", offset = (\d+) : i32, readable = (\w+), writeable = (\w+)', bank)
    assert registry == [(n, str(4*i), 'true' if i == 5 else 'false', 'true') for i, n in enumerate(NAMES)]
    assert permission_checks(bank) == [('top.clock', f'not(top.mcr.read[{i}].ready)',
                                       'not(top.reset)', 'NIC register is write only') for i in range(5)]
    header = a.header.read_text(); match = re.search(r'SIMPLENICBRIDGEMODULE_struct\{(.*?)\}', header, re.S)
    addresses = re.findall(r'\.(\w+) = (\d+)', match[1])
    assert addresses == [(n, str(512+4*i)) for i, n in enumerate(NAMES)], addresses
    run(['firtool', out / 'bank.mlir', '--format=mlir', '--disable-all-randomization', '-o', out / 'native.sv'], 'firtool')

    # Retain only the actual SFC register cone, including its write-only read
    # diagnostics. Queue/FAME scheduling was compared at earlier boundaries.
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
    # Require all six original state declarations and thirteen output assigns.
    assert sum(l.strip().startswith('reg ') for l in lines) == 6
    assert sum(l.strip().startswith('assign ') for l in lines) == 13
    start = body.index('  always @(posedge clock)')
    clocked = body[start:body.index('\n`ifdef RANDOMIZE', start)]
    assert sum(f'~crFile_io_mcr_read_{i}_ready' in clocked for i in range(5)) == 5
    ref = decl + '\n' + '\n'.join(lines) + '\n' + clocked + '\nendmodule\n'
    ref = ref.replace('crFile_io_', '').replace('hPort_hBits_', '').replace("{{31'd0}, _T}", "{{31'd0}, nic_done}")
    (out / 'sfc-registers.sv').write_text(ref)
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n, w in inputs]
    test += [f'wire [{w-1}:0] {pre}_{n};' for pre in ('n', 's') for n, w in payloads]
    test += [f'wire {n};' for n, _ in controls]
    for kind, pre, ps in [('GGNICMMIOBank', 'n', inputs + payloads + controls), ('SFCNICRegisters', 's', inputs + payloads)]:
        bindings = [f'.{n}({pre + "_" + n if (n,w) in payloads else n})' for n, w in ps]
        test.append(kind + ' ' + pre + '(' + ','.join(bindings) + ');')
    test += ['integer cycles=0,resetWrites=0,zeroStrobeWrites=0,doneOverrides=0,doneSamples=0;',
             'task tick; begin clock=0; #2; clock=1; #2; clock=0; #2; cycles++;']
    for n, _ in payloads: test.append(f'if(n_{n}!==s_{n}) $fatal(1,"cycle %0d {n}: native=%h SFC=%h",cycles,n_{n},s_{n});')
    for n, _ in controls: test.append(f'if({n}!==1) $fatal(1,"{n} not always ready/valid");')
    test += ['resetWrites+=(reset&&mcr_write_0_valid);zeroStrobeWrites+=(!mcr_wstrb&&mcr_write_0_valid);',
             'doneOverrides+=mcr_write_5_valid;doneSamples+=!mcr_write_5_valid;end endtask',
             'task data;begin']
    for i in range(6): test.append(f'mcr_write_{i}_bits=$urandom;')
    test += ['end endtask', 'initial begin reset=1;mcr_wstrb=0;data();']
    test += [f'mcr_write_{i}_valid=1;' for i in range(6)]
    test += ['tick();',
             # Exhaustive write masks/strobes; neither host reset nor strobes
             # gate the five config writes or the done override/sample.
             'for(integer mask=0;mask<64;mask++)for(integer strobes=0;strobes<16;strobes++)for(integer mode=0;mode<4;mode++)begin',
             'reset=mode[0];nic_done=mode[1];mcr_wstrb=strobes;data();']
    for i in range(6): test.append(f'mcr_write_{i}_valid=mask[{i}];mcr_read_{i}_ready=reset;')
    test += ['tick();end', 'for(integer j=0;j<50000;j++)begin reset=$urandom;nic_done=$urandom;mcr_wstrb=$urandom;data();']
    for i in range(6): test.append(f'mcr_write_{i}_valid=$urandom;mcr_read_{i}_ready={"$urandom" if i == 5 else "reset&&($urandom&1)"};')
    test += ['tick();end',
             'if(resetWrites<1000||zeroStrobeWrites<100||doneOverrides<1000||doneSamples<1000)$fatal(1,"coverage");',
             '$display("PASS SFC NIC MMIO: cycles=%0d resetWrites=%0d zeroStrobeWrites=%0d doneOverrides=%0d doneSamples=%0d exhaustiveMasks=64 strobes=16 modes=4 random=50000",cycles,resetWrites,zeroStrobeWrites,doneOverrides,doneSamples);$finish;end endmodule']
    (out / 'check.sv').write_text('\n'.join(test) + '\n')
    run(['verilator', '--binary', '--timing', '--assert', '-Wno-fatal', '--top-module', 'check', '--Mdir', out / 'obj', '-o', 'check', out / 'check.sv', out / 'native.sv', out / 'sfc-registers.sv'], 'verilator')
    run([out / 'obj/check'], 'simulation')
    def artifact(path): return dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report = dict(golden=artifact(a.golden), header=artifact(a.header), candidate=artifact(a.mmio_candidate),
                  registers=registry, sfc_global_addresses=addresses, unchanged_prior_modules=len(before),
                  production_ports=len(native_ports(top)), annotation_records=len(records), result='matched',
                  simulation=(out / 'simulation.log').read_text(),
                  limitation='Decoded six-word NIC register bank only; Nasti control binding, global address allocation, driver emission and complete NIC runtime remain pending. Uninitialized register values are unspecified.')
    (out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n'); print(report['simulation'])


if __name__ == '__main__': main()
