#!/usr/bin/env python3
"""Execute native NIC AW/W/B queue composition against archived SFC RTL.

The oracle retains AXI4Buffer_2 and all of its original queue modules plus the
SFC incoming write cone. Read pins are inactive. No manager verification runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from PrintControlResponsesCompare import clean, module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate', 'buffered-candidate', 'golden', 'header', 'compose-tool', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    # Reuse the SFC-cone projection and allocation checks, retaining their own
    # independent unbuffered comparison evidence.
    ref = out / 'engine-reference'
    run([sys.executable, Path(__file__).with_name('NICCPUStreamWriteCompare.py'),
         '--candidate', a.candidate, '--golden', a.golden, '--header', a.header,
         '--output-dir', ref], 'engine-reference')
    run([a.compose_tool, '--compose', ref/'write.mlir', out/'buffered.mlir'], 'compose')
    native = (out/'buffered.mlir').read_text(); production = a.buffered_candidate.read_text()
    for name in ('GGCPUStreamWrite', 'GGCPUStreamAWQueue2', 'GGCPUStreamWQueue2', 'GGCPUStreamBQueue2'):
        pattern = r'^    firrtl.module @' + name + r'\(.*?^    }'
        # The helper bodies are identical native operations, not a rewritten
        # software or RTL queue model standing in for the production compiler.
        assert re.search(pattern,native,re.M|re.S)[0] == re.search(pattern,production,re.M|re.S)[0], name
    # Compare every production annotation, including nested targets, through
    # both wrapper transfers. Consumed AW/W and B identities remain inward.
    def transfer(value, old, new, held):
        if isinstance(value,str):
            if value == '~'+old: return '~'+new
            if not value.startswith('~'+old+'|'): return value
            suffix=value[len(old)+1:]
            prefix='|'+old+'>'
            if suffix.startswith(prefix):
                port=re.split(r'[.\[]',suffix[len(prefix):])[0]
                if not any(port.startswith(x) for x in held): suffix='|'+new+'>'+suffix[len(prefix):]
            return '~'+new+suffix
        if isinstance(value,list): return [transfer(v,old,new,held) for v in value]
        if isinstance(value,dict): return {k:transfer(v,old,new,held) for k,v in value.items()}
        return value
    before=json.loads(a.candidate.with_name(a.candidate.stem+'-all.json').read_text())
    expected=transfer(before,'GGCPUStreamWriteWrapper','GGCPUStreamWriteBufferWrapper',('cpu_stream_aw_','cpu_stream_w_'))
    expected=transfer(expected,'GGCPUStreamWriteBufferWrapper','GGCPUStreamWriteResponseBufferWrapper',('cpu_stream_b_',))
    after=json.loads(a.buffered_candidate.with_name(a.buffered_candidate.stem+'-all.json').read_text())
    assert after==expected, 'production buffered annotation transfer differs'
    # Synthetic test annotations have already been checked above and in the
    # composition tool; omit that fixture-only archive for ordinary lowering.
    (out/'buffered-lower.mlir').write_text(re.sub(r' attributes \{rawAnnotations = \[.*?\]\}', '', native, count=1))
    run(['firtool',out/'buffered-lower.mlir','--format=mlir','--disable-all-randomization','-o',out/'native.sv'], 'firtool')
    source = clean(a.golden.read_text()); h,b = module(source,'AXI4Buffer_2')
    queues = sorted(set(re.findall(r'\b(Queue_\d+)\s+node(?:In|Out)_\w+\s*\(',b)))
    assert queues == ['Queue_55','Queue_56','Queue_57','Queue_59'], queues
    original = 'module AXI4Buffer_2(' + h + ');\n' + b + '\nendmodule\n'
    for queue in queues:
        qh,qb = module(source,queue); original += 'module '+queue+'('+qh+');\n'+qb+'\nendmodule\n'
    inputs = [('clock',1),('reset',1),('aw_valid',1),('aw_bits_id',16),('aw_bits_addr',64),('aw_bits_len',8),('aw_bits_size',3),
              ('w_valid',1),('w_bits_data',512),('w_bits_strb',64),('b_ready',1),('stream_ready',1)]
    outputs = [('aw_ready',1),('w_ready',1),('b_valid',1),('b_bits_id',16),('b_bits_resp',2),('stream_valid',1),('stream_bits',512)]
    oracle = ['module SFCBufferedWrite(' + ','.join(f'{d} [{w-1}:0] {n}' for d,ports in [('input',inputs),('output',outputs)] for n,w in ports) + ');']
    axi = inputs[2:-1] + outputs[:5]
    oracle += [f'wire [{w-1}:0] inner_{n};' for n,w in axi]
    bindings = []
    for direction,width,port in re.findall(r'\b(input|output)\s*(\[[^]]+\])?\s*(\w+)',h):
        if port in ('clock','reset'): value=port
        elif port.startswith('auto_in_') and port[8:].split('_')[0] in ('aw','w','b'): value=port[8:]
        elif port.startswith('auto_out_') and port[9:].split('_')[0] in ('aw','w','b'): value='inner_'+port[9:]
        else: value="'0" if direction=='input' else ''
        bindings.append(f'.{port}({value})')
    oracle.append('AXI4Buffer_2 transport('+','.join(bindings)+');')
    bindings = [f'.{n}({n})' for n in ('clock','reset','stream_ready','stream_valid','stream_bits')]
    bindings += [f'.{n}(inner_{n})' for n,w in axi]
    oracle += ['SFCNICWrite engine('+','.join(bindings)+');','endmodule']
    (out/'sfc-buffered.sv').write_text(original+'\n'+'\n'.join(oracle)+'\n')
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n,w in inputs]
    test += [f'wire [{w-1}:0] {pre}_{n};' for pre in ('n','s') for n,w in outputs]
    for kind,pre in [('GGCPUStreamWriteResponseBufferWrapper','n'),('SFCBufferedWrite','s')]:
        bindings=[]
        for n,w in inputs+outputs:
            port = (dict(clock='hostClock',reset='hostReset').get(n,n) if n.startswith('stream_') or n in ('clock','reset') else 'cpu_stream_'+n) if pre=='n' else n
            value = n if (n,w) in inputs else pre+'_'+n
            bindings.append(f'.{port}({value})')
        test.append(kind+' '+pre+'('+','.join(bindings)+');')
    test += ['integer cycles=0, transfers=0, awAccepted=0, completions=0, wStalls=0, bStalls=0, resets=0;',
             'bit awFire,wFire,bFire;', 'task tick; begin #2;']
    for n,w in outputs:
        guard = 'n_b_valid' if n.startswith('b_bits_') else 'n_stream_valid' if n=='stream_bits' else "1'b1"
        test.append(f'if({guard} && n_{n} !== s_{n}) $fatal(1,"cycle %0d {n}: native=%h SFC=%h",cycles,n_{n},s_{n});')
    test += ['awFire=n_aw_ready&&aw_valid; wFire=n_w_ready&&w_valid; bFire=n_b_valid&&b_ready;',
             'awAccepted+=awFire; completions+=bFire; transfers+=(n_stream_valid&&stream_ready);',
             'wStalls+=(w_valid&&!n_w_ready); bStalls+=(n_b_valid&&!b_ready); resets+=reset;',
             'clock=1; #2; clock=0; cycles++; end endtask',
             'task data; begin for(integer j=0;j<16;j++) w_bits_data[j*32+:32]=$urandom; end endtask',
             'initial begin aw_bits_size=6; w_bits_strb=~64\'h0; reset=1; tick(); reset=0;',
             # Independent AW/W arrival and stable accepted payloads on every burst length.
             'for(integer len=0;len<256;len++) begin',
             'integer beats=0, waited=0; bit addrDone=0, responseDone=0;',
             'aw_bits_len=len; aw_bits_addr=$urandom & 64\'h3ffff; aw_bits_id=$urandom;',
             'while(!responseDone) begin',
             'waited=waited+1; if(waited>20000) $fatal(1,"burst timeout %0d",len);',
             'if(!addrDone && !aw_valid) aw_valid=$urandom;',
             'if(beats<=len && !w_valid) begin w_valid=$urandom; data(); end',
             'b_ready=$urandom; stream_ready=$urandom; tick();',
             'if(awFire) begin addrDone=1; aw_valid=0; end',
             'if(wFire) begin beats++; w_valid=0; end',
             'if(bFire) responseDone=1; end',
             'if(!addrDone || beats!=len+1) $fatal(1,"lost burst beat/address"); end',
             # Six single-beat requests exceed all queue capacities. Hold host
             # response ready low first, then release and require all retirement.
             'begin integer addr=0,beats=0,responses=0; aw_bits_len=0; aw_bits_addr=0;',
             'for(integer j=0;j<300;j++) begin',
             'if(addr<6 && !aw_valid) begin aw_valid=1; aw_bits_id=addr; end',
             'if(beats<6 && !w_valid) begin w_valid=1; data(); end',
             'b_ready=(j>=30); stream_ready=(j>=8); tick();',
             'if(awFire) begin addr++; aw_valid=0; end if(wFire) begin beats++; w_valid=0; end responses+=bFire;',
             'end if(addr!=6 || beats!=6 || responses!=6) $fatal(1,"queued requests lost"); end',
             # Host reset flushes only queue controls and engine counters; RAM
             # writes still follow pre-edge handshakes. Arbitrary valid payloads
             # here exercise misses and resets while AW/W/B queues hold tokens.
             'for(integer j=0;j<50000;j++) begin',
             'reset=($urandom_range(0,255)==0); aw_valid=$urandom; w_valid=$urandom;',
             'aw_bits_id=$urandom; aw_bits_len=$urandom; aw_bits_addr=(j%31==0)?~64\'h0:($urandom & 64\'h3ffff);',
             'b_ready=$urandom; stream_ready=$urandom; data(); tick(); end',
             'if(transfers<30000 || awAccepted<256 || completions<256 || wStalls<100 || bStalls<10 || resets<100) $fatal(1,"missing coverage");',
             '$display("PASS buffered SFC NIC write: cycles=%0d transfers=%0d AW=%0d B=%0d Wstalls=%0d Bstalls=%0d resets=%0d heldBursts=256",cycles,transfers,awAccepted,completions,wStalls,bStalls,resets);',
             '$finish; end endmodule']
    (out/'check.sv').write_text('\n'.join(test)+'\n')
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',
         out/'check.sv',out/'native.sv',out/'sfc-buffered.sv',ref/'sfc-write.sv'], 'verilator')
    run([out/'obj/check'], 'simulation')
    def artifact(path): return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report = dict(golden=artifact(a.golden), candidate=artifact(a.buffered_candidate), queues=queues,
                  result='matched', annotation_records=len(after), held_bursts=256, randomized_cycles=50000, simulation=(out/'simulation.log').read_text(),
                  limitation='AW/W/B transport only; NIC MMIO, AR/R buffers and complete NIC platform execution remain pending.')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n'); print(report['simulation'])


if __name__ == '__main__': main()
