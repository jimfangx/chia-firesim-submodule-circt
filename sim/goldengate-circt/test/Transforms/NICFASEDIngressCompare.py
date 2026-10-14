#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare production NIC FASED token/counter/queue boundaries with SFC.

Host issue, egress timing, slave-1 MMIO and platform integration remain external.
The shared helper comparison connects existing behavioral tests to this replay.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlWriteRoutingCompare import transfer
from PrintRocketControlMasterCompare import native_ports
from PrintRocketResponsesCompare import wiring
from PrintRocketFASEDIngressCompare import compare as compare_ingress
from PrintSelectedFASEDTokensCompare import compare as compare_tokens, compare_sfc
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, module

STAGES = ('blockdev-response-scheduler', 'fased-token', 'fased-host-outstanding',
          'fased-ingress-aw', 'fased-ingress-w', 'fased-ingress-ar')
TOPS = ('GGBlockDevResponseSchedulerWrapper', 'GGFASEDTokenWrapper',
        'GGFASEDHostOutstandingWrapper', 'GGFASEDIngressAWWrapper',
        'GGFASEDIngressWQueueWrapper', 'GGFASEDIngressARQueueWrapper')
HELPERS = (('GGFASEDTokenEngine',), ('GGFASEDHostOutstanding',),
           ('GGFASEDIngressAW', 'GGFASEDIngressAWQueue10'),
           ('GGFASEDIngressWQueue16',), ('GGFASEDIngressARQueue4',))


def annos(path):
    return json.loads(path.with_name(path.stem + '-all.json').read_text())


