#!/usr/bin/env python3
"""Compare native incoming NIC DMA RTL with the archived SFC write cone.

Retains original SFC equations and sequential statements. FIFO readiness is
independent stimulus; no FireSim manager gate is invoked. AXI input/response
buffers, NIC controls and complete platform execution are outside this test.
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
    a = p.parse_args()
    out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    ir = a.candidate.read_text()
    helper = re.search(r'^    firrtl.module @GGCPUStreamWrite\(.*?^    }', ir, re.M | re.S)[0]
    rows = re.findall(r'\{bufferBaseAddress = (\d+) : i64, depth = (\d+) : i64, index = (\d+) : i64, name = "([^"]+)", port = "([^"]+)", widthBytes = (\d+) : i64\}', helper)
    allocations = [(name, int(index), int(base), int(depth), int(width))
                   for base, depth, index, name, port, width in rows]
    incoming = 'SIMPLENICBRIDGEMODULE_0_from_cpu_stream'
    assert allocations == [(incoming, 0, 0, 3072, 64)]
    assert 'goldengate.streamAddressSpaceBits = 18 : i64' in helper
    header = a.header.read_text()
    serialized = re.findall(r'StreamParameters\(\s*std::string\(R"ESC\(([^)]+)\)ESC"\),\s*(\d+)ULL,\s*(\d+)ULL,\s*(\d+),\s*(\d+)\s*\)', header)
    assert [(n,int(base),int(count),int(depth),int(width)) for n,base,count,depth,width in serialized] == [
        (incoming,0,600,3072,64), ('SIMPLENICBRIDGEMODULE_0_to_cpu_stream',0,592,3072,64),
        ('TRACERVBRIDGEMODULE_0_to_cpu_stream',524288,596,6144,64)]
    (out / 'write.mlir').write_text('module { firrtl.circuit "GGCPUStreamWrite" {\n' + helper + '\n} }\n')
    run(['firtool', out/'write.mlir', '--format=mlir', '--disable-all-randomization', '-o', out/'native.sv'], 'firtool')
    source = clean(a.golden.read_text()); _, body = module(source, 'CPUManagedStreamEngine'); nets = equations(body)
    # SFC prunes constant OKAY into its downstream response queue.
    _, buffer_body = module(source, 'AXI4Buffer_2')
    response_queue = re.search(r'(\w+) nodeIn_b_deq_q\s*\(', buffer_body)[1]
    _, response_body = module(source, response_queue)
    assert equations(response_body)['ram_resp_MPORT_data'].strip() == "2'h0"
    nets['auto_cpu_managed_axi4_in_b_bits_resp'] = "2'h0"
    pre = incoming + '_'
    replacements = {'auto_cpu_managed_axi4_in_' + n: n for n in (
        'aw_valid','aw_ready','aw_bits_id','aw_bits_addr','aw_bits_len','aw_bits_size',
        'w_ready','w_valid','w_bits_data','w_bits_strb','b_ready','b_valid','b_bits_id','b_bits_resp')}
    for field in ('ready','valid','bits'):
        replacements[pre + 'ser_des_io_narrow_in_' + field] = 'stream_' + field
    outputs = ['aw_ready','w_ready','b_valid','b_bits_id','b_bits_resp','stream_valid','stream_bits']
    reverse = {v:k for k,v in replacements.items()}
    reg = pre + 'writeBeatCounter'
    sequential = body[body.index('always @(posedge clock)'):]
    start = re.search(r'    if \(reset\) begin\s*' + reg, sequential).start()
    sequential = 'always @(posedge clock) begin\n' + sequential[start:sequential.index('    `ifndef SYNTHESIS')] + '  end\n'
    required = set(); leaves = set(replacements) - {reverse[x] for x in outputs}
    def need(name):
        if name in required or name in leaves or name in ('clock','reset'): return
        required.add(name)
        if name in nets:
            for token in re.findall(r'\b[A-Za-z_]\w*\b', nets[name]):
                if token in nets or token in leaves or token == reg: need(token)
        elif name != reg: raise AssertionError('unknown SFC dependency ' + name)
    for output in outputs: need(reverse[output])
    for token in re.findall(r'\b[A-Za-z_]\w*\b', sequential):
        if token in nets or token == reg: need(token)
    def rename(text):
        return re.sub(r'\b[A-Za-z_]\w*\b', lambda m: replacements.get(m[0],m[0]), text)
    native_header, _ = module(clean((out/'native.sv').read_text()), 'GGCPUStreamWrite')
    declarations = []
    for name in sorted(required):
        if name in reverse.values() and replacements[name] in outputs: continue
        decl = re.search(r'\b(wire|reg)\s*(\[[^\]]+\])?\s*' + re.escape(name) + r'\b', body)
        assert decl, name
        declarations.append(f'  {decl[1]} {decl[2] or ""} {name};')
    assigns = [f'  assign {rename(name)} = {rename(nets[name])};' for name in sorted(required) if name in nets]
    oracle = 'module SFCNICWrite(' + native_header + ');\n' + '\n'.join(declarations+assigns) + '\n' + rename(sequential) + '\nendmodule\n'
    (out/'sfc-write.sv').write_text(oracle)
    ports = [('clock',1),('reset',1),('aw_valid',1),('aw_bits_id',16),('aw_bits_addr',64),('aw_bits_len',8),('aw_bits_size',3),
             ('w_valid',1),('w_bits_data',512),('w_bits_strb',64),('b_ready',1),('stream_ready',1)]
    outs = [('aw_ready',1),('w_ready',1),('b_valid',1),('b_bits_id',16),('b_bits_resp',2),('stream_valid',1),('stream_bits',512)]
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n,w in ports]
    test += [f'wire [{w-1}:0] {prefix}_{n};' for prefix in ('n','s') for n,w in outs]
    for kind,prefix in [('GGCPUStreamWrite','n'),('SFCNICWrite','s')]:
        bindings = [f'.{n}({n})' for n,w in ports] + [f'.{n}({prefix}_{n})' for n,w in outs]
        test.append(kind + ' ' + prefix + '(' + ','.join(bindings) + ');')
    test += ['integer cycles=0, transfers=0, stalls=0, misses=0, completions=0, heldCompletions=0;',
             'task tick; begin #2;']
    for n,w in outs:
        test.append(f'if(n_{n} !== s_{n}) $fatal(1,"cycle %0d {n}: native=%h SFC=%h",cycles,n_{n},s_{n});')
    test += ['if(n_w_ready && w_valid) transfers++; if(w_valid && !n_w_ready) stalls++;',
             'if(aw_valid && aw_bits_addr[63:18]!=0) misses++; if(n_b_valid && b_ready) completions++;',
             'clock=1; #2; clock=0; cycles++; end endtask',
             'initial begin aw_bits_size=6; w_bits_strb=~64\'h0; reset=1; tick(); reset=0;',
             'for(integer len=0;len<256;len++) begin',
             'integer sent=0; reset=1; aw_valid=0; w_valid=0; tick(); reset=0; aw_valid=1; aw_bits_len=len;',
             'aw_bits_addr=$urandom & 64\'h3ffff; aw_bits_id=$urandom;',
             'while(sent<=len) begin b_ready=$urandom; stream_ready=$urandom;',
             'if(!w_valid) begin w_valid=$urandom; for(integer j=0;j<16;j++) w_bits_data[j*32+:32]=$urandom; end',
             'tick(); if(n_w_ready && w_valid) begin sent++; w_valid=0; end end heldCompletions++; end',
             'for(integer cycle=0;cycle<50000;cycle++) begin',
             'reset=($urandom_range(0,1023)==0); aw_valid=$urandom; w_valid=$urandom; b_ready=$urandom; stream_ready=$urandom;',
             'aw_bits_id=$urandom; aw_bits_len=$urandom; aw_bits_addr=(cycle%7==0)?~64\'h0:($urandom & 64\'h7ffff);',
             'for(integer j=0;j<16;j++) w_bits_data[j*32+:32]=$urandom; tick(); end',
             'if(!transfers || !stalls || !misses || !completions || heldCompletions!=256) $fatal(1,"missing coverage");',
             '$display("PASS SFC incoming NIC write cone: cycles=%0d transfers=%0d stalls=%0d misses=%0d completions=%0d heldBursts=%0d",cycles,transfers,stalls,misses,completions,heldCompletions); $finish;end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc-write.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    report = dict(golden=str(a.golden.resolve()), golden_sha256=hashlib.sha256(a.golden.read_bytes()).hexdigest(),
        header=str(a.header.resolve()), candidate=str(a.candidate.resolve()), candidate_sha256=hashlib.sha256(a.candidate.read_bytes()).hexdigest(),
        sink_allocations=allocations, held_bursts=256, randomized_cycles=50000, result='matched', simulation=(out/'simulation.log').read_text(),
        limitation='Incoming write cone and allocation only; AXI buffering, NIC controls and complete platform execution remain unsupported.')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n'); print(report['simulation'])

if __name__ == '__main__': main()
