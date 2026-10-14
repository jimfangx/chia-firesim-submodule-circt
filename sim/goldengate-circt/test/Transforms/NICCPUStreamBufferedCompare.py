#!/usr/bin/env python3
"""Execute the live NIC AW/W/B/AR/R composition against archived SFC RTL.

The reference retains AXI4Buffer_2 and its original queue modules, together
with both CPUManagedStreamEngine cones. Independent stream pins replace only
FIFO endpoints; the real native operations and all production annotation
transfers are checked before standalone lowering. No manager gate is invoked.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from PrintControlResponsesCompare import clean, module
from PrintRocketControlMasterCompare import native_ports
from PrintRocketLoadMemEngineCompare import wiring


def transfer(value, old, new, held):
    if isinstance(value, str):
        if value == '~' + old: return '~' + new
        if not value.startswith('~' + old + '|'): return value
        suffix = value[len(old) + 1:]
        prefix = '|' + old + '>'
        if suffix.startswith(prefix):
            port = re.split(r'[.\[]', suffix[len(prefix):])[0]
            if not any(port.startswith(x) for x in held): suffix = '|' + new + '>' + suffix[len(prefix):]
        return '~' + new + suffix
    if isinstance(value, list): return [transfer(v, old, new, held) for v in value]
    if isinstance(value, dict): return {k: transfer(v, old, new, held) for k, v in value.items()}
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate', 'write-buffered-candidate', 'buffered-candidate', 'golden', 'header', 'compose-tool', 'output-dir'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args(); out = a.output_dir.resolve(); out.mkdir(parents=True, exist_ok=False)
    def run(cmd, name):
        with (out / (name + '.log')).open('w') as log:
            subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=True)
    refs = {}
    for direction in ('Read', 'Write'):
        ref = out / (direction.lower() + '-reference'); refs[direction] = ref
        run([sys.executable, Path(__file__).with_name('NICCPUStream' + direction + 'Compare.py'),
             '--candidate', a.candidate, '--golden', a.golden, '--header', a.header, '--output-dir', ref], direction.lower() + '-reference')
    run([a.compose_tool, '--compose-duplex', refs['Write'] / 'write.mlir', refs['Read'] / 'read.mlir', out / 'buffered.mlir'], 'compose')
    native = (out / 'buffered.mlir').read_text(); production = a.buffered_candidate.read_text()
    def modules(text):
        return {m[1]: m[0] for m in re.finditer(r'^    firrtl.module @(\w+)\(.*?^    }', text, re.M | re.S)}
    nm, pm = modules(native), modules(production)
    helpers = ('GGCPUStreamWrite', 'GGCPUStreamRead', 'GGCPUStreamAWQueue2', 'GGCPUStreamWQueue2', 'GGCPUStreamBQueue2', 'GGCPUStreamRQueue2')
    for name in helpers: assert nm[name] == pm[name], 'production helper differs: ' + name
    # Check complete production wrapper wiring, including every copied port;
    # standalone fixtures have fewer unrelated ports than the full candidate.
    old = 'GGCPUStreamWriteResponseBufferWrapper'
    for new, queue, channel, fields in (
        ('GGNICCPUStreamReadBufferWrapper', 'arQueue', 'ar', ('id', 'addr', 'len', 'size')),
        ('GGNICCPUStreamResponseBufferWrapper', 'rQueue', 'r', ('id', 'data', 'resp', 'last'))):
        ports = native_ports(pm[old]); assert native_ports(pm[new]) == ports
        consumed = ['cpu_stream_' + channel + '_' + n for n in ('ready', 'valid')] + ['cpu_stream_' + channel + '_bits_' + f for f in fields]
        expected = {('sim.' + n if d == 'in' else 'top.' + n): ('top.' + n if d == 'in' else 'sim.' + n) for d, n, _ in ports if n not in consumed}
        expected.update({queue + '.clock': 'top.hostClock', queue + '.reset': 'top.hostReset'})
        if channel == 'ar':
            expected.update({'top.cpu_stream_ar_ready': queue + '.enq.ready', queue + '.enq.valid': 'top.cpu_stream_ar_valid', queue + '.deq.ready': 'sim.cpu_stream_ar_ready', 'sim.cpu_stream_ar_valid': queue + '.deq.valid'})
            for f in fields:
                expected[queue + '.enq.bits.' + f] = 'top.cpu_stream_ar_bits_' + f
                expected['sim.cpu_stream_ar_bits_' + f] = queue + '.deq.bits.' + f
        else:
            expected.update({'sim.cpu_stream_r_ready': queue + '.enq.ready', queue + '.enq.valid': 'sim.cpu_stream_r_valid', queue + '.deq.ready': 'top.cpu_stream_r_ready', 'top.cpu_stream_r_valid': queue + '.deq.valid'})
            for f in fields:
                expected[queue + '.enq.bits.' + f] = 'sim.cpu_stream_r_bits_' + f
                expected['top.cpu_stream_r_bits_' + f] = queue + '.deq.bits.' + f
        assert wiring(pm[new]) == expected, new
        old = new
    previous = modules(a.write_buffered_candidate.read_text())
    assert set(pm) - set(previous) == {'GGNICCPUStreamReadBufferWrapper', 'GGNICCPUStreamResponseBufferWrapper', 'GGCPUStreamRQueue2'}
    for name, body in previous.items(): assert pm[name] == body, 'prior body mutated: ' + name
    def annotations(path): return json.loads(path.with_name(path.stem + '-all.json').read_text())
    expected = transfer(annotations(a.write_buffered_candidate), 'GGCPUStreamWriteResponseBufferWrapper', 'GGNICCPUStreamReadBufferWrapper', ('cpu_stream_ar_',))
    expected = transfer(expected, 'GGNICCPUStreamReadBufferWrapper', 'GGNICCPUStreamResponseBufferWrapper', ('cpu_stream_r_',))
    after = annotations(a.buffered_candidate); assert after == expected, 'production target transfer differs'
    (out / 'buffered-lower.mlir').write_text(re.sub(r' attributes \{rawAnnotations = \[.*?\]\}', '', native, count=1))
    run(['firtool', out / 'buffered-lower.mlir', '--format=mlir', '--disable-all-randomization', '-o', out / 'native.sv'], 'firtool')
    source = clean(a.golden.read_text()); h, b = module(source, 'AXI4Buffer_2')
    queues = sorted(set(re.findall(r'\b(Queue_\d+)\s+node(?:In|Out)_\w+\s*\(', b)))
    assert queues == ['Queue_55', 'Queue_56', 'Queue_57', 'Queue_59']
    original = 'module AXI4Buffer_2(' + h + ');\n' + b + '\nendmodule\n'
    for queue in queues:
        qh, qb = module(source, queue); original += 'module ' + queue + '(' + qh + ');\n' + qb + '\nendmodule\n'
    write_inputs = [('aw_valid',1),('aw_bits_id',16),('aw_bits_addr',64),('aw_bits_len',8),('aw_bits_size',3),('w_valid',1),('w_bits_data',512),('w_bits_strb',64),('b_ready',1)]
    read_inputs = [('ar_valid',1),('ar_bits_id',16),('ar_bits_addr',64),('ar_bits_len',8),('ar_bits_size',3),('r_ready',1)]
    write_outputs = [('aw_ready',1),('w_ready',1),('b_valid',1),('b_bits_id',16),('b_bits_resp',2)]
    read_outputs = [('ar_ready',1),('r_valid',1),('r_bits_id',16),('r_bits_data',512),('r_bits_last',1),('r_bits_resp',2)]
    inputs = [('clock',1),('reset',1)] + write_inputs + read_inputs + [('stream_ready',1),('stream_0_valid',1),('stream_0_bits',512),('stream_1_valid',1),('stream_1_bits',512)]
    outputs = write_outputs + read_outputs + [('stream_valid',1),('stream_bits',512),('stream_0_ready',1),('stream_1_ready',1)]
    oracle = ['module SFCBufferedCPU(' + ','.join(f'{d} [{w-1}:0] {n}' for d, ports in [('input', inputs),('output', outputs)] for n,w in ports) + ');']
    axi = write_inputs + read_inputs + write_outputs + read_outputs
    oracle += [f'wire [{w-1}:0] inner_{n};' for n,w in axi]
    bindings = []
    for direction,width,port in re.findall(r'\b(input|output)\s*(\[[^]]+\])?\s*(\w+)', h):
        if port in ('clock','reset'): value = port
        elif port.startswith('auto_in_'): value = port[8:] if any(n == port[8:] for n,w in axi) else ("'0" if direction == 'input' else '')
        elif port.startswith('auto_out_'): value = 'inner_' + port[9:] if any(n == port[9:] for n,w in axi) else ("'0" if direction == 'input' else '')
        else: raise AssertionError(port)
        bindings.append(f'.{port}({value})')
    oracle.append('AXI4Buffer_2 transport(' + ','.join(bindings) + ');')
    for kind, pins, stream_pins in [('SFCNICWrite',write_inputs+write_outputs,['stream_ready','stream_valid','stream_bits']),('SFCNICRead',read_inputs+read_outputs,['stream_0_ready','stream_0_valid','stream_0_bits','stream_1_ready','stream_1_valid','stream_1_bits'])]:
        bindings = [f'.{n}({n})' for n in ['clock','reset'] + stream_pins] + [f'.{n}(inner_{n})' for n,w in pins]
        oracle.append(kind + ' engine' + kind + '(' + ','.join(bindings) + ');')
    oracle.append('endmodule'); (out / 'sfc-buffered.sv').write_text(original + '\n'.join(oracle) + '\n')
    test = ['module check;'] + [f'reg [{w-1}:0] {n}=0;' for n,w in inputs]
    test += [f'wire [{w-1}:0] {pre}_{n};' for pre in ('n','s') for n,w in outputs]
    for kind, pre in [('GGNICCPUStreamResponseBufferWrapper','n'),('SFCBufferedCPU','s')]:
        bindings = []
        for n,w in inputs + outputs:
            port = (dict(clock='hostClock',reset='hostReset').get(n,n) if n.startswith('stream_') or n in ('clock','reset') else 'cpu_stream_' + n) if pre == 'n' else n
            bindings.append(f'.{port}({n if (n,w) in inputs else pre + "_" + n})')
        test.append(kind + ' ' + pre + '(' + ','.join(bindings) + ');')
    test += ['integer cycles=0,writes=0,reads=0,nicReads=0,tracerReads=0,concurrent=0,arAccepted=0,rStalls=0,arStalls=0,resets=0,readBursts=0;',
             'bit awFire,wFire,bFire,arFire,rFire; bit readAddressDone=0; bit pump=1;',
             'task data; begin for(integer j=0;j<16;j++) begin w_bits_data[j*32+:32]=$urandom; stream_0_bits[j*32+:32]=$urandom; stream_1_bits[j*32+:32]=$urandom; end end endtask',
             'task tick; begin',
             'if(pump)begin r_ready=$urandom; stream_0_valid=$urandom; stream_1_valid=$urandom;',
             'for(integer j=0;j<16;j++)begin stream_0_bits[j*32+:32]=$urandom; stream_1_bits[j*32+:32]=$urandom;end',
             'if(!ar_valid && !readAddressDone)begin ar_valid=1; ar_bits_len=readBursts%256; ar_bits_id=readBursts; ar_bits_addr=64\'(readBursts/256%2)*524288+123;end end', '#2;']
    for n,w in outputs:
        guard = 'n_b_valid' if n.startswith('b_bits_') else 'n_r_valid' if n.startswith('r_bits_') else 'n_stream_valid' if n == 'stream_bits' else "1'b1"
        test.append(f'if({guard} && n_{n}!==s_{n}) $fatal(1,"cycle %0d {n}: native=%h SFC=%h",cycles,n_{n},s_{n});')
    test += ['awFire=n_aw_ready&&aw_valid; wFire=n_w_ready&&w_valid; bFire=n_b_valid&&b_ready; arFire=n_ar_ready&&ar_valid; rFire=n_r_valid&&r_ready;',
             'writes+=(n_stream_valid&&stream_ready); reads+=rFire; nicReads+=(stream_0_valid&&n_stream_0_ready); tracerReads+=(stream_1_valid&&n_stream_1_ready);',
             'concurrent+=(rFire&&n_stream_valid&&stream_ready); arAccepted+=arFire; rStalls+=(n_r_valid&&!r_ready); arStalls+=(ar_valid&&!n_ar_ready); resets+=reset;',
             'begin bit lastFire; lastFire=rFire&&n_r_bits_last;',
             'clock=1; #2; clock=0; cycles++;',
             'if(pump)begin if(arFire)begin ar_valid=0;readAddressDone=1;end',
             'if(lastFire)begin if(!readAddressDone)$fatal(1,"R completed without AR"); readBursts++;readAddressDone=0;end end end end endtask',
             'initial begin aw_bits_size=6; ar_bits_size=6; w_bits_strb=~64\'h0; reset=1; pump=0; tick(); reset=0; pump=1;',
             'for(integer len=0;len<256;len++) begin integer beats=0,waited=0;bit addrDone=0,responseDone=0;',
             'aw_bits_len=len;aw_bits_addr=$urandom&64\'h3ffff;aw_bits_id=$urandom;',
             'while(!responseDone)begin waited=waited+1;if(waited>20000)$fatal(1,"write timeout");',
             'if(!addrDone&&!aw_valid)aw_valid=$urandom;if(beats<=len&&!w_valid)begin w_valid=$urandom;data();end',
             'b_ready=$urandom;stream_ready=$urandom;tick();',
             'if(awFire)begin addrDone=1;aw_valid=0;end if(wFire)begin beats++;w_valid=0;end if(bFire)responseDone=1;end',
             'if(!addrDone||beats!=len+1)$fatal(1,"write lost");end',
             'while(readBursts<512)begin if(cycles>500000)$fatal(1,"read timeout");tick();end',
             'pump=0;ar_valid=0;repeat(8)tick();',
             # Six read addresses independently accepted while the host stalls
             # the depth-two R queue, then both sources drain without loss.
             'begin integer accepted=0,responses=0; ar_bits_len=0; ar_bits_addr=0;stream_0_valid=1;stream_1_valid=1;',
             'for(integer j=0;j<300;j++)begin if(accepted<6&&!ar_valid)begin ar_valid=1;ar_bits_id=accepted;ar_bits_addr=64\'(accepted%2)*524288;end',
             'r_ready=(j>=30);data();tick();if(arFire)begin accepted++;ar_valid=0;end responses+=rFire;end',
             'if(accepted!=6||responses!=6)$fatal(1,"queued read lost");end',
             # Arbitrary request changes and misses supplement held traffic.
             # Reset flushes all five queues and all three engine counters.
             'for(integer j=0;j<50000;j++)begin reset=($urandom_range(0,255)==0);',
             'aw_valid=$urandom;w_valid=$urandom;aw_bits_id=$urandom;aw_bits_len=$urandom;aw_bits_addr=(j%31==0)?~64\'h0:($urandom&64\'h3ffff);',
             'ar_valid=$urandom;ar_bits_id=$urandom;ar_bits_len=$urandom;ar_bits_addr=64\'($urandom_range(0,3))*524288+$urandom_range(0,524287);',
             'b_ready=$urandom;r_ready=$urandom;stream_ready=$urandom;stream_0_valid=$urandom;stream_1_valid=$urandom;data();tick();end',
             'if(writes<30000||reads<60000||nicReads<30000||tracerReads<30000||concurrent<1000||arAccepted<512||rStalls<100||arStalls<100||resets<100)$fatal(1,"coverage");',
             '$display("PASS duplex SFC NIC transport: cycles=%0d writes=%0d reads=%0d NIC=%0d TracerV=%0d concurrent=%0d AR=%0d Rstalls=%0d ARstalls=%0d resets=%0d readBursts=512 writeBursts=256",cycles,writes,reads,nicReads,tracerReads,concurrent,arAccepted,rStalls,arStalls,resets);$finish;end endmodule']
    (out / 'check.sv').write_text('\n'.join(test) + '\n')
    run(['verilator','--binary','--timing','-Wno-fatal','--top-module','check','--Mdir',out/'obj','-o','check',out/'check.sv',out/'native.sv',out/'sfc-buffered.sv',refs['Read']/'sfc-read.sv',refs['Write']/'sfc-write.sv'], 'verilator')
    run([out / 'obj/check'], 'simulation')
    def artifact(path): return dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    report = dict(golden=artifact(a.golden),header=artifact(a.header),candidate=artifact(a.buffered_candidate),helpers=list(helpers),queues=queues,
                  unchanged_prior_modules=len(previous),production_ports=len(native_ports(pm['GGNICCPUStreamResponseBufferWrapper'])),annotation_records=len(after),
                  result='matched',read_bursts=512,write_bursts=256,randomized_cycles=50000,simulation=(out/'simulation.log').read_text(),
                  limitation='CPU AXI transport only; NIC MMIO, complete platform integration and runtime remain pending. Invalid response payload is unspecified.')
    (out / 'comparison.json').write_text(json.dumps(report,indent=2)+'\n'); print(report['simulation'])


if __name__ == '__main__': main()
