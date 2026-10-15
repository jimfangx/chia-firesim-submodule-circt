#!/usr/bin/env python3
"""Compare production single-NIC or quad-TracerV AW/W route queue and dispatch against SFC RTL.

The decoded route, tracker readiness and normal/error slave readiness are
explicit test boundaries. Widget binding, responses and platform are pending.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module, equations, instance
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring


def transfer(value, old, new, copied):
    if isinstance(value, str):
        if value == '~'+old: return '~'+new
        if not value.startswith('~'+old+'|'): return value
        suffix = value[len(old)+1:]
        prefix = '|'+old+'>'
        if suffix.startswith(prefix) and re.split(r'[.\[]', suffix[len(prefix):])[0] in copied:
            suffix = '|'+new+'>'+suffix[len(prefix):]
        return '~'+new+suffix
    if isinstance(value,list): return [transfer(x,old,new,copied) for x in value]
    if isinstance(value,dict): return {k:transfer(x,old,new,copied) for k,x in value.items()}
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('decoder','route','dispatch','golden','export-tool','output-dir'):
        p.add_argument('--'+n,type=Path,required=True)
    p.add_argument("--quad", action="store_true", help="compare the fourteen-region quad Rocket boundary")
    a=p.parse_args(); count=14 if a.quad else 12; out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    def annos(path): return json.loads(path.with_name(path.stem+'-all.json').read_text())
    before,routed,dispatched=[modules(x.read_text()) for x in (a.decoder,a.route,a.dispatch)]
    boundaries=[]
    consumed={'ctrl_write_route_aw_slave_ready','ctrl_write_route_w_slave_ready',
              'ctrl_error_aw_valid','ctrl_error_aw_bits_addr','ctrl_error_aw_bits_id',
              'ctrl_error_w_valid','ctrl_error_w_bits_last'}
    for oldmods,newmods,path,nextpath,old,new,helper,held in (
        (before,routed,a.decoder,a.route,'GGControlDecodeWrapper','GGControlWriteRouteWrapper','GGControlWriteRoute',set()),
        (routed,dispatched,a.route,a.dispatch,'GGControlWriteRouteWrapper','GGControlWriteDispatchWrapper','GGControlWriteDispatch',consumed)):
        assert set(newmods)-set(oldmods)=={new,helper}
        assert all(newmods[n]==body for n,body in oldmods.items()),'prior body changed'
        copied=[x for x in native_ports(oldmods[old]) if x[1] not in held]
        hp=native_ports(newmods[helper])
        additions=([ (d,'ctrl_write_route_'+n,t) for d,n,t in hp[3:] ] if not held else
                   [(d,'ctrl_write_dispatch_'+n,t) for d,n,t in hp if n.startswith('slave_') or n in {'master_aw_bits_len','master_aw_bits_id','master_w_bits_data'}])
        assert native_ports(newmods[new])==copied+additions,'boundary ports differ'
        assert annos(nextpath)==transfer(annos(path),old,new,{n for _,n,_ in copied}),'target transfer differs'
        expected={('sim.'+n if d=='in' else 'top.'+n):('top.'+n if d=='in' else 'sim.'+n) for d,n,_ in copied}
        if not held:
            expected.update({'controlWriteRoute.clock':'top.hostClock','controlWriteRoute.reset':'top.hostReset','controlWriteRoute.aw_route':'sim.ctrl_decode_aw_route'})
            for d,n,_ in hp[3:]:
                expected['controlWriteRoute.'+n if d=='in' else 'top.ctrl_write_route_'+n]='top.ctrl_write_route_'+n if d=='in' else 'controlWriteRoute.'+n
        else:
            for h,n in (('aw_route','ctrl_decode_aw_route'),('w_route','ctrl_write_route_w_route'),('aw_valid','ctrl_write_route_aw_slave_valid'),('w_valid','ctrl_write_route_w_slave_valid'),('err_slave_aw_ready','ctrl_error_aw_ready'),('err_slave_w_ready','ctrl_error_w_ready')):
                expected['controlWriteDispatch.'+h]='sim.'+n
            expected.update({'controlWriteDispatch.master_aw_bits_addr':'top.ctrl_decode_aw_addr','controlWriteDispatch.master_w_bits_last':'top.ctrl_write_route_w_last'})
            for h,n in (('aw_ready','ctrl_write_route_aw_slave_ready'),('w_ready','ctrl_write_route_w_slave_ready'),('err_slave_aw_valid','ctrl_error_aw_valid'),('err_slave_aw_bits_addr','ctrl_error_aw_bits_addr'),('err_slave_aw_bits_id','ctrl_error_aw_bits_id'),('err_slave_w_valid','ctrl_error_w_valid'),('err_slave_w_bits_last','ctrl_error_w_bits_last')):
                expected['sim.'+n]='controlWriteDispatch.'+h
            for d,n,_ in hp:
                if n.startswith('slave_') or n in {'master_aw_bits_len','master_aw_bits_id','master_w_bits_data'}:
                    expected['controlWriteDispatch.'+n if d=='in' else 'top.ctrl_write_dispatch_'+n]='top.ctrl_write_dispatch_'+n if d=='in' else 'controlWriteDispatch.'+n
        assert wiring(newmods[new])==expected,'live route/dispatch connection differs'
        boundaries.append(dict(candidate=str(nextpath),preserved_modules=len(oldmods),copied_ports=len(copied),connections=len(expected),annotations=len(annos(nextpath))))
    assert f'goldengate.queueDepth = {count} : i32' in routed['GGControlWriteRoute']
    assert 'goldengate.queueFlow = false' in routed['GGControlWriteRoute'] and 'goldengate.queuePipe = false' in routed['GGControlWriteRoute']
    regions=re.search(r'goldengate.controlRegions = (\[.*?\])',before['GGControlAddressDecode'])[1]
    assert regions in dispatched['GGControlWriteDispatch'],'catalog changed'
    run([a.export_tool,a.dispatch,out]+(['--quad'] if a.quad else []),'production-test')
    for name in ('GGControlWriteRoute','GGControlWriteDispatch'):
        assert modules((out/(name+'.mlir')).read_text())[name]==dispatched[name],'export rebuilt helper'
        run(['firtool',out/(name+'.mlir'),'--format=mlir','--disable-all-randomization','-o',out/(name+'.sv')],name+'-firtool')

    # Preserve the actual SFC queue and combinational expressions. Replace only
    # the earlier address-decoder, tracker and slave-ready boundaries.
    golden=clean(a.golden.read_text()); _,router=module(golden,'NastiRouter');eq=equations(router)
    kind=re.search(r'(\w+) w_queue \(',router)[1];qh,qb=module(golden,kind)
    assert re.search(rf'reg \[{count-1}:0\] ram \[0:{count-1}\]',qb),'wrong SFC queue geometry'
    qi=instance(router,kind,'w_queue')
    assert qi=={'clock':'w_queue_clock','reset':'w_queue_reset','io_enq_ready':'w_queue_io_enq_ready','io_enq_valid':'w_queue_io_enq_valid','io_enq_bits':'w_queue_io_enq_bits','io_deq_ready':'w_queue_io_deq_ready','io_deq_valid':'w_queue_io_deq_valid','io_deq_bits':'w_queue_io_deq_bits'}
    inputs={'clock':1,'reset':1,'aw_route':count,'aw_queue_io_enq_ready':1,'io_master_aw_valid':1,'io_master_w_valid':1,'io_master_w_bits_last':1,
            'io_master_aw_bits_addr':25,'io_master_aw_bits_len':8,'io_master_aw_bits_id':12,'io_master_w_bits_data':32,'err_slave_io_aw_ready':1,'err_slave_io_w_ready':1}
    for i in range(count):
        for ch in ('aw','w'): inputs[f'io_slave_{i}_{ch}_ready']=1
    outputs={'io_master_aw_ready':1,'io_master_w_ready':1,'aw_valid':1,'w_valid':1,'aw_queue_io_enq_valid':1}
    for i in range(count):
        for field,width in (('aw_valid',1),('aw_bits_addr',25),('aw_bits_len',8),('aw_bits_id',12),('w_valid',1),('w_bits_data',32)):
            # Quad's CPU count bank is read-only; SFC removes this payload
            # from NastiRouter. Keep every handshake and other payload exact.
            if a.quad and i == 12 and field == 'w_bits_data':
                assert f'io_slave_{i}_{field}' not in eq
                continue
            outputs[f'io_slave_{i}_{field}']=width
    for field,width in (('aw_valid',1),('aw_bits_addr',25),('aw_bits_id',12),('w_valid',1),('w_bits_last',1)):
        outputs['err_slave_io_'+field]=width
    leaves=set(inputs)|{'w_queue_io_enq_ready','w_queue_io_deq_valid','w_queue_io_deq_bits'}
    # SFC constructs AW from its low/high decoder concatenation; aw_route is
    # exactly that same decoded route boundary. Override no queue/control expression.
    eq['w_queue_io_enq_bits']='aw_route'
    needed={}
    def visit(name):
        if name in leaves or name in needed:return
        assert name in eq,'unresolved SFC cone '+name
        needed[name]=eq[name]
        expr=re.sub(r"\d*'[sS]?[bBoOdDhH][0-9a-fA-F_xXzZ]+",'',eq[name])
        for dep in re.findall(r'\b[a-zA-Z_]\w*\b',expr):visit(dep)
    for n in set(outputs)|set(qi.values()):visit(n)
    widths={n:w for w,n in re.findall(r'wire\s+\[(\d+:\d+)\]\s+(\w+)',router)}
    def decl(d,n,w):return f'{d} '+(f'[{w-1}:0] ' if w>1 else '')+n
    ports=[decl('input',n,w) for n,w in inputs.items()]+[decl('output',n,w) for n,w in outputs.items()]
    lines=['module SFCWrite('+','.join(ports)+');']
    for n,expr in needed.items():
        lines.append(('assign ' if n in outputs else 'wire '+('['+widths[n]+'] ' if n in widths else ''))+n+' = '+expr+';')
    lines+=[f'wire w_queue_io_enq_ready,w_queue_io_deq_valid; wire [{count-1}:0] w_queue_io_deq_bits;',
            kind+' w_queue('+','.join('.'+n+'('+v+')' for n,v in qi.items())+');','endmodule',
            'module '+kind+'('+qh+');'+qb+'endmodule']
    (out/'sfc.sv').write_text('\n'.join(lines)+'\n')

    # Compose the cloned native helpers using the same live queue/dispatch
    # connections checked above in the production wrappers.
    tb=['module check;', 'reg clock=0,reset,tracker,av,wv,last;',
        f'reg [{count-1}:0] route; reg [{count}:0] ar,wr; reg [24:0] addr; reg [7:0] len; reg [11:0] id; reg [31:0] data;',
        f'wire n_aw_ready,n_w_ready,n_av,n_wv,n_track,d_aw_ready,d_w_ready,n_error;wire [{count-1}:0] n_route;',
        f'wire [{count}:0] n_avs,n_wvs,s_avs,s_wvs;wire s_aw_ready,s_w_ready,s_av,s_wv,s_track;',
        f'wire [{25*(count+1)-1}:0] n_addr,s_addr; wire [{8*(count+1)-1}:0] n_len;wire [{12*(count+1)-1}:0] n_id,s_id;wire [{32*(count+1)-1}:0] n_data;wire [{count}:0] n_last;',
        'GGControlWriteRoute q(.clock(clock),.reset(reset),.aw_route(route),.aw_valid(av),.aw_tracker_ready(tracker),.aw_slave_ready(d_aw_ready),.w_valid(wv),.w_last(last),.w_slave_ready(d_w_ready),.aw_ready(n_aw_ready),.aw_slave_valid(n_av),.aw_track_valid(n_track),.w_ready(n_w_ready),.w_slave_valid(n_wv),.w_route(n_route),.w_error(n_error));']
    conns={'aw_route':'route','w_route':'n_route','aw_valid':'n_av','w_valid':'n_wv','master_aw_bits_addr':'addr','master_aw_bits_len':'len','master_aw_bits_id':'id','master_w_bits_data':'data','master_w_bits_last':'last','aw_ready':'d_aw_ready','w_ready':'d_w_ready'}
    for i in range(count+1):
        prefix=('err_slave' if i==count else 'slave_'+str(i))+'_'
        for ch in ('aw','w'):conns[prefix+ch+'_ready']=f'{"ar" if ch=="aw" else "wr"}[{i}]';conns[prefix+ch+'_valid']=f'n_{"avs" if ch=="aw" else "wvs"}[{i}]'
        for field,width,vec in (('aw_bits_addr',25,'addr'),('aw_bits_len',8,'len'),('aw_bits_id',12,'id'),('w_bits_data',32,'data'),('w_bits_last',1,'last')):
            conns[prefix+field]=f'n_{vec}[{i*width} +: {width}]'
    tb.append('GGControlWriteDispatch d('+','.join('.'+n+'('+v+')' for n,v in conns.items())+');')
    sc={'clock':'clock','reset':'reset','aw_route':'route','aw_queue_io_enq_ready':'tracker','io_master_aw_valid':'av','io_master_w_valid':'wv','io_master_w_bits_last':'last','io_master_aw_bits_addr':'addr','io_master_aw_bits_len':'len','io_master_aw_bits_id':'id','io_master_w_bits_data':'data','err_slave_io_aw_ready':f'ar[{count}]','err_slave_io_w_ready':f'wr[{count}]',
        'io_master_aw_ready':'s_aw_ready','io_master_w_ready':'s_w_ready','aw_valid':'s_av','w_valid':'s_wv','aw_queue_io_enq_valid':'s_track'}
    for i in range(count+1):
        prefix='err_slave_io_' if i==count else f'io_slave_{i}_'
        if i<count:
            for ch in ('aw','w'):sc[prefix+ch+'_ready']=f'{"ar" if ch=="aw" else "wr"}[{i}]'
        for ch in ('aw','w'):sc[prefix+ch+'_valid']=f's_{"avs" if ch=="aw" else "wvs"}[{i}]'
        for field,width,vec in (('aw_bits_addr',25,'addr'),('aw_bits_id',12,'id')):sc[prefix+field]=f's_{vec}[{i*width} +: {width}]'
        if i<count:
            tb.append(f'wire [7:0] s_len_{i};wire [31:0] s_data_{i};')
            sc[prefix+'aw_bits_len']=f's_len_{i}'
            if a.quad and i == 12: tb.append(f'assign s_data_{i} = data;')
            else: sc[prefix+'w_bits_data']=f's_data_{i}'
        else:
            tb.append('wire s_last;');sc[prefix+'w_bits_last']='s_last'
    assert set(sc)==set(inputs)|set(outputs),'SFC interface omitted'
    tb.append('SFCWrite s('+','.join('.'+n+'('+v+')' for n,v in sc.items())+');')
    tb+=[f'integer cycles=0,pushes=0,pops=0,occupancy=0,full_pop=0,empty_w=0,held=0,simultaneous=0,reset_writes=0;reg [{count}:0] seen=0;',
         "reg [63:0] random_state=64'h181c71;",
         'function automatic [63:0] randomize_input();random_state=random_state^(random_state<<13);random_state=random_state^(random_state>>7);random_state=random_state^(random_state<<17);return random_state;endfunction',
         f'task automatic cycle(input bit rst,aa,tt,ww,ll,input [{count-1}:0] rr,input [{count}:0] ars,wrs);',
         'bit push,pop;reset=rst;av=aa;tracker=tt;wv=ww;last=ll;route=rr;ar=ars;wr=wrs;addr=randomize_input();len=randomize_input();id=randomize_input();data=randomize_input();#1;',
         'if({n_aw_ready,n_w_ready,n_av,n_wv,n_track,n_avs,n_wvs}!=={s_aw_ready,s_w_ready,s_av,s_wv,s_track,s_avs,s_wvs})$fatal(1,"handshake/route mismatch cycle %0d",cycles);',
         f'if(n_addr!==s_addr || n_id!==s_id || n_last!={{{count+1}{{last}}}} || s_last!==last)$fatal(1,"payload differs");']
    for i in range(count):tb.append(f'if(n_len[{8*i} +: 8]!==s_len_{i} || n_data[{32*i} +: 32]!==s_data_{i})$fatal(1,"slave {i} payload differs");')
    tb+=[f'push=av&&n_aw_ready;pop=wv&&n_w_ready&&last;seen=seen|n_avs;pushes+=push;pops+=pop;full_pop+=(occupancy=={count}&&pop&&av);empty_w+=(occupancy==0&&wv);held+=(occupancy>0&&wv&&n_w_ready&&!last);simultaneous+=push&&pop;reset_writes+=rst&&push;',
         "if(!rst)occupancy=occupancy+int'(push)-int'(pop);else occupancy=0;",
         f'if(occupancy<0||occupancy>{count})$fatal(1,"occupancy differs");clock=1;#1;clock=0;#1;cycles++;endtask',
         'initial begin',
         "cycle(1,0,1,0,1,0,'1,'1);",
         'for(integer repeatno=0;repeatno<100;repeatno++)begin',
         f"for(integer i=0;i<{count};i++)cycle(0,1,1,0,1,{count}'b1<<i,'1,'1);",
         "cycle(0,1,1,1,1,0,'1,'1);",
         f"for(integer i=0;i<{count-1};i++)begin cycle(0,0,1,1,0,0,'1,'1);cycle(0,0,1,1,1,0,'1,'1);end",
         "cycle(0,1,1,0,1,0,'1,'1);cycle(0,0,1,1,1,0,'1,'1);",
         "cycle(1,1,1,0,1,1,'1,'1);end",
         'for(integer i=0;i<100000;i++)cycle(i%97==0,randomize_input(),randomize_input(),randomize_input(),randomize_input(),randomize_input(),randomize_input(),randomize_input());',
         f"if(pushes<1000||pops<1000||full_pop<100||empty_w<100||held<1000||simultaneous<100||reset_writes<10||seen!=={count+1}'h{(1<<(count+1))-1:x})$fatal(1,\"insufficient corner coverage\");",
         f'$display("PASS {count}-region SFC/native queued write routing: %0d cycles, %0d pushes, %0d pops, full-pop %0d, empty-W %0d, held %0d, simultaneous %0d, reset writes %0d, all {count+1} endpoints",cycles,pushes,pops,full_pop,empty_w,held,simultaneous,reset_writes);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'GGControlWriteRoute.sv',out/'GGControlWriteDispatch.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
    report=dict(golden=str(a.golden),golden_sha256=digest(a.golden),queue=kind,queue_sha256=hashlib.sha256((qh+qb).encode()).hexdigest(),candidate=str(a.dispatch),candidate_sha256=digest(a.dispatch),boundaries=boundaries,sfc_cone_expressions=len(needed),sfc_pruned_read_only_payloads=['io_slave_12_w_bits_data'] if a.quad else [],production_test=(out/'production-test.log').read_text(),simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])

if __name__=='__main__':main()
