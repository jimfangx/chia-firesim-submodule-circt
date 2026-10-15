#!/usr/bin/env python3
# See LICENSE for license details.
"""Execute the four production quad-Rocket queues against the SFC Queue_50.

Standalone compiler-unit Verilator differential. Checks routing, annotations,
independent storage, count, full/empty, wrap, stalls and reset-time RAM writes.
CPU allocation, global MMIO and simulator integration remain separate stages.
"""
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import sys
from NICMMIOBankCompare import modules
from PrintControlResponsesCompare import clean, module, instance, equations
from PrintRocketControlMasterCompare import native_ports
from PrintRocketFASEDMMIOBankCompare import wiring
from PrintSelectedFASEDTimingCycleCompare import transfer


def composition(prior, text):
    before, after = modules(prior), modules(text)
    wrapper = 'GGTracerVStreamQueueWrapper'
    original = native_ports(before['GGTracerVBridgeControlWrapper'])
    current = native_ports(after[wrapper])
    assert current[:len(original)] == original
    connections = {}
    streams = set()
    for bank in range(4):
        suffix = f'_{bank}' if bank else ''
        stream, count = 'tracerv_stream'+suffix, 'tracerv_stream_count'+suffix
        streams.add(stream)
        assert current[len(original)+bank] == ('out', count, '!firrtl.uint<13>')
        fifo = f'TRACERVBRIDGEMODULE_{bank}_to_cpu_stream_outgoingQueueIO_q'
        connections.update({fifo+'.clock': 'top.hostClock', fifo+'.reset': 'top.hostReset',
                            fifo+'.enq': 'sim.'+stream, 'top.'+stream: fifo+'.deq',
                            'top.'+count: fifo+'.count'})
        helper = after['GGTracerVStreamQueue6144'+suffix]
        assert f'index = {bank} : i64' in helper
        assert f'name = "TRACERVBRIDGEMODULE_{bank}_to_cpu_stream"' in helper
        assert 'widthBytes = 64 : i64' in helper and 'depth = 6144 : i64' in helper
        memories = [line for line in helper.splitlines() if ' = firrtl.mem ' in line]
        assert len(memories) == 1 and all(x in memories[0] for x in
            ('depth = 6144', 'readLatency = 0', 'writeLatency = 1', 'goldengate.ramStyle = "ULTRA"'))
        assert len(re.findall(r' = firrtl.regreset ', helper)) == 3
        assert len(re.findall(r' = firrtl.reg ', helper)) == 1
    assert len(current) == len(original)+4
    for direction, name, _ in original:
        if name in streams: continue
        connections[('sim.' if direction == 'in' else 'top.')+name] = ('top.' if direction == 'in' else 'sim.')+name
    assert wiring(after[wrapper]) == connections, 'crossed or missing stream/count route'
    assert all(after[n] == body for n, body in before.items())
    assert set(after)-set(before) == {wrapper} | {'GGTracerVStreamQueue6144'+(f'_{i}' if i else '') for i in range(4)}
    return len(before), {name for _, name, _ in original}-streams


