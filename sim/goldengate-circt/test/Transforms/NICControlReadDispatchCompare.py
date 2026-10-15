#!/usr/bin/env python3
"""Compare production NIC or quad Rocket AR binding with archived SFC NastiRouter.

Decoded route/target and tracker capacity are explicit boundaries. This checks
AR dispatch, not response tracking, R arbitration or platform integration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring

FIELDS = {'addr':25, 'len':8, 'size':3, 'burst':2, 'lock':1, 'cache':4,
          'prot':3, 'qos':4, 'region':4, 'id':12, 'user':1}
BINDINGS = [(2,'tracerv_ctrl','TracerVBridgeModule_0'),
            (4,'loadmem_ctrl','LoadMemWidget_0'),
            (5,'peekPokeBridge_ctrl','PeekPokeBridgeModule_0'),
            (6,'uartBridge_ctrl','UARTBridgeModule_0'),
            (8,'clockBridge_ctrl','ClockBridgeModule_0'),
            (11,'resetBridge_ctrl','ResetPulseBridgeModule_0'),
            (10,'cpuStream_ctrl','CPUManagedStreamEngine_0'),
            (7,'nicBridge_ctrl','SimpleNICBridgeModule_0')]
QUAD_BINDINGS = [(2+i, 'tracerv_ctrl'+(f'_{i}' if i else ''),
                  f'TracerVBridgeModule_{i}') for i in range(4)]+[
                 (7,'loadmem_ctrl','LoadMemWidget_0'),
                 (8,'peekPokeBridge_ctrl','PeekPokeBridgeModule_0'),
                 (9,'uartBridge_ctrl','UARTBridgeModule_0'),
                 (10,'clockBridge_ctrl','ClockBridgeModule_0'),
                 (13,'resetBridge_ctrl','ResetPulseBridgeModule_0'),
                 (12,'cpuStream_ctrl','CPUManagedStreamEngine_0')]


def transfer(value, copied, bound):
    old, new = 'GGControlWidgetWriteWrapper', 'GGControlReadDispatchWrapper'
    if isinstance(value,str):
        if value=='~'+old:return '~'+new
        if not value.startswith('~'+old+'|'):return value
        suffix=value[len(old)+1:];prefix='|'+old+'>'
        if suffix.startswith(prefix):
            ref=suffix[len(prefix):];name=re.split(r'[.\[]',ref)[0]
            move=name in copied
            if name in bound:
                tail=ref[len(name):]
                move=tail in ('.b','.r') or tail.startswith(('.b.','.r.'))
            if move:suffix='|'+new+'>'+ref
        return '~'+new+suffix
    if isinstance(value,list):return [transfer(x,copied,bound) for x in value]
    if isinstance(value,dict):return {k:transfer(x,copied,bound) for k,x in value.items()}
    return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('writes','reads','golden','export-tool','output-dir'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--quad',action='store_true',help='compare the archived quad Rocket 14-region geometry')
    a=parser.parse_args();out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=False)
    binding_rows=QUAD_BINDINGS if a.quad else BINDINGS
    count=14 if a.quad else 12; endpoints=count+1; target_width=count.bit_length()
    label='quad Rocket' if a.quad else 'NIC'
    def run(cmd,name):
        with (out/(name+'.log')).open('w') as log:
            subprocess.run(list(map(str,cmd)),stdout=log,stderr=subprocess.STDOUT,check=True)
    def annos(path):return json.loads(path.with_name(path.stem+'-all.json').read_text())
    before,after=[modules(p.read_text()) for p in (a.writes,a.reads)]
    old,new='GGControlWidgetWriteWrapper','GGControlReadDispatchWrapper'
    assert set(after)-set(before)=={new,'GGControlReadDispatch'}
    assert all(after[n]==v for n,v in before.items()),'prior operation body changed'
    bindings={name:i for i,name,_ in binding_rows}
    for i,name,widget in binding_rows:
        row=f'name = "{widget}", port = "{name}", slave = {i} : i32'
        assert row in before[old] and row in after[new],'allocation/binding differs'
    regions=re.search(r'goldengate.controlRegions = (\[.*?\])',before['GGControlAddressDecode'])[1]
    assert regions in after['GGControlReadDispatch'],'dispatch catalog changed'
    region_rows=re.findall(r'slave = (\d+) : i32',regions)
    assert list(map(int,region_rows))==list(range(count)), 'unexpected normal-region geometry'
    consumed={'ctrl_decode_ar_addr','ctrl_error_ar_valid','ctrl_error_ar_bits_addr',
              'ctrl_error_ar_bits_len','ctrl_error_ar_bits_id'}
    copied=[p for p in native_ports(before[old]) if p[1] not in consumed]
    expected_ports=[];expected={}
    for direction,name,ty in copied:
        if name in bindings:
            ty=ty[:ty.index(', ar:')]+ty[ty.index(', r flip:'):]
            for ch in ('b','r'):expected['top.'+name+'.'+ch]='sim.'+name+'.'+ch
        else:expected['sim.'+name if direction=='in' else 'top.'+name]='top.'+name if direction=='in' else 'sim.'+name
        expected_ports.append((direction,name,ty))
    hp=native_ports(after['GGControlReadDispatch'])
    assert len(hp)==count+8 and hp[:2]==[('in','route',f'!firrtl.uint<{count}>'),('in','target',f'!firrtl.uint<{target_width}>')]
    exposed={'master_ar','tracker_ready','track_valid','track_tag','track_target'}|{f'slave_{i}_ar' for i in set(range(count))-set(bindings.values())}
    for direction,name,ty in hp:
        if name in exposed:
            expected_ports.append((direction,'ctrl_read_dispatch_'+name,ty))
            expected['controlReadDispatch.'+name if direction=='in' else 'top.ctrl_read_dispatch_'+name]='top.ctrl_read_dispatch_'+name if direction=='in' else 'controlReadDispatch.'+name
    assert native_ports(after[new])==expected_ports,'production interface differs'
    expected.update({'controlReadDispatch.route':'sim.ctrl_decode_ar_route',
                     'controlReadDispatch.target':'sim.ctrl_decode_ar_target',
                     'sim.ctrl_decode_ar_addr':'top.ctrl_read_dispatch_master_ar.bits.addr',
                     'controlReadDispatch.err_slave_ar.ready':'sim.ctrl_error_ar_ready',
                     'sim.ctrl_error_ar_valid':'controlReadDispatch.err_slave_ar.valid'})
    for i,name,_ in binding_rows:expected['sim.'+name+'.ar']=f'controlReadDispatch.slave_{i}_ar'
    for field in ('addr','len','id'):expected['sim.ctrl_error_ar_bits_'+field]='controlReadDispatch.err_slave_ar.bits.'+field
    assert wiring(after[new])==expected,'production AR wiring differs'
    assert annos(a.reads)==transfer(annos(a.writes),{n for _,n,_ in copied},bindings),'retained targets differ'
    run([a.export_tool,a.writes,out/'production.mlir',out/'helper.mlir'],'production-test')
    assert modules((out/'production.mlir').read_text())==after,'exporter pass differs from compiler'
    assert modules((out/'helper.mlir').read_text())['GGControlReadDispatch']==after['GGControlReadDispatch'],'helper was rebuilt'
    run(['firtool',out/'helper.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'],'firtool')

    golden=clean(a.golden.read_text());_,router=module(golden,'NastiRouter');eq=equations(router)
    _,top=module(golden,'FPGATop');top_eq=equations(top)
    for i,_,widget in binding_rows:
        for field in ('valid','bits_addr','bits_len','bits_id'):
            assert top_eq[widget+'_io_ctrl_ar_'+field]==f'ctrlInterconnect_io_slaves_{i}_ar_'+field
        assert top_eq[f'ctrlInterconnect_io_slaves_{i}_ar_ready']==widget+'_io_ctrl_ar_ready'
    # Extract original expressions, cutting only the already compared decoder,
    # tracker-capacity and slave-ready boundaries. Preserve all readiness muxes.
    inputs={'ar_route':count,'ar_queue_io_enq_ready':1,'io_master_ar_valid':1,
            'io_master_ar_bits_addr':25,'io_master_ar_bits_len':8,'io_master_ar_bits_id':12,
            'err_slave_io_ar_ready':1}
    inputs.update({f'io_slave_{i}_ar_ready':1 for i in range(count)})
    outputs={'io_master_ar_ready':1,'ar_queue_io_enq_valid':1,'ar_queue_io_enq_bits_tag':12}
    for i in range(endpoints):
        prefix='err_slave_io_' if i==count else f'io_slave_{i}_'
        for field,width in (('ar_valid',1),('ar_bits_addr',25),('ar_bits_len',8),('ar_bits_id',12)):
            outputs[prefix+field]=width
    needed={}
    def visit(name):
        if name in inputs or name in needed:return
        assert name in eq,'unresolved SFC expression '+name
        needed[name]=eq[name]
        expr=re.sub(r"\d*'[sS]?[bBoOdDhH][0-9a-fA-F_xXzZ]+",'',eq[name])
        for dep in re.findall(r'\b[a-zA-Z_]\w*\b',expr):visit(dep)
    for name in outputs:visit(name)
    widths={n:w for w,n in re.findall(r'wire\s+\[(\d+:\d+)\]\s+(\w+)',router)}
    def decl(d,n,w):return f'{d} '+(f'[{w-1}:0] ' if w>1 else '')+n
    lines=['module SFCRead('+','.join([decl('input',n,w) for n,w in inputs.items()]+[decl('output',n,w) for n,w in outputs.items()])+');']
    for n,expr in needed.items():lines.append(('assign ' if n in outputs else 'wire '+('['+widths[n]+'] ' if n in widths else ''))+n+' = '+expr+';')
    lines.append('endmodule');(out/'sfc.sv').write_text('\n'.join(lines)+'\n')
    tb=['module check;', f'reg valid,capacity;reg [{count-1}:0] route;reg [{target_width-1}:0] target;reg [{endpoints-1}:0] ready;',
        f'wire nr,sr,nt,st;wire [11:0] ntag,stag;wire [{target_width-1}:0] ntarget;wire [{endpoints-1}:0] nv,sv;']
    for field,width in FIELDS.items():
        tb.append(decl('reg',field,width)+';'+decl('wire','n_'+field,endpoints*width)+';')
        if field in ('addr','len','id'):tb.append(decl('wire','s_'+field,endpoints*width)+';')
    nc={'route':'route','target':'target','tracker_ready':'capacity','master_ar_valid':'valid',
        'master_ar_ready':'nr','track_valid':'nt','track_tag':'ntag','track_target':'ntarget'}
    nc.update({'master_ar_bits_'+f:f for f in FIELDS})
    sc={'ar_route':'route','ar_queue_io_enq_ready':'capacity','io_master_ar_valid':'valid',
        'io_master_ar_ready':'sr','ar_queue_io_enq_valid':'st','ar_queue_io_enq_bits_tag':'stag',
        'err_slave_io_ar_ready':f'ready[{count}]'}
    sc.update({'io_master_ar_bits_'+f:f for f in ('addr','len','id')})
    for i in range(endpoints):
        np='err_slave_ar_' if i==count else f'slave_{i}_ar_'
        sp='err_slave_io_ar_' if i==count else f'io_slave_{i}_ar_'
        nc[np+'ready']=f'ready[{i}]';nc[np+'valid']=f'nv[{i}]';sc[sp+'valid']=f'sv[{i}]'
        if i<count:sc[sp+'ready']=f'ready[{i}]'
        for f,w in FIELDS.items():nc[np+'bits_'+f]=f'n_{f}[{i*w} +: {w}]'
        for f in ('addr','len','id'):sc[sp+'bits_'+f]=f's_{f}[{i*FIELDS[f]} +: {FIELDS[f]}]'
    assert set(sc)==set(inputs)|set(outputs)
    tb+=['GGControlReadDispatch n('+','.join('.'+n+'('+v+')' for n,v in nc.items())+');',
         'SFCRead s('+','.join('.'+n+'('+v+')' for n,v in sc.items())+');',
         f"reg [63:0] random_state=64'h183c71;integer cases=0,fires=0,stalls=0;reg [{endpoints-1}:0] seen=0;",
         'function automatic [63:0] randomize_input();random_state=random_state^(random_state<<13);random_state=random_state^(random_state>>7);random_state=random_state^(random_state<<17);return random_state;endfunction',
         f'task automatic sample(input [{count-1}:0] rr,input bit vv,cc,input [{endpoints-1}:0] ready_mask);',
         'route=rr;valid=vv;capacity=cc;ready=ready_mask;target=randomize_input();']
    tb += [f'{f}=randomize_input();' for f in FIELDS]
    tb += ['#1;if({nr,nt,ntag,nv}!=={sr,st,stag,sv})$fatal(1,"SFC/native AR mismatch case %0d",cases);',
           'if(ntarget!==target || (valid&&nr)!==(nt&&capacity))$fatal(1,"tracker identity/fire mismatch");']
    for f in FIELDS:
        tb.append(f'if(n_{f}!=={{{endpoints}{{{f}}}}})$fatal(1,"broadcast {f} differs");')
        if f in ('addr','len','id'):tb.append(f'if(n_{f}!==s_{f})$fatal(1,"SFC {f} differs");')
    tb += ['seen=seen|nv;fires+=valid&&nr;stalls+=valid&&!nr;cases++;endtask',
           'initial begin',
           f"for(integer endpoint=0;endpoint<{endpoints};endpoint++)for(integer gates=0;gates<4;gates++)for(integer mask=0;mask<{1<<endpoints};mask++)sample(endpoint=={count}?0:{count}'b1<<endpoint,gates[0],gates[1],{endpoints}'(mask));",
           'for(integer i=0;i<100000;i++)sample(randomize_input(),randomize_input(),randomize_input(),randomize_input());',
           f"if(seen!=={endpoints}'h{(1<<endpoints)-1:x} || fires<1000 || stalls<1000)$fatal(1,\"missing endpoint/backpressure coverage\");",
           f'$display("PASS {label} SFC/native AR dispatch: %0d cases, %0d fires, %0d stalls, all {endpoints} endpoints",cases,fires,stalls);$finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','--assert','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc.sv'],'verilator')
    run([out/'obj/check'],'simulation')
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    report=dict(golden=str(a.golden),golden_sha256=digest(a.golden),candidate=str(a.reads),candidate_sha256=digest(a.reads),
                preserved_modules=len(before),copied_ports=len(copied),connections=len(expected),annotations=len(annos(a.reads)),
                normal_regions=count,response_endpoints=endpoints,bindings=binding_rows,
                sfc_widget_connections=5*len(binding_rows),sfc_cone_expressions=len(needed),production_test=(out/'production-test.log').read_text(),
                simulation=(out/'simulation.log').read_text(),limitations=__doc__)
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n');print(report['simulation'])

if __name__=='__main__':main()
