#!/usr/bin/env python3
# See LICENSE for license details.
"""Compare the live quad TracerV DMA allocation and native read RTL to SFC.

Projects the read cone and its four sequential counters from the archived SFC
engine, retaining its expressions and clocked statements. Queue outputs are
independent stimulus. Compare payload/last only with R.valid asserted, as AXI
requires; invalid-cycle SFC payload defaults differ from native zero defaults.
This standalone unit simulation does not invoke a FireSim manager gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import resource
from NICMMIOBankCompare import modules
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer
from PrintControlResponsesCompare import clean, equations, module


def composition(candidate):
    prior = (candidate/'post-fame-tracerv-stream-queue.mlir').read_text()
    read = (candidate/'post-fame-cpu-stream-read.mlir').read_text()
    count = (candidate/'post-fame-cpu-stream-count.mlir').read_text()
    streams = {'tracerv_stream'+(f'_{i}' if i else '') for i in range(4)}
    counts = {'tracerv_stream_count'+(f'_{i}' if i else '') for i in range(4)}
    checked = []
    for before,after,old,new,helper,inst,removed in (
        (prior,read,'GGTracerVStreamQueueWrapper','GGCPUStreamReadWrapper','GGCPUStreamRead','cpuStreamRead',streams),
        (read,count,'GGCPUStreamReadWrapper','GGCPUStreamCountWrapper','GGCPUStreamCountBank','streamCount',counts)):
        bm,am=modules(before),modules(after)
        assert all(am[n]==body for n,body in bm.items()), 'changed preceding module'
        assert set(am)-set(bm)=={new,helper}
        copied=[(d,n,t) for d,n,t in native_ports(bm[old]) if n not in removed]
        ports=native_ports(am[new]); assert ports[:len(copied)]==copied
        nets={('sim.' if d=='in' else 'top.')+n:('top.' if d=='in' else 'sim.')+n for d,n,t in copied}
        nets.update({inst+'.clock':'top.hostClock',inst+'.reset':'top.hostReset'})
        for i in range(4):
            name=('tracerv_stream' if inst=='cpuStreamRead' else 'tracerv_stream_count')+(f'_{i}' if i else '')
            nets[inst+('.stream_' if inst=='cpuStreamRead' else '.count_')+str(i)]='sim.'+name
        if inst=='cpuStreamRead':
            for d,n,t in native_ports(am[helper])[6:]:
                nets[('cpuStreamRead.' if d=='in' else 'top.cpu_stream_')+n]=('top.cpu_stream_' if d=='in' else 'cpuStreamRead.')+n
            assert len(ports)==len(copied)+12
        else:
            nets['top.cpuStream_mcr']='streamCount.mcr'; assert len(ports)==len(copied)+1
        assert wiring(am[new])==nets, 'crossed or missing live stream/count route'
        # Deliberately cross the fourth live connection: the comparison must fail.
        source='tracerv_stream' if inst=='cpuStreamRead' else 'tracerv_stream_count'
        bad=re.sub(r'(firrtl\.(?:strictconnect|connect) %\w+, %sim_'+source+r')_3\b',r'\1_2',am[new])
        assert bad!=am[new] and wiring(bad)!=nets, 'crossed fourth route was not detected'
        checked.append(dict(wrapper=new,connections=len(nets),retained_modules=len(bm)))
        aa=json.loads((candidate/('post-fame-tracerv-stream-queue-all.json' if inst=='cpuStreamRead' else 'post-fame-cpu-stream-read-all.json')).read_text())
        bb=json.loads((candidate/('post-fame-cpu-stream-read-all.json' if inst=='cpuStreamRead' else 'post-fame-cpu-stream-count-all.json')).read_text())
        assert transfer(aa,old,new,{n for d,n,t in copied})==bb
    return checked


def count_check(ir, engine, out, run):
    helper = modules(ir)['GGCPUStreamCountBank']
    (out/'count.mlir').write_text('module { firrtl.circuit "GGCPUStreamCountBank" {\n'+helper+'\n} }\n')
    run(['firtool',out/'count.mlir','--format=mlir','--disable-all-randomization','-o',out/'count.sv'],'count-firtool')
    header,_=module(clean((out/'count.sv').read_text()),'GGCPUStreamCountBank')
    ports=[]; direction=None; width=''
    for declaration in header.split(','):
        m=re.fullmatch(r'\s*(?:(input|output)\s+)?(\[[^\]]+\])?\s*(\w+)\s*',declaration)
        assert m, declaration
        if m[1]: direction,width=m[1],m[2] or ''
        assert direction
        ports.append((direction,width,m[3]))
    assert len([n for d,w,n in ports if n.startswith('count_')])==4
    tb=['module checkCount;']
    for direction,width,name in ports:
        tb.append(f'{"reg" if direction=="input" else "wire"} {width} {name}'+('=0;' if direction=='input' else ';'))
    tb.append('GGCPUStreamCountBank dut('+','.join(f'.{n}({n})' for d,w,n in ports)+');')
    tb += ['initial begin for(integer cycle=0;cycle<8192;cycle++)begin',
           'reset=cycle%2;']
    for i in range(4):tb.append(f'count_{i}=13\'(cycle+1957*{i});')
    tb.append('#1;')
    nets=equations(engine)
    for i in range(4):
        expression=nets[f'crFile_io_mcr_read_{i}_bits']
        leaf=f'TRACERVBRIDGEMODULE_{i}_to_cpu_stream_outgoingQueueIO_q_io_count'
        assert expression=="{{19'd0}, "+leaf+"}"
        tb.append(f'if(mcr_read_{i}_bits!=='+expression.replace(leaf,f'count_{i}')+f')$fatal(1,"count {i}");')
        tb.append(f'if(mcr_read_{i}_valid!==1 || mcr_write_{i}_ready!==1)$fatal(1,"count handshakes {i}");')
    tb += ['end $display("PASS SFC four count expressions, 8192 values per bank, readonly handshakes"); $finish; end endmodule']
    (out/'count-check.sv').write_text('\n'.join(tb)+'\n')
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','checkCount','--Mdir',out/'count-obj','-o','check',out/'count-check.sv',out/'count.sv'],'count-verilator')
    run([out/'count-obj/check'],'count-simulation')
    return (out/'count-simulation.log').read_text()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('candidate', 'golden', 'header', 'output-dir'):
        p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    composed = composition(a.candidate.parent)
    ir = a.candidate.read_text()
    helper = re.search(r'^    firrtl.module @GGCPUStreamRead\(.*?^    }', ir, re.M | re.S)[0]
    rows = re.findall(r'\{bufferBaseAddress = (\d+) : i64, depth = (\d+) : i64, index = (\d+) : i64, name = "([^"]+)", port = "([^"]+)", widthBytes = (\d+) : i64\}', helper)
    allocations = [(name, int(index), int(base), int(depth), int(width))
                   for base, depth, index, name, port, width in rows]
    names = [f'TRACERVBRIDGEMODULE_{i}_to_cpu_stream' for i in range(4)]
    assert allocations == [(name, i, i*524288, 6144, 64) for i,name in enumerate(names)]
    assert [port for base,depth,index,name,port,width in rows] == ['tracerv_stream' + (f'_{i}' if i else '') for i in range(4)]
    header = a.header.read_text()
    serialized = re.findall(r'StreamParameters\(\s*std::string\(R"ESC\(([^)]+)\)ESC"\),\s*(\d+)ULL,\s*(\d+)ULL,\s*(\d+),\s*(\d+)\s*\)', header)
    assert [(n,int(base),int(count),int(depth),int(width)) for n,base,count,depth,width in serialized] == [
        (name,i*524288,752+4*i,6144,64) for i,name in enumerate(names)]
    for i,name in enumerate(names):
        assert f'name = "{name}_count", offset = {4*i} : i32, readable = true, writeable = false' in ir
    (out / 'read.mlir').write_text('module { firrtl.circuit "GGCPUStreamRead" {\n' + helper + '\n} }\n')
    run(['firtool', out/'read.mlir', '--format=mlir', '--disable-all-randomization', '-o', out/'native.sv'], 'firtool')
    source = clean(a.golden.read_text()); _, body = module(source, 'CPUManagedStreamEngine'); nets = equations(body)
    count_simulation = count_check(ir,body,out,run)
    # SFC prunes the engine's constant response field into the downstream
    # AXI buffer queue. Its accepted entries store only OKAY responses.
    _, buffer_body = module(source, 'AXI4Buffer_2')
    response_queue = re.search(r'(\w+) nodeIn_r_deq_q\s*\(', buffer_body)[1]
    _, response_body = module(source, response_queue)
    assert equations(response_body)['ram_resp_MPORT_data'].strip() == "2'h0"
    nets['auto_cpu_managed_axi4_in_r_bits_resp'] = "2'h0"
    prefixes = [name + '_' for name in names]
    replacements = {'auto_cpu_managed_axi4_in_' + n: n for n in (
        'ar_valid','ar_ready','ar_bits_id','ar_bits_addr','ar_bits_len','ar_bits_size',
        'r_ready','r_valid','r_bits_id','r_bits_data','r_bits_last','r_bits_resp')}
    for i,pre in enumerate(prefixes):
        for field in ('ready','valid','bits'):
            replacements[pre + 'ser_des_io_narrow_out_' + field] = f'stream_{i}_{field}'
    outputs = ['ar_ready','r_valid','r_bits_id','r_bits_data','r_bits_last','r_bits_resp',*[f'stream_{i}_ready' for i in range(4)]]
    reverse = {v:k for k,v in replacements.items()}
    regs = [pre + 'readBeatCounter' for pre in prefixes]
    sequential = body[body.index('always @(posedge clock)'):]
    # Keep the four original counter updates. Assertions/random initialization
    # after this point concern other engine interfaces, outside the read cone.
    sequential = sequential[:sequential.index('    `ifndef SYNTHESIS')] + '  end\n'
    assert len(re.findall(r'readBeatCounter <=', sequential)) == 12
    required = set(); leaves = set(replacements) - {reverse[x] for x in outputs}
    def need(name):
        if name in required or name in leaves or name in ('clock','reset'): return
        required.add(name)
        if name in nets:
            for token in re.findall(r'\b[A-Za-z_]\w*\b', nets[name]):
                if token in nets or token in leaves or token in regs: need(token)
        elif name not in regs: raise AssertionError('unknown SFC dependency ' + name)
    for output in outputs: need(reverse[output])
    for token in re.findall(r'\b[A-Za-z_]\w*\b', sequential):
        if token in nets or token in regs: need(token)
    def rename(text):
        return re.sub(r'\b[A-Za-z_]\w*\b', lambda m: replacements.get(m[0],m[0]), text)
    native_header, _ = module(clean((out/'native.sv').read_text()), 'GGCPUStreamRead')
    declarations = []
    for name in sorted(required):
        if name in reverse.values() and replacements[name] in outputs: continue
        decl = re.search(r'\b(wire|reg)\s*(\[[^\]]+\])?\s*' + re.escape(name) + r'\b', body)
        assert decl, name
        declarations.append(f'  {decl[1]} {decl[2] or ""} {name};')
    assigns = [f'  assign {rename(name)} = {rename(nets[name])};' for name in sorted(required) if name in nets]
    oracle = 'module SFCQuadRead(' + native_header + ');\n' + '\n'.join(declarations+assigns) + '\n' + rename(sequential) + '\nendmodule\n'
    (out/'sfc-read.sv').write_text(oracle)
    ports = [('clock',1),('reset',1),('ar_valid',1),('ar_bits_id',16),('ar_bits_addr',64),('ar_bits_len',8),('ar_bits_size',3),('r_ready',1),
             *[(n,w) for i in range(4) for n,w in [(f'stream_{i}_valid',1),(f'stream_{i}_bits',512)]]]
    outs = [('ar_ready',1),('r_valid',1),('r_bits_id',16),('r_bits_data',512),('r_bits_last',1),('r_bits_resp',2),*[(f'stream_{i}_ready',1) for i in range(4)]]
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n,w in ports]
    test += [f'wire [{w-1}:0] {prefix}_{n};' for prefix in ('n','s') for n,w in outs]
    for kind,prefix in [('GGCPUStreamRead','n'),('SFCQuadRead','s')]:
        bindings = [f'.{n}({n})' for n,w in ports] + [f'.{n}({prefix}_{n})' for n,w in outs]
        test.append(kind + ' ' + prefix + '(' + ','.join(bindings) + ');')
    test += ["integer transfers=0, stalls=0, misses=0, reads[4]='{0,0,0,0}, lastBeats=0;",
      'initial begin reset=1; ar_bits_size=6; #2; clock=1; #2; clock=0; reset=0;',
      'for(integer cycle=0;cycle<50000;cycle=cycle+1)begin',
      'reset=(cycle%997==996); ar_valid=$urandom; r_ready=$urandom; ar_bits_id=$urandom; ar_bits_len=$urandom;',
      'ar_bits_addr=64\'($urandom_range(0,5))*524288 + $urandom_range(0,524287);',
      # Periodic held AR requests supplement arbitrary address/length changes.
      'if(cycle%512<256)begin ar_valid=1; ar_bits_len=15; ar_bits_addr=64\'(cycle/512%4)*524288;end',
      'if(cycle%512>=256&&cycle%512<288)ar_bits_len=0;',
      'stream_0_valid=$urandom; stream_1_valid=$urandom; stream_2_valid=$urandom; stream_3_valid=$urandom;',
      'for(integer w=0;w<16;w=w+1)begin stream_0_bits[w*32+:32]=$urandom; stream_1_bits[w*32+:32]=$urandom; stream_2_bits[w*32+:32]=$urandom; stream_3_bits[w*32+:32]=$urandom;end', '#1;']
    for name,width in outs:
        guard = 'if(n_r_valid)' if name in ('r_bits_data','r_bits_last') else ''
        test.append(f'{guard} if(n_{name}!==s_{name}) $fatal(1,"{name} mismatch cycle %0d",cycle);')
    test += ['if(n_r_valid&&r_ready)begin transfers++; reads[ar_bits_addr/524288]++;end',
      'if(n_r_valid&&!r_ready)stalls++; if(ar_bits_addr>=2097152)misses++; if(n_r_valid&&r_ready&&n_r_bits_last)lastBeats++;',
      '#1; clock=1; #1; clock=0; end',
      'if(transfers<1000||stalls<1000||misses<1000||reads[0]<500||reads[1]<500||reads[2]<500||reads[3]<500||lastBeats<100)$fatal(1,"coverage");',
      '$display("PASS SFC quad TracerV read cone 50000 cycles, transfers=%0d stalls=%0d misses=%0d streams=%0d,%0d,%0d,%0d last=%0d",transfers,stalls,misses,reads[0],reads[1],reads[2],reads[3],lastBeats); $finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc-read.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    # An executable negative control must detect a missing fourth stream ready.
    native=(out/'native.sv').read_text()
    mutated,n=re.subn(r'(assign stream_3_ready\s*=)[^;]+;', r"\1 1'h0;", native)
    assert n==1
    (out/'mutant.sv').write_text(mutated)
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'mutant-obj','-o','check',out/'check.sv',out/'mutant.sv',out/'sfc-read.sv'], 'mutant-verilator')
    with (out/'mutant-simulation.log').open('w') as log:
        rejected=subprocess.run([out/'mutant-obj/check'],stdout=log,stderr=subprocess.STDOUT,
            preexec_fn=lambda:resource.setrlimit(resource.RLIMIT_CORE,(0,0)))
    assert rejected.returncode!=0 and 'stream_3_ready mismatch' in (out/'mutant-simulation.log').read_text()

    report = dict(golden=str(a.golden.resolve()), golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
        header=str(a.header.resolve()), candidate=str(a.candidate.resolve()), source_allocations=allocations,
        composition=composed, count_offsets=[0,4,8,12], golden_global_count_addresses=[752,756,760,764], cycles=50000, fourth_stream_RTL_mutation_detected=True, result='matched', count_simulation=count_simulation, simulation=(out/'simulation.log').read_text(),
        limitation='Read cone only; invalid R payload/last not compared. Global control address allocation, driver emission and complete quad platform execution remain unsupported.')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n'); print(report['simulation'])

if __name__ == '__main__': main()
