#!/usr/bin/env python3
"""Compare selected native host counters/AW FIFO with literal immutable SFC cones.

Component evidence only: this does not exercise the complete functional memory,
MMIO, target compiler, platform, formal proofs, runtime, or compiler timing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def resources(value):
    if isinstance(value, dict):
        if {'path', 'sha256', 'bytes'} <= value.keys():
            yield value
        else:
            for nested in value.values():
                yield from resources(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from resources(nested)


def golden(path):
    receipt = json.loads(path.read_text())
    manifest_path = Path(receipt['manifest']['path'])
    assert receipt['status'] == 'complete'
    assert digest(manifest_path) == receipt['manifest']['sha256']
    manifest = json.loads(manifest_path.read_text())
    assert manifest['status'] == 'complete' and manifest['target'] == receipt['target']
    rtl = manifest_path.parent / 'sfc/FireSim-generated.sv'
    identities = {str(path.resolve()): digest(path), str(manifest_path): digest(manifest_path)}
    assert digest(rtl) == manifest['sfc_artifacts'][rtl.name]['sha256']
    identities[str(rtl)] = digest(rtl)
    for resource in resources(manifest['preMIDAS']):
        file = Path(resource['path'])
        assert file.stat().st_size == resource['bytes'] and digest(file) == resource['sha256']
        identities[str(file)] = resource['sha256']
    text = rtl.read_text()
    modules = {m.group(1): m.group(0) for m in re.finditer(
        r'(?ms)^module\s+(\w+)\b.*?^endmodule\b[^\n]*(?:\n|$)', text)}
    parent, ingress = modules['FASEDMemoryTimingModel'], modules['IngressModule']
    counters = re.findall(r'\b(SatUpDownCounter\w*)\s+SatUpDownCounter\w*\s*\(', parent)
    assert len(counters) == 2 and counters[0] == counters[1]
    queue = re.search(r'\b(\w+)\s+awQueue\s*\(', ingress).group(1)
    count = modules[counters[0]]
    depth = int(re.search(r'ram_addr\s*\[0:(\d+)\]', modules[queue]).group(1)) + 1
    selected = receipt['target'] != 'rocket'
    expected = 20 if receipt['target'] in {'megaboomv3', 'saturn_shuttle'} else 10
    assert depth == expected
    dynamic = bool(re.search(r'input\s+\[\d+:0\]\s+io_max\b', count))
    if dynamic:
        assert receipt['target'] == 'rocket'
        assert re.search(r"assign SatUpDownCounter_io_max = 4'ha;", parent)
        assert re.search(r"assign SatUpDownCounter_1_io_max = 4'ha;", parent)
    else:
        value = int(re.search(r"assign io_full = value >= \d+'h([0-9a-f]+);", count).group(1), 16)
        assert value == expected
    # Record the actual oracle wiring, rather than assuming an unrelated counter
    # of the right width is the functional-host instance.
    bindings = [line.strip() for line in parent.splitlines()
                if re.match(r'\s*assign (SatUpDownCounter\w*_io_(inc|dec|max)|ingress_io_host_(mem_idle|read_inflight))\b', line)]
    assert len(bindings) == (8 if dynamic else 6)
    cone = count + '\n' + modules[queue]
    return dict(target=receipt['target'], capacity=expected, base_capacity=32 if selected else 16,
                counter=counters[0], queue=queue, dynamic_max=dynamic,
                identities=identities, host_bindings=bindings, modules=[counters[0], queue],
                cone_sha256=hashlib.sha256(cone.encode()).hexdigest()), cone


def testbench(info):
    cap = info['capacity']
    width = cap.bit_length()
    fields = dict(addr=35, len=8, size=3, burst=2, lock=1, cache=4, prot=3, qos=4, id=4)
    extra = dict(region=4, user=1)
    lines = ['module tb;', 'logic clock=0,reset=1;',
             'logic arReady=0,arValid=0,rReady=0,rValid=0,rLast=0,awReady=0,awValid=0,bReady=0,bValid=0;',
             f'wire [{width-1}:0] nativeReads,nativeWrites,sfcReads,sfcWrites;',
             'wire nativeIdle,nativeInflight,readEmpty,writeEmpty;',
             'GGFASEDHostOutstanding nativeHost(.clock(clock),.reset(reset),',
             '.transactions_arReady(arReady),.transactions_arValid(arValid),',
             '.transactions_rReady(rReady),.transactions_rValid(rValid),.transactions_rLast(rLast),',
             '.transactions_awReady(awReady),.transactions_awValid(awValid),',
             '.transactions_bReady(bReady),.transactions_bValid(bValid),',
             '.hostMemIdle(nativeIdle),.hostReadInflight(nativeInflight),.readCount(nativeReads),.writeCount(nativeWrites));']
    for n, inc, dec in [('read', 'arReady&&arValid', 'rReady&&rValid&&rLast'),
                         ('write', 'awReady&&awValid', 'bReady&&bValid')]:
        maximum = f',.io_max({width}\'d{cap})' if info['dynamic_max'] else ''
        lines += [f"{info['counter']} sfc{n}(.clock(clock),.reset(reset),.io_inc({inc}),.io_dec({dec}),"
                  f".io_value(sfc{'Reads' if n=='read' else 'Writes'}),.io_empty({n}Empty){maximum});"]
    lines += ['logic enqValid=0,deqReady=0;wire nativeReady,nativeValid,sfcReady,sfcValid;']
    for n, w in (fields | extra).items():
        lines += [f'logic [{w-1}:0] enq_{n}=0;wire [{w-1}:0] native_{n};']
        if n in fields:
            lines += [f'wire [{w-1}:0] sfc_{n};']
    native = ['.clock(clock)', '.reset(reset)', '.enq_valid(enqValid)', '.enq_ready(nativeReady)',
              '.deq_ready(deqReady)', '.deq_valid(nativeValid)']
    sfc = ['.clock(clock)', '.reset(reset)', '.io_enq_valid(enqValid)', '.io_enq_ready(sfcReady)',
           '.io_deq_ready(deqReady)', '.io_deq_valid(sfcValid)']
    for n in fields | extra:
        native += [f'.enq_bits_{n}(enq_{n})', f'.deq_bits_{n}(native_{n})']
        if n in fields:
            sfc += [f'.io_enq_bits_{n}(enq_{n})', f'.io_deq_bits_{n}(sfc_{n})']
    lines += [f'GGFASEDIngressAWQueue{cap} nativeQueue(' + ','.join(native) + ');',
              info['queue'] + ' sfcQueue(' + ','.join(sfc) + ');',
              'integer cycles=0,fullCycles=0,emptyCycles=0,pushes=0,pops=0,simultaneous=0,stalls=0,resets=0;',
              'integer readFull=0,writeFull=0,saturating=0,concurrent=0;',
              'task compare;begin',
              'if(nativeReads!==sfcReads || nativeWrites!==sfcWrites) $fatal(1,"host count mismatch cycle=%0d native=%0d/%0d sfc=%0d/%0d",cycles,nativeReads,nativeWrites,sfcReads,sfcWrites);',
              'if(nativeIdle!==(readEmpty&&writeEmpty) || nativeInflight!==!readEmpty) $fatal(1,"host status mismatch");',
              'if(nativeReady!==sfcReady || nativeValid!==sfcValid) $fatal(1,"AW FIFO status mismatch cycle=%0d",cycles);',
              'if(nativeValid)begin']
    lines += [f'if(native_{n}!==sfc_{n}) $fatal(1,"AW payload {n} mismatch cycle=%0d",cycles);' for n in fields]
    lines += ['end end endtask', 'task step;begin #2;compare();',
              'if(!nativeReady)fullCycles++;if(!nativeValid)emptyCycles++;',
              'if(nativeReady&&enqValid)pushes++;if(nativeValid&&deqReady)pops++;',
              'if(nativeReady&&enqValid&&nativeValid&&deqReady)simultaneous++;',
              'if(nativeValid&&!deqReady)stalls++;if(reset)resets++;',
              f'if(nativeReads=={cap})readFull++;if(nativeWrites=={cap})writeFull++;',
              f'if((nativeReads=={cap}&&arReady&&arValid&&!rValid)||(nativeWrites=={cap}&&awReady&&awValid&&!bValid))saturating++;',
              'if(arReady&&arValid&&rReady&&rValid&&rLast)concurrent++;',
              'clock=1;#2;compare();clock=0;#2;cycles++;end endtask',
              'task payload;begin']
    lines += [f'enq_{n}={w}\'($urandom);' for n, w in (fields | extra).items()]
    lines += ['end endtask', 'integer i,j;initial begin step();reset=0;',
              # Fill beyond both capacities; prove no wrap at 16 for capacity20.
              'arReady=1;arValid=1;awReady=1;awValid=1;enqValid=1;',
              f'for(i=0;i<{cap+6};i++)begin payload();step();end',
              f'if(nativeReads!={cap}||nativeWrites!={cap}||nativeReady!==0)$fatal(1,"selected capacity not reached");',
              'rReady=1;rValid=1;rLast=1;bReady=1;bValid=1;deqReady=1;',
              f'for(i=0;i<{cap*5};i++)begin payload();step();end',
              'arValid=0;awValid=0;enqValid=0;',
              f'for(i=0;i<{cap+6};i++)step();',
              # Directed repeated FIFO wraps, partial occupancy, resets with live traffic.
              'for(j=0;j<4;j++)begin', f'for(i=0;i<{cap-2};i++)begin enqValid=1;deqReady=0;payload();step();end',
              f'for(i=0;i<{cap*3};i++)begin enqValid=1;deqReady=1;payload();step();end',
              'reset=1;step();reset=0;end',
              'for(i=0;i<30000;i++)begin',
              'reset=($urandom_range(0,255)==0);enqValid=($urandom_range(0,4)!=0);deqReady=($urandom_range(0,4)!=0);payload();',
              'arReady=1\'($urandom);arValid=1\'($urandom);rReady=1\'($urandom);rValid=1\'($urandom);rLast=1\'($urandom);',
              'awReady=1\'($urandom);awValid=1\'($urandom);bReady=1\'($urandom);bValid=1\'($urandom);step();end',
              f'if(fullCycles<5||emptyCycles<5||pushes<{cap*20}||pops<{cap*20}||simultaneous<100||stalls<100||resets<5||readFull<5||writeFull<5||saturating<5||concurrent<5)$fatal(1,"missing coverage");',
              '$display("PASS functional capacity cycles=%0d fields=15 full=%0d empty=%0d pushes=%0d pops=%0d simultaneous=%0d stalls=%0d resets=%0d readFull=%0d writeFull=%0d saturating=%0d concurrent=%0d",cycles,fullCycles,emptyCycles,pushes,pops,simultaneous,stalls,resets,readFull,writeFull,saturating,concurrent);',
              '$finish;end endmodule']
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['host-fixture', 'aw-fixture', 'firtool', 'verilator', 'output-dir']:
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--golden-receipt', action='append', required=True, type=Path)
    p.add_argument('--jobs', type=int, default=1)
    a = p.parse_args()
    a.output_dir.mkdir(parents=True, exist_ok=False)
    d = a.output_dir.resolve()
    implementation = Path(__file__).resolve().parents[2]
    source_paths = [implementation / relative for relative in [
        'lib/Transforms/FASEDHostOutstanding.cpp',
        'lib/Transforms/FASEDIngressAWQueue.cpp',
        'lib/Transforms/FASEDRequestLimits.cpp',
        'include/goldengate/FASEDRequestLimits.h',
        'test/Transforms/FASEDHostOutstandingTest.cpp',
        'test/Transforms/FASEDIngressAWQueueTest.cpp']]
    report = dict(passed=False, timing_scope=None,
                  sources={str(x): digest(x) for x in source_paths},
                  tool_versions={str(x.resolve()): subprocess.check_output(
                      [str(x.resolve()), '--version'], text=True).strip()
                      for x in [a.firtool, a.verilator]},
                  scope='Selected host outstanding counter and ingress AW queue literal SFC components only. Counter inc/dec use independently generated host handshakes. Extracted SFC parent has R/B ready constant true; explicit native-ready terms are supplied to counter dec for arbitrary unit handshake stress. AW payload compared only when valid; SFC dead-pruned region/user excluded. Reset flushes queue pointers, preserves RAM, and permits pre-edge RAM writes in both. Verilator initializes unreset state/RAM to zero; valid payload is always explicitly written. This is not whole-target parity, formal proof, F2 runtime or compiler timing.',
                  tools={str(x.resolve()): digest(x) for x in [a.host_fixture, a.aw_fixture, a.firtool, a.verilator]},
                  comparator_sha256=digest(Path(__file__)), targets=[], cones=[])
    try:
        seen = {}
        for receipt in a.golden_receipt:
            info, cone = golden(receipt)
            report['targets'].append(info)
            key = info['cone_sha256']
            if key in seen:
                info['evidence'] = seen[key]
                continue
            work = d / key[:12]
            work.mkdir()
            seen[key] = str(work / 'stage-5.log')
            info['evidence'] = seen[key]
            (work / 'sfc.sv').write_text(cone)
            (work / 'tb.sv').write_text(testbench(info))
            commands = []
            for kind, fixture in [('host', a.host_fixture), ('aw', a.aw_fixture)]:
                commands += [[str(fixture.resolve()), '--fixture', str(info['capacity']), str(info['base_capacity']), str(work / (kind + '.mlir'))],
                             [str(a.firtool.resolve()), str(work / (kind + '.mlir')), '--format=mlir', '--verilog', '-o', str(work / (kind + '.sv'))]]
            commands += [[str(a.verilator.resolve()), '--binary', '--timing', '-DSYNTHESIS', '--top-module', 'tb', '-Wno-fatal', '-j', str(a.jobs), '--Mdir', str(work / 'obj'), str(work / 'host.sv'), str(work / 'aw.sv'), str(work / 'sfc.sv'), str(work / 'tb.sv')], [str(work / 'obj/Vtb')]]
            entry = dict(cone_sha256=key, modules=info['modules'], capacity=info['capacity'], commands=commands, passed=False)
            report['cones'].append(entry)
            for i, command in enumerate(commands):
                with (work / f'stage-{i}.log').open('w') as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=240)
            evidence = (work / 'stage-5.log').read_text()
            assert 'PASS functional capacity' in evidence
            entry.update(passed=True, evidence=evidence,
                         artifacts={str(f.relative_to(work)): digest(f) for f in work.iterdir() if f.is_file()})
        assert {t['target'] for t in report['targets']} == {'rocket', 'megaboomv3', 'shuttle', 'saturn_shuttle'}
        for target in report['targets']:
            assert all(digest(Path(path)) == expected for path, expected in target['identities'].items())
        assert digest(Path(__file__)) == report['comparator_sha256']
        assert all(digest(Path(path)) == expected for path, expected in report['tools'].items())
        assert all(digest(Path(path)) == expected for path, expected in report['sources'].items())
        report['passed'] = True
    finally:
        (d / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    for cone in report['cones']:
        print(cone['evidence'])


if __name__ == '__main__':
    main()
