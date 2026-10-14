#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare actual NIC credit/order operations with recorded SFC ingress.

Checks production wrapper ports, events/reset, all annotation transfers and
shared helpers. The isolated differential couples completed-write credits to
transaction ordering; host issue gates, timing/MMIO and NIC execution remain
outside this boundary. SFC's unused DualQueue.next lookahead is not exported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, equations, module
from PrintSelectedFASEDIngressCreditsCompare import compare as credits, oracle as credit_oracle
from PrintSelectedFASEDIngressOrderCompare import compare as order, oracle as order_oracle

HELPERS = {'GGFASEDTokenEngine', 'GGFASEDHostOutstanding', 'GGFASEDIngressAW',
           'GGFASEDIngressAWQueue10', 'GGFASEDIngressWQueue16', 'GGFASEDIngressARQueue4',
           'GGFASEDIngressCredits', 'GGFASEDIngressOrder20'}


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'helpers.mlir', out/'helpers.sv'
    run([export_tool, candidate, exported], 'export')
    production, isolated = modules(candidate.read_text()), modules(exported.read_text())
    assert set(isolated) == HELPERS
    for n, body in isolated.items():
        assert body.replace('firrtl.module public @', 'firrtl.module private @') == production[n].replace(
            'firrtl.module public @', 'firrtl.module private @'), 'export rebuilt helper '+n
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    sfc = clean(golden.read_text()); lowered = clean(native.read_text())
    oracle = []
    for n in ('SatUpDownCounter_1', 'DualQueue', 'Queue_10'):
        h, b = module(sfc, n); oracle.append('module '+n+'('+h+');\n'+b+'\nendmodule\n')
    # Use the actual SFC IngressModule expressions, with its accepted enqueue
    # and final-beat events supplied as inputs at this compiler boundary.
    nets = equations(module(sfc, 'IngressModule')[1])
    bindings = {'SatUpDownCounter_io_value': 's_w', 'SatUpDownCounter_1_io_value': 's_aw',
                'SatUpDownCounter_io_inc': 'awEnqFire', 'SatUpDownCounter_1_io_inc': 'wLastEnqFire',
                '_T_1': 'wLastDeqFire', 'wQueue_io_deq_bits_last': "1'b1", '_T_5': 'awDeqFire',
                'io_relaxed': 'relaxed'}
    emitted = []
    def resolve(n):
        if n in bindings: return bindings[n]
        assert n in nets, 'unresolved SFC credit net '+n
        expression = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: resolve(m[0]), nets[n])
        symbol = 'sfc_'+n
        emitted.append('wire '+symbol+' = '+expression+';')
        bindings[n] = symbol
        return symbol
    done = resolve('write_req_done')
    w_dec = resolve('SatUpDownCounter_io_dec')
    aw_dec = resolve('SatUpDownCounter_1_io_dec')
    tb = ['module check; reg clock=0,reset=1,relaxed=0,awEnqFire=0,wLastEnqFire=0,awDeqFire=0,wLastDeqFire=0,readDone=0,deqReady=0;',
          'integer cycles=0,dual=0,partial=0,fullStalls=0,emptyStalls=0,resetPushes=0,strictCycles=0,relaxedCycles=0,saturations=0;',
          'wire [3:0] s_aw,s_w; wire s_awEmpty,s_wEmpty;', *emitted,
          f'SatUpDownCounter_1 awCounter(.clock(clock),.reset(reset),.io_inc(wLastEnqFire),.io_dec({aw_dec}),.io_max(4\'d10),.io_value(s_aw),.io_empty(s_awEmpty),.io_full());',
          f'SatUpDownCounter_1 wCounter(.clock(clock),.reset(reset),.io_inc(awEnqFire),.io_dec({w_dec}),.io_max(4\'d10),.io_value(s_w),.io_empty(s_wEmpty),.io_full());']
    expected_ports = {'clock', 'reset', 'relaxed', 'awEnqFire', 'wLastEnqFire', 'awDeqFire', 'wLastDeqFire',
                      'status_awValue', 'status_wValue', 'status_awEmpty', 'status_wEmpty', 'status_writeReqDone'}
    cp = ports(module(lowered, 'GGFASEDIngressCredits')[0]); assert set(cp) == expected_ports
    cc = {n: n for n in cp if cp[n][0] == 'input'}
    for n, (direction, width) in cp.items():
        if direction == 'output':
            tb.append(f'wire [{width-1}:0] n_{n};'); cc[n] = 'n_'+n
    tb.append('GGFASEDIngressCredits credits('+','.join('.'+p+'('+v+')' for p,v in cc.items())+');')
    op = ports(module(lowered, 'GGFASEDIngressOrder20')[0])
    assert set(op) == {'clock','reset','enqA_valid','enqB_valid','deq_ready','enqA_ready','enqB_ready','deq_valid','deq_bits'}
    oc = dict(clock='clock', reset='reset', enqA_valid='readDone', enqB_valid='n_status_writeReqDone', deq_ready='deqReady')
    for n, (direction, width) in op.items():
        assert width == 1
        if direction == 'output': tb.append('wire n_'+n+';'); oc[n] = 'n_'+n
    tb.append('GGFASEDIngressOrder20 order('+','.join('.'+p+'('+v+')' for p,v in oc.items())+');')
    tb += ['wire s_ar,s_br,s_dv,s_bits;',
           f'DualQueue sfcOrder(.clock(clock),.reset(reset),.io_enqA_valid(readDone),.io_enqA_ready(s_ar),.io_enqB_valid({done}),.io_enqB_ready(s_br),.io_deq_ready(deqReady),.io_deq_valid(s_dv),.io_deq_bits(s_bits));',
           'task compare;begin']
    for pin, value in {'status_awValue':'s_aw', 'status_wValue':'s_w', 'status_awEmpty':'s_awEmpty',
                       'status_wEmpty':'s_wEmpty', 'status_writeReqDone':done,
                       'enqA_ready':'s_ar', 'enqB_ready':'s_br', 'deq_valid':'s_dv'}.items():
        tb.append(f'if(n_{pin} !== {value})$fatal(1,"cycle %0d {pin}",cycles);')
    tb += ['if(s_dv && n_deq_bits !== s_bits)$fatal(1,"cycle %0d order payload",cycles);',
           'end endtask',
           'task cycle;begin #1;compare();',
           f'if(readDone && s_ar && {done} && s_br)dual++;',
           f'if(readDone && {done} && (s_ar != s_br))partial++;',
           f'if((readDone && !s_ar)||({done} && !s_br))fullStalls++;',
           'if(deqReady && !s_dv)emptyStalls++;',
           f'if(reset && ((readDone && s_ar)||({done} && s_br)))resetPushes++;',
           'if(relaxed)relaxedCycles++;else strictCycles++;',
           'if(s_aw==10 || s_w==10)saturations++;',
           'clock=1;#1;clock=0;#1;cycles++;compare();end endtask',
           'initial begin clock=1;#1;clock=0;#1;reset=0;',
           # Directed fill/drain and asymmetric event streams exercise bank
           # wraps, full/empty states, saturation and reset with accepted writes.
           'repeat(12)begin readDone=1;awEnqFire=1;wLastEnqFire=1;cycle();end',
           'repeat(24)begin readDone=0;awEnqFire=0;wLastEnqFire=0;deqReady=1;cycle();end',
           'relaxed=1;deqReady=0;wLastEnqFire=0;awEnqFire=1;repeat(12)cycle();',
           'awEnqFire=0;wLastEnqFire=1;repeat(12)cycle();',
           'reset=1;readDone=1;awEnqFire=1;wLastEnqFire=1;cycle();reset=0;',
           'repeat(40000)begin reset=($urandom%997)==0;relaxed=$urandom;awEnqFire=$urandom;wLastEnqFire=$urandom;awDeqFire=$urandom;wLastDeqFire=$urandom;readDone=$urandom;deqReady=$urandom;cycle();end',
           'if(dual<100||partial<10||fullStalls<100||emptyStalls<10||resetPushes<5||saturations<10||strictCycles<1000||relaxedCycles<1000)$fatal(1,"coverage");',
           '$display("coverage cycles=%0d dual=%0d partial=%0d fullStalls=%0d emptyStalls=%0d resetPushes=%0d saturations=%0d strictCycles=%0d relaxedCycles=%0d",cycles,dual,partial,fullStalls,emptyStalls,resetPushes,saturations,strictCycles,relaxedCycles);',
           '$display("PASS NIC coupled SFC ingress credits and transaction ordering");$finish;end endmodule']
    (out/'sfc.sv').write_text('\n'.join(oracle)); (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator', '--binary', '-j', '4', '--timing', '--assert', '-Wno-fatal', '--top-module', 'check',
         '--Mdir', out/'obj', '-o', 'check', out/'check.sv', native, out/'sfc.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    result = (out/'simulation.log').read_text()
    assert 'PASS NIC coupled SFC ingress credits and transaction ordering' in result
    return dict(rtl=native, log=str(out/'simulation.log'), cycles=int(re.search(r'cycles=(\d+)', result)[1]),
                exported_helpers=8, cycled_helpers=2, result=result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir','baseline','golden','export-tool','output-dir','report-json'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args()
    before = a.candidate_dir/'post-fame-fased-ingress-ar.mlir'
    credit = a.candidate_dir/'post-fame-fased-ingress-credits.mlir'
    after = a.candidate_dir/'post-fame-fased-ingress-order.mlir'
    credit_oracle(a.golden); order_oracle(a.golden)
    behavior = differential(after, a.golden, a.export_tool, a.output_dir.resolve())
    baseline = a.baseline.read_text(); rtl = behavior.pop('rtl')
    report = dict(golden=str(a.golden.resolve()), golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                  candidate=str(after.resolve()), candidate_sha256=hashlib.sha256(after.read_bytes()).hexdigest(),
                  credits=credits(before,credit,baseline,'GGFASEDIngressARQueueWrapper','GGFASEDIngressCreditsWrapper',rtl),
                  order=order(credit,after,baseline,'GGFASEDIngressCreditsWrapper','GGFASEDIngressOrderWrapper',rtl),
                  behavioral=behavior, limitation=__doc__)
    a.report_json.write_text(json.dumps(report,indent=2)+'\n')
    print('PASS NIC production credits/order wrappers, annotations, exhaustive states and coupled SFC differential')


if __name__ == '__main__': main()
