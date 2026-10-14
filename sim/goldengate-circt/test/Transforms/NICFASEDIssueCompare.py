#!/usr/bin/env python3
# See LICENSE for license details.
"""Check production NIC issue and accepted transaction-order retirement.

The isolated RTL differential uses actual SFC IngressModule equations and its
write-data register update, then couples issue to actual SFC DualQueue/Queue_10.
Ingress credit/queue events and host status are boundary inputs. Full payload
queues, responses, timing/MMIO, platform composition and NIC execution remain
outside this test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICFASEDCreditsOrderCompare import HELPERS
from NICMMIOBankCompare import modules
from PrintControlMasterCompare import ports
from PrintControlResponsesCompare import clean, equations, module
from PrintSelectedFASEDIngressIssueCompare import compare, oracle


def differential(candidate, golden, export_tool, out):
    out.mkdir(parents=True, exist_ok=False)
    def run(command, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True)
    exported, native = out/'helpers.mlir', out/'helpers.sv'
    run([export_tool, candidate, exported], 'export')
    production, isolated = modules(candidate.read_text()), modules(exported.read_text())
    assert set(isolated) == HELPERS | {'GGFASEDIngressIssue'}
    for n, body in isolated.items():
        assert body.replace('firrtl.module public @', 'firrtl.module private @') == production[n].replace(
            'firrtl.module public @', 'firrtl.module private @'), 'export rebuilt helper '+n
    run(['firtool', exported, '--format=mlir', '--disable-all-randomization', '-o', native], 'firtool')
    sfc, lowered = clean(golden.read_text()), clean(native.read_text())
    body = module(sfc, 'IngressModule')[1]; nets = equations(body)
    cp = ports(module(lowered, 'GGFASEDIngressIssue')[0])
    bindings = {'io_relaxed':'relaxed', 'io_host_mem_idle':'hostMemIdle',
                'io_host_read_inflight':'hostReadInflight', 'SatUpDownCounter_1_io_empty':'awEmpty',
                'SatUpDownCounter_io_empty':'wEmpty', 'xaction_order_io_deq_valid':'s_orderValid',
                'xaction_order_io_deq_bits':'s_orderBits', 'do_hwrite_data_reg':'s_writeData'}
    for ch in ('aw','w','ar'):
        bindings[ch+'Queue_io_deq_valid'] = ch+'_valid'
        bindings['io_nastiOutputs_'+ch+'_ready'] = 'requests_'+ch+'_ready'
    bindings['io_nastiOutputs_w_bits_last'] = 'w_bits_last'
    emitted = []
    # Recognize complete SV literals before identifiers (1'h0 is a literal,
    # not a dependency named h0). Every other dependency comes from SFC.
    token = r"\d+'[sS]?[bBoOdDhH][0-9a-fA-F_xzXZ]+|\b[A-Za-z_]\w*\b"
    def resolve(n):
        if n in bindings: return bindings[n]
        assert n in nets, 'unresolved SFC issue net '+n
        def expand(m): return m[0] if "'" in m[0] else resolve(m[0])
        expression = re.sub(token, expand, nets[n]); symbol = 'sfc_'+n
        emitted.append('wire '+symbol+' = '+expression+';'); bindings[n] = symbol
        return symbol
    outputs = {'order_ready':resolve('xaction_order_io_deq_ready')}
    for ch in ('aw','w','ar'):
        outputs[ch+'_ready'] = resolve(ch+'Queue_io_deq_ready')
        outputs['requests_'+ch+'_valid'] = resolve('io_nastiOutputs_'+ch+'_valid')
    next_state = resolve('_GEN_3')
    state = re.search(r"if \(reset\) begin\s+do_hwrite_data_reg <= 1'h0;\s+end else begin\s+do_hwrite_data_reg <= _GEN_3;",body)[0]
    state = state.replace('do_hwrite_data_reg','s_writeData').replace('_GEN_3',next_state)
    # Recorded SFC payload wires preserve queue fields; native optional user
    # fields are checked for identity alongside the shared fields.
    shared_payload = 0
    for ch in ('aw','w','ar'):
        for pin in cp:
            prefix = 'requests_'+ch+'_bits_'
            if pin.startswith(prefix):
                leaf = pin[len(prefix):]; name = 'io_nastiOutputs_'+ch+'_bits_'+leaf
                if name in nets:
                    assert nets[name].strip() == ch+'Queue_io_deq_bits_'+leaf, name
                    shared_payload += 1
    assert shared_payload >= 20
    tb = ['module check; reg clock=0,reset=1,coupled=0,orderValid=0,orderBits=0,readDone=0,writeDone=0;',
          'reg s_writeData=0;',
          'integer cycles=0,awFinalW=0,resetAccepted=0,stalls=0,readOverlap=0,strictCycles=0,relaxedCycles=0,fullStalls=0,retirements=0;',
          'wire s_qvalid,s_qbits,s_ar,s_br,n_qvalid,n_qbits,n_ar,n_br;',
          'wire s_orderValid=coupled?s_qvalid:orderValid; wire s_orderBits=coupled?s_qbits:orderBits;']
    cc = {}
    payloads = []
    for n,(direction,width) in cp.items():
        if n == 'clock' or n == 'reset': cc[n]=n
        elif n == 'order_valid': cc[n]='coupled?n_qvalid:orderValid'
        elif n == 'order_bits': cc[n]='coupled?n_qbits:orderBits'
        elif direction == 'input':
            tb.append(f'reg [{width-1}:0] {n}=0;'); cc[n]=n
            if '_bits_' in n: payloads.append((n,width))
        else: tb.append(f'wire [{width-1}:0] n_{n};'); cc[n]='n_'+n
    tb.extend(emitted)
    tb.append('always @(posedge clock) begin '+state+' end end')
    tb.append('GGFASEDIngressIssue issue('+','.join('.'+p+'('+v+')' for p,v in cc.items())+');')
    tb.append('GGFASEDIngressOrder20 order(.clock(clock),.reset(reset),.enqA_valid(readDone),.enqB_valid(writeDone),.enqA_ready(n_ar),.enqB_ready(n_br),.deq_ready(coupled && n_order_ready),.deq_valid(n_qvalid),.deq_bits(n_qbits));')
    tb.append('DualQueue sfcOrder(.clock(clock),.reset(reset),.io_enqA_valid(readDone),.io_enqB_valid(writeDone),.io_enqA_ready(s_ar),.io_enqB_ready(s_br),.io_deq_ready(coupled && '+outputs['order_ready']+'),.io_deq_valid(s_qvalid),.io_deq_bits(s_qbits));')
    tb.append('task compare; begin')
    for n,reference in outputs.items(): tb.append(f'if(n_{n} !== {reference})$fatal(1,"cycle %0d {n}",cycles);')
    for n,width in payloads:
        tb.append(f'if(n_requests_{n} !== {n})$fatal(1,"cycle %0d payload {n}",cycles);')
    tb += ['if(n_ar!==s_ar||n_br!==s_br||n_qvalid!==s_qvalid||(s_qvalid && n_qbits!==s_qbits))$fatal(1,"cycle %0d order queue",cycles);',
           'end endtask', 'task cycle; begin #1;compare();',
           'if(n_requests_aw_valid && requests_aw_ready && n_requests_w_valid && requests_w_ready && w_bits_last)awFinalW++;',
           'if(reset && ((n_requests_aw_valid && requests_aw_ready)||(n_requests_w_valid && requests_w_ready)))resetAccepted++;',
           'if((n_requests_ar_valid && !requests_ar_ready)||(n_requests_aw_valid && !requests_aw_ready)||(n_requests_w_valid && !requests_w_ready))stalls++;',
           'if(!relaxed && hostReadInflight && !hostMemIdle && n_requests_ar_valid && requests_ar_ready)readOverlap++;',
           'if(coupled && ((readDone&&!s_ar)||(writeDone&&!s_br)))fullStalls++;',
           'if(coupled && s_qvalid && n_order_ready)retirements++;',
           'if(relaxed)relaxedCycles++;else strictCycles++;',
           'clock=1;#1;clock=0;#1;cycles++;compare();end endtask',
           'task randomize_inputs;begin reset=($urandom%997)==0;']
    for n,(direction,width) in cp.items():
        if direction=='input' and n not in ('clock','reset','order_valid','order_bits'):
            tb.append(n+" = {"+','.join('$urandom' for _ in range((width+31)//32))+'};')
    tb += ['orderValid=$urandom;orderBits=$urandom;readDone=$urandom;writeDone=$urandom;end endtask',
           'initial begin clock=1;#1;clock=0;#1;reset=0;',
           'repeat(40000)begin randomize_inputs();cycle();end',
           'coupled=1;reset=1;cycle();reset=0;relaxed=0;hostMemIdle=0;hostReadInflight=0;readDone=1;writeDone=1;repeat(24)cycle();',
           'repeat(40000)begin randomize_inputs();cycle();end',
           'if(awFinalW<100||resetAccepted<5||stalls<100||readOverlap<10||strictCycles<1000||relaxedCycles<1000||fullStalls<100||retirements<100)$fatal(1,"coverage");',
           '$display("coverage cycles=%0d awFinalW=%0d resetAccepted=%0d stalls=%0d readOverlap=%0d strictCycles=%0d relaxedCycles=%0d fullStalls=%0d retirements=%0d",cycles,awFinalW,resetAccepted,stalls,readOverlap,strictCycles,relaxedCycles,fullStalls,retirements);',
           '$display("PASS NIC SFC host issue and coupled order retirement");$finish;end endmodule']
    recorded = []
    for n in ('DualQueue','Queue_10'):
        h,b=module(sfc,n);recorded.append('module '+n+'('+h+');\n'+b+'\nendmodule\n')
    (out/'sfc.sv').write_text('\n'.join(recorded));(out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','-j','4','--timing','--assert','-Wno-fatal','--top-module','check',
         '--Mdir',out/'obj','-o','check',out/'check.sv',native,out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation');result=(out/'simulation.log').read_text()
    assert 'PASS NIC SFC host issue and coupled order retirement' in result
    return native,dict(log=str(out/'simulation.log'),cycles=int(re.search(r'cycles=(\d+)',result)[1]),
                       exported_helpers=9,cycled_helpers=2,shared_sfc_payload_fields=shared_payload,result=result)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('candidate-dir','baseline','golden','export-tool','output-dir','report-json'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();before=a.candidate_dir/'post-fame-fased-ingress-order.mlir'
    after=a.candidate_dir/'post-fame-fased-ingress-issue.mlir';oracle(a.golden)
    rtl,behavior=differential(after,a.golden,a.export_tool,a.output_dir.resolve())
    report=dict(golden=str(a.golden.resolve()),golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
                candidate=str(after.resolve()),candidate_sha256=hashlib.sha256(after.read_bytes()).hexdigest(),
                issue=compare(before,after,a.baseline.read_text(),'GGFASEDIngressOrderWrapper','GGFASEDIngressIssueWrapper',rtl),
                behavioral=behavior,limitation=__doc__)
    a.report_json.write_text(json.dumps(report,indent=2)+'\n')
    print('PASS NIC issue wrapper, annotations, exhaustive gates and coupled SFC retirement differential')


if __name__=='__main__':main()
