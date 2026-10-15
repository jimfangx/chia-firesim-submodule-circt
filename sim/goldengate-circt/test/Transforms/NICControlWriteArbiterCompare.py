#!/usr/bin/env python3
"""Compare production NIC or quad TracerV B arbitration with archived SFC RTL.

SFC specializes normal responses to resp=0 and error responses to resp=3,
with user=0. The native CTest covers general response payloads separately.
Both arbiters retain unreset lastGrant; test every representable initial
state by depositing the same state into the actual native and SFC registers.
This boundary does not establish write tracking or full target/platform parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlReadDispatchCompare import BINDINGS, QUAD_BINDINGS
from NICControlReadTrackerCompare import tracker_wiring
from NICControlWriteRoutingCompare import transfer
from PrintControlResponsesCompare import clean, module, equations, instance, terms
from PrintControlMasterCompare import ports
from PrintRocketControlMasterCompare import native_ports


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('read-arbiter', 'write-arbiter', 'golden', 'export-tool', 'output-dir'):
        p.add_argument('--'+n, type=Path, required=True)
    p.add_argument('--quad', action='store_true', help='compare four TracerV banks and fourteen normal regions')
    a = p.parse_args(); count = 14 if a.quad else 12; sources = count + 1
    bindings = QUAD_BINDINGS if a.quad else BINDINGS
    index_width = (sources - 1).bit_length(); masks = 1 << sources
    label = 'quad TracerV' if a.quad else 'NIC'
    out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    before, after = [modules(x.read_text()) for x in (a.read_arbiter, a.write_arbiter)]
    old, new, helper = 'GGControlReadArbiterWrapper', 'GGControlWriteArbiterWrapper', 'GGControlWriteArbiter'
    assert set(after)-set(before) == {new, helper}
    assert all(after[n] == v for n,v in before.items()), 'prior module changed'
    bound = {i for i,_,_ in bindings}; widget_ports = {n for _,n,_ in bindings}
    copied = [(d,n,t) for d,n,t in native_ports(before[old])
              if n not in widget_ports and not n.startswith('ctrl_error_b_')]
    hp = native_ports(after[helper]); assert len(hp) == 5*sources+7
    assert f'goldengate.writeArbiterSources = {sources} : i32' in after[helper]
    exposed = [(d,n,t) for d,n,t in hp[1:] if n.startswith('out_') or n == 'chosen' or
               int(n.split('_')[1]) in set(range(count))-bound]
    additions = [(d,'ctrl_write_arb_'+n,t) for d,n,t in exposed]
    additions += [('out',f'ctrl_write_tracker_deq_{i}_{f}',f'!firrtl.uint<{w}>')
                  for i in range(sources) for f,w in (('valid',1),('tag',12))]
    assert native_ports(after[new]) == copied+additions, 'production boundary differs'
    expected = {('sim.'+n if d == 'in' else 'top.'+n):('top.'+n if d == 'in' else 'sim.'+n)
                for d,n,_ in copied}
    expected['writeArbiter.clock'] = 'top.hostClock'
    for d,n,_ in exposed:
        expected['writeArbiter.'+n if d == 'in' else 'top.ctrl_write_arb_'+n] = 'top.ctrl_write_arb_'+n if d == 'in' else 'writeArbiter.'+n
    for i,name,widget in bindings:
        row = f'name = "{widget}", port = "{name}", slave = {i} : i32'
        assert row in before[old]
        assert f'instance = "sim", {row}' in after[new], 'widget response identity differs'
    for i,name in [(i,n) for i,n,_ in bindings]+[(count,'ctrl_error')]:
        prefix = 'sim.ctrl_error_b_' if i == count else 'sim.'+name+'.b.'
        bits = prefix+('bits_' if i == count else 'bits.')
        expected[prefix+'ready'] = f'writeArbiter.in_{i}_ready'
        expected[f'writeArbiter.in_{i}_valid'] = prefix+'valid'
        for field in ('resp', 'id', 'user'):
            expected[f'writeArbiter.in_{i}_bits_'+field] = bits+field
    for i in range(sources):
        prefix = f'writeArbiter.in_{i}_'
        expected[f'top.ctrl_write_tracker_deq_{i}_valid'] = frozenset(prefix+x for x in ('ready', 'valid'))
        expected[f'top.ctrl_write_tracker_deq_{i}_tag'] = prefix+'bits_id'
    assert tracker_wiring(after[new]) == expected, 'production B wiring differs'
    assert annos(a.write_arbiter) == transfer(annos(a.read_arbiter), old, new, {n for _,n,_ in copied})
    run([a.export_tool, a.read_arbiter, out/'production.mlir', out/'arbiter.mlir'], 'production-test')
    assert modules((out/'production.mlir').read_text()) == after, 'production pass differs'
    assert modules((out/'arbiter.mlir').read_text())[helper] == after[helper], 'export rebuilt helper'
    run(['firtool', out/'arbiter.mlir', '--format=mlir', '--disable-all-randomization', '-o', out/'arbiter.sv'], 'firtool')

    golden = clean(a.golden.read_text()); _,router = module(golden, 'NastiRouter'); eq = equations(router)
    kind = re.search(r'(\w+) b_arb \(', router)[1]; sh,sb = module(golden, kind)
    si = instance(router, kind, 'b_arb'); assert set(si) == set(ports(sh))
    state = re.search(rf'reg\s+\[{index_width-1}:0\]\s+(\w*lastGrant);', sb)[1]
    assert 'reset' not in ports(sh), 'SFC RR state unexpectedly reset'
    _,top = module(golden, 'FPGATop'); te = equations(top)
    for i,_,widget in bindings:
        for field in ('ready', 'valid', 'bits_id'):
            dest = widget+'_io_ctrl_b_ready' if field == 'ready' else f'ctrlInterconnect_io_slaves_{i}_b_'+field
            src = f'ctrlInterconnect_io_slaves_{i}_b_ready' if field == 'ready' else widget+'_io_ctrl_b_'+field
            assert te[dest] == src, 'SFC bank allocation differs'
    for i in range(sources):
        prefix = 'err_slave_io_b_' if i == count else f'io_slave_{i}_b_'
        ap = f'b_arb_io_in_{i}_'
        assert eq[prefix+'ready'] == ap+'ready' and eq[ap+'valid'] == prefix+'valid'
        assert eq[ap+'bits_id'] == prefix+'bits_id'
        names = {prefix+'ready':'ready',prefix+'valid':'valid'}
        assert terms(eq[f'aw_queue_io_deq_{i}_valid'],eq,names) == {'ready','valid'}
        assert eq[f'aw_queue_io_deq_{i}_tag'] == prefix+'bits_id'
    assert eq['b_arb_clock'] == 'clock' and eq['b_arb_io_out_ready'] == 'io_master_b_ready'
    for field in ('valid','bits_resp','bits_id'):
        assert eq['io_master_b_'+field] == 'b_arb_io_out_'+field
    # Keep the whole original helper, including its unreset sequential state.
    (out/'sfc.sv').write_text('module '+kind+'('+sh+');'+sb+'endmodule\n')
    tb = [f'module check;reg clock=0,ready;reg [{sources-1}:0] rv;reg [11:0] ids[{sources}];',
          f'wire [{sources-1}:0] nr,sr;wire nv,sv,nu;wire [1:0] np,sp;wire [11:0] ni,si;wire [{index_width-1}:0] nc,sc;']
    nc = {'clock':'clock','out_ready':'ready','out_valid':'nv','out_bits_resp':'np',
          'out_bits_id':'ni','out_bits_user':'nu','chosen':'nc'}
    sc = {'clock':'clock','io_out_ready':'ready','io_out_valid':'sv',
          'io_out_bits_resp':'sp','io_out_bits_id':'si','io_chosen':'sc'}
    for i in range(sources):
        n = f'in_{i}_'; s = 'io_'+n
        nc.update({n+'ready':f'nr[{i}]',n+'valid':f'rv[{i}]',n+'bits_resp':"2'd3" if i == count else "2'd0",
                   n+'bits_id':f'ids[{i}]',n+'bits_user':"1'b0"})
        sc.update({s+'ready':f'sr[{i}]',s+'valid':f'rv[{i}]',s+'bits_id':f'ids[{i}]'})
    assert set(sc) == set(si)
    for typ,name,pins in [(helper,'native',nc),(kind,'sfc',sc)]:
        tb.append(typ+' '+name+'('+','.join('.'+k+'('+v+')' for k,v in pins.items())+');')
    tb += [f"reg [63:0] random_state=64'h186ab21;integer cursor=0,cases=0,fires=0,stalls=0,empty=0,multi_ready=0;reg [{sources-1}:0] seen=0;",
           'function automatic [63:0] randomize_input();random_state=random_state^(random_state<<13);random_state=random_state^(random_state>>7);random_state=random_state^(random_state<<17);return random_state;endfunction',
           f'task automatic sample();integer chosen,accepted;reg [{sources-1}:0] er;bit valid,maskedAny,earlierMasked,earlierValid;clock=0;#1;',
           'if({nr,nv,np,ni,nu,nc}!=={sr,sv,sp,si,1\'b0,sc})$fatal(1,"SFC/native B arbiter differs cycle %0d cursor %0d",cases,cursor);',
           f'chosen={count};for(integer i={count};i>=0;i--)if(rv[i])chosen=i;for(integer i={count};i>cursor;i--)if(rv[i])chosen=i;',
           f'maskedAny=0;for(integer i=0;i<{sources};i++)maskedAny|=(i>cursor)&&rv[i];earlierMasked=0;earlierValid=0;er=0;accepted=0;',
           f'for(integer i=0;i<{sources};i++)begin er[i]=ready&&((!earlierMasked&&i>cursor)||(!maskedAny&&!earlierValid));earlierMasked|=(i>cursor)&&rv[i];earlierValid|=rv[i];accepted+=nr[i]&&rv[i];if((nr[i]&&rv[i])!==(sr[i]&&rv[i]))$fatal(1,"retirement differs");end',
           f'valid=rv[chosen];if(nr!==er || nv!==valid || nc!==chosen || ni!==ids[chosen] || np!==(chosen=={count}?2\'d3:2\'d0) || accepted!=int\'(valid&&ready))$fatal(1,"scoreboard differs");',
           'fires+=valid&&ready;stalls+=valid&&!ready;empty+=!valid;multi_ready+=$countones(nr)>1;',
           f'if(valid&&ready)begin seen[chosen]=1;cursor=chosen;end clock=1;#1;if(native.lastGrant!=={index_width}\'(cursor)||sfc.{state}!=={index_width}\'(cursor))$fatal(1,"RR state hold/advance differs");clock=0;cases++;endtask',
           f'task automatic defaults();ready=0;rv=0;for(integer i=0;i<{sources};i++)ids[i]=randomize_input();endtask',
           'initial begin defaults();',
           # No reset exists: deposit every representable state, including unused indices.
           f'for(integer start=0;start<{1<<index_width};start++)begin',
           f'defaults();cursor=start;native.lastGrant={index_width}\'(start);sfc.{state}={index_width}\'(start);#1;',
           f'for(integer mask=0;mask<{masks};mask++)begin ready=0;rv={sources}\'(mask);sample();end',
           # Accept every mask separately from the same initial state.
           f'for(integer mask=0;mask<{masks};mask++)begin clock=0;cursor=start;native.lastGrant={index_width}\'(start);sfc.{state}={index_width}\'(start);ready=1;rv={sources}\'(mask);for(integer i=0;i<{sources};i++)ids[i]=randomize_input();sample();end end',
           f'defaults();ready=1;rv={sources}\'h{masks-1:x};for(integer i=0;i<{10*sources};i++)sample();',
           'for(integer n=0;n<50000;n++)begin defaults();ready=randomize_input();rv=randomize_input();sample();end',
           f'if(seen!=={sources}\'h{masks-1:x} || fires<1000 || stalls<1000 || empty<10 || multi_ready<1000)$fatal(1,"missing arbitration coverage");',
           f'$display("PASS {label} SFC/native B arbiter: %0d cycles, {masks} masks at all {1<<index_width} initial states and both ready values, %0d fires, %0d stalls, %0d empty, %0d multi-ready; all {sources} sources",cases,fires,stalls,empty,multi_ready);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'arbiter.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    digest = lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
    report = dict(golden=str(a.golden),golden_sha256=digest(a.golden),candidate=str(a.write_arbiter),candidate_sha256=digest(a.write_arbiter),
                  preserved_modules=len(before),copied_ports=len(copied),connections=len(expected),annotations=len(annos(a.write_arbiter)),
                  normal_regions=count,bound_widgets=bindings,sources=sources,initial_states=1<<index_width,sfc_bank_connections=3*len(bindings),sfc_retirement_expressions=sources,sfc_helper=kind,
                  production_test=(out/'production-test.log').read_text(),simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])


if __name__ == '__main__': main()