def compare_boundary(before, after, old, new, helpers, index):
    previous, current = modules(before.read_text()), modules(after.read_text())
    assert set(current) - set(previous) == {new, *helpers}, 'unexpected module additions'
    assert all(current[n] == body for n, body in previous.items()), 'earlier module changed'
    consumed = ('fased_readiness', 'fased_ingress', 'fased_ingress_w_enq', 'fased_ingress_ar_enq')[index]
    copied = [p for p in native_ports(previous[old]) if p[1] != consumed]
    added = ((('in', 'fased_egress_readiness'), ('in', 'fased_host_transactions'),
              ('out', 'fased_host_mem_idle'), ('out', 'fased_host_read_inflight')),
             tuple(('out', 'fased_ingress_'+n) for n in ('aw_deq', 'w_enq', 'ar_enq', 'aw_enq_fire')),
             (('out', 'fased_ingress_w_deq'), ('out', 'fased_ingress_w_last_fire')),
             (('out', 'fased_ingress_ar_deq'), ('out', 'fased_ingress_ar_enq_fire')))[index]
    ports_after = native_ports(current[new])
    assert ports_after[:len(copied)] == copied, 'copied port geometry/order differs'
    assert tuple((d, n) for d, n, _ in ports_after[len(copied):]) == added, 'boundary ports differ'
    expected = {('sim.'+n if d == 'in' else 'top.'+n): ('top.'+n if d == 'in' else 'sim.'+n)
                for d, n, _ in copied}
    if index == 0:
        expected.update({'hostOutstanding.clock': 'top.hostClock', 'hostOutstanding.reset': 'top.hostReset',
                         'hostOutstanding.transactions': 'top.fased_host_transactions',
                         'sim.fased_readiness.hostMemIdle': 'hostOutstanding.hostMemIdle',
                         'top.fased_host_mem_idle': 'hostOutstanding.hostMemIdle',
                         'top.fased_host_read_inflight': 'hostOutstanding.hostReadInflight'})
        for n in ('readValid', 'writeValid'):
            expected['sim.fased_readiness.'+n] = 'top.fased_egress_readiness.'+n
    else:
        inst = ('ingressAW', 'wQueue', 'arQueue')[index-1]
        expected.update({inst+'.clock': 'top.hostClock', inst+'.reset': 'sim.fased_ingress_reset'})
        if index == 1:
            expected[inst+'.ingress'] = 'sim.fased_ingress'
            for n in ('aw_deq', 'w_enq', 'ar_enq', 'aw_enq_fire'):
                expected['top.fased_ingress_'+n] = inst+'.'+n
        else:
            ch = ('w', 'ar')[index-2]
            expected[inst+'.enq'] = 'sim.fased_ingress_'+ch+'_enq'
            expected['top.fased_ingress_'+ch+'_deq'] = inst+'.deq'
            event = 'last_fire' if ch == 'w' else 'enq_fire'
            expected['top.fased_ingress_'+ch+'_'+event] = inst+'.'+('enq_last_fire' if ch == 'w' else event)
    observed = wiring(current[new], new, aggregate=True)
    assert observed == expected, (new, {k: (v, expected.get(k)) for k, v in observed.items() if v != expected.get(k)},
                                 set(expected)-set(observed))
    wanted = transfer(annos(before), old, new, {n for _, n, _ in copied})
    if index == 0:
        def readiness(v):
            if isinstance(v, str):
                for source, dest in (('readValid', 'fased_egress_readiness.readValid'),
                                     ('writeValid', 'fased_egress_readiness.writeValid'),
                                     ('hostMemIdle', 'fased_host_mem_idle')):
                    if v == '~'+new+'|'+old+'>fased_readiness.'+source:
                        return '~'+new+'|'+new+'>'+dest
                return v
            if isinstance(v, list): return [readiness(x) for x in v]
            if isinstance(v, dict): return {k: readiness(x) for k, x in v.items()}
            return v
        wanted = readiness(wanted)
    assert annos(after) == wanted, 'complete annotation archive transfer differs'
    return dict(candidate=str(after), preserved_modules=len(previous), copied_ports=len(copied),
                connections=len(expected), annotations=len(wanted))


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'helpers.mlir', out/'helpers.sv'
    run([export_tool, candidate, exported], 'export')
    actual, isolated = modules(candidate.read_text()), modules(exported.read_text())
    assert set(isolated) == {n for group in HELPERS for n in group}
    for n, body in isolated.items():
        # Visibility is the only permitted export change.
        assert body.replace('firrtl.module public @', 'firrtl.module private @') == actual[n].replace(
            'firrtl.module public @', 'firrtl.module private @'), 'export rebuilt helper '+n
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    lowered, sfc = clean(native.read_text()), clean(golden.read_text())
    tb = ['module check; reg clock=0,reset=1; integer cycles=0,pushes=0,pops=0,fullStalls=0,emptyStalls=0,resetPushes=0;',
          'reg enq_valid=0,deq_ready=0;']
    oracle, randomize, assertions, coverage = [], [], [], []
    for ch, depth, symbol in (('aw', 10, 'Queue_7'), ('w', 16, 'Queue_8'), ('ar', 4, 'Queue_9')):
        name = f'GGFASEDIngress{ch.upper()}Queue{depth}'
        np = ports(module(lowered, name)[0]); sp = ports(module(sfc, symbol)[0])
        connections = {}
        for pin, (direction, width) in np.items():
            if pin in ('clock', 'reset', 'enq_valid', 'deq_ready'): connections[pin] = pin
            else:
                signal = ch+'_'+pin
                tb.append(f'{"reg" if direction == "input" else "wire"} [{width-1}:0] {signal};')
                connections[pin] = signal
                if direction == 'input': randomize.append(f'{signal}={{$urandom,$urandom,$urandom}};')
        tb.append(name+' n_'+ch+'('+','.join('.'+p+'('+s+')' for p, s in connections.items())+');')
        golden_connections = {}
        for pin, (direction, width) in sp.items():
            npin = pin.removeprefix('io_')
            assert np[npin] == [direction, width], 'SFC/native queue field width differs'
            if direction == 'input': golden_connections[pin] = connections[npin]
            else:
                signal = 's_'+ch+'_'+pin
                tb.append(f'wire [{width-1}:0] {signal};'); golden_connections[pin] = signal
                guard = ch+'_deq_valid' if npin.startswith('deq_bits_') else "1'b1"
                assertions.append(f'if(({guard}) && {signal} !== {connections[npin]}) $fatal(1,"cycle %0d {ch} {pin}",cycles);')
        tb.append(symbol+' s_'+ch+'('+','.join('.'+p+'('+s+')' for p, s in golden_connections.items())+');')
        header, body = module(sfc, symbol); oracle.append('module '+symbol+'('+header+');\n'+body+'\nendmodule\n')
        coverage += [f'if(enq_valid && {ch}_enq_ready)pushes++;', f'if(deq_ready && {ch}_deq_valid)pops++;',
                     f'if(enq_valid && !{ch}_enq_ready)fullStalls++;', f'if(deq_ready && !{ch}_deq_valid)emptyStalls++;',
                     f'if(reset && enq_valid && {ch}_enq_ready)resetPushes++;']
    np = ports(module(lowered, 'GGFASEDHostOutstanding')[0]); connections = {}
    for pin, (direction, width) in np.items():
        if pin in ('clock', 'reset'): connections[pin] = pin; continue
        tb.append(f'{"reg" if direction == "input" else "wire"} [{width-1}:0] host_{pin};')
        connections[pin] = 'host_'+pin
        if direction == 'input': randomize.append('host_'+pin+'=$urandom;')
    tb.append('GGFASEDHostOutstanding host('+','.join('.'+p+'('+s+')' for p,s in connections.items())+');')
    for ch, inc, dec in (
        ('read', 'host_transactions_arReady & host_transactions_arValid',
         'host_transactions_rReady & host_transactions_rValid & host_transactions_rLast'),
        ('write', 'host_transactions_awReady & host_transactions_awValid',
         'host_transactions_bReady & host_transactions_bValid')):
        tb += [f'wire [3:0] s_{ch}Count;wire s_{ch}Empty;',
               f'SatUpDownCounter_1 s_{ch}(.clock(clock),.reset(reset),.io_inc({inc}),.io_dec({dec}),.io_max(4\'d10),.io_value(s_{ch}Count),.io_empty(s_{ch}Empty),.io_full());']
        assertions.append(f'if(host_{ch}Count !== s_{ch}Count)$fatal(1,"cycle %0d host {ch} counter",cycles);')
    assertions += ['if(host_hostMemIdle !== (s_readEmpty & s_writeEmpty))$fatal(1,"host idle");',
                   'if(host_hostReadInflight !== !s_readEmpty)$fatal(1,"host read inflight");']
    header, body = module(sfc, 'SatUpDownCounter_1'); oracle.append('module SatUpDownCounter_1('+header+');\n'+body+'\nendmodule\n')
    tb += ['task compare;begin', *assertions, 'end endtask',
           'task cycle;begin #1;compare();', *coverage,
           'clock=1;#1;clock=0;#1;cycles++;compare();end endtask',
           'initial begin', *randomize, 'clock=1;#1;clock=0;#1;reset=0;',
           'repeat(20)begin enq_valid=1;deq_ready=0;cycle();end',
           'repeat(20)begin enq_valid=0;deq_ready=1;cycle();end',
           'repeat(20)begin enq_valid=1;deq_ready=1;cycle();end',
           'reset=1;enq_valid=1;deq_ready=0;cycle();reset=0;',
           'repeat(40000)begin', *randomize, 'reset=($urandom%97)==0;enq_valid=$urandom;deq_ready=$urandom;cycle();end',
           'if(pushes<1000||pops<1000||fullStalls<100||emptyStalls<100||resetPushes<10)$fatal(1,"coverage");',
           '$display("coverage cycles=%0d pushes=%0d pops=%0d fullStalls=%0d emptyStalls=%0d resetPushes=%0d",cycles,pushes,pops,fullStalls,emptyStalls,resetPushes);',
           '$display("PASS NIC FASED SFC queues and host counters");$finish;end endmodule']
    (out/'sfc.sv').write_text('\n'.join(oracle)); (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator', '--binary', '-j', '4', '--timing', '--assert', '-Wno-fatal', '--top-module', 'check',
         '--Mdir', out/'obj', '-o', 'check', out/'check.sv', native, out/'sfc.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    result = (out/'simulation.log').read_text(); assert 'PASS NIC FASED SFC queues and host counters' in result
    return dict(log=str(out/'simulation.log'), cycles=int(re.search(r'cycles=(\d+)', result)[1]),
                exported_helpers=6, cycled_helpers=4, result=result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir', 'baseline', 'golden', 'report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    for n in ('export-tool', 'output-dir'): p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args()
    paths = [a.candidate_dir / ('post-fame-'+s+'.mlir') for s in STAGES]
    baseline = a.baseline.read_text()
    compare_sfc(a.golden)
    token = compare_tokens(paths[0], paths[1], baseline, TOPS[0], TOPS[1])
    boundaries = [compare_boundary(paths[i], paths[i+1], TOPS[i], TOPS[i+1], HELPERS[i], i-1)
                  for i in range(1, 5)]
    ingress = compare_ingress(a.golden, [paths[-1]], baseline)
    report = dict(golden=str(a.golden.resolve()), golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate=str(paths[-1].resolve()), candidate_sha256=hashlib.sha256(paths[-1].read_bytes()).hexdigest(),
                  token=token, boundaries=boundaries, ingress=ingress, limitation=__doc__)
    report['behavioral'] = differential(paths[-1], a.golden, a.export_tool, a.output_dir.resolve())
    a.report_json.write_text(json.dumps(report, indent=2)+'\n')
    print('PASS NIC FASED five production boundaries, 11 channels, 46 payload routes, 256 token/reset cases and SFC ingress geometry')


if __name__ == '__main__': main()
