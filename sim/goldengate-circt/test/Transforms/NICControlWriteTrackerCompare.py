#!/usr/bin/env python3
"""Compare production NIC AW tracking with the archived SFC reorder queue.

The acceptance and B-retirement signals are the tested helper boundary.
SFC removes unused route storage; native route data uses an independent
scoreboard. Invalid retirement assertions are covered by the native CTest.
This does not establish remaining widget completion or NIC platform parity.
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
from NICControlWriteRoutingCompare import transfer
from PrintControlResponsesCompare import clean, module, equations, instance, terms
from PrintRocketControlMasterCompare import native_ports


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('write-arbiter', 'tracker', 'golden', 'export-tool', 'output-dir'):
        p.add_argument('--'+n, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    before, after = [modules(x.read_text()) for x in (a.write_arbiter, a.tracker)]
    old, new, helper = 'GGControlWriteArbiterWrapper', 'GGControlWriteTrackerWrapper', 'GGControlWriteTracker'
    assert set(after)-set(before) == {new, helper}
    assert all(after[n] == v for n,v in before.items()), 'prior module changed'
    consumed = {'ctrl_write_route_aw_tracker_ready', 'ctrl_write_route_aw_track_valid'}
    consumed |= {f'ctrl_write_tracker_deq_{i}_{f}' for i in range(13) for f in ('valid', 'tag')}
    copied = [x for x in native_ports(before[old]) if x[1] not in consumed]
    hp = native_ports(after[helper]); assert len(hp) == 58
    for key, value in [('Slots',64), ('TagWidth',12), ('DequeuePorts',13), ('RouteWidth',4)]:
        assert f'goldengate.tracker{key} = {value} : i32' in after[helper]
    exposed = [(d,n,t) for d,n,t in hp if d == 'out' and n.startswith('deq_')]
    assert native_ports(after[new]) == copied+[(d,'ctrl_write_tracker_'+n,t) for d,n,t in exposed]
    expected = {('sim.'+n if d == 'in' else 'top.'+n):('top.'+n if d == 'in' else 'sim.'+n)
                for d,n,_ in copied}
    expected.update({'controlWriteTracker.clock':'top.hostClock', 'controlWriteTracker.reset':'top.hostReset',
                     'controlWriteTracker.enq_valid':'sim.ctrl_write_route_aw_track_valid',
                     'controlWriteTracker.enq_bits_tag':'top.ctrl_write_dispatch_master_aw_bits_id',
                     'controlWriteTracker.enq_bits_data':'sim.ctrl_decode_aw_target',
                     'sim.ctrl_write_route_aw_tracker_ready':'controlWriteTracker.enq_ready'})
    for _,n,_ in exposed: expected['top.ctrl_write_tracker_'+n] = 'controlWriteTracker.'+n
    for i in range(13):
        for f in ('valid','tag'):
            expected[f'controlWriteTracker.deq_{i}_{f}'] = f'sim.ctrl_write_tracker_deq_{i}_{f}'
    assert tracker_wiring(after[new]) == expected, 'production tracker connection differs'
    assert annos(a.tracker) == transfer(annos(a.write_arbiter), old, new, {n for _,n,_ in copied}), 'annotation target transfer differs'
    run([a.export_tool, a.write_arbiter, out/'production.mlir', out/'tracker.mlir'], 'production-test')
    assert modules((out/'production.mlir').read_text()) == after, 'standalone production pass differs'
    assert modules((out/'tracker.mlir').read_text())[helper] == after[helper], 'export rebuilt helper'
    run(['firtool', out/'tracker.mlir', '--format=mlir', '--disable-all-randomization', '-o', out/'tracker.sv'], 'firtool')

    golden = clean(a.golden.read_text()); _,router = module(golden,'NastiRouter'); eq = equations(router)
    kind = re.search(r'(\w+) aw_queue \(',router)[1]
    qh,qb = module(golden,kind); qi = instance(router,kind,'aw_queue')
    assert len(re.findall(r'reg\s+roq_free_\d+;',qb)) == 64, 'SFC slot count differs'
    assert len(re.findall(r'reg\s+\[5:0\]\s+roq_tags_\d+;',qb)) == 64, 'SFC alias tag geometry differs'
    assert not re.search(r'\broq_data_\d+\b',qb), 'SFC route data no longer optimized away'
    assert eq[qi['clock']] == 'clock' and eq[qi['reset']] == 'reset'
    assert eq[qi['io_enq_bits_tag']] == 'io_master_aw_bits_id'
    assert terms(eq[qi['io_enq_valid']],eq,{'io_master_aw_valid':'valid','w_queue_io_enq_ready':'route_capacity','aw_ready':'destination_ready'}) == {'valid','route_capacity','destination_ready'}
    assert terms(eq['io_master_aw_ready'],eq,{'aw_queue_io_enq_ready':'tracker_capacity','w_queue_io_enq_ready':'route_capacity','aw_ready':'destination_ready'}) == {'tracker_capacity','route_capacity','destination_ready'}
    for i in range(13):
        prefix = 'err_slave_io_' if i == 12 else f'io_slave_{i}_'
        assert terms(eq[qi[f'io_deq_{i}_valid']],eq,{prefix+'b_ready':'ready',prefix+'b_valid':'valid'}) == {'ready','valid'}
        assert eq[qi[f'io_deq_{i}_tag']] == prefix+'b_bits_id'
    _,top = module(golden,'FPGATop'); te = equations(top)
    for i,_,widget in BINDINGS:
        for field in ('ready','valid','bits_id'):
            dest = f'ctrlInterconnect_io_slaves_{i}_b_'+field if field != 'ready' else widget+'_io_ctrl_b_ready'
            src = widget+'_io_ctrl_b_'+field if field != 'ready' else f'ctrlInterconnect_io_slaves_{i}_b_ready'
            assert te[dest] == src, 'SFC bank allocation differs'
    # Preserve the entire SFC queue body, including its sequential updates.
    (out/'sfc.sv').write_text('module '+kind+'('+qh+');'+qb+'endmodule\n')
    tb = ['module check;reg clock=0,reset,enq;reg [11:0] tag;reg [3:0] route;',
          'reg [12:0] retire;reg [11:0] ids[13];wire nr,sr;wire [12:0] nm,sm;wire [3:0] nd[13];']
    nt = {'clock':'clock','reset':'reset','enq_valid':'enq','enq_bits_tag':'tag','enq_bits_data':'route','enq_ready':'nr'}
    sc = {'clock':'clock','reset':'reset','io_enq_valid':'enq','io_enq_bits_tag':'tag','io_enq_ready':'sr'}
    for i in range(13):
        nt.update({f'deq_{i}_valid':f'retire[{i}]',f'deq_{i}_tag':f'ids[{i}]',f'deq_{i}_data':f'nd[{i}]',f'deq_{i}_matches':f'nm[{i}]'})
        sc.update({f'io_deq_{i}_valid':f'retire[{i}]',f'io_deq_{i}_tag':f'ids[{i}]',f'io_deq_{i}_matches':f'sm[{i}]'})
    assert set(sc) == set(qi)
    for k,n,conn in [(helper,'tracker',nt),(kind,'sfc',sc)]:
        tb.append(k+' '+n+'('+','.join('.'+p+'('+v+')' for p,v in conn.items())+');')
    tb += ["reg [63:0] random_state=64'h187ab21;bit free_slot[64],known[64];reg [5:0] high_tag[64];reg [3:0] data[64];",
        'integer cases=0,accepts=0,stalls=0,retires=0,held_matches=0,simultaneous=0,resets=0;reg [12:0] seen=0;',
        'function automatic [63:0] randomize_input();random_state=random_state^(random_state<<13);random_state=random_state^(random_state>>7);random_state=random_state^(random_state<<17);return random_state;endfunction',
        'task automatic sample();bit push,pop;integer slot;clock=0;#1;',
        'if({nr,nm}!=={sr,sm})$fatal(1,"SFC/native AW tracker differs cycle %0d",cases);',
        'if(nr!==free_slot[tag[5:0]])$fatal(1,"alias capacity differs");',
        'for(integer i=0;i<13;i++)begin slot=ids[i][5:0];if(nm[i]!==(!free_slot[slot]&&high_tag[slot]==ids[i][11:6]))$fatal(1,"tag alias match differs");if(known[slot]&&nd[i]!==data[slot])$fatal(1,"native route storage differs");held_matches+=nm[i]&&!retire[i];end',
        'push=enq&&nr;accepts+=push;stalls+=enq&&!nr;resets+=reset;',
        'if(push)begin slot=tag[5:0];free_slot[slot]=0;high_tag[slot]=tag[11:6];data[slot]=route;known[slot]=1;end',
        'pop=0;for(integer i=0;i<13;i++)if(retire[i])begin free_slot[ids[i][5:0]]=1;retires++;seen[i]=1;pop=1;end',
        'simultaneous+=push&&pop;if(reset)for(integer i=0;i<64;i++)free_slot[i]=1;clock=1;#1;clock=0;cases++;endtask',
        'task automatic defaults();reset=0;enq=0;tag=0;route=12;retire=0;for(integer i=0;i<13;i++)ids[i]=0;endtask',
        'initial begin defaults();for(integer i=0;i<64;i++)begin free_slot[i]=1;known[i]=0;end reset=1;#1;clock=1;#1;clock=0;sample();reset=0;',
        'for(integer i=0;i<64;i++)begin enq=1;tag=12\'(i+64*13);route=4\'(i%13);sample();end',
        'for(integer i=0;i<64;i++)begin tag=12\'(i+64*14);sample();end',
        # All 64 occupied aliases stall at the retirement edge, then accept on
        # the following edge. Each response can match even while not accepted.
        'for(integer i=0;i<64;i++)begin defaults();enq=1;tag=12\'(i+64*14);route=4\'(i%13);ids[route]=12\'(i+64*13);sample();retire[route]=1;sample();retire=0;sample();end',
        # Reset only initializes free bits. Tag and data still capture enqueue.
        'defaults();reset=1;enq=1;tag=4095;sample();route=5;retire=13\'h1fff;for(integer i=0;i<13;i++)ids[i]=4095;sample();defaults();',
        'for(integer endpoint=0;endpoint<13;endpoint++)begin defaults();enq=1;tag=12\'(2048+endpoint);route=4\'(endpoint);sample();enq=0;retire[endpoint]=1;ids[endpoint]=tag;sample();end',
        'for(integer n=0;n<100000;n++)begin defaults();reset=n%397==0;enq=randomize_input();tag=randomize_input();route=4\'(randomize_input()%16);',
        'for(integer i=0;i<13;i++)begin integer slot;slot=integer\'(randomize_input()%64);ids[i]={high_tag[slot],6\'(slot)};retire[i]=!free_slot[slot]&&(randomize_input()%8==0);end sample();end',
        'if(seen!==13\'h1fff || accepts<1000 || stalls<1000 || retires<1000 || simultaneous<100 || held_matches<100 || resets<100)$fatal(1,"missing coverage");',
        '$display("PASS NIC SFC/native AW tracker: %0d cycles, %0d accepts, %0d alias stalls, %0d retires, %0d held matches, %0d simultaneous, %0d resets; all 13 endpoints",cases,accepts,stalls,retires,held_matches,simultaneous,resets);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'tracker.sv',out/'sfc.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = dict(golden=str(a.golden),golden_sha256=digest(a.golden),candidate=str(a.tracker),candidate_sha256=digest(a.tracker),preserved_modules=len(before),copied_ports=len(copied),connections=len(expected),annotations=len(annos(a.tracker)),sfc_bank_connections=24,sfc_retirement_ports=13,production_test=(out/'production-test.log').read_text(),simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])


if __name__ == '__main__': main()