def main():
    candidate, golden, firtool, out = [Path(p).resolve() for p in sys.argv[1:]]
    out.mkdir(parents=True, exist_ok=False)
    prior = (candidate/'post-fame-tracerv-control.mlir').read_text()
    text = (candidate/'post-fame-tracerv-stream-queue.mlir').read_text()
    unchanged, copied = composition(prior, text)
    bad = text.replace('firrtl.connect %TRACERVBRIDGEMODULE_3_to_cpu_stream_outgoingQueueIO_q_enq, %sim_tracerv_stream_3',
                       'firrtl.connect %TRACERVBRIDGEMODULE_3_to_cpu_stream_outgoingQueueIO_q_enq, %sim_tracerv_stream_2')
    assert bad != text
    try: composition(prior, bad)
    except AssertionError: pass
    else: raise AssertionError('crossed fourth queue accepted')
    annotations = json.loads((candidate/'post-fame-tracerv-control-all.json').read_text())
    observed = json.loads((candidate/'post-fame-tracerv-stream-queue-all.json').read_text())
    assert transfer(annotations, 'GGTracerVBridgeControlWrapper', 'GGTracerVStreamQueueWrapper', copied) == observed
    reference = clean(golden.read_text())
    engine = module(reference, 'CPUManagedStreamEngine')[1]
    oracle_nets = equations(engine)
    for bank in range(4):
        fifo = instance(engine, 'Queue_50', f'TRACERVBRIDGEMODULE_{bank}_to_cpu_stream_outgoingQueueIO_q')
        assert oracle_nets.get(fifo['clock'], fifo['clock']) == 'clock'
        assert oracle_nets.get(fifo['reset'], fifo['reset']) == 'reset'
        for field in ('bits', 'valid'):
            assert oracle_nets[fifo['io_enq_'+field]] == f'streamsToHostCPU_{bank}_{field}'
        assert oracle_nets[f'streamsToHostCPU_{bank}_ready'] == fifo['io_enq_ready']
    h, body = module(reference, 'Queue_50')
    assert re.search(r'reg \[511:0\] ram \[0:6143\]', body)
    (out/'oracle.sv').write_text('module Queue_50('+h+');'+body+'endmodule\n')
    def run(cmd, name, check=True):
        with (out/(name+'.log')).open('w') as log:
            return subprocess.run(list(map(str, cmd)), stdout=log, stderr=subprocess.STDOUT, check=check,
                preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
    native = {}
    for bank in range(4):
        name = 'GGTracerVStreamQueue6144'+(f'_{bank}' if bank else '')
        helper = modules(text)[name]
        mlir, rtl = out/(name+'.mlir'), out/(name+'.sv')
        mlir.write_text('module { firrtl.circuit "'+name+'" {\n'+helper+'\n} }\n')
        run([firtool, mlir, '--format=mlir', '--disable-all-randomization', '-o', rtl], name)
        for m in re.finditer(r'^module (\w+)\(.*?^endmodule', clean(rtl.read_text()), re.M | re.S):
            if m[1] in native: assert native[m[1]] == m[0]
            native[m[1]] = m[0]
    (out/'native.sv').write_text('\n'.join(native.values())+'\n')
    tb = [r'''module check;
  reg clock=0;
  reg [3:0] reset=15, enq_valid=0, deq_ready=0;
  reg [511:0] enq_bits[4];
  wire [3:0] nready,nvalid,sready,svalid;
  wire [511:0] nbits[4],sbits[4];
  wire [12:0] ncount[4],scount[4];
''']
    for bank in range(4):
        name = 'GGTracerVStreamQueue6144'+(f'_{bank}' if bank else '')
        for kind, prefix in [(name, ''), ('Queue_50', 'io_')]:
            pins = [f'.{prefix}{pin}({signal}[{bank}])' for pin, signal in
                [('enq_ready','nready' if not prefix else 'sready'),('enq_valid','enq_valid'),
                 ('enq_bits','enq_bits'),('deq_ready','deq_ready'),('deq_valid','nvalid' if not prefix else 'svalid'),
                 ('deq_bits','nbits' if not prefix else 'sbits'),('count','ncount' if not prefix else 'scount')]]
            tb.append(kind+f' {"reference" if prefix else "native"}{bank}(.clock(clock),.reset(reset[{bank}]),'+','.join(pins)+');')
    tb.append(r'''
  reg [511:0] expected[4][6144];
  integer head[4],tail[4],count[4],fullCycles[4],emptyCycles[4],duals[4],resets[4],pushes[4],pops[4],wraps[4],resetWrites[4];
  reg push,pop;
  initial begin
    for(integer j=0;j<4;j++)begin
      head[j]=0;tail[j]=0;count[j]=0;enq_bits[j]=0;
      fullCycles[j]=0;emptyCycles[j]=0;duals[j]=0;resets[j]=0;pushes[j]=0;pops[j]=0;wraps[j]=0;resetWrites[j]=0;
    end
    #2;clock=1;#2;clock=0;reset=0;
    for(integer cycle=0;cycle<50000;cycle++)begin
      for(integer j=0;j<4;j++)begin
        for(integer word=0;word<16;word++)enq_bits[j][word*32+:32]=$urandom;
        // Staggered fill/drain and long simultaneous runs force distinct bank
        // occupancies, full stalls, both pointer wraps and read/write collisions.
        enq_valid[j]=cycle>=j*500 && cycle<6500+j*500 || cycle>=14000 && cycle<35000;
        deq_ready[j]=cycle>=6400+j*500 && cycle<14000 || cycle>=15500+j*300 && cycle<35000;
        reset[j]=0;
        if(cycle>=35000)begin
          enq_valid[j]=($urandom%4)!=0;deq_ready[j]=($urandom%4)!=0;
          reset[j]=cycle%(113+j*14)==0;
        end
      end
      #2;
      for(integer j=0;j<4;j++)begin
        if(nready[j] !== sready[j] || nready[j] !== (count[j]<6144))$fatal(1,"ready %d/%d",cycle,j);
        if(nvalid[j] !== svalid[j] || nvalid[j] !== (count[j]>0))$fatal(1,"valid %d/%d",cycle,j);
        if(ncount[j] !== scount[j] || ncount[j] !== 13'(count[j]))$fatal(1,"count %d/%d",cycle,j);
        if(nbits[j] !== sbits[j])$fatal(1,"RAM lookahead/reset payload %d/%d",cycle,j);
        if(nvalid[j] && nbits[j] !== expected[j][head[j]])$fatal(1,"ordering %d/%d",cycle,j);
        push=nready[j]&&enq_valid[j];pop=nvalid[j]&&deq_ready[j];
        fullCycles[j]+=count[j]==6144;emptyCycles[j]+=count[j]==0;duals[j]+=push&&pop;
        resets[j]+=reset[j];resetWrites[j]+=reset[j]&&push;pushes[j]+=push;pops[j]+=pop;
        if(push)begin expected[j][tail[j]]=enq_bits[j];wraps[j]+=tail[j]==6143;tail[j]=(tail[j]+1)%6144;end
        if(pop)begin wraps[j]+=head[j]==6143;head[j]=(head[j]+1)%6144;end
        count[j]+=integer'(push)-integer'(pop);
        if(reset[j])begin head[j]=0;tail[j]=0;count[j]=0;end
      end
      clock=1;#2;clock=0;
    end
    for(integer j=0;j<4;j++)begin
      $display("coverage bank %0d: push=%0d pop=%0d empty=%0d wrap=%0d",j,pushes[j],pops[j],emptyCycles[j],wraps[j]);
      if(fullCycles[j]<200 || emptyCycles[j]<200 || duals[j]<18000 || resets[j]<70 || pushes[j]<34000 || pops[j]<34000 ||
         wraps[j]<8 || resetWrites[j]<40)$fatal(1,"coverage bank %0d",j);
      $display("PASS bank %0d: full=%0d empty=%0d dual=%0d reset=%0d push=%0d pop=%0d wrap=%0d resetWrite=%0d",
        j,fullCycles[j],emptyCycles[j],duals[j],resets[j],pushes[j],pops[j],wraps[j],resetWrites[j]);
    end
    $finish;
  end
endmodule
''')
    (out/'check.sv').write_text('\n'.join(tb))
    def build(rtl, label):
        run(['verilator','--binary','--timing','--top-module','check','-Wno-fatal','--Mdir',out/label,
             '-o','check','-j','2',rtl,out/'oracle.sv',out/'check.sv'], label+'-build')
        return run([out/label/'check'], label+'-behavior', check=False).returncode
    assert build(out/'native.sv', 'positive') == 0
    # A valid same-width fourth-bank wrap mutation must fail the executed oracle.
    name = 'GGTracerVStreamQueue6144_3'
    original = modules(text)[name]
    bad = original.replace('firrtl.constant 6143 :', 'firrtl.constant 6142 :')
    assert bad != original
    mlir, rtl = out/'bad-wrap.mlir', out/'bad-wrap.sv'
    mlir.write_text('module { firrtl.circuit "'+name+'" {\n'+bad+'\n} }\n')
    run([firtool,mlir,'--format=mlir','--disable-all-randomization','-o',rtl], 'bad-wrap-lowering')
    h,b = module(clean(rtl.read_text()), name)
    altered = dict(native);altered[name] = 'module '+name+'('+h+');'+b+'endmodule'
    (out/'bad-native.sv').write_text('\n'.join(altered.values())+'\n')
    assert build(out/'bad-native.sv', 'negative-wrap') != 0
    negative = (out/'negative-wrap-behavior.log').read_text()
    assert re.search(r'Assertion failed.*ready\s+\d+/\s*3\b', negative), negative
    report = dict(golden=str(golden),golden_sha256=hashlib.sha256(golden.read_bytes()).hexdigest(),
        golden_module='Queue_50',queues=4,cycles_per_queue=50000,unchanged_modules=unchanged,
        unchanged_annotation_records=len(observed),negative_controls=['crossed fourth queue','valid IR fourth queue wrap'],
        behavior=(out/'positive-behavior.log').read_text(),result='matched',
        limitation='queue boundary only; multi-stream CPU allocation and quad-core simulator remain pending')
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    print(report['behavior'],end='')


if __name__ == '__main__': main()
