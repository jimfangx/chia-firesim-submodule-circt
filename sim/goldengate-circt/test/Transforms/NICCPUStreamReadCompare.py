#!/usr/bin/env python3
"""Compare the live NIC/TracerV DMA allocation and native read RTL to SFC.

Projects the read cone and its two sequential counters from the archived SFC
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
from PrintControlResponsesCompare import clean, equations, module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for k in ('candidate', 'golden', 'header', 'output-dir'):
        p.add_argument('--' + k, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    ir = a.candidate.read_text()
    helper = re.search(r'^    firrtl.module @GGCPUStreamRead\(.*?^    }', ir, re.M | re.S)[0]
    rows = re.findall(r'\{bufferBaseAddress = (\d+) : i64, depth = (\d+) : i64, index = (\d+) : i64, name = "([^"]+)", port = "([^"]+)", widthBytes = (\d+) : i64\}', helper)
    allocations = [(name, int(index), int(base), int(depth), int(width))
                   for base, depth, index, name, port, width in rows]
    nic = 'SIMPLENICBRIDGEMODULE_0_to_cpu_stream'
    tracer = 'TRACERVBRIDGEMODULE_0_to_cpu_stream'
    incoming = 'SIMPLENICBRIDGEMODULE_0_from_cpu_stream'
    assert allocations == [(nic, 0, 0, 3072, 64), (tracer, 1, 524288, 6144, 64)]
    header = a.header.read_text()
    serialized = re.findall(r'StreamParameters\(\s*std::string\(R"ESC\(([^)]+)\)ESC"\),\s*(\d+)ULL,\s*(\d+)ULL,\s*(\d+),\s*(\d+)\s*\)', header)
    assert [(n,int(base),int(count),int(depth),int(width)) for n,base,count,depth,width in serialized] == [
        (incoming,0,600,3072,64), (nic,0,592,3072,64), (tracer,524288,596,6144,64)]
    for i,name in enumerate((nic,tracer,incoming)):
        assert f'name = "{name}_count", offset = {4*i} : i32, readable = true, writeable = false' in ir
    (out / 'read.mlir').write_text('module { firrtl.circuit "GGCPUStreamRead" {\n' + helper + '\n} }\n')
    run(['firtool', out/'read.mlir', '--format=mlir', '--disable-all-randomization', '-o', out/'native.sv'], 'firtool')
    source = clean(a.golden.read_text()); _, body = module(source, 'CPUManagedStreamEngine'); nets = equations(body)
    # SFC prunes the engine's constant response field into the downstream
    # AXI buffer queue. Its accepted entries store only OKAY responses.
    _, buffer_body = module(source, 'AXI4Buffer_2')
    response_queue = re.search(r'(\w+) nodeIn_r_deq_q\s*\(', buffer_body)[1]
    _, response_body = module(source, response_queue)
    assert equations(response_body)['ram_resp_MPORT_data'].strip() == "2'h0"
    nets['auto_cpu_managed_axi4_in_r_bits_resp'] = "2'h0"
    prefixes = (nic + '_', tracer + '_')
    replacements = {'auto_cpu_managed_axi4_in_' + n: n for n in (
        'ar_valid','ar_ready','ar_bits_id','ar_bits_addr','ar_bits_len','ar_bits_size',
        'r_ready','r_valid','r_bits_id','r_bits_data','r_bits_last','r_bits_resp')}
    for i,pre in enumerate(prefixes):
        for field in ('ready','valid','bits'):
            replacements[pre + 'ser_des_io_narrow_out_' + field] = f'stream_{i}_{field}'
    outputs = ['ar_ready','r_valid','r_bits_id','r_bits_data','r_bits_last','r_bits_resp','stream_0_ready','stream_1_ready']
    reverse = {v:k for k,v in replacements.items()}
    regs = [pre + 'readBeatCounter' for pre in prefixes]
    sequential = body[body.index('always @(posedge clock)'):]
    # The original always block begins with the two outgoing counters. Stop
    # before the incoming write counter, which belongs to a later porting step.
    incoming_reset = re.search(r'    if \(reset\) begin\s*' + incoming + '_writeBeatCounter', sequential)
    assert incoming_reset
    sequential = sequential[:incoming_reset.start()] + '  end\n'
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
    oracle = 'module SFCNICRead(' + native_header + ');\n' + '\n'.join(declarations+assigns) + '\n' + rename(sequential) + '\nendmodule\n'
    (out/'sfc-read.sv').write_text(oracle)
    ports = [('clock',1),('reset',1),('ar_valid',1),('ar_bits_id',16),('ar_bits_addr',64),('ar_bits_len',8),('ar_bits_size',3),('r_ready',1),
             ('stream_0_valid',1),('stream_0_bits',512),('stream_1_valid',1),('stream_1_bits',512)]
    outs = [('ar_ready',1),('r_valid',1),('r_bits_id',16),('r_bits_data',512),('r_bits_last',1),('r_bits_resp',2),('stream_0_ready',1),('stream_1_ready',1)]
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n,w in ports]
    test += [f'wire [{w-1}:0] {prefix}_{n};' for prefix in ('n','s') for n,w in outs]
    for kind,prefix in [('GGCPUStreamRead','n'),('SFCNICRead','s')]:
        bindings = [f'.{n}({n})' for n,w in ports] + [f'.{n}({prefix}_{n})' for n,w in outs]
        test.append(kind + ' ' + prefix + '(' + ','.join(bindings) + ');')
    test += ['integer transfers=0, stalls=0, misses=0, nicReads=0, tracerReads=0, lastBeats=0;',
      'initial begin reset=1; ar_bits_size=6; #2; clock=1; #2; clock=0; reset=0;',
      'for(integer cycle=0;cycle<50000;cycle=cycle+1)begin',
      'reset=(cycle%997==996); ar_valid=$urandom; r_ready=$urandom; ar_bits_id=$urandom; ar_bits_len=$urandom;',
      'ar_bits_addr=64\'($urandom_range(0,3))*524288 + $urandom_range(0,524287);',
      # Periodic held AR requests supplement arbitrary address/length changes.
      'if(cycle%512<256)begin ar_valid=1; ar_bits_len=15; ar_bits_addr=64\'(cycle/512%2)*524288;end',
      'if(cycle%512>=256&&cycle%512<288)ar_bits_len=0;',
      'stream_0_valid=$urandom; stream_1_valid=$urandom;',
      'for(integer w=0;w<16;w=w+1)begin stream_0_bits[w*32+:32]=$urandom; stream_1_bits[w*32+:32]=$urandom;end', '#1;']
    for name,width in outs:
        guard = 'if(n_r_valid)' if name in ('r_bits_data','r_bits_last') else ''
        test.append(f'{guard} if(n_{name}!==s_{name}) $fatal(1,"{name} mismatch cycle %0d",cycle);')
    test += ['if(n_r_valid&&r_ready)begin transfers++; if(ar_bits_addr<524288)nicReads++;else tracerReads++;end',
      'if(n_r_valid&&!r_ready)stalls++; if(ar_bits_addr>=1048576)misses++; if(n_r_valid&&r_ready&&n_r_bits_last)lastBeats++;',
      '#1; clock=1; #1; clock=0; end',
      'if(transfers<1000||stalls<1000||misses<1000||nicReads<500||tracerReads<500||lastBeats<100)$fatal(1,"coverage");',
      '$display("PASS SFC NIC/TracerV read cone 50000 cycles, transfers=%0d stalls=%0d misses=%0d NIC=%0d TracerV=%0d last=%0d",transfers,stalls,misses,nicReads,tracerReads,lastBeats); $finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc-read.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    report = dict(golden=str(a.golden.resolve()), golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
        header=str(a.header.resolve()), candidate=str(a.candidate.resolve()), source_allocations=allocations,
        count_offsets=[0,4,8], cycles=50000, result='matched', simulation=(out/'simulation.log').read_text(),
        limitation='Read cone only; invalid R payload/last not compared. Incoming AXI writes, NIC controls and complete platform execution remain unsupported.')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n'); print(report['simulation'])

if __name__ == '__main__': main()
