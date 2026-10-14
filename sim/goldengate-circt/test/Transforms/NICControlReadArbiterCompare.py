#!/usr/bin/env python3
"""Compare the production NIC R arbiter with the archived SFC HellaPeekingArbiter.

Normal SFC MMIO responses specialize last=1, resp=0 and user=0; error data=0,
resp=3 and user=0. Keep those specializations for differential execution.
The native CTest exercises general payloads and nonfinal normal beats.
This boundary does not establish widget completion or NIC platform parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlReadDispatchCompare import BINDINGS
from NICControlReadTrackerCompare import tracker_wiring
from PrintControlResponsesCompare import clean, module, equations, instance, terms
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports


def transfer(value, copied, bound):
    old, new = 'GGControlReadTrackerWrapper', 'GGControlReadArbiterWrapper'
    if isinstance(value, str):
        if value == '~'+old: return '~'+new
        if not value.startswith('~'+old+'|'): return value
        suffix = value[len(old)+1:]; prefix = '|'+old+'>'
        if suffix.startswith(prefix):
            ref = suffix[len(prefix):]; name = re.split(r'[.\[]', ref)[0]
            tail = ref[len(name):]
            if name in copied and (name not in bound or tail == '.b' or tail.startswith('.b.')):
                suffix = '|'+new+'>'+ref
        return '~'+new+suffix
    if isinstance(value, list): return [transfer(x, copied, bound) for x in value]
    if isinstance(value, dict): return {k:transfer(x, copied, bound) for k,x in value.items()}
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('tracker', 'arbiter', 'golden', 'export-tool', 'output-dir'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    before, after = [modules(x.read_text()) for x in (a.tracker, a.arbiter)]
    old, new, helper = 'GGControlReadTrackerWrapper', 'GGControlReadArbiterWrapper', 'GGControlReadArbiter'
    assert set(after)-set(before) == {new, helper}
    assert all(after[n] == v for n,v in before.items()), 'prior module changed'
    bound = {i for i,_,_ in BINDINGS}; widget_ports = {n for _,n,_ in BINDINGS}
    consumed = {n for _,n,_ in native_ports(before[old]) if n.startswith('ctrl_error_r_')}
    consumed |= {f'ctrl_read_tracker_deq_{i}_{f}' for i in range(12) if i not in bound for f in ('valid', 'tag')}
    copied = [(d,n,t.rsplit(', r flip: ',1)[0]+'>' if n in widget_ports else t)
              for d,n,t in native_ports(before[old]) if n not in consumed]
    hp = native_ports(after[helper]); assert len(hp) == 100
    assert 'goldengate.readArbiterSources = 13 : i32' in after[helper]
    exposed = [(d,n,t) for d,n,t in hp[2:] if n.startswith('out_') or
               int(n.split('_')[1]) in set(range(12))-bound]
    assert native_ports(after[new]) == copied+[(d,'ctrl_read_arb_'+n,t) for d,n,t in exposed]
    expected = {('sim.'+n if d == 'in' else 'top.'+n):('top.'+n if d == 'in' else 'sim.'+n)
                for d,n,_ in copied if n not in widget_ports}
    expected.update({'top.'+n+'.b':'sim.'+n+'.b' for n in widget_ports})
    expected.update({'readArbiter.clock':'top.hostClock', 'readArbiter.reset':'top.hostReset'})
    for d,n,_ in exposed:
        expected['readArbiter.'+n if d == 'in' else 'top.ctrl_read_arb_'+n] = 'top.ctrl_read_arb_'+n if d == 'in' else 'readArbiter.'+n
    for i,name,widget in BINDINGS:
        row = f'name = "{widget}", port = "{name}", slave = {i} : i32'
        assert row in before[old] and row in after[new]
    for i,name in [(i,n) for i,n,_ in BINDINGS]+[(12,'ctrl_error')]:
        prefix = 'sim.ctrl_error_r_' if i == 12 else 'sim.'+name+'.r.'
        bits = prefix+('bits_' if i == 12 else 'bits.')
        expected[prefix+'ready'] = f'readArbiter.in_{i}_ready'
        expected[f'readArbiter.in_{i}_valid'] = prefix+'valid'
        for field in ('resp', 'data', 'last', 'id', 'user'):
            expected[f'readArbiter.in_{i}_bits_'+field] = bits+field
    for i in set(range(12))-bound:
        prefix = f'readArbiter.in_{i}_'
        expected[f'sim.ctrl_read_tracker_deq_{i}_valid'] = frozenset(prefix+x for x in ('ready', 'valid', 'bits_last'))
        expected[f'sim.ctrl_read_tracker_deq_{i}_tag'] = prefix+'bits_id'
    assert tracker_wiring(after[new]) == expected, 'production arbiter wiring differs'
    assert annos(a.arbiter) == transfer(annos(a.tracker), {n for _,n,_ in copied}, widget_ports)
    run([a.export_tool, a.tracker, out/'production.mlir', out/'arbiter.mlir'], 'production-test')
    assert modules((out/'production.mlir').read_text()) == after, 'production pass differs'
    assert modules((out/'arbiter.mlir').read_text())[helper] == after[helper], 'export rebuilt helper'
    run(['firtool', out/'arbiter.mlir', '--format=mlir', '--disable-all-randomization', '-o', out/'arbiter.sv'], 'firtool')

    golden = clean(a.golden.read_text()); _,router = module(golden, 'NastiRouter'); eq = equations(router)
    kind = re.search(r'(\w+) r_arb \(', router)[1]; sh,sb = module(golden, kind)
    si = instance(router, kind, 'r_arb'); assert set(si) == set(ports(sh))
    assert re.search(r'reg\s+\[3:0\]\s+lockIdx;', sb) and re.search(r'reg\s+locked;', sb)
    _,top = module(golden, 'FPGATop'); te = equations(top)
    for i,_,widget in BINDINGS:
        for field in ('ready', 'valid', 'bits_data', 'bits_id'):
            dest = widget+'_io_ctrl_r_ready' if field == 'ready' else f'ctrlInterconnect_io_slaves_{i}_r_'+field
            src = f'ctrlInterconnect_io_slaves_{i}_r_ready' if field == 'ready' else widget+'_io_ctrl_r_'+field
            assert te[dest] == src, 'SFC bank allocation differs'
    for i in range(13):
        prefix = 'err_slave_io_r_' if i == 12 else f'io_slave_{i}_r_'
        ap = f'r_arb_io_in_{i}_'
        assert eq[prefix+'ready'] == ap+'ready' and eq[ap+'valid'] == prefix+'valid'
        for field in (('last','id') if i == 12 else ('data','id')):
            assert eq[ap+'bits_'+field] == prefix+'bits_'+field
        names = {prefix+'ready':'ready',prefix+'valid':'valid',prefix+'bits_last':'last'}
        assert terms(eq[f'ar_queue_io_deq_{i}_valid'],eq,names) == ({'ready','valid','last'} if i == 12 else {'ready','valid'})
        assert eq[f'ar_queue_io_deq_{i}_tag'] == prefix+'bits_id'
    assert eq['r_arb_reset'] == 'reset' and eq['r_arb_clock'] == 'clock'
    assert eq['r_arb_io_out_ready'] == 'io_master_r_ready'
    for field in ('valid','bits_resp','bits_data','bits_last','bits_id'):
        assert eq['io_master_r_'+field] == 'r_arb_io_out_'+field
    # Use the entire original helper body, including its sequential lock logic.
    (out/'sfc.sv').write_text('module '+kind+'('+sh+');'+sb+'endmodule\n')
    tb = ['module check;reg clock=0,reset,ready,last;reg [12:0] rv;reg [31:0] data[13];reg [11:0] ids[13];',
          'wire [12:0] nr,sr;wire nv,sv,nl,sl,nu;wire [1:0] np,sp;wire [31:0] nd,sd;wire [11:0] ni,si;']
    nc = {'clock':'clock','reset':'reset','out_ready':'ready','out_valid':'nv',
          'out_bits_resp':'np','out_bits_data':'nd','out_bits_last':'nl','out_bits_id':'ni','out_bits_user':'nu'}
    sc = {'clock':'clock','reset':'reset','io_out_ready':'ready','io_out_valid':'sv',
          'io_out_bits_resp':'sp','io_out_bits_data':'sd','io_out_bits_last':'sl','io_out_bits_id':'si'}
    for i in range(13):
        n = f'in_{i}_'; s = 'io_'+n
        nc.update({n+'ready':f'nr[{i}]',n+'valid':f'rv[{i}]',n+'bits_resp':"2'd3" if i == 12 else "2'd0",
                   n+'bits_data':"32'd0" if i == 12 else f'data[{i}]',n+'bits_last':'last' if i == 12 else "1'b1",
                   n+'bits_id':f'ids[{i}]',n+'bits_user':"1'b0"})
        sc.update({s+'ready':f'sr[{i}]',s+'valid':f'rv[{i}]',s+'bits_id':f'ids[{i}]',
                   s+('bits_last' if i == 12 else 'bits_data'):'last' if i == 12 else f'data[{i}]'})
    assert set(sc) == set(si)
    for typ,name,pins in [(helper,'native',nc),(kind,'sfc',sc)]:
        tb.append(typ+' '+name+'('+','.join('.'+k+'('+v+')' for k,v in pins.items())+');')
    tb += ["reg [63:0] random_state=64'h185ab21;integer cursor=0,cases=0,fires=0,stalls=0,gaps=0,nonfinal=0,resets=0;bit locked=0;reg [12:0] seen=0;",
           'function automatic [63:0] randomize_input();random_state=random_state^(random_state<<13);random_state=random_state^(random_state>>7);random_state=random_state^(random_state<<17);return random_state;endfunction',
           'task automatic sample();integer chosen;reg [12:0] er;bit valid,finalbeat;clock=0;#1;',
           'if({nr,nv,np,nd,nl,ni,nu}!=={sr,sv,sp,sd,sl,si,1\'b0})$fatal(1,"SFC/native arbiter differs cycle %0d cursor %0d locked %0b",cases,cursor,locked);',
           'chosen=cursor;if(!locked)for(integer offset=13;offset>0;offset--)if(rv[(cursor+offset)%13])chosen=(cursor+offset)%13;',
           'valid=rv[chosen];finalbeat=chosen==12?last:1;er=ready?(13\'b1<<chosen):0;',
           'if(nr!==er || nv!==valid || ni!==ids[chosen] || nd!==(chosen==12?0:data[chosen]) || np!==(chosen==12?2\'d3:2\'d0) || nl!==finalbeat)$fatal(1,"scoreboard selection differs");',
           'for(integer i=0;i<13;i++)if((nr[i]&&rv[i]&&(i!=12||last))!==(sr[i]&&rv[i]&&(i!=12||last)))$fatal(1,"final retirement differs");',
           'fires+=valid&&ready;stalls+=valid&&!ready;gaps+=locked&&!valid;nonfinal+=valid&&ready&&!finalbeat;resets+=reset;',
           'if(valid&&ready)begin seen[chosen]=1;if(!locked)cursor=chosen;locked=!finalbeat;end if(reset)begin cursor=0;locked=0;end clock=1;#1;clock=0;cases++;endtask',
           'task automatic defaults();reset=0;ready=0;rv=0;last=1;for(integer i=0;i<13;i++)begin data[i]=randomize_input();ids[i]=randomize_input();end endtask',
           'initial begin defaults();reset=1;#1;clock=1;#1;clock=0;sample();',
           # Hold each reachable cursor while exhausting every valid-source mask.
           'for(integer start=0;start<13;start++)begin defaults();reset=1;sample();defaults();rv=13\'b1<<start;ready=1;sample();ready=0;for(integer mask=0;mask<8192;mask++)begin rv=13\'(mask);sample();end end',
           'defaults();rv=13\'b1<<12;ready=1;last=0;sample();rv=13\'hfff;sample();rv=13\'h1fff;ready=0;last=1;sample();ready=1;sample();',
           'last=0;rv=13\'b1<<12;sample();reset=1;sample();defaults();ready=1;rv=13\'h1fff;for(integer i=0;i<26;i++)sample();',
           'for(integer n=0;n<50000;n++)begin defaults();reset=n%397==0;ready=randomize_input();rv=randomize_input();last=randomize_input();sample();end',
           'if(seen!==13\'h1fff || fires<1000 || stalls<1000 || gaps<100 || nonfinal<100 || resets<100)$fatal(1,"missing arbitration coverage");',
           '$display("PASS NIC SFC/native R arbiter: %0d cycles, 8192 masks at all 13 cursors, %0d fires, %0d stalls, %0d locked gaps, %0d nonfinal beats, %0d resets; all 13 sources",cases,fires,stalls,gaps,nonfinal,resets);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'arbiter.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    digest = lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
    report = dict(golden=str(a.golden),golden_sha256=digest(a.golden),candidate=str(a.arbiter),candidate_sha256=digest(a.arbiter),
                  preserved_modules=len(before),copied_ports=len(copied),connections=len(expected),annotations=len(annos(a.arbiter)),
                  sources=13,sfc_bank_connections=32,sfc_retirement_expressions=13,sfc_helper=kind,
                  production_test=(out/'production-test.log').read_text(),simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])


if __name__ == '__main__': main()
