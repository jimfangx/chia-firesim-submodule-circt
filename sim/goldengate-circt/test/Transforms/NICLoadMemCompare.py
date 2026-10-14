#!/usr/bin/env python3
"""Compare the NIC's shared native LoadMem boundary with archived SFC RTL.

The writer is tested in isolation; host memory arbitration, global control
routing and the NIC platform are still unsupported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring


def transfer(value):
    old, new = 'GGNICCPUStreamControlWrapper', 'GGLoadMemWriterWrapper'
    if isinstance(value, str):
        if value == '~' + old: return '~' + new
        if not value.startswith('~' + old + '|'): return value
        suffix = value[len(old) + 1:]
        if suffix.startswith('|' + old + '>'):
            suffix = '|' + new + '>' + suffix[len(old) + 2:]
        return '~' + new + suffix
    if isinstance(value, list): return [transfer(v) for v in value]
    if isinstance(value, dict): return {k: transfer(v) for k, v in value.items()}
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('before', 'writer', 'control', 'baseline-control', 'golden', 'immutable-golden', 'export-tool', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    before, writer = modules(a.before.read_text()), modules(a.writer.read_text())
    assert set(writer) - set(before) == {'GGLoadMemWriter', 'GGLoadMemWriterWrapper'}
    assert all(writer[n] == body for n, body in before.items()), 'prior module changed'
    old_ports = native_ports(before['GGNICCPUStreamControlWrapper'])
    helper_ports = native_ports(writer['GGLoadMemWriter'])
    top = writer['GGLoadMemWriterWrapper']
    expected_ports = old_ports + [(d, 'loadmem_' + n, t) for d, n, t in helper_ports[2:]]
    assert native_ports(top) == expected_ports
    nets = {('sim.' if d == 'in' else 'top.') + n:
            ('top.' if d == 'in' else 'sim.') + n for d, n, _ in old_ports}
    nets.update({'loadmemWriter.clock': 'top.hostClock', 'loadmemWriter.reset': 'top.hostReset'})
    for d, n, _ in helper_ports[2:]:
        nets[('loadmemWriter.' + n if d == 'in' else 'top.loadmem_' + n)] = (
            'top.loadmem_' + n if d == 'in' else 'loadmemWriter.' + n)
    assert wiring(top) == nets, 'copied transport or writer routes differ'
    def annotations(path):
        return json.loads(path.with_name(path.stem + '-all.json').read_text())
    assert annotations(a.writer) == transfer(annotations(a.before)), 'nested target transfer differs'
    current, baseline = modules(a.control.read_text()), modules(a.baseline_control.read_text())
    helpers = ['GGLoadMemWriter', 'GGLoadMemRequestQueue2', 'GGLoadMemRequestArbiter',
               'GGLoadMemWriteMMIOBank', 'GGLoadMemWriteDataFIFO', 'GGLoadMemReadRequests',
               'GGLoadMemReadDataFIFO', 'GGLoadMemMCRFile']
    for n in helpers: assert current[n] == baseline[n], 'shared helper differs: ' + n
    # Confirm the archived NIC widget has the same host bus geometry. The
    # isolated reference below retains the SFC writer's original operations.
    golden = clean(a.golden.read_text()); header, body = module(golden, 'LoadMemWriter')
    assert module(clean(a.immutable_golden.read_text()), 'LoadMemWriter') == (header, body), 'immutable U250 writer differs'
    sfc_ports = ports(header)
    widget_header, _ = module(golden, 'LoadMemWidget'); widget_ports = ports(widget_header)
    for n in ('aw_bits_addr', 'ar_bits_addr'):
        assert widget_ports['auto_to_host_memory_out_' + n] == ['output', 34]
    assert widget_ports['auto_to_host_memory_out_w_bits_data'] == ['output', 64]
    assert 'wLen > 34\'h20 ? 34\'h20 : wLen' in body and 'reg [4:0] wBeatsLeft;' in body
    run([a.export_tool, a.writer, out / 'writer.mlir'], 'export')
    assert modules((out / 'writer.mlir').read_text())['GGLoadMemWriter'] == writer['GGLoadMemWriter']
    run(['firtool', out / 'writer.mlir', '--format=mlir', '--disable-all-randomization',
         '-o', out / 'native.sv'], 'firtool')
    native_header, _ = module(clean((out / 'native.sv').read_text()), 'GGLoadMemWriter')
    native = ports(native_header)
    assert native == {n.removeprefix('io_'): spec for n, spec in sfc_ports.items()}
    (out / 'sfc-writer.sv').write_text('module LoadMemWriter(' + header + ');' + body + 'endmodule\n')
    inputs = [(n, w) for n, (d, w) in native.items() if d == 'input']
    outputs = [(n, w) for n, (d, w) in native.items() if d == 'output']
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n, w in inputs]
    test += [f'wire [{w-1}:0] {prefix}_{n};' for prefix in ('n', 's') for n, w in outputs]
    test += ['GGLoadMemWriter n(' + ','.join(f'.{n}({"n_"+n if d=="output" else n})'
             for n, (d, w) in native.items()) + ');',
             'LoadMemWriter s(' + ','.join(f'.{n}({"s_"+n.removeprefix("io_") if d=="output" else n.removeprefix("io_")})'
             for n, (d, w) in sfc_ports.items()) + ');',
             'integer cycles=0,requests=0,bursts=0,beats=0,responses=0,resets=0;task compare;begin']
    for n, w in outputs:
        guard = 'n_mem_aw_valid' if n.startswith('mem_aw_bits_') else 'n_mem_w_valid' if n.startswith('mem_w_bits_') else '1'
        test += [f'if(({guard})&&n_{n}!==s_{n})$fatal(1,"cycle %0d {n} native=%h SFC=%h",cycles,n_{n},s_{n});']
    test += ['end endtask', 'task tick;begin clock=0;#2;compare();',
             'requests+=(req_valid&&n_req_ready);bursts+=(mem_aw_ready&&n_mem_aw_valid);',
             'beats+=(mem_w_ready&&n_mem_w_valid);responses+=(mem_b_valid&&n_mem_b_ready);resets+=reset;',
             'clock=1;#2;clock=0;#2;cycles++;compare();end endtask',
             'initial begin reset=1;tick();reset=0;',
             # All short lengths include zero underflow, maxBurst boundaries,
             # multiple bursts, zero fill and wrap at the 34-bit address limit.
             'for(integer zero=0;zero<2;zero++)for(integer len=0;len<97;len++)begin',
             "req_bits_zero=zero;req_bits_len=len;req_bits_addr=34'h3fffffff8;req_valid=1;tick();req_valid=0;",
             'for(integer timeout=0;!n_req_ready;timeout++)begin',
             'if(timeout>2000)$fatal(1,"writer did not complete");',
             'data_valid=($urandom_range(0,3)!=0);data_bits={$urandom,$urandom};',
             'mem_aw_ready=($urandom_range(0,3)!=0);mem_w_ready=($urandom_range(0,3)!=0);',
             'mem_b_valid=($urandom_range(0,3)!=0);tick();end end',
             # Reset on each of the four FSM stages, including handshakes.
             'for(integer stage=0;stage<4;stage++)for(integer zero=0;zero<2;zero++)begin',
             'reset=1;req_valid=0;tick();reset=0;req_bits_zero=zero;req_bits_len=1;req_valid=1;',
             'data_valid=1;mem_aw_ready=1;mem_w_ready=1;mem_b_valid=1;',
             'repeat(stage)tick();reset=1;tick();reset=0;req_valid=0;end',
             'for(integer j=0;j<100000;j++)begin reset=($urandom_range(0,99)==0);',
             'req_valid=$urandom;req_bits_zero=$urandom;req_bits_len=$urandom_range(0,96);',
             'req_bits_addr={$urandom,$urandom};data_valid=$urandom;data_bits={$urandom,$urandom};',
             'mem_aw_ready=$urandom;mem_w_ready=$urandom;mem_b_valid=$urandom;tick();end',
             'if(requests<1000||bursts<1000||beats<10000||responses<100||resets<500)$fatal(1,"coverage");',
             '$display("PASS SFC NIC LoadMem writer: cycles=%0d requests=%0d bursts=%0d beats=%0d responses=%0d resets=%0d shortLengths=194 stages=4 random=100000",cycles,requests,bursts,beats,responses,resets);$finish;end endmodule']
    (out / 'check.sv').write_text('\n'.join(test) + '\n')
    run(['verilator', '--binary', '--timing', '--assert', '-Wno-fatal', '--top-module', 'check',
         '--Mdir', out / 'obj', '-o', 'check', out / 'check.sv', out / 'native.sv', out / 'sfc-writer.sv'], 'verilator')
    run([out / 'obj/check'], 'simulation')
    def artifact(path): return dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report = dict(result='matched', golden=artifact(a.golden), immutable_golden=artifact(a.immutable_golden),
                  candidate=artifact(a.writer),
                  unchanged_prior_modules=len(before), copied_ports=len(old_ports), writer_ports=18,
                  annotation_records=len(annotations(a.writer)), shared_helpers=helpers,
                  simulation=(out / 'simulation.log').read_text(), limitation=__doc__)
    (out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['simulation'])


if __name__ == '__main__': main()
