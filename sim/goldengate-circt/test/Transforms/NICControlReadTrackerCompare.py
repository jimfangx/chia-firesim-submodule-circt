#!/usr/bin/env python3
"""Compare production NIC or quad Rocket AR tracking with the archived SFC reorder queue.

Response readiness is the arbitration boundary. Normal SFC MMIO R.last is
optimized to one; the native wrapper's general last gating is checked separately.
SFC removes unused route storage, so native data is checked against a scoreboard.
This does not establish R arbitration, widget completion or platform parity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from NICControlReadDispatchCompare import BINDINGS, QUAD_BINDINGS, FIELDS
from NICControlWriteRoutingCompare import transfer
from PrintControlResponsesCompare import clean, module, equations, instance, terms
from PrintRocketControlMasterCompare import native_ports


def tracker_wiring(body):
    # Expand only the wrapper's actual AND operations to compare final-beat
    # retirement without depending on SSA numbering or rebuilding a helper.
    values = {v:'top.'+v[1:] for v in re.findall(r'%\w+(?=:)',body.splitlines()[0])}
    result = {}
    for line in body.splitlines()[1:]:
        if ' = firrtl.instance ' in line:
            left,right=line.split(' = firrtl.instance ',1)
            values.update(zip(re.findall(r'%\w+',left),(right.split()[0]+'.'+p for p in re.findall(r'\b(?:in|out) (\w+):',right))))
        if m:=re.search(r'(%\w+) = firrtl.subfield (%\w+)\[(\w+)\]',line):values[m[1]]=values[m[2]]+'.'+m[3]
        if m:=re.search(r'(%\w+) = firrtl.and (%\w+), (%\w+)',line):
            flatten=lambda x:x if isinstance(x,frozenset) else frozenset([x])
            values[m[1]]=flatten(values[m[2]])|flatten(values[m[3]])
        if m:=re.search(r'firrtl.(?:strictconnect|connect) (%\w+), (%\w+)',line):
            assert values[m[1]] not in result,'duplicate production driver'
            result[values[m[1]]]=values[m[2]]
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('reads','tracker','golden','export-tool','output-dir'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--quad',action='store_true',help='compare the archived quad Rocket 14-region geometry')
    a=p.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    bindings = QUAD_BINDINGS if a.quad else BINDINGS
    count = 14 if a.quad else 12
    endpoints = count + 1
    label = 'quad Rocket' if a.quad else 'NIC'
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    def annos(path):return json.loads(path.with_name(path.stem+'-all.json').read_text())
    before,after=[modules(x.read_text()) for x in (a.reads,a.tracker)]
    old,new='GGControlReadDispatchWrapper','GGControlReadTrackerWrapper'
    helper='GGControlReadTracker'
    assert set(after)-set(before)=={new,helper}
    assert all(after[n]==v for n,v in before.items()),'prior module body changed'
    consumed={'ctrl_read_dispatch_tracker_ready','ctrl_read_dispatch_track_valid','ctrl_read_dispatch_track_tag','ctrl_read_dispatch_track_target'}
    copied=[x for x in native_ports(before[old]) if x[1] not in consumed]
    hp=native_ports(after[helper]);assert len(hp)==6+4*endpoints
    for key,value in [('Slots',64),('TagWidth',12),('DequeuePorts',endpoints),('RouteWidth',4)]:
        assert f'goldengate.tracker{key} = {value} : i32' in after[helper]
    bound={i for i,_,_ in bindings}
    exposed=[x for index,x in enumerate(hp) if index>=6 and (x[0]=='out' or (index-6)//4 not in bound|{count})]
    assert native_ports(after[new])==copied+[(d,'ctrl_read_tracker_'+n,t) for d,n,t in exposed]
    expected={('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in copied}
    expected.update({'controlReadTracker.clock':'top.hostClock','controlReadTracker.reset':'top.hostReset',
                     'controlReadTracker.enq_valid':'sim.ctrl_read_dispatch_track_valid',
                     'controlReadTracker.enq_bits_tag':'sim.ctrl_read_dispatch_track_tag',
                     'controlReadTracker.enq_bits_data':'sim.ctrl_read_dispatch_track_target',
                     'sim.ctrl_read_dispatch_tracker_ready':'controlReadTracker.enq_ready'})
    for d,n,_ in exposed:expected['controlReadTracker.'+n if d=='in' else 'top.ctrl_read_tracker_'+n]='top.ctrl_read_tracker_'+n if d=='in' else 'controlReadTracker.'+n
    for i,name,widget in bindings:
        row=f'name = "{widget}", port = "{name}", slave = {i} : i32'
        assert row in before[old] and row in after[new]
        expected[f'controlReadTracker.deq_{i}_valid']=frozenset(['top.'+name+'.r.ready','sim.'+name+'.r.valid','sim.'+name+'.r.bits.last'])
        expected[f'controlReadTracker.deq_{i}_tag']='sim.'+name+'.r.bits.id'
    expected[f'controlReadTracker.deq_{count}_valid']=frozenset(['top.ctrl_error_r_ready','sim.ctrl_error_r_valid','sim.ctrl_error_r_bits_last'])
    expected[f'controlReadTracker.deq_{count}_tag']='sim.ctrl_error_r_bits_id'
    assert tracker_wiring(after[new])==expected,'production tracker connection differs'
    assert annos(a.tracker)==transfer(annos(a.reads),old,new,{n for _,n,_ in copied}),'annotation target transfer differs'
    run([a.export_tool,a.reads,out/'production.mlir',out/'dispatch.mlir',out/'tracker.mlir'],'production-test')
    assert modules((out/'production.mlir').read_text())==after,'standalone production pass differs'
    for name,file in [('GGControlReadDispatch','dispatch'),(helper,'tracker')]:
        assert modules((out/(file+'.mlir')).read_text())[name]==after[name],'export rebuilt helper'
        run(['firtool',out/(file+'.mlir'),'--format=mlir','--disable-all-randomization','-o',out/(file+'.sv')],file+'-firtool')

    golden=clean(a.golden.read_text());_,router=module(golden,'NastiRouter');eq=equations(router)
    qh,qb=module(golden,'ReorderQueue');qi=instance(router,'ReorderQueue','ar_queue')
    assert len(re.findall(r'reg\s+roq_free_\d+;',qb))==64,'SFC slot count differs'
    assert len(re.findall(r'reg\s+\[5:0\]\s+roq_tags_\d+;',qb))==64,'SFC alias tag geometry differs'
    _,top=module(golden,'FPGATop');te=equations(top)
    for i,_,widget in bindings:
        for field in ('ready','valid','bits_id'):
            dest=f'ctrlInterconnect_io_slaves_{i}_r_'+field if field!='ready' else widget+'_io_ctrl_r_ready'
            src=widget+'_io_ctrl_r_'+field if field!='ready' else f'ctrlInterconnect_io_slaves_{i}_r_ready'
            assert te[dest]==src,'SFC bank allocation differs'
    inputs={'clock':1,'reset':1,'ar_route':count,'io_master_ar_valid':1,'io_master_ar_bits_id':12,'err_slave_io_ar_ready':1,'err_slave_io_r_ready':1,'err_slave_io_r_valid':1,'err_slave_io_r_bits_last':1,'err_slave_io_r_bits_id':12}
    for i in range(count):
        for f,w in [('ar_ready',1),('r_ready',1),('r_valid',1),('r_bits_id',12)]:inputs[f'io_slave_{i}_'+f]=w
    outputs={'io_master_ar_ready':1,'ar_queue_io_enq_ready':1,'ar_queue_io_enq_valid':1,'ar_queue_io_enq_bits_tag':12}
    for i in range(endpoints):outputs[f'ar_queue_io_deq_{i}_matches']=1
    leaves=set(inputs)|{n for n in qi.values() if n.endswith(('matches','enq_ready'))}
    needed={}
    def visit(n):
        if n in leaves or n in needed:return
        assert n in eq,'unresolved SFC cone '+n
        needed[n]=eq[n]
        expr=re.sub(r"\d*'[sS]?[bBoOdDhH][0-9a-fA-F_xXzZ]+",'',eq[n])
        for dep in re.findall(r'\b[a-zA-Z_]\w*\b',expr):visit(dep)
    for n in set(outputs)|set(qi.values()):visit(n)
    for i in range(endpoints):
        prefix='err_slave_io_' if i==count else f'io_slave_{i}_'
        names={prefix+'r_ready':'ready',prefix+'r_valid':'valid',prefix+'r_bits_last':'last'}
        assert terms(eq[f'ar_queue_io_deq_{i}_valid'],eq,names)==({'ready','valid','last'} if i==count else {'ready','valid'})
        assert eq[f'ar_queue_io_deq_{i}_tag']==prefix+'r_bits_id'
    widths={n:w for w,n in re.findall(r'wire\s+\[(\d+:\d+)\]\s+(\w+)',router)}
    def decl(d,n,w):return f'{d} '+(f'[{w-1}:0] ' if w>1 else '')+n
    lines=['module SFCReadTracker('+','.join([decl('input',n,w) for n,w in inputs.items()]+[decl('output',n,w) for n,w in outputs.items()])+');']
    for n,expr in needed.items():lines.append(('assign ' if n in outputs else 'wire '+('['+widths[n]+'] ' if n in widths else ''))+n+' = '+expr+';')
    lines+=['ReorderQueue ar_queue('+','.join('.'+n+'('+v+')' for n,v in qi.items())+');','endmodule','module ReorderQueue('+qh+');'+qb+'endmodule']
    (out/'sfc.sv').write_text('\n'.join(lines)+'\n')

    # Compose the two cloned helpers with the exact enqueue/ready connections
    # checked above. Retirement is the ready/valid/last boundary checked above
    # and by all eight truth-table combinations in the native CTest.
    tb=[f'module check;localparam integer NORMAL={count},ENDPOINTS={endpoints};reg clock=0,reset,av;reg [NORMAL-1:0] route;reg [11:0] tag;reg [3:0] target;',
        'reg [NORMAL:0] arready,rv,rr,rl;reg [11:0] ids[ENDPOINTS];wire nr,sr,capacity,scapacity,track,st;wire [11:0] ntag,stag;wire [3:0] nroute;wire [NORMAL:0] nm,sm;wire [3:0] nd[ENDPOINTS];']
    nc={'route':'route','target':'target','tracker_ready':'capacity','master_ar_valid':'av','master_ar_ready':'nr','track_valid':'track','track_tag':'ntag','track_target':'nroute'}
    nc.update({'master_ar_bits_'+f:('tag' if f=='id' else "'0") for f in FIELDS})
    nt={'clock':'clock','reset':'reset','enq_valid':'track','enq_bits_tag':'ntag','enq_bits_data':'nroute','enq_ready':'capacity'}
    sc={'clock':'clock','reset':'reset','ar_route':'route','io_master_ar_valid':'av','io_master_ar_bits_id':'tag','io_master_ar_ready':'sr','ar_queue_io_enq_ready':'scapacity','ar_queue_io_enq_valid':'st','ar_queue_io_enq_bits_tag':'stag'}
    for i in range(endpoints):
        np='err_slave_ar_' if i==count else f'slave_{i}_ar_';sp='err_slave_io_' if i==count else f'io_slave_{i}_'
        nc[np+'ready']=f'arready[{i}]';nc[np+'valid']=''
        for f in FIELDS:nc[np+'bits_'+f]=''
        nt.update({f'deq_{i}_valid':f'(rv[{i}]&&rr[{i}]&&rl[{i}])',f'deq_{i}_tag':f'ids[{i}]',f'deq_{i}_data':f'nd[{i}]',f'deq_{i}_matches':f'nm[{i}]'})
        sc.update({sp+'ar_ready':f'arready[{i}]',sp+'r_ready':f'rr[{i}]',sp+'r_valid':f'rv[{i}]',sp+'r_bits_id':f'ids[{i}]',f'ar_queue_io_deq_{i}_matches':f'sm[{i}]'})
    sc['err_slave_io_r_bits_last']=f'rl[{count}]';assert set(sc)==set(inputs)|set(outputs)
    for kind,name,connections in [('GGControlReadDispatch','dispatch',nc),(helper,'tracker',nt),('SFCReadTracker','sfc',sc)]:tb.append(kind+' '+name+'('+','.join('.'+n+'('+v+')' for n,v in connections.items())+');')
    tb += ["reg [63:0] random_state=64'h184ab21;bit free_slot[64],known[64];reg [5:0] high_tag[64];reg [3:0] data[64];",
        'integer cases=0,accepts=0,stalls=0,retires=0,held=0,collisions=0,simultaneous=0,resets=0;reg [NORMAL:0] seen=0;',
        'function automatic [63:0] randomize_input();random_state=random_state^(random_state<<13);random_state=random_state^(random_state>>7);random_state=random_state^(random_state<<17);return random_state;endfunction',
        'task automatic sample();bit push,pop;integer slot;clock=0;#1;',
        'if({nr,capacity,track,ntag,nm}!=={sr,scapacity,st,stag,sm})$fatal(1,"SFC/native tracker differs cycle %0d",cases);',
        'if(capacity!==free_slot[tag[5:0]] || (av&&nr)!==(track&&capacity))$fatal(1,"capacity/fire differs case %0d capacity %0b free %0b valid %0b ready %0b track %0b",cases,capacity,free_slot[tag[5:0]],av,nr,track);',
        'for(integer i=0;i<ENDPOINTS;i++)begin slot=ids[i][5:0];if(nm[i]!==(!free_slot[slot]&&high_tag[slot]==ids[i][11:6]))$fatal(1,"tag alias matches differ");if(known[slot]&&nd[i]!==data[slot])$fatal(1,"native route storage differs");end',
        'push=track&&capacity;accepts+=push;stalls+=av&&!nr;collisions+=av&&!capacity;resets+=reset;',
        'if(push)begin slot=tag[5:0];free_slot[slot]=0;high_tag[slot]=tag[11:6];data[slot]=target;known[slot]=1;end',
        'pop=0;for(integer i=0;i<ENDPOINTS;i++)if(rv[i]&&rr[i]&&rl[i])begin free_slot[ids[i][5:0]]=1;retires++;seen[i]=1;pop=1;end else if(rv[i]&&(!rr[i]||!rl[i]))held++;',
        'simultaneous+=push&&pop;if(reset)for(integer i=0;i<64;i++)free_slot[i]=1;clock=1;#1;clock=0;cases++;endtask',
        'task automatic defaults();reset=0;av=0;route=0;target=NORMAL;tag=0;arready=\'1;rv=0;rr=0;rl=\'1;for(integer i=0;i<ENDPOINTS;i++)ids[i]=0;endtask',
        'initial begin defaults();for(integer i=0;i<64;i++)begin free_slot[i]=1;known[i]=0;end reset=1;#1;clock=1;#1;clock=0;sample();reset=0;',
        'for(integer i=0;i<64;i++)begin av=1;tag=12\'(i+64*13);target=4\'(i%ENDPOINTS);route=target==NORMAL?0:NORMAL\'(1)<<target;sample();end',
        'for(integer i=0;i<64;i++)begin tag=12\'(i+64*14);sample();end',
        # Directed backpressure plus nonfinal error beats: no retirement until
        # the old transaction is accepted with last. An alias at that edge
        # remains stalled because readiness uses pre-edge free bits.
        'for(integer i=0;i<64;i++)begin defaults();av=1;tag=12\'(i+64*14);target=4\'(i%ENDPOINTS);route=target==NORMAL?0:NORMAL\'(1)<<target;rv[target]=1;ids[target]=12\'(i+64*13);sample();rr[target]=1;if(target==NORMAL)begin rl[NORMAL]=0;sample();rl[NORMAL]=1;end sample();rv=0;sample();end',
        'defaults();reset=1;av=1;tag=4095;sample();defaults();',
        'for(integer endpoint=0;endpoint<ENDPOINTS;endpoint++)begin defaults();av=1;tag=12\'(2048+endpoint);target=4\'(endpoint);route=endpoint==NORMAL?0:NORMAL\'(1)<<endpoint;sample();av=0;rv[endpoint]=1;rr[endpoint]=1;ids[endpoint]=tag;sample();end',
        'for(integer n=0;n<100000;n++)begin defaults();reset=n%397==0;av=randomize_input();tag=randomize_input();target=4\'(randomize_input()%ENDPOINTS);route=target==NORMAL?0:NORMAL\'(1)<<target;arready=randomize_input();',
        'for(integer i=0;i<ENDPOINTS;i++)begin integer slot;slot=integer\'(randomize_input()%64);ids[i]={high_tag[slot],6\'(slot)};rr[i]=randomize_input();rv[i]=!free_slot[slot]&&(randomize_input()%4==0);if(i==NORMAL)rl[i]=randomize_input();end sample();end',
        'if(seen!==\'1 || accepts<1000 || stalls<1000 || retires<1000 || collisions<100 || simultaneous<100 || held<100 || resets<100)$fatal(1,"missing coverage");',
        '$display("PASS LABEL SFC/native AR tracker: %0d cycles, %0d accepts, %0d stalls, %0d final retires, %0d held beats, %0d aliases, %0d simultaneous, %0d resets; all ENDPOINTS endpoints",cases,accepts,stalls,retires,held,collisions,simultaneous,resets);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb).replace('all ENDPOINTS endpoints',f'all {endpoints} endpoints').replace('PASS LABEL',f'PASS {label}')+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'dispatch.sv',out/'tracker.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    report=dict(target=label,normal_regions=count,response_endpoints=endpoints,bound_banks=bindings,golden=str(a.golden),golden_sha256=digest(a.golden),candidate=str(a.tracker),candidate_sha256=digest(a.tracker),preserved_modules=len(before),copied_ports=len(copied),connections=len(expected),annotations=len(annos(a.tracker)),sfc_bank_connections=3*len(bindings),sfc_cone_expressions=len(needed),production_test=(out/'production-test.log').read_text(),simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])


if __name__=='__main__':main()
